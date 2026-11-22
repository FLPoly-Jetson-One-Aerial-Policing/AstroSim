package api

import (
	"bufio"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/session"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/status"
)

const host = "127.0.0.1:8770"

type fakeSession struct {
	mu       sync.Mutex
	snap     status.Snapshot
	startErr error
	stopErr  error
	starts   int
}

func (f *fakeSession) Start() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.starts++
	return f.startErr
}
func (f *fakeSession) Stop() error { return f.stopErr }
func (f *fakeSession) Snapshot() status.Snapshot {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.snap
}

func newServer(t *testing.T) (*httptest.Server, *fakeSession, *status.Broadcaster) {
	t.Helper()
	fs := &fakeSession{snap: status.Snapshot{Phase: "stopped", CanStart: true, Components: []status.Component{{ID: "engine", Name: "Engine"}}}}
	bc := status.NewBroadcaster()
	srv := httptest.NewServer(NewHandler(Options{Session: fs, Broadcaster: bc, AllowedHosts: []string{host, "localhost:8770"}}))
	t.Cleanup(srv.Close)
	return srv, fs, bc
}

// do sends a request with an explicit Host (as a browser would for the real address).
func do(t *testing.T, srv *httptest.Server, method, path, hostHdr string, hdr map[string]string) *http.Response {
	t.Helper()
	req, err := http.NewRequest(method, srv.URL+path, strings.NewReader("{}"))
	if err != nil {
		t.Fatal(err)
	}
	req.Host = hostHdr
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	res, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { res.Body.Close() })
	return res
}

var jsonHdr = map[string]string{"Content-Type": "application/json"}

func TestStatusShape(t *testing.T) {
	srv, _, _ := newServer(t)
	res := do(t, srv, "GET", "/api/v1/status", host, nil)
	if res.StatusCode != 200 || res.Header.Get("Content-Type") != "application/json" {
		t.Fatalf("status %d type %q", res.StatusCode, res.Header.Get("Content-Type"))
	}
	if res.Header.Get("Cache-Control") != "no-store" || res.Header.Get("X-Content-Type-Options") != "nosniff" {
		t.Errorf("headers %v", res.Header)
	}
	if csp := res.Header.Get("Content-Security-Policy"); !strings.Contains(csp, "default-src 'self'") {
		t.Errorf("csp %q", csp)
	}
	var m map[string]any
	if err := json.NewDecoder(res.Body).Decode(&m); err != nil {
		t.Fatal(err)
	}
	if m["phase"] != "stopped" || m["canStart"] != true {
		t.Errorf("body %v", m)
	}
	if comps, _ := m["components"].([]any); len(comps) != 1 {
		t.Errorf("components %v", m["components"])
	}
}

func TestStartStop(t *testing.T) {
	srv, fs, _ := newServer(t)
	for _, p := range []string{"/api/v1/start", "/api/v1/stop"} {
		if res := do(t, srv, "POST", p, host, jsonHdr); res.StatusCode != http.StatusAccepted {
			t.Errorf("%s = %d, want 202", p, res.StatusCode)
		}
	}
	fs.startErr, fs.stopErr = session.ErrNotAllowed, session.ErrNotAllowed
	for _, p := range []string{"/api/v1/start", "/api/v1/stop"} {
		res := do(t, srv, "POST", p, host, jsonHdr)
		var m map[string]string
		_ = json.NewDecoder(res.Body).Decode(&m)
		if res.StatusCode != http.StatusConflict || m["error"] != "not_allowed" || m["phase"] != "stopped" {
			t.Errorf("%s = %d %v, want 409 not_allowed", p, res.StatusCode, m)
		}
	}
}

func TestGuards(t *testing.T) {
	srv, fs, _ := newServer(t)
	tests := []struct {
		name   string
		method string
		path   string
		host   string
		hdr    map[string]string
		want   int
	}{
		{"bad host GET", "GET", "/api/v1/status", "evil.example:8770", nil, 403},
		{"bad host index", "GET", "/", "evil.example", nil, 403},
		{"bad host POST", "POST", "/api/v1/start", "evil.example", jsonHdr, 403},
		{"non-json POST", "POST", "/api/v1/start", host, map[string]string{"Content-Type": "text/plain"}, 415},
		{"no content type POST", "POST", "/api/v1/start", host, nil, 415},
		{"json with charset", "POST", "/api/v1/start", host, map[string]string{"Content-Type": "application/json; charset=utf-8"}, 202},
		{"bad origin", "POST", "/api/v1/start", host, map[string]string{"Content-Type": "application/json", "Origin": "http://evil.example"}, 403},
		{"https origin", "POST", "/api/v1/start", host, map[string]string{"Content-Type": "application/json", "Origin": "https://" + host}, 403},
		{"good origin", "POST", "/api/v1/start", host, map[string]string{"Content-Type": "application/json", "Origin": "http://" + host}, 202},
		{"localhost host", "GET", "/api/v1/status", "localhost:8770", nil, 200},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			before := fs.starts
			res := do(t, srv, tc.method, tc.path, tc.host, tc.hdr)
			if res.StatusCode != tc.want {
				t.Fatalf("got %d, want %d", res.StatusCode, tc.want)
			}
			if tc.want >= 400 && fs.starts != before {
				t.Error("rejected request still reached the session")
			}
		})
	}
}

func TestStaticFiles(t *testing.T) {
	srv, _, _ := newServer(t)
	for path, ctype := range map[string]string{
		"/":           "text/html",
		"/index.html": "text/html",
		"/app.js":     "text/javascript",
		"/style.css":  "text/css",
	} {
		res := do(t, srv, "GET", path, host, nil)
		body, _ := io.ReadAll(res.Body)
		if res.StatusCode != 200 || !strings.HasPrefix(res.Header.Get("Content-Type"), ctype) || len(body) == 0 {
			t.Errorf("%s: %d %q (%d bytes)", path, res.StatusCode, res.Header.Get("Content-Type"), len(body))
		}
		if res.Header.Get("X-Content-Type-Options") != "nosniff" {
			t.Errorf("%s lacks nosniff", path)
		}
	}
}

func TestEvents(t *testing.T) {
	srv, fs, bc := newServer(t)
	req, _ := http.NewRequest("GET", srv.URL+"/api/v1/events", nil)
	req.Host = host
	res, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer res.Body.Close()
	if ct := res.Header.Get("Content-Type"); ct != "text/event-stream" {
		t.Fatalf("content type %q", ct)
	}
	lines := make(chan string, 64)
	go func() {
		sc := bufio.NewScanner(res.Body)
		for sc.Scan() {
			lines <- sc.Text()
		}
		close(lines)
	}()
	next := func(prefix string) string {
		t.Helper()
		deadline := time.After(3 * time.Second)
		for {
			select {
			case l, ok := <-lines:
				if !ok {
					t.Fatalf("stream closed waiting for %q", prefix)
				}
				if strings.HasPrefix(l, prefix) {
					return l
				}
			case <-deadline:
				t.Fatalf("timed out waiting for %q", prefix)
			}
		}
	}
	next("retry: 2000")
	next("event: status")
	if d := next("data: "); !strings.Contains(d, `"phase":"stopped"`) {
		t.Fatalf("first event %s", d)
	}
	fs.mu.Lock()
	fs.snap.Phase = "running"
	snap := fs.snap
	fs.mu.Unlock()
	bc.Publish(snap)
	next("event: status")
	if d := next("data: "); !strings.Contains(d, `"phase":"running"`) {
		t.Fatalf("update %s", d)
	}
}
