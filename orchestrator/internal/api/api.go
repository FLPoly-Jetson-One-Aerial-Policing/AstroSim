// Package api serves the orchestrator's HTTP interface: a small JSON API, a
// server-sent-events stream of status snapshots, and the embedded dashboard.
//
// Routes:
//
//	GET  /api/v1/status  current status.Snapshot as JSON
//	POST /api/v1/start   202 with the snapshot, or 409 {"error":"not_allowed","phase":...}
//	POST /api/v1/stop    same
//	GET  /api/v1/events  server-sent events; event "status" carries a Snapshot
//	GET  /api/v1/logs/download  a zip of the session's logs so far, or 503
//	     {"error":"logs_unavailable"} when file logging is off, or 429
//	     {"error":"busy"} while another download is running
//	GET  /               the dashboard
//
// Every request must carry a Host header from the allow list (a defence
// against DNS rebinding). POSTs must be application/json, and an Origin
// header, if present, must name an allowed host (a defence against
// cross-site requests).
package api

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"log/slog"
	"net/http"
	"net/url"
	"path"
	"strings"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/logs"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/session"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/status"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/web"
)

const (
	// sseMinInterval is the shortest gap between two pushed updates.
	sseMinInterval = 250 * time.Millisecond
	// ssePingInterval keeps idle connections and proxies alive.
	ssePingInterval = 15 * time.Second
	// maxBody bounds the (ignored) request body of POSTs.
	maxBody = 4 << 10
)

// Controller is what the API needs from the session.
type Controller interface {
	// Start requests a start; session.ErrNotAllowed means a 409.
	Start() error
	// Stop requests a stop; session.ErrNotAllowed means a 409.
	Stop() error
	// Snapshot returns the current status.
	Snapshot() status.Snapshot
}

// LogArchive is the session log the API can hand out as a download. The
// logs package's Recorder satisfies it, including a nil one.
type LogArchive interface {
	// Name is the session folder name; "" means logging is unavailable.
	Name() string
	// WriteZip writes a zip of the log files to w. It returns logs.ErrBusy,
	// writing nothing, while another download is running.
	WriteZip(ctx context.Context, w io.Writer) error
}

// Options configures NewHandler.
type Options struct {
	// Session is the stack controller. Required.
	Session Controller
	// Broadcaster supplies snapshot updates for the event stream. Required.
	Broadcaster *status.Broadcaster
	// AllowedHosts lists the accepted Host header values, such as
	// "127.0.0.1:8770" and "localhost:8770".
	AllowedHosts []string
	// Logs serves the log download; nil means logging is unavailable.
	Logs LogArchive
	// Logger receives log lines; nil uses slog.Default.
	Logger *slog.Logger
}

type handler struct {
	opts    Options
	allowed map[string]bool
	log     *slog.Logger
}

// NewHandler returns the HTTP handler serving the API and the dashboard.
func NewHandler(opts Options) http.Handler {
	h := &handler{opts: opts, allowed: map[string]bool{}, log: opts.Logger}
	if h.log == nil {
		h.log = slog.Default()
	}
	for _, a := range opts.AllowedHosts {
		h.allowed[strings.ToLower(a)] = true
	}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/v1/status", h.status)
	mux.HandleFunc("POST /api/v1/start", h.command(opts.Session.Start))
	mux.HandleFunc("POST /api/v1/stop", h.command(opts.Session.Stop))
	mux.HandleFunc("GET /api/v1/events", h.events)
	mux.HandleFunc("GET /api/v1/logs/download", h.logsDownload)
	static, err := fs.Sub(web.Static, "static")
	if err != nil {
		panic(err) // the embed is a compile-time constant
	}
	mux.Handle("GET /", fixedTypes(http.FileServerFS(static)))
	return h.guard(mux)
}

// staticTypes maps the extensions of the embedded dashboard files to their
// Content-Type. The standard library would consult the host's MIME database
// (the registry on Windows), which gives different answers per OS.
var staticTypes = map[string]string{
	".html": "text/html; charset=utf-8",
	".js":   "text/javascript; charset=utf-8",
	".css":  "text/css; charset=utf-8",
}

// fixedTypes wraps the static file handler so the dashboard files are served
// with the same Content-Type on every OS. A path ending in "/" is the index
// page. Errors and unknown extensions keep the file server's own behaviour.
func fixedTypes(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		ext := path.Ext(r.URL.Path)
		if strings.HasSuffix(r.URL.Path, "/") {
			ext = ".html"
		}
		if ct, ok := staticTypes[ext]; ok {
			w.Header().Set("Content-Type", ct)
		}
		next.ServeHTTP(w, r)
	})
}

// guard applies the Host, Content-Type and Origin checks and sets the
// security headers.
func (h *handler) guard(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hd := w.Header()
		hd.Set("X-Content-Type-Options", "nosniff")
		hd.Set("Content-Security-Policy", "default-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'")
		hd.Set("Referrer-Policy", "no-referrer")
		if strings.HasPrefix(r.URL.Path, "/api/") {
			hd.Set("Cache-Control", "no-store")
		}
		if !h.allowed[strings.ToLower(r.Host)] {
			http.Error(w, "forbidden host", http.StatusForbidden)
			return
		}
		if origin := r.Header.Get("Origin"); origin != "" {
			u, err := url.Parse(origin)
			if err != nil || u.Scheme != "http" || !h.allowed[strings.ToLower(u.Host)] {
				http.Error(w, "forbidden origin", http.StatusForbidden)
				return
			}
		}
		if r.Method == http.MethodPost {
			ct := r.Header.Get("Content-Type")
			if i := strings.IndexByte(ct, ';'); i >= 0 {
				ct = ct[:i]
			}
			if strings.ToLower(strings.TrimSpace(ct)) != "application/json" {
				http.Error(w, "content type must be application/json", http.StatusUnsupportedMediaType)
				return
			}
			r.Body = http.MaxBytesReader(w, r.Body, maxBody)
		}
		next.ServeHTTP(w, r)
	})
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func (h *handler) status(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, h.opts.Session.Snapshot())
}

// startWriter delays the response headers until the first byte, so a
// download that fails before producing anything can still answer with an
// error status.
type startWriter struct {
	w       http.ResponseWriter
	start   func()
	started bool
}

func (s *startWriter) Write(p []byte) (int, error) {
	if !s.started {
		s.started = true
		s.start()
	}
	return s.w.Write(p)
}

// logsDownload streams the live session's logs as a zip.
func (h *handler) logsDownload(w http.ResponseWriter, r *http.Request) {
	la := h.opts.Logs
	if la == nil || la.Name() == "" {
		writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "logs_unavailable"})
		return
	}
	sw := &startWriter{w: w, start: func() {
		hd := w.Header()
		hd.Set("Content-Type", "application/zip")
		hd.Set("Content-Disposition", fmt.Sprintf("attachment; filename=%q", "astrosim-logs-"+la.Name()+".zip"))
		w.WriteHeader(http.StatusOK)
	}}
	err := la.WriteZip(r.Context(), sw)
	switch {
	case err == nil:
		if !sw.started { // cannot happen for a real zip, but never send an empty 200
			sw.Write(nil)
		}
	case !sw.started && errors.Is(err, logs.ErrBusy):
		writeJSON(w, http.StatusTooManyRequests, map[string]string{"error": "busy"})
	case !sw.started:
		h.log.Error("log download failed", "err", err)
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "logs_failed"})
	default:
		// The status is already sent. Abort the connection so the client
		// sees a truncated download rather than a zip that looks complete.
		h.log.Error("log download failed part way", "err", err)
		panic(http.ErrAbortHandler)
	}
}

// command returns a handler for a start or stop request.
func (h *handler) command(do func() error) http.HandlerFunc {
	return func(w http.ResponseWriter, _ *http.Request) {
		err := do()
		snap := h.opts.Session.Snapshot()
		switch {
		case err == nil:
			writeJSON(w, http.StatusAccepted, snap)
		case errors.Is(err, session.ErrNotAllowed):
			writeJSON(w, http.StatusConflict, map[string]string{"error": "not_allowed", "phase": snap.Phase})
		default:
			h.log.Error("command failed", "err", err)
			writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "unavailable", "phase": snap.Phase})
		}
	}
}

// events streams snapshots as server-sent events.
func (h *handler) events(w http.ResponseWriter, r *http.Request) {
	fl, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming unsupported", http.StatusInternalServerError)
		return
	}
	hd := w.Header()
	hd.Set("Content-Type", "text/event-stream")
	hd.Set("Cache-Control", "no-store")
	hd.Set("X-Accel-Buffering", "no")
	w.WriteHeader(http.StatusOK)

	send := func(s status.Snapshot) bool {
		data, err := json.Marshal(s)
		if err != nil {
			return false
		}
		_, err = fmt.Fprintf(w, "event: status\ndata: %s\n\n", data)
		fl.Flush()
		return err == nil
	}
	// Subscribe before reading the current snapshot so no update is lost.
	ch, cancel := h.opts.Broadcaster.Subscribe()
	defer cancel()
	if _, err := fmt.Fprint(w, "retry: 2000\n\n"); err != nil {
		return
	}
	if !send(h.opts.Session.Snapshot()) {
		return
	}

	ping := time.NewTicker(ssePingInterval)
	defer ping.Stop()
	last := time.Now()
	var pending *status.Snapshot
	var flushTimer <-chan time.Time
	for {
		select {
		case <-r.Context().Done():
			return
		case s := <-ch:
			// Coalesce: send at once if quiet, else hold the newest and
			// send it when the minimum interval has passed.
			if wait := sseMinInterval - time.Since(last); wait > 0 {
				pending = &s
				if flushTimer == nil {
					flushTimer = time.After(wait)
				}
				continue
			}
			if !send(s) {
				return
			}
			last = time.Now()
		case <-flushTimer:
			flushTimer = nil
			if pending != nil {
				s := *pending
				pending = nil
				if !send(s) {
					return
				}
				last = time.Now()
			}
		case <-ping.C:
			if _, err := fmt.Fprint(w, ": ping\n\n"); err != nil {
				return
			}
			fl.Flush()
		}
	}
}
