package proc

import (
	"strings"
	"syscall"
	"testing"
	"time"
)

func wait(t *testing.T, c *Child, d time.Duration) {
	t.Helper()
	select {
	case <-c.Done():
	case <-time.After(d):
		t.Fatal("child did not exit")
	}
}

func TestExitCodes(t *testing.T) {
	c, err := Start("true", nil, Options{})
	if err != nil {
		t.Fatal(err)
	}
	wait(t, c, 2*time.Second)
	if c.Code() != 0 {
		t.Fatalf("true -> %d", c.Code())
	}
	c, _ = Start("false", nil, Options{})
	wait(t, c, 2*time.Second)
	if c.Code() != 1 {
		t.Fatalf("false -> %d", c.Code())
	}
}

func TestMissingBinary(t *testing.T) {
	if _, err := Start("/nonexistent/astrosim-test", nil, Options{}); err == nil {
		t.Fatal("expected error")
	}
}

func TestStopKillsWholeGroup(t *testing.T) {
	// the shell spawns a grandchild sleeper; Stop must take both down
	c, err := Start("sh", []string{"-c", "sleep 60 & echo $! ; wait"}, Options{Stdout: NewTail(5)})
	if err != nil {
		t.Fatal(err)
	}
	time.Sleep(200 * time.Millisecond)
	start := time.Now()
	c.Stop(2 * time.Second)
	if time.Since(start) > 1500*time.Millisecond {
		t.Fatalf("stop took %v", time.Since(start))
	}
	if err := syscall.Kill(-c.Pid(), 0); err == nil {
		t.Fatal("process group still alive")
	}
}

func TestStopEscalatesToSIGKILL(t *testing.T) {
	c, err := Start("sh", []string{"-c", "trap '' TERM; while true; do sleep 1; done"}, Options{})
	if err != nil {
		t.Fatal(err)
	}
	time.Sleep(200 * time.Millisecond)
	start := time.Now()
	c.Stop(300 * time.Millisecond)
	if d := time.Since(start); d < 250*time.Millisecond || d > 2*time.Second {
		t.Fatalf("escalation timing %v", d)
	}
	if c.Code() != -1 {
		t.Fatalf("code %d, want -1 (signal)", c.Code())
	}
}

func TestStopAfterExitIsNoop(t *testing.T) {
	c, _ := Start("true", nil, Options{})
	wait(t, c, 2*time.Second)
	c.Stop(time.Second)
}

func TestTail(t *testing.T) {
	tl := NewTail(2)
	tl.Write([]byte("a\nb\nc\npart"))
	if got := tl.String(); got != "b\nc\npart" {
		t.Fatalf("%q", got)
	}
	tl.Write([]byte("ial\n"))
	if !strings.HasSuffix(tl.String(), "partial\n") {
		t.Fatalf("%q", tl.String())
	}
}
