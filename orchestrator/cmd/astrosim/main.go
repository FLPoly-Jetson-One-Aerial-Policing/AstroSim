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
	"strings"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/api"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/browser"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/components"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/config"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/instance"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/logging"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/session"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/status"
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
	// probeTimeout bounds the "is the other instance alive" request.
	probeTimeout = 2 * time.Second
)

func main() {
	os.Exit(run(os.Args[1:], os.Getenv, os.Stdout, os.Stderr))
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
	if cfg.ShowVersion {
		fmt.Fprintf(stdout, "astrosim %s\n", version)
		return exitOK
	}
	defs := components.All()
	if err := cfg.Validate(goruntime.GOOS, defs); err != nil {
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
	pingCtx, cancelPing := context.WithTimeout(context.Background(), 20*time.Second)
	info, perr := rt.Ping(pingCtx)
	if perr != nil {
		log.Warn("no usable container runtime yet; the dashboard will explain what to do", "err", perr)
	} else {
		log.Info("container runtime found", "runtime", info.Name, "version", info.Version, "path", info.Path)
		// Only while holding the lock is it safe to remove every managed
		// container: they can only be leftovers of a crashed run.
		if err := session.CleanupStale(pingCtx, rt); err != nil {
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
		SessionID:    newSessionID(),
		Version:      version,
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
			Session: sess, Broadcaster: bc, Logger: log,
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
	log.Info("stopped")
	return code
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

// autostart presses Start once the first check has reached "stopped".
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

// newSessionID returns a random identifier labelling this run's containers.
func newSessionID() string {
	var b [8]byte
	if _, err := rand.Read(b[:]); err != nil {
		return fmt.Sprintf("%x", time.Now().UnixNano())
	}
	return hex.EncodeToString(b[:])
}

// indent prefixes every line with two spaces.
func indent(s string) string {
	return "  " + strings.ReplaceAll(s, "\n", "\n  ")
}
