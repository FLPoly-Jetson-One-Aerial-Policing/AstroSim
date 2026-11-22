package runtime

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"os/exec"
	"time"
)

// Result is the outcome of a finished command.
type Result struct {
	// Stdout and Stderr are the captured output streams.
	Stdout, Stderr string
	// ExitCode is the process exit code.
	ExitCode int
}

// Commander runs an external command to completion. It is the seam that lets
// tests substitute a scripted fake for the real podman or docker executable.
//
// A non-zero exit code is reported in Result, not as an error. The error is
// reserved for failing to run the command at all (executable missing, context
// cancelled).
type Commander interface {
	// Run executes name with args and waits for it to finish.
	Run(ctx context.Context, name string, args ...string) (Result, error)
}

// ExecCommander is the os/exec Commander.
type ExecCommander struct{}

// killGrace bounds how long a cancelled command may linger before its pipes
// are forcibly closed.
const killGrace = 2 * time.Second

// Run implements Commander using os/exec.
func (ExecCommander) Run(ctx context.Context, name string, args ...string) (Result, error) {
	cmd := exec.CommandContext(ctx, name, args...)
	var stdout, stderr bytes.Buffer
	cmd.Stdout, cmd.Stderr = &stdout, &stderr
	cmd.WaitDelay = killGrace
	err := cmd.Run()
	res := Result{Stdout: stdout.String(), Stderr: stderr.String()}
	if err == nil {
		return res, nil
	}
	if ctx.Err() != nil {
		return res, ctx.Err()
	}
	var ee *exec.ExitError
	if errors.As(err, &ee) {
		res.ExitCode = ee.ExitCode()
		return res, nil
	}
	return res, fmt.Errorf("run %s: %w", name, err)
}
