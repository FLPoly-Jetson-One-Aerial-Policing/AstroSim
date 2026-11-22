package runtime

import (
	"context"
	"strings"
	"sync"
	"time"
)

// Lazy is a Runtime that finds its underlying runtime on demand.
//
// It lets the dashboard start even when no container runtime is installed or
// running: Ping retries Detect until it succeeds, so the preflight can report
// runtime_missing or runtime_not_running and then recover by itself when the
// user installs or starts podman or docker. Until a runtime is found every
// other method fails with the error of the last detection attempt.
type Lazy struct {
	opts DetectOptions

	mu      sync.Mutex
	rt      Runtime
	lastErr error
}

var _ Runtime = (*Lazy)(nil)

// NewLazy returns a Lazy runtime that uses opts for detection. It performs no
// detection until Ping is called.
func NewLazy(opts DetectOptions) *Lazy {
	return &Lazy{opts: opts, lastErr: ErrNotInstalled}
}

// inner returns the detected runtime, or the last detection error.
func (l *Lazy) inner() (Runtime, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.rt, l.lastErr
}

// Info implements Runtime. Before a runtime is found it reports only a name,
// podman unless docker was requested explicitly, so that build hints read
// sensibly.
func (l *Lazy) Info() Info {
	if rt, _ := l.inner(); rt != nil {
		return rt.Info()
	}
	name := "podman"
	if strings.EqualFold(l.opts.Name, "docker") {
		name = "docker"
	}
	return Info{Name: name}
}

// Ping implements Runtime. It detects the runtime if that has not succeeded
// yet, and otherwise pings the one found.
func (l *Lazy) Ping(ctx context.Context) (Info, error) {
	if rt, _ := l.inner(); rt != nil {
		return rt.Ping(ctx)
	}
	rt, info, err := Detect(ctx, l.opts)
	l.mu.Lock()
	defer l.mu.Unlock()
	if err != nil {
		l.lastErr = err
		return Info{}, err
	}
	l.rt, l.lastErr = rt, nil
	return info, nil
}

// ImageExists implements Runtime.
func (l *Lazy) ImageExists(ctx context.Context, image string) (bool, error) {
	rt, err := l.inner()
	if rt == nil {
		return false, err
	}
	return rt.ImageExists(ctx, image)
}

// EnsureNetwork implements Runtime.
func (l *Lazy) EnsureNetwork(ctx context.Context, name string, labels map[string]string) error {
	rt, err := l.inner()
	if rt == nil {
		return err
	}
	return rt.EnsureNetwork(ctx, name, labels)
}

// ListNetworksByLabel implements Runtime.
func (l *Lazy) ListNetworksByLabel(ctx context.Context, key, value string) ([]string, error) {
	rt, err := l.inner()
	if rt == nil {
		return nil, err
	}
	return rt.ListNetworksByLabel(ctx, key, value)
}

// RemoveNetwork implements Runtime.
func (l *Lazy) RemoveNetwork(ctx context.Context, name string) error {
	rt, err := l.inner()
	if rt == nil {
		return err
	}
	return rt.RemoveNetwork(ctx, name)
}

// Run implements Runtime.
func (l *Lazy) Run(ctx context.Context, spec RunSpec) (string, error) {
	rt, err := l.inner()
	if rt == nil {
		return "", err
	}
	return rt.Run(ctx, spec)
}

// Stop implements Runtime.
func (l *Lazy) Stop(ctx context.Context, name string, grace time.Duration) error {
	rt, err := l.inner()
	if rt == nil {
		return err
	}
	return rt.Stop(ctx, name, grace)
}

// Remove implements Runtime.
func (l *Lazy) Remove(ctx context.Context, name string) error {
	rt, err := l.inner()
	if rt == nil {
		return err
	}
	return rt.Remove(ctx, name)
}

// Wait implements Runtime.
func (l *Lazy) Wait(ctx context.Context, name string) (int, error) {
	rt, err := l.inner()
	if rt == nil {
		return 0, err
	}
	return rt.Wait(ctx, name)
}

// Inspect implements Runtime.
func (l *Lazy) Inspect(ctx context.Context, name string) (ContainerState, error) {
	rt, err := l.inner()
	if rt == nil {
		return ContainerState{}, err
	}
	return rt.Inspect(ctx, name)
}

// ListByLabel implements Runtime.
func (l *Lazy) ListByLabel(ctx context.Context, key, value string) ([]string, error) {
	rt, err := l.inner()
	if rt == nil {
		return nil, err
	}
	return rt.ListByLabel(ctx, key, value)
}

// Logs implements Runtime.
func (l *Lazy) Logs(ctx context.Context, name string, tail int) (string, error) {
	rt, err := l.inner()
	if rt == nil {
		return "", err
	}
	return rt.Logs(ctx, name, tail)
}
