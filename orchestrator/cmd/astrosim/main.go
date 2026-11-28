// Command astrosim starts the AstroSim simulation stack and serves the local
// web dashboard that controls it.
//
// It checks for a container runtime and the component images, serves the
// dashboard on localhost (opening it in the browser), and on Start launches
// and supervises the containers. Ctrl+C, SIGTERM or closing the terminal
// stops the containers and exits.
// Run "astrosim --help" for the flags.
package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/api"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/browser"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/component"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/components"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/config"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/instance"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/logging"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/logs"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/session"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/status"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/supervisor"
	goruntime "runtime"
)

// Exit codes.
const (
	exitOK        = 0
	exitFailure   = 1
	exitUsage     = 2
	exitInterrupt = 130 // second Ctrl+C: the shell convention for SIGINT
)

const (
	// shutdownGrace bounds how long the HTTP server may take to close.
	shutdownGrace = 2 * time.Second
	// logCloseGrace bounds how long closing the session log may take.
	logCloseGrace = 2 * time.Second
	// probeTimeout bounds the "is the other instance alive" request.
	probeTimeout = 2 * time.Second
)

func main() {
	os.Exit(run(os.Args[1:], os.Getenv, os.Stdout, os.Stderr))
}

// releaseImages returns the image settings this binary was built with.
func releaseImages() components.Images {
	return components.Images{Repository: imageRepository, Tag: imageTag}
}

// buildError reports image settings baked into the binary at build time that
// cannot be used. It is a defect of the build, not of the user's
// configuration, so it exits differently from a usage error.
type buildError struct{ err error }

func (e *buildError) Error() string {
	return "this build's image settings are invalid: " + e.err.Error()
}

func (e *buildError) Unwrap() error { return e.err }

// componentDefs returns the component definitions for this run: the built-in
// ones with the build's image references applied, checked against cfg. The
// per-component overrides of cfg are not applied here (see
// config.Config.ApplyOverrides), so a "components.<id>.image" setting still
// wins over the release image. A broken build is a *buildError; any other
// error comes from cfg.Validate.
func componentDefs(cfg config.Config, im components.Images) ([]component.Definition, error) {
	if err := im.Validate(); err != nil {
		return nil, &buildError{err}
	}
	defs := im.Apply(components.All())
	if err := cfg.Validate(goruntime.GOOS, defs); err != nil {
		return nil, err
	}
	return defs, nil
}

// versionText is the output of --version: "astrosim <version>", and for a
// release build a second line "images <engine image> <fc image>" naming the
// images it downloads (scripts read it). A development build prints only the
// first line.
func versionText(im components.Images) (string, error) {
	if err := im.Validate(); err != nil {
		return "", &buildError{err}
	}
	text := fmt.Sprintf("astrosim %s\n", version)
	if im.Release() {
		refs := []string{"images"}
		for _, d := range im.Apply(components.All()) {
			refs = append(refs, d.Image)
		}
		text += strings.Join(refs, " ") + "\n"
	}
	return text, nil
}

// run is main without the process-global parts, and returns the exit code.
func run(args []string, getenv func(string) string, stdout, stderr io.Writer) int {
	cfg, err := config.LoadTo(stderr, args, getenv, os.ReadFile)
	if errors.Is(err, flag.ErrHelp) {
		return exitOK
	}
	if err != nil {
		fmt.Fprintf(stderr, "astrosim: %v\nRun \"astrosim --help\" for usage.\n", err)
		return exitUsage
	}
	if cfg.PrintLogDir {
		// For scripts: cd "$(astrosim --print-log-dir)". It takes no lock,
		// touches no runtime and creates nothing, and wins over every
		// other setting.
		root, err := logRoot(cfg, goruntime.GOOS, getenv)
		if err != nil {
			fmt.Fprintf(stderr, "astrosim: %v\n", err)
			return exitFailure
		}
		fmt.Fprintln(stdout, root)
		return exitOK
	}
	images := releaseImages()
	if cfg.ShowVersion {
		text, err := versionText(images)
		if err != nil {
			fmt.Fprintf(stderr, "astrosim: %v\n", err)
			return exitFailure
		}
		fmt.Fprint(stdout, text)
		return exitOK
	}
	defs, err := componentDefs(cfg, images)
	var broken *buildError
	if errors.As(err, &broken) {
		fmt.Fprintf(stderr, "astrosim: %v\n", broken)
		return exitFailure
	}
	if err != nil {
		fmt.Fprintf(stderr, "astrosim: invalid configuration:\n%s\nRun \"astrosim --help\" for usage.\n", indent(err.Error()))
		return exitUsage
	}
	// If the terminal goes away, logging must not kill the process before
	// the shutdown signal handler has stopped the containers.
	defer survivePipeBreak()()
	level, _ := logging.ParseLevel(cfg.LogLevel) // validated above
	log := logging.New(stderr, level)

	// Single instance: a second launch just opens the first one's dashboard.
	lockPath, err := instance.DefaultPath()
	if err != nil {
		log.Error("cannot locate the lock file", "err", err)
		return exitFailure
	}
	lock, err := instance.Acquire(lockPath)
	if errors.Is(err, instance.ErrHeld) {
		if cfg.Cleanup {
			return refuseCleanup(lockPath, stdout, log)
		}
		return alreadyRunning(lockPath, cfg, stdout, stderr, log)
	}
	if err != nil {
		log.Error("cannot take the single-instance lock", "path", lockPath, "err", err)
		return exitFailure
	}
	defer lock.Release()

	// The runtime is found lazily so the dashboard can still start, and say
	// what is wrong, when podman or docker is missing or stopped.
	rt := runtime.NewLazy(runtime.DetectOptions{Name: cfg.Runtime, Path: cfg.RuntimePath})
	root, rootErr := logRoot(cfg, goruntime.GOOS, getenv)
	if cfg.Cleanup {
		code := cleanup(rt, cfg.Prune, stdout, log)
		if cfg.Prune {
			// Pruning also archives log folders a hard kill left behind. It
			// never deletes logs, only the folders it has archived.
			if rootErr != nil {
				log.Warn("cannot locate the log folder; leftover logs were not archived", "err", rootErr)
			} else if !archiveStrayLogs(root, stdout, log) {
				code = exitFailure
			}
		}
		return code
	}

	// From here on the orchestrator's own log lines go to the session log
	// (and, as before, to the terminal). If the log cannot be opened the
	// orchestrator runs without file logging.
	sessionID := newSessionID()
	var rec *logs.Recorder
	if rootErr == nil {
		rec, err = logs.Open(logs.Options{
			Root: root, SessionID: sessionID, Version: version, Source: rt,
			Console: stderr, Verbose: cfg.Verbose, FileMaxBytes: int64(cfg.LogFileMaxMB) << 20,
		})
		if err != nil {
			rec, rootErr = nil, err
		}
	}
	if w := rec.OrchestratorWriter(); w != nil {
		log = logging.New(w, level)
	}
	if rootErr != nil {
		log.Warn("session logs are not being saved to files", "err", rootErr)
	}
	if images.Release() {
		log.Info("release build: missing images are downloaded when pull is missing", "repository", images.Repository, "tag", images.Tag, "pull", cfg.Pull)
	}
	recovered := make(chan struct{})
	if rec != nil {
		go func() {
			defer close(recovered)
			recoverLogs(root, rec.Name(), cfg, log)
		}()
		// Registered after the lock's defer, so it runs first: the logs are
		// archived while this instance still holds the lock and no other
		// instance can be recovering the same folders.
		defer func() {
			log.Info("stopped")
			finishLogs(rec, recovered, cfg, stderr, log)
		}()
	} else {
		defer log.Info("stopped")
	}
	pingCtx, cancelPing := context.WithTimeout(context.Background(), 20*time.Second)
	info, perr := rt.Ping(pingCtx)
	if perr != nil {
		log.Warn("no usable container runtime yet; the dashboard will explain what to do", "err", perr)
	} else {
		log.Info("container runtime found", "runtime", info.Name, "version", info.Version, "path", info.Path)
		// Only while holding the lock is it safe to remove every managed
		// container: they can only be leftovers of a crashed run.
		if _, err := session.CleanupStale(pingCtx, rt); err != nil {
			log.Warn("could not remove leftover containers", "err", err)
		}
	}
	cancelPing()

	ln, err := net.Listen("tcp", cfg.Listen)
	if err != nil {
		log.Error("cannot start the dashboard server", "listen", cfg.Listen, "err", err,
			"hint", "another program may be using that port; pick another with --listen")
		return exitFailure
	}
	url := dashboardURL(ln.Addr())
	if err := lock.Publish(url); err != nil {
		log.Warn("could not publish the dashboard URL for later instances", "err", err)
	}

	bc := status.NewBroadcaster()
	sess, err := session.New(session.Options{
		Runtime:      rt,
		Definitions:  cfg.ApplyOverrides(defs),
		StackContext: cfg.StackContext(),
		Policy:       cfg.RestartPolicy(),
		SessionID:    sessionID,
		Version:      version,
		Logs:         logCapture(rec),
		PullMissing:  cfg.Pull == config.PullMissing,
		LogDir:       rec.Dir(),
		Broadcaster:  bc,
		Logger:       log,
	})
	if err != nil {
		log.Error("invalid component configuration", "err", err)
		_ = ln.Close()
		return exitFailure
	}

	baseCtx, cancelBase := context.WithCancel(context.Background())
	defer cancelBase()
	srv := &http.Server{
		Handler: api.NewHandler(api.Options{
			Session: sess, Broadcaster: bc, Logger: log, Logs: rec,
			AllowedHosts: allowedHosts(ln.Addr(), os.Hostname, net.InterfaceAddrs),
		}),
		ReadHeaderTimeout: 10 * time.Second,
		BaseContext:       func(net.Listener) context.Context { return baseCtx },
		ErrorLog:          slog.NewLogLogger(log.Handler(), slog.LevelWarn),
	}
	serveErr := make(chan error, 1)
	go func() { serveErr <- srv.Serve(ln) }()

	runErr := make(chan error, 1)
	go func() { runErr <- sess.Run(context.Background()) }()

	// Signals (including SIGHUP when the terminal closes): the first stops the containers and exits; a second one
	// gives up waiting.
	sigs := make(chan os.Signal, 2)
	signal.Notify(sigs, quitSignals()...)
	defer signal.Stop(sigs)
	go func() {
		<-sigs
		log.Info("shutting down; stopping the containers (press Ctrl+C again to quit immediately)")
		sess.Shutdown()
		<-sigs
		log.Warn("quitting immediately; containers may be left running and are removed on the next start")
		os.Exit(exitInterrupt)
	}()

	fmt.Fprintf(stdout, "AstroSim dashboard: %s (press Ctrl+C to quit)\n", url)
	if !cfg.NoBrowser {
		if err := browser.Open(url); err != nil {
			log.Warn("could not open a browser; open the dashboard address yourself", "err", err)
		}
	}
	if cfg.Autostart {
		go autostart(sess, bc, baseCtx, log)
	}

	code := exitOK
	select {
	case err := <-runErr:
		if err != nil {
			log.Error("session ended with an error", "err", err)
			code = exitFailure
		}
	case err := <-serveErr:
		// The dashboard died: without it the user cannot control anything,
		// so stop the stack and leave.
		log.Error("dashboard server failed", "err", err)
		code = exitFailure
		sess.Shutdown()
		<-runErr
	}

	cancelBase() // ends event-stream requests so the server can close
	ctx, cancel := context.WithTimeout(context.Background(), shutdownGrace)
	defer cancel()
	if err := srv.Shutdown(ctx); err != nil {
		_ = srv.Close()
	}
	return code
}

// logRoot is the folder holding all session logs: --log-dir (or its
// environment variable or JSON key) made absolute, else the operating
// system's default for goos.
func logRoot(cfg config.Config, goos string, getenv func(string) string) (string, error) {
	if cfg.LogDir != "" {
		return filepath.Abs(cfg.LogDir)
	}
	return logs.DefaultDir(goos, getenv, homeDir(goos, getenv))
}

// homeDir returns the user's home directory for goos as getenv reports it
// (USERPROFILE on windows, HOME elsewhere), or "" when unset. It reads only
// getenv, never the process environment, so the injected environment is the
// single source of settings.
func homeDir(goos string, getenv func(string) string) string {
	if goos == "windows" {
		return getenv("USERPROFILE")
	}
	return getenv("HOME")
}

// logCapture returns the session's log capture for rec, or nil when file
// logging is off. (A nil *Recorder inside the interface would also work,
// but a nil interface lets the session skip the calls altogether.)
func logCapture(rec *logs.Recorder) session.LogCapture {
	if rec == nil {
		return nil
	}
	return sessionLogs{rec}
}

// recoverLogs runs at startup in the background: it archives the folders of
// sessions that did not shut down cleanly (all but the current one, skip),
// then applies the retention limits.
func recoverLogs(root, skip string, cfg config.Config, log *slog.Logger) {
	archived, err := logs.Recover(root, skip)
	for _, z := range archived {
		log.Info("archived the logs of an earlier session", "file", z)
	}
	if err != nil {
		log.Warn("could not archive all earlier session logs", "err", err)
	}
	retain(root, cfg, log)
}

// retain deletes the oldest log archives beyond the configured limits.
func retain(root string, cfg config.Config, log *slog.Logger) {
	removed, err := logs.Retain(root, cfg.LogKeep, int64(cfg.LogMaxMB)<<20)
	for _, z := range removed {
		log.Info("removed an old log archive", "file", z)
	}
	if err != nil {
		log.Warn("could not remove old log archives", "err", err)
	}
}

// finishLogs closes the session log, archives its folder and applies
// retention, then tells the user where the archive is. It waits for the
// startup recovery first so the two never work on the same files.
func finishLogs(rec *logs.Recorder, recovered <-chan struct{}, cfg config.Config, stderr io.Writer, log *slog.Logger) {
	ctx, cancel := context.WithTimeout(context.Background(), logCloseGrace)
	defer cancel()
	if err := rec.Close(ctx); err != nil {
		log.Warn("closing the session log timed out; the last lines may be missing", "err", err)
	}
	<-recovered
	zp, err := logs.Archive(rec.Dir())
	if err != nil {
		// The next start finishes the job (logs.Recover).
		log.Warn("could not fully archive the session log", "err", err)
	}
	if zp != "" {
		retain(filepath.Dir(rec.Dir()), cfg, log)
		fmt.Fprintf(stderr, "logs saved to %s\n", zp)
	}
}

// archiveStrayLogs is the log part of --prune: it archives every session
// folder in root. It reports whether that went without error.
func archiveStrayLogs(root string, stdout io.Writer, log *slog.Logger) bool {
	archived, err := logs.Recover(root, "")
	for _, z := range archived {
		fmt.Fprintf(stdout, "archived logs %s\n", z)
	}
	if err != nil {
		log.Error("could not archive all leftover logs", "err", err)
		return false
	}
	return true
}

// alreadyRunning handles a failed lock: it finds the owner's dashboard and
// sends the user there.
func alreadyRunning(lockPath string, cfg config.Config, stdout, stderr io.Writer, log *slog.Logger) int {
	url, err := instance.ReadOwnerURL(lockPath)
	if err != nil {
		log.Error("AstroSim is already running, but its dashboard address is unknown", "lock", lockPath, "err", err)
		return exitFailure
	}
	client := &http.Client{Timeout: probeTimeout}
	resp, err := client.Get(url + "/api/v1/status")
	if err == nil {
		_ = resp.Body.Close()
	}
	if err != nil || resp.StatusCode != http.StatusOK {
		log.Error("another AstroSim holds the lock but its dashboard does not answer", "url", url, "err", err)
		return exitFailure
	}
	fmt.Fprintf(stdout, "AstroSim is already running at %s\n", url)
	if !cfg.NoBrowser {
		if err := browser.Open(url); err != nil {
			log.Warn("could not open a browser; open the address above yourself", "err", err)
		}
	}
	return exitOK
}

// cleanupTimeout bounds the whole --cleanup or --prune run.
const cleanupTimeout = 2 * time.Minute

// cleanup is the --cleanup and --prune command: it removes what a crashed
// run left behind, prints one line per removed resource, and returns the
// exit code. The caller holds the single-instance lock, so no live session
// can be affected.
func cleanup(rt runtime.Runtime, prune bool, stdout io.Writer, log *slog.Logger) int {
	ctx, cancel := context.WithTimeout(context.Background(), cleanupTimeout)
	defer cancel()
	if _, err := rt.Ping(ctx); err != nil {
		log.Error("no usable container runtime; nothing was cleaned up", "err", err)
		return exitFailure
	}
	var removed session.Pruned
	var err error
	if prune {
		removed, err = session.Prune(ctx, rt)
	} else {
		removed.Containers, err = session.CleanupStale(ctx, rt)
	}
	for _, n := range removed.Containers {
		fmt.Fprintf(stdout, "removed container %s\n", n)
	}
	for _, n := range removed.Networks {
		fmt.Fprintf(stdout, "removed network %s\n", n)
	}
	if err != nil {
		log.Error("cleanup incomplete", "err", err)
		return exitFailure
	}
	if removed.Empty() {
		fmt.Fprintln(stdout, "nothing to clean up")
	}
	return exitOK
}

// refuseCleanup handles --cleanup or --prune while another orchestrator is
// running: removing its containers would pull the simulation out from under
// it, so it only says where that instance is.
func refuseCleanup(lockPath string, stdout io.Writer, log *slog.Logger) int {
	url, err := instance.ReadOwnerURL(lockPath)
	if err != nil {
		log.Error("AstroSim is running; stop it before cleaning up", "err", err)
		return exitFailure
	}
	fmt.Fprintf(stdout, "AstroSim is running at %s; stop it before cleaning up\n", url)
	return exitFailure
}

// autostart presses Start once the first check, and any download it began,
// has reached "stopped". It does nothing when the user cancelled the download.
func autostart(sess *session.Session, bc *status.Broadcaster, ctx context.Context, log *slog.Logger) {
	ch, cancel := bc.Subscribe()
	defer cancel()
	for {
		select {
		case <-ctx.Done():
			return
		case snap := <-ch:
			if snap.Phase != "stopped" {
				continue
			}
			if snap.Reason.Code == supervisor.ReasonPullCancelled {
				// The user cancelled the download: pressing Start would
				// begin it again.
				log.Info("autostart cancelled with the download")
				return
			}
			if err := sess.Start(); err != nil {
				log.Warn("autostart failed", "err", err)
			} else {
				log.Info("autostart: starting the simulation")
			}
			return
		}
	}
}

// dashboardURL is the address to give the user for a listener. A wildcard
// bind is reached through loopback.
func dashboardURL(a net.Addr) string {
	host, port, err := net.SplitHostPort(a.String())
	if err != nil {
		return "http://" + a.String()
	}
	if ip := net.ParseIP(host); ip == nil || ip.IsUnspecified() {
		host = "127.0.0.1"
	}
	return "http://" + net.JoinHostPort(host, port)
}

// allowedHosts lists the Host header values the API accepts. They are the
// bound address, localhost and 127.0.0.1 on the bound port; the Host check
// is what stops a web page from reaching the dashboard through a rebound
// DNS name.
//
// For a wildcard bind (a deliberate --allow-remote choice) the machine's
// hostname and the addresses of its network interfaces are accepted too, so
// the dashboard can be reached by name or by IP address from another
// machine. A DNS alias of the machine that is neither is rejected.
func allowedHosts(a net.Addr, hostname func() (string, error), ifaceAddrs func() ([]net.Addr, error)) []string {
	host, port, err := net.SplitHostPort(a.String())
	if err != nil {
		return nil
	}
	seen := map[string]bool{}
	var out []string
	add := func(h string) {
		hp := strings.ToLower(net.JoinHostPort(h, port))
		if !seen[hp] {
			seen[hp] = true
			out = append(out, hp)
		}
	}
	add(host)
	add("localhost")
	add("127.0.0.1")
	if ip := net.ParseIP(host); ip != nil && ip.IsUnspecified() {
		if name, err := hostname(); err == nil && name != "" {
			add(name)
		}
		if addrs, err := ifaceAddrs(); err == nil {
			for _, ia := range addrs {
				if n, ok := ia.(*net.IPNet); ok {
					add(n.IP.String())
				}
			}
		}
	}
	return out
}

// newSessionID returns a random identifier labelling this run's containers
// and its log folder: always 16 hex digits, which the log folder name needs.
func newSessionID() string {
	var b [8]byte
	if _, err := rand.Read(b[:]); err != nil {
		return fmt.Sprintf("%016x", uint64(time.Now().UnixNano()))
	}
	return hex.EncodeToString(b[:])
}

// indent prefixes every line with two spaces.
func indent(s string) string {
	return "  " + strings.ReplaceAll(s, "\n", "\n  ")
}
