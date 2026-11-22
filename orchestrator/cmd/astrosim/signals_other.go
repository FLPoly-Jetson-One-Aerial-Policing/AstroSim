//go:build !unix

package main

import (
	"os"
	"syscall"
)

// quitSignals lists the signals that ask the orchestrator to stop the
// simulation and exit. On Windows the Go runtime reports console close,
// logoff and shutdown as SIGTERM, so no hangup signal is needed.
func quitSignals() []os.Signal {
	return []os.Signal{os.Interrupt, syscall.SIGTERM}
}

// survivePipeBreak is a no-op: Windows has no SIGPIPE.
func survivePipeBreak() func() { return func() {} }
