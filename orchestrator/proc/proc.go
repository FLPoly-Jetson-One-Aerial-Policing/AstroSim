// Package proc runs child processes in their own process group so a stop kills the whole tree.
package proc

import (
	"io"
	"os"
	"os/exec"
	"sync"
	"syscall"
	"time"
)

type Child struct {
	cmd  *exec.Cmd
	done chan struct{}
	code int
}

type Options struct {
	Env    []string // nil = inherit
	Stdout io.Writer
	Stderr io.Writer
}

// Start launches name with args. The child gets SIGTERM if this process dies (Pdeathsig).
func Start(name string, args []string, o Options) (*Child, error) {
	cmd := exec.Command(name, args...)
	cmd.Env = o.Env
	cmd.Stdout, cmd.Stderr = o.Stdout, o.Stderr
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true, Pdeathsig: syscall.SIGTERM}
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	c := &Child{cmd: cmd, done: make(chan struct{})}
	go func() {
		err := cmd.Wait()
		c.code = cmd.ProcessState.ExitCode() // -1 if killed by a signal
		_ = err
		close(c.done)
	}()
	return c, nil
}

func (c *Child) Pid() int              { return c.cmd.Process.Pid }
func (c *Child) Done() <-chan struct{} { return c.done }

// Code is valid after Done is closed. Signal deaths report -1.
func (c *Child) Code() int { return c.code }

// Stop sends SIGTERM to the whole group, then SIGKILL after grace.
func (c *Child) Stop(grace time.Duration) {
	select {
	case <-c.done:
		return
	default:
	}
	_ = syscall.Kill(-c.Pid(), syscall.SIGTERM)
	select {
	case <-c.done:
	case <-time.After(grace):
		_ = syscall.Kill(-c.Pid(), syscall.SIGKILL)
		<-c.done
	}
}

// Tail is an io.Writer that keeps the last N lines written (for error reports).
type Tail struct {
	mu    sync.Mutex
	n     int
	lines []string
	part  string
}

func NewTail(n int) *Tail { return &Tail{n: n} }

func (t *Tail) Write(p []byte) (int, error) {
	t.mu.Lock()
	defer t.mu.Unlock()
	s := t.part + string(p)
	for {
		i := indexNL(s)
		if i < 0 {
			break
		}
		t.lines = append(t.lines, s[:i])
		if len(t.lines) > t.n {
			t.lines = t.lines[1:]
		}
		s = s[i+1:]
	}
	t.part = s
	return len(p), nil
}

func (t *Tail) String() string {
	t.mu.Lock()
	defer t.mu.Unlock()
	out := ""
	for _, l := range t.lines {
		out += l + "\n"
	}
	return out + t.part
}

func indexNL(s string) int {
	for i := 0; i < len(s); i++ {
		if s[i] == '\n' {
			return i
		}
	}
	return -1
}

var _ = os.Stderr
