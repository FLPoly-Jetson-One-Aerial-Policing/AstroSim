package instance

import (
	"bufio"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
)

const helperEnv = "ASTROSIM_INSTANCE_TEST_HELPER"

// TestMain doubles as the helper process: when helperEnv names a lock file
// it takes the lock, publishes a URL, reports "locked" and holds the lock
// until stdin closes.
func TestMain(m *testing.M) {
	if path := os.Getenv(helperEnv); path != "" {
		l, err := Acquire(path)
		if err != nil {
			os.Stdout.WriteString("error: " + err.Error() + "\n")
			os.Exit(1)
		}
		if err := l.Publish("http://127.0.0.1:9999"); err != nil {
			os.Stdout.WriteString("error: " + err.Error() + "\n")
			os.Exit(1)
		}
		os.Stdout.WriteString("locked\n")
		buf := make([]byte, 1)
		_, _ = os.Stdin.Read(buf)
		_ = l.Release()
		os.Exit(0)
	}
	os.Exit(m.Run())
}

func startHelper(t *testing.T, path string) (stop func()) {
	t.Helper()
	cmd := exec.Command(os.Args[0])
	cmd.Env = append(os.Environ(), helperEnv+"="+path)
	in, err := cmd.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}
	out, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	line, _ := bufio.NewReader(out).ReadString('\n')
	if line != "locked\n" {
		_ = cmd.Process.Kill()
		t.Fatalf("helper said %q", line)
	}
	return func() {
		_ = in.Close()
		done := make(chan error, 1)
		go func() { done <- cmd.Wait() }()
		select {
		case <-done:
		case <-time.After(5 * time.Second):
			_ = cmd.Process.Kill()
			t.Error("helper did not exit")
		}
	}
}

func TestSecondProcessIsRefused(t *testing.T) {
	path := filepath.Join(t.TempDir(), "sub", "orchestrator.lock")
	stop := startHelper(t, path)

	if _, err := Acquire(path); !errors.Is(err, ErrHeld) {
		t.Fatalf("Acquire while held = %v, want ErrHeld", err)
	}
	url, err := ReadOwnerURL(path)
	if err != nil || url != "http://127.0.0.1:9999" {
		t.Fatalf("ReadOwnerURL = %q, %v", url, err)
	}

	stop() // owner exits: the lock is free again
	l, err := Acquire(path)
	if err != nil {
		t.Fatalf("Acquire after owner exit: %v", err)
	}
	if err := l.Release(); err != nil {
		t.Fatal(err)
	}
}

func TestAcquirePublishRelease(t *testing.T) {
	path := filepath.Join(t.TempDir(), "x.lock")
	l, err := Acquire(path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := ReadOwnerURL(path); err == nil {
		t.Error("ReadOwnerURL before Publish succeeded")
	}
	if _, err := Acquire(path); !errors.Is(err, ErrHeld) {
		t.Fatalf("second Acquire in process = %v, want ErrHeld", err)
	}
	if err := l.Publish("http://a:1"); err != nil {
		t.Fatal(err)
	}
	if err := l.Publish("http://b:2"); err != nil { // overwrites, no leftovers
		t.Fatal(err)
	}
	if url, err := ReadOwnerURL(path); err != nil || url != "http://b:2" {
		t.Fatalf("ReadOwnerURL = %q, %v", url, err)
	}
	if err := l.Release(); err != nil {
		t.Fatal(err)
	}
	if err := l.Release(); err != nil {
		t.Fatalf("second Release: %v", err)
	}
	l2, err := Acquire(path)
	if err != nil {
		t.Fatalf("Acquire after Release: %v", err)
	}
	_ = l2.Release()
}

func TestDefaultPath(t *testing.T) {
	p, err := DefaultPath()
	if err != nil {
		t.Skip("no cache dir:", err)
	}
	if filepath.Base(p) != "orchestrator.lock" || filepath.Base(filepath.Dir(p)) != "astrosim" {
		t.Fatalf("path = %s", p)
	}
}
