//go:build unix

package main

import (
	"syscall"
	"testing"
)

func TestQuitSignalsIncludeHangup(t *testing.T) {
	found := false
	for _, s := range quitSignals() {
		if s == syscall.SIGHUP {
			found = true
		}
	}
	if !found {
		t.Error("quitSignals lacks SIGHUP")
	}
}

func TestSurvivePipeBreak(t *testing.T) {
	stop := survivePipeBreak()
	defer stop()
	// With SIGPIPE handled, raising it must not terminate the test binary.
	if err := syscall.Kill(syscall.Getpid(), syscall.SIGPIPE); err != nil {
		t.Fatal(err)
	}
}
