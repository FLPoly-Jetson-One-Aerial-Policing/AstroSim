// Package fake provides an in-memory runtime.Runtime for tests.
//
// Containers started with Run stay "running" until the test ends them with
// Exit or Kill, or the code under test calls Stop or Remove. Wait blocks
// accordingly, which lets session tests script crashes and clean exits
// without any real container engine.
package fake

import (
	"context"
	"fmt"
	"sort"
	"sync"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
)

// Exit codes the fake reports for orchestrated terminations, matching what
// real engines report (128 plus the signal number).
const (
	// StopExitCode is reported after Stop (SIGTERM).
	StopExitCode = 143
	// KillExitCode is reported after Kill or a forced Remove (SIGKILL).
	KillExitCode = 137
)

// container is one fake container.
type container struct {
	spec     runtime.RunSpec
	id       string
	state    runtime.ContainerState
	logs     string
	finished chan struct{} // closed when the container has exited
}

// Runtime is an in-memory runtime.Runtime. The zero value is not usable; call New.
type Runtime struct {
	mu         sync.Mutex
	info       runtime.Info
	images     map[string]bool
	networks   map[string]bool
	containers map[string]*container
	nextID     int

	// PingErr, if set, is returned by Ping (simulating a stopped daemon).
	PingErr error
	// RunErr, if set, is consulted by Run before starting; a non-nil result
	// is returned as the error.
	RunErr func(spec runtime.RunSpec) error
	// Now supplies timestamps; nil uses time.Now.
	Now  func() time.Time
	runs []runtime.RunSpec
}

var _ runtime.Runtime = (*Runtime)(nil)

// New returns an empty fake runtime that reports itself as podman.
func New() *Runtime {
	return &Runtime{
		info:       runtime.Info{Name: "podman", Path: "/fake/podman", Version: "5.0.0-fake"},
		images:     map[string]bool{},
		networks:   map[string]bool{},
		containers: map[string]*container{},
	}
}

func (r *Runtime) now() time.Time {
	if r.Now != nil {
		return r.Now()
	}
	return time.Now()
}

// SetImage marks an image as present (true) or absent (false).
func (r *Runtime) SetImage(image string, present bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.images[image] = present
}

// SetLogs sets the output Logs returns for a container.
func (r *Runtime) SetLogs(name, logs string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if c, ok := r.containers[name]; ok {
		c.logs = logs
	}
}

// SetHealth sets the image-health status Inspect reports for a container.
func (r *Runtime) SetHealth(name, health string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if c, ok := r.containers[name]; ok {
		c.state.Health = health
	}
}

// Networks returns the sorted names of the networks created so far.
func (r *Runtime) Networks() []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	var out []string
	for n := range r.networks {
		out = append(out, n)
	}
	sort.Strings(out)
	return out
}

// Spec returns the RunSpec a live container was started with.
func (r *Runtime) Spec(name string) (runtime.RunSpec, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	c, ok := r.containers[name]
	if !ok {
		return runtime.RunSpec{}, false
	}
	return c.spec, true
}

// Runs returns every RunSpec passed to Run, in order, including those that
// then failed validation.
func (r *Runtime) Runs() []runtime.RunSpec {
	r.mu.Lock()
	defer r.mu.Unlock()
	return append([]runtime.RunSpec(nil), r.runs...)
}

// Exit ends a running container with the given exit code, as if its main
// process had terminated. It reports whether the container was running.
func (r *Runtime) Exit(name string, code int) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.exitLocked(name, code)
}

// Kill is Exit with the SIGKILL exit code.
func (r *Runtime) Kill(name string) bool { return r.Exit(name, KillExitCode) }

func (r *Runtime) exitLocked(name string, code int) bool {
	c, ok := r.containers[name]
	if !ok || !c.state.Running {
		return false
	}
	c.state.Running = false
	c.state.Status = "exited"
	c.state.ExitCode = code
	c.state.FinishedAt = r.now()
	close(c.finished)
	return true
}

// SetVersion sets the version string Ping reports, to exercise the minimum
// version check.
func (r *Runtime) SetVersion(v string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.info.Version = v
}

// Info implements runtime.Runtime.
func (r *Runtime) Info() runtime.Info { return r.info }

// Ping implements runtime.Runtime.
func (r *Runtime) Ping(context.Context) (runtime.Info, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.PingErr != nil {
		return runtime.Info{}, r.PingErr
	}
	return r.info, nil
}

// ImageExists implements runtime.Runtime.
func (r *Runtime) ImageExists(_ context.Context, image string) (bool, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.images[image], nil
}

// EnsureNetwork implements runtime.Runtime.
func (r *Runtime) EnsureNetwork(_ context.Context, name string) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.networks[name] = true
	return nil
}

// Run implements runtime.Runtime. It fails if the image is absent or the
// container name is already taken, like a real engine.
func (r *Runtime) Run(_ context.Context, spec runtime.RunSpec) (string, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.runs = append(r.runs, spec)
	if r.RunErr != nil {
		if err := r.RunErr(spec); err != nil {
			return "", err
		}
	}
	if !r.images[spec.Image] {
		return "", fmt.Errorf("%w: image %s", runtime.ErrNotFound, spec.Image)
	}
	if _, dup := r.containers[spec.Name]; dup {
		return "", fmt.Errorf("fake: container name %q already in use", spec.Name)
	}
	r.nextID++
	c := &container{
		spec:     spec,
		id:       fmt.Sprintf("fake%08d", r.nextID),
		finished: make(chan struct{}),
	}
	c.state = runtime.ContainerState{Status: "running", Running: true, StartedAt: r.now()}
	r.containers[spec.Name] = c
	return c.id, nil
}

// Stop implements runtime.Runtime. The grace period is ignored: the fake
// container exits at once with StopExitCode.
func (r *Runtime) Stop(_ context.Context, name string, _ time.Duration) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, ok := r.containers[name]; !ok {
		return fmt.Errorf("%w: container %s", runtime.ErrNotFound, name)
	}
	r.exitLocked(name, StopExitCode)
	return nil
}

// Remove implements runtime.Runtime. Removing a running container kills it
// first, releasing any Wait.
func (r *Runtime) Remove(_ context.Context, name string) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.exitLocked(name, KillExitCode)
	delete(r.containers, name)
	return nil
}

// Wait implements runtime.Runtime.
func (r *Runtime) Wait(ctx context.Context, name string) (int, error) {
	r.mu.Lock()
	c, ok := r.containers[name]
	r.mu.Unlock()
	if !ok {
		return 0, fmt.Errorf("%w: container %s", runtime.ErrNotFound, name)
	}
	select {
	case <-c.finished:
		r.mu.Lock()
		defer r.mu.Unlock()
		return c.state.ExitCode, nil
	case <-ctx.Done():
		return 0, ctx.Err()
	}
}

// Inspect implements runtime.Runtime.
func (r *Runtime) Inspect(_ context.Context, name string) (runtime.ContainerState, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	c, ok := r.containers[name]
	if !ok {
		return runtime.ContainerState{}, fmt.Errorf("%w: container %s", runtime.ErrNotFound, name)
	}
	return c.state, nil
}

// ListByLabel implements runtime.Runtime.
func (r *Runtime) ListByLabel(_ context.Context, key, value string) ([]string, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	var names []string
	for n, c := range r.containers {
		if v, ok := c.spec.Labels[key]; ok && v == value {
			names = append(names, n)
		}
	}
	sort.Strings(names)
	return names, nil
}

// Logs implements runtime.Runtime. The tail argument is ignored.
func (r *Runtime) Logs(_ context.Context, name string, _ int) (string, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	c, ok := r.containers[name]
	if !ok {
		return "", fmt.Errorf("%w: container %s", runtime.ErrNotFound, name)
	}
	return c.logs, nil
}
