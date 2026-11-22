//go:build unix

package main

import (
	"os"
	"os/signal"
	"syscall"
)

// quitSignals lists the signals that ask the orchestrator to stop the
// simulation and exit. SIGHUP is what closing the terminal window sends on
// Linux and macOS; without handling it Go's default action would kill the
// process with no teardown and leave the containers running.
func quitSignals() []os.Signal {
	return []os.Signal{os.Interrupt, syscall.SIGTERM, syscall.SIGHUP}
}

// survivePipeBreak keeps the process alive when its terminal disappears.
//
// Once the terminal is gone, writing to stdout or stderr fails (EIO on a
// closed pty, EPIPE on a closed pipe). For writes to fd 1 and 2 the Go
// runtime turns EPIPE into a fatal SIGPIPE unless the program has called
// signal.Notify for SIGPIPE, which would skip the teardown that follows
// SIGHUP. Registering a channel that nobody reads makes the runtime drop
// the signal, so the failed write just returns an error that the logging
// handler ignores. signal.Ignore is avoided on purpose: an ignored
// disposition is inherited by the podman and docker child processes,
// whereas a handled one is reset to the default on exec.
//
// The returned function undoes the registration.
func survivePipeBreak() func() {
	ch := make(chan os.Signal, 1)
	signal.Notify(ch, syscall.SIGPIPE)
	return func() { signal.Stop(ch) }
}
