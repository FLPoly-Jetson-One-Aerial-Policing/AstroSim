package runtime

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
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

// Streamer is implemented by Commanders that can run a command with its
// output delivered incrementally instead of buffered. Long-running output,
// such as `logs --follow`, needs it.
type Streamer interface {
	// Stream executes name with args, copying standard output to stdout and
	// standard error to stderr as the command produces it, and waits for it
	// to finish. As with Run, a non-zero exit is reported in the exit code
	// and the error is reserved for failing to run the command or for a
	// cancelled context (then the error is ctx.Err()).
	Stream(ctx context.Context, stdout, stderr io.Writer, name string, args ...string) (exitCode int, err error)
}

// ExecCommander is the os/exec Commander.
type ExecCommander struct{}

// killGrace bounds how long a cancelled command may linger before its pipes
// are forcibly closed.
const killGrace = 2 * time.Second

var (
	_ Commander = ExecCommander{}
	_ Streamer  = ExecCommander{}
)

// Run implements Commander using os/exec.
func (ExecCommander) Run(ctx context.Context, name string, args ...string) (Result, error) {
	cmd := exec.CommandContext(ctx, name, args...)
	var stdout, stderr bytes.Buffer
	cmd.Stdout, cmd.Stderr = &stdout, &stderr
	cmd.WaitDelay = killGrace
	hideConsole(cmd)
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

// Stream implements Streamer using os/exec.
func (ExecCommander) Stream(ctx context.Context, stdout, stderr io.Writer, name string, args ...string) (int, error) {
	cmd := exec.CommandContext(ctx, name, args...)
	cmd.Stdout, cmd.Stderr = stdout, stderr
	cmd.WaitDelay = killGrace
	hideConsole(cmd)
	err := cmd.Run()
	if err == nil {
		return 0, nil
	}
	if ctx.Err() != nil {
		return 0, ctx.Err()
	}
	var ee *exec.ExitError
	if errors.As(err, &ee) {
		return ee.ExitCode(), nil
	}
	return 0, fmt.Errorf("run %s: %w", name, err)
}
