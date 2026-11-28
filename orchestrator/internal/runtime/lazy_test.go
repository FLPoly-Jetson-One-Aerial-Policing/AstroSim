package runtime

import (
	"context"
	"errors"
	"io"
	"testing"
)

func TestLazyDetectsLate(t *testing.T) {
	cmd := &byBinary{results: map[string]Result{"/bin/podman": {ExitCode: 1, Stderr: "cannot connect"}}}
	l := NewLazy(DetectOptions{Commander: cmd, LookPath: lookIn(map[string]string{"podman": "/bin/podman"})})
	ctx := context.Background()

	if got := l.Info().Name; got != "podman" {
		t.Errorf("Info before detection = %q", got)
	}
	if _, err := l.Ping(ctx); !errors.Is(err, ErrNotRunning) {
		t.Fatalf("Ping = %v, want ErrNotRunning", err)
	}
	if _, err := l.ListByLabel(ctx, "k", "v"); !errors.Is(err, ErrNotRunning) {
		t.Fatalf("ListByLabel before detection = %v, want ErrNotRunning", err)
	}

	// The user starts podman.
	cmd.results["/bin/podman"] = Result{Stdout: "5.0.1\n"}
	info, err := l.Ping(ctx)
	if err != nil || info.Version != "5.0.1" || l.Info().Version != "5.0.1" {
		t.Fatalf("Ping after start = %+v, %v", info, err)
	}
	if _, err := l.Ping(ctx); err != nil {
		t.Fatalf("second Ping: %v", err)
	}
}

func TestLazyNotInstalled(t *testing.T) {
	l := NewLazy(DetectOptions{Name: "docker", Commander: &byBinary{}, LookPath: lookIn(nil)})
	if _, err := l.Ping(context.Background()); !errors.Is(err, ErrNotInstalled) {
		t.Fatalf("Ping = %v", err)
	}
	if _, err := l.Logs(context.Background(), "x", 1); !errors.Is(err, ErrNotInstalled) {
		t.Fatalf("Logs = %v", err)
	}
	if got := l.Info().Name; got != "docker" {
		t.Errorf("Info name = %q", got)
	}
}

func TestLazyFollowLogsBeforeDetection(t *testing.T) {
	l := NewLazy(DetectOptions{Commander: &byBinary{}, LookPath: lookIn(nil)})
	err := l.FollowLogs(context.Background(), "x", LogOptions{}, io.Discard, io.Discard)
	if !errors.Is(err, ErrNotInstalled) {
		t.Fatalf("FollowLogs = %v", err)
	}
}

func TestLazyPullBeforeDetection(t *testing.T) {
	l := NewLazy(DetectOptions{Commander: &byBinary{}, LookPath: lookIn(nil)})
	if err := l.Pull(context.Background(), "ghcr.io/o/x:1", io.Discard); !errors.Is(err, ErrNotInstalled) {
		t.Fatalf("Pull = %v", err)
	}
}
