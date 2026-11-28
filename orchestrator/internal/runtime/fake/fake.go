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
	"io"
	"maps"
	"sort"
	"strings"
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

// Output streams for WriteLog.
const (
	// Stdout selects the container's standard output.
	Stdout = "stdout"
	// Stderr selects the container's standard error.
	Stderr = "stderr"
)

// chunk is one piece of container output. bol records whether the chunk
// starts at the beginning of a line, which is where the runtime would put
// its timestamp.
type chunk struct {
	t      time.Time
	stream string
	data   string
	bol    bool
}

// stamped renders the chunk as `logs --timestamps` would: the RFC 3339
// timestamp is prefixed at the start of every line the chunk begins.
func (c chunk) stamped() string {
	ts := c.t.Format(time.RFC3339Nano) + " "
	var b strings.Builder
	atStart := c.bol
	rest := c.data
	for rest != "" {
		if atStart {
			b.WriteString(ts)
		}
		i := strings.IndexByte(rest, '\n')
		if i < 0 {
			b.WriteString(rest)
			break
		}
		b.WriteString(rest[:i+1])
		rest = rest[i+1:]
		atStart = true
	}
	return b.String()
}

// container is one fake container.
type container struct {
	spec     runtime.RunSpec
	id       string
	state    runtime.ContainerState
	logs     []chunk
	midLine  map[string]bool // per stream: the last chunk did not end in a newline
	notify   chan struct{}   // closed and replaced whenever logs change or the container exits
	finished chan struct{}   // closed when the container has exited
}

// wake releases every follower waiting for a change.
func (c *container) wake() {
	close(c.notify)
	c.notify = make(chan struct{})
}

// Runtime is an in-memory runtime.Runtime. The zero value is not usable; call New.
type Runtime struct {
	mu         sync.Mutex
	info       runtime.Info
	images     map[string]bool
	networks   map[string]map[string]string
	containers map[string]*container
	nextID     int

	// PingErr, if set, is returned by Ping (simulating a stopped daemon).
	PingErr error
	// RunErr, if set, is consulted by Run before starting; a non-nil result
	// is returned as the error.
	RunErr func(spec runtime.RunSpec) error
	// FollowHang, if set, lists containers whose FollowLogs does not end when
	// the container exits (it ends only when its context does), simulating a
	// hung `logs --follow`.
	FollowHang map[string]bool
	// FollowErr, if set, is consulted by FollowLogs before streaming; a
	// non-nil result is returned as the error.
	FollowErr func(name string) error
	// PullHook, if set, runs inside Pull after the call is recorded. It may
	// block (honouring ctx), write progress lines and return an error; on a
	// nil result the image is marked present. Nil uses a default that writes
	// "pulled <image>" and succeeds.
	PullHook func(ctx context.Context, image string, progress io.Writer) error
	// Now supplies timestamps; nil uses time.Now.
	Now   func() time.Time
	runs  []runtime.RunSpec
	ops   []string
	pulls []string
}

var _ runtime.Runtime = (*Runtime)(nil)

// New returns an empty fake runtime that reports itself as podman.
func New() *Runtime {
	return &Runtime{
		info:       runtime.Info{Name: "podman", Path: "/fake/podman", Version: "5.0.0-fake"},
		images:     map[string]bool{},
		networks:   map[string]map[string]string{},
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

// SetLogs replaces a container's output with logs, as standard output.
func (r *Runtime) SetLogs(name, logs string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if c, ok := r.containers[name]; ok {
		c.logs, c.midLine = nil, map[string]bool{}
		r.writeLocked(c, Stdout, logs)
	}
}

// WriteLog appends text to a container's stdout or stderr (see Stdout and
// Stderr). The timestamp is added at the start of each line, so partial
// lines may be written in several calls. Followers wake at once. It reports
// whether the container exists.
func (r *Runtime) WriteLog(name, stream, text string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	c, ok := r.containers[name]
	if !ok {
		return false
	}
	r.writeLocked(c, stream, text)
	return true
}

func (r *Runtime) writeLocked(c *container, stream, text string) {
	if text == "" {
		return
	}
	t := r.now()
	// Keep timestamps strictly increasing so --since is deterministic even
	// with a coarse or frozen clock.
	if n := len(c.logs); n > 0 && !t.After(c.logs[n-1].t) {
		t = c.logs[n-1].t.Add(time.Nanosecond)
	}
	c.logs = append(c.logs, chunk{t: t, stream: stream, data: text, bol: !c.midLine[stream]})
	c.midLine[stream] = !strings.HasSuffix(text, "\n")
	c.wake()
}

// Ops returns the journal of container-affecting calls in order, for
// example "stop astrosim-a" and "remove astrosim-a" (also for names that no
// longer exist). Run is recorded as "run <name>".
func (r *Runtime) Ops() []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	return append([]string(nil), r.ops...)
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
	c.wake()
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

// Pull implements runtime.Runtime. It records "pull <image>" in Ops and the
// image in Pulls, then runs PullHook (or the default, which writes
// "pulled <image>" to progress). When that succeeds the image becomes
// present. A nil progress is allowed.
func (r *Runtime) Pull(ctx context.Context, image string, progress io.Writer) error {
	r.mu.Lock()
	r.ops = append(r.ops, "pull "+image)
	r.pulls = append(r.pulls, image)
	hook := r.PullHook
	r.mu.Unlock()
	if progress == nil {
		progress = io.Discard
	}
	var err error
	if hook != nil {
		err = hook(ctx, image, progress)
	} else {
		_, err = io.WriteString(progress, "pulled "+image+"\n")
	}
	if err != nil {
		return err
	}
	r.SetImage(image, true)
	return nil
}

// Pulls returns every image passed to Pull, in order.
func (r *Runtime) Pulls() []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	return append([]string(nil), r.pulls...)
}

// EnsureNetwork implements runtime.Runtime.
func (r *Runtime) EnsureNetwork(_ context.Context, name string, labels map[string]string) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, ok := r.networks[name]; !ok {
		r.networks[name] = maps.Clone(labels)
	}
	return nil
}

// ListNetworksByLabel implements runtime.Runtime.
func (r *Runtime) ListNetworksByLabel(_ context.Context, key, value string) ([]string, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	var names []string
	for n, labels := range r.networks {
		if v, ok := labels[key]; ok && v == value {
			names = append(names, n)
		}
	}
	sort.Strings(names)
	return names, nil
}

// RemoveNetwork implements runtime.Runtime. Removing a missing network
// succeeds, as with the real implementation.
func (r *Runtime) RemoveNetwork(_ context.Context, name string) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.networks, name)
	return nil
}

// Run implements runtime.Runtime. It fails if the image is absent or the
// container name is already taken, like a real engine.
func (r *Runtime) Run(_ context.Context, spec runtime.RunSpec) (string, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.runs = append(r.runs, spec)
	r.ops = append(r.ops, "run "+spec.Name)
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
		midLine:  map[string]bool{},
		notify:   make(chan struct{}),
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
	r.ops = append(r.ops, "stop "+name)
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
	r.ops = append(r.ops, "remove "+name)
	r.exitLocked(name, KillExitCode)
	if c, ok := r.containers[name]; ok {
		c.wake() // release followers so they see the container is gone
	}
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

// Logs implements runtime.Runtime. The output has no timestamps and the tail
// argument is ignored.
func (r *Runtime) Logs(_ context.Context, name string, _ int) (string, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	c, ok := r.containers[name]
	if !ok {
		return "", fmt.Errorf("%w: container %s", runtime.ErrNotFound, name)
	}
	var b strings.Builder
	for _, ch := range c.logs {
		b.WriteString(ch.data)
	}
	return b.String(), nil
}

// FollowLogs implements runtime.Runtime. It replays the output after
// opts.Since (exclusive, like podman) and, with opts.Follow, keeps delivering
// new output until the container has exited and everything is delivered, or
// until ctx ends. A container listed in FollowHang never ends by itself.
func (r *Runtime) FollowLogs(ctx context.Context, name string, opts runtime.LogOptions, stdout, stderr io.Writer) error {
	r.mu.Lock()
	err := r.FollowErr
	r.mu.Unlock()
	if err != nil {
		if e := err(name); e != nil {
			return e
		}
	}
	delivered := 0
	for {
		r.mu.Lock()
		c, ok := r.containers[name]
		if !ok {
			r.mu.Unlock()
			return fmt.Errorf("%w: container %s", runtime.ErrNotFound, name)
		}
		var out []chunk
		for ; delivered < len(c.logs); delivered++ {
			ch := c.logs[delivered]
			if opts.Since.IsZero() || ch.t.After(opts.Since) {
				out = append(out, ch)
			}
		}
		notify, finished := c.notify, !c.state.Running
		hang := r.FollowHang[name]
		r.mu.Unlock()

		for _, ch := range out {
			w := stdout
			if ch.stream == Stderr {
				w = stderr
			}
			if _, werr := io.WriteString(w, ch.stamped()); werr != nil {
				return werr
			}
		}
		if len(out) > 0 {
			continue // re-check: more output or the exit may have arrived meanwhile
		}
		if !opts.Follow || (finished && !hang) {
			return nil
		}
		select {
		case <-notify:
		case <-ctx.Done():
			return ctx.Err()
		}
	}
}
