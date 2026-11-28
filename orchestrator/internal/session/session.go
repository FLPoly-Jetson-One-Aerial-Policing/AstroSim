// Package session is the driver that runs the supervisor state machine
// against a real (or fake) container runtime.
//
// One goroutine, Run, owns the supervisor. Everything slow (preflight,
// launching, teardown, waiting for exits, health probes, timers) runs in
// helper goroutines that report back by posting supervisor events on a
// channel, so the supervisor itself is only ever touched from one place and
// needs no locking. After every step the driver publishes a status snapshot.
package session

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"strings"
	"sync"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/component"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/health"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/status"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/supervisor"
)

// ErrNotAllowed is returned by Start and Stop when the current phase does
// not allow the request.
var ErrNotAllowed = errors.New("session: request not allowed in the current phase")

// ErrClosed is returned by Start and Stop once Run has returned.
var ErrClosed = errors.New("session: closed")

const (
	// snapshotInterval is how often live facts are republished while active.
	snapshotInterval = 500 * time.Millisecond
	// logTail is how many log lines are captured from a failed container.
	logTail = 40
	// preflightTimeout bounds one preflight run.
	preflightTimeout = 20 * time.Second
	// opTimeout bounds a single runtime call that has no stop grace of its own.
	opTimeout = 30 * time.Second
	// inspectPoll is the fallback exit-detection poll interval.
	inspectPoll = time.Second
	// drainTimeout bounds how long teardown waits for a stopped container's
	// last output to reach the session log before the container is removed.
	drainTimeout = 3 * time.Second
)

// LogCapture records the output of the containers a Session launches. The
// logs package's Recorder satisfies it through a small adapter in main.
// A nil LogCapture in Options disables capture.
type LogCapture interface {
	// Begin starts following a freshly started container's output and
	// returns the handle teardown uses to read the tail and drain.
	Begin(component string, gen uint64, container, containerID, image string) Capture
	// Note writes a marker line, such as an exit code, to the component's log.
	Note(component, msg string)
	// Line writes one plain line, such as a line of download progress, to
	// the component's log.
	Line(component, line string)
}

// Capture is one container's log follower.
type Capture interface {
	// Drain waits until the follower has delivered the container's final
	// output, or ctx ends, so the container can be removed safely.
	Drain(ctx context.Context)
	// Tail returns up to n of the most recent lines seen.
	Tail(n int) []string
}

// Options configures a Session.
type Options struct {
	// Runtime is the container runtime. Required.
	Runtime runtime.Runtime
	// Definitions are the components, with any configuration overrides
	// already applied. Required.
	Definitions []component.Definition
	// StackContext is the deployment-wide settings.
	StackContext component.StackContext
	// Policy bounds automatic recovery.
	Policy supervisor.RestartPolicy
	// SessionID labels the containers this session creates.
	SessionID string
	// Version is the orchestrator version shown in snapshots.
	Version string
	// Broadcaster receives a snapshot after every change. Required.
	Broadcaster *status.Broadcaster
	// Logger receives operational log lines; nil discards them.
	Logger *slog.Logger
	// Clock supplies the time passed to the supervisor; nil means time.Now.
	Clock func() time.Time
	// Logs records each container's output into the session log; nil
	// disables capture. See LogCapture.
	Logs LogCapture
	// LogDir is the folder the session log is written to, published in
	// snapshots so the dashboard can show it. Empty means no file logging.
	LogDir string
	// PullMissing allows downloading a missing image whose reference names
	// its registry. When false such an image is reported as missing, like
	// any other.
	PullMissing bool
	// AfterFunc schedules supervisor timers; nil means time.AfterFunc.
	// Tests use it to compress the supervisor's long delays.
	AfterFunc func(d time.Duration, f func()) *time.Timer
}

// input is a message to the driver loop: a supervisor event, optionally
// guarded by a predicate on the state and answered on reply.
type input struct {
	ev    supervisor.Event
	allow func(supervisor.State) bool
	reply chan error
}

// run is one launch of one component.
type run struct {
	gen      uint64
	launched chan struct{} // closed when the launch goroutine has finished
	cancel   context.CancelFunc
	stopMon  context.CancelFunc

	mu      sync.Mutex
	monitor *health.Monitor
	capture Capture // set by doLaunch before Launched is posted; may be nil
}

func (r *run) facts() []health.Fact {
	r.mu.Lock()
	m := r.monitor
	r.mu.Unlock()
	if m == nil {
		return nil
	}
	return m.Facts()
}

// Session drives a supervisor. Create it with New and run it with Run.
type Session struct {
	opts Options
	log  *slog.Logger
	sup  *supervisor.Supervisor
	defs map[string]component.Definition
	ids  []string // component IDs in launch order
	// initial holds the actions of the startup check, run by Run.
	initial []supervisor.Action

	in   chan input
	done chan struct{} // closed when Run returns

	root       context.Context // lives until the driver exits
	cancelRoot context.CancelFunc
	wg         sync.WaitGroup // action goroutines

	mu       sync.Mutex
	cur      status.Snapshot
	runs     map[string]*run
	lastLogs map[string][]string
	pull     *pullJob // the latest download, kept after it ends; nil before the first
}

// New builds a Session. It fails if the definitions do not form a valid plan.
func New(opts Options) (*Session, error) {
	plan, err := supervisor.NewPlan(opts.Definitions)
	if err != nil {
		return nil, err
	}
	if opts.Runtime == nil || opts.Broadcaster == nil {
		return nil, errors.New("session: Runtime and Broadcaster are required")
	}
	if opts.Logger == nil {
		opts.Logger = slog.New(slog.NewTextHandler(io.Discard, nil))
	}
	if opts.Clock == nil {
		opts.Clock = time.Now
	}
	if opts.AfterFunc == nil {
		opts.AfterFunc = time.AfterFunc
	}
	s := &Session{
		opts:     opts,
		log:      opts.Logger,
		sup:      supervisor.New(plan, opts.Policy),
		defs:     map[string]component.Definition{},
		in:       make(chan input, 256),
		done:     make(chan struct{}),
		runs:     map[string]*run{},
		lastLogs: map[string][]string{},
	}
	for _, d := range plan.Definitions() {
		s.defs[d.ID] = d
		s.ids = append(s.ids, d.ID)
	}
	s.root, s.cancelRoot = context.WithCancel(context.Background())
	// Apply the startup check now, so the very first snapshot already says
	// "checking" rather than offering a Start that would be refused. The
	// resulting preflight action is executed when Run begins.
	s.initial = s.sup.Step(supervisor.Event{Kind: supervisor.CheckRequested}, opts.Clock())
	s.cur = s.buildSnapshot(s.sup.Snapshot())
	return s, nil
}

// Snapshot returns the latest status snapshot. It is safe for concurrent use.
func (s *Session) Snapshot() status.Snapshot {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.cur
}

// Start asks the stack to start. It returns ErrNotAllowed if the current
// phase does not accept it, and ErrClosed after Run has returned.
func (s *Session) Start() error {
	return s.request(supervisor.StartRequested, func(st supervisor.State) bool { return st.CanStart })
}

// Stop asks the stack to stop. It returns ErrNotAllowed if the current
// phase does not accept it, and ErrClosed after Run has returned.
func (s *Session) Stop() error {
	return s.request(supervisor.StopRequested, func(st supervisor.State) bool { return st.CanStop })
}

// Shutdown asks the driver to tear everything down and make Run return. It
// does not wait; wait for Run instead. It is idempotent.
func (s *Session) Shutdown() { s.post(supervisor.Event{Kind: supervisor.ShutdownRequested}) }

func (s *Session) request(kind supervisor.EventKind, allow func(supervisor.State) bool) error {
	reply := make(chan error, 1)
	select {
	case s.in <- input{ev: supervisor.Event{Kind: kind}, allow: allow, reply: reply}:
	case <-s.done:
		return ErrClosed
	}
	select {
	case err := <-reply:
		return err
	case <-s.done:
		return ErrClosed
	}
}

// post delivers a supervisor event to the driver loop, dropping it if the
// driver has finished.
func (s *Session) post(ev supervisor.Event) {
	select {
	case s.in <- input{ev: ev}:
	case <-s.done:
	}
}

// Run drives the stack until the supervisor asks to exit, which happens
// after Shutdown (or cancellation of ctx, which acts like Shutdown) and the
// teardown of every container. It runs a startup check first.
func (s *Session) Run(ctx context.Context) error {
	defer close(s.done)
	s.exec(s.initial)
	s.publish()

	ticker := time.NewTicker(snapshotInterval)
	defer ticker.Stop()
	ctxDone := ctx.Done()
	for {
		select {
		case m := <-s.in:
			if m.allow != nil && !m.allow(s.sup.Snapshot()) {
				m.reply <- ErrNotAllowed
				continue
			}
			if m.ev.Kind == supervisor.StartRequested {
				s.mu.Lock()
				s.lastLogs = map[string][]string{}
				s.mu.Unlock()
			}
			exit := s.step(m.ev)
			if m.reply != nil {
				m.reply <- nil
			}
			if exit {
				return s.finish()
			}
		case <-ticker.C:
			switch s.sup.Snapshot().Phase {
			case supervisor.PhaseStarting, supervisor.PhaseRunning, supervisor.PhaseRecovering, supervisor.PhaseDownloading:
				s.publish()
			}
		case <-ctxDone:
			ctxDone = nil
			s.log.Info("context cancelled; shutting down")
			s.post(supervisor.Event{Kind: supervisor.ShutdownRequested})
		}
	}
}

// finish releases every helper goroutine once the supervisor has exited.
func (s *Session) finish() error {
	s.cancelRoot()
	s.wg.Wait()
	s.publish()
	return nil
}

// step feeds one event to the supervisor, executes the resulting actions and
// publishes a snapshot. It reports whether the driver should exit.
func (s *Session) step(ev supervisor.Event) (exit bool) {
	before := s.sup.Snapshot().Phase
	actions := s.sup.Step(ev, s.opts.Clock())
	if after := s.sup.Snapshot(); after.Phase != before {
		s.log.Info("phase changed", "from", before, "to", after.Phase, "reason", after.Reason.Code)
	}
	exit = s.exec(actions)
	s.publish()
	return exit
}

// exec carries out the supervisor's actions and reports whether one asked to exit.
func (s *Session) exec(actions []supervisor.Action) (exit bool) {
	for _, a := range actions {
		switch a.Kind {
		case supervisor.ActionPreflight:
			s.goRun(func() { s.doPreflight(a.Gen) })
		case supervisor.ActionLaunch:
			s.startLaunch(a)
		case supervisor.ActionTeardown:
			r := s.runFor(a.Component, a.Gen)
			s.goRun(func() { s.doTeardown(a, r) })
		case supervisor.ActionPull:
			s.startPull(a)
		case supervisor.ActionCancelPull:
			s.cancelPull(a.Gen)
		case supervisor.ActionSetTimer:
			s.opts.AfterFunc(a.After, func() {
				s.post(supervisor.Event{Kind: supervisor.TimerFired, Timer: a.Timer, Key: a.Key, Gen: a.Gen})
			})
		case supervisor.ActionExit:
			exit = true
		}
	}
	return exit
}

func (s *Session) goRun(f func()) {
	s.wg.Add(1)
	go func() {
		defer s.wg.Done()
		f()
	}()
}

func (s *Session) runFor(id string, gen uint64) *run {
	s.mu.Lock()
	defer s.mu.Unlock()
	if r := s.runs[id]; r != nil && r.gen == gen {
		return r
	}
	return nil
}

// publish rebuilds the snapshot and sends it to subscribers.
func (s *Session) publish() {
	snap := s.buildSnapshot(s.sup.Snapshot())
	s.mu.Lock()
	s.cur = snap
	s.mu.Unlock()
	s.opts.Broadcaster.Publish(snap)
}

// buildSnapshot merges the supervisor state with definitions, probe facts,
// captured logs and the preflight report.
func (s *Session) buildSnapshot(st supervisor.State) status.Snapshot {
	info := s.opts.Runtime.Info()
	sc := s.opts.StackContext
	snap := status.Snapshot{
		Version: s.opts.Version,
		Phase:   string(st.Phase),
		Since:   st.Since,
		Reason: status.Reason{
			Code: st.Reason.Code, Detail: st.Reason.Detail, Component: st.Reason.Component,
		},
		Attempt:     st.Attempt,
		MaxAttempts: st.MaxAttempts,
		CanStart:    st.CanStart,
		CanStop:     st.CanStop,
		Runtime:     status.Runtime{Name: info.Name, Version: info.Version, Network: sc.NetworkMode},
		LogDir:      s.opts.LogDir,
		Components:  []status.Component{},
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.opts.Clock()
	for _, cs := range st.Components {
		d := s.defs[cs.ID]
		c := status.Component{
			ID: d.ID, Name: d.DisplayName, Description: d.Description, Image: d.Image,
			Installed:    true,
			BuildCommand: strings.ReplaceAll(d.BuildHint, "{runtime}", info.Name),
			State:        string(cs.State),
			Facts:        []health.Fact{},
			ExitCode:     cs.LastExitCode,
			LastLog:      append([]string{}, s.lastLogs[d.ID]...),
			Endpoints:    []status.Endpoint{},
		}
		if runtime.Pullable(d.Image) {
			c.PullCommand = info.Name + " pull " + d.Image
		}
		for _, img := range st.Preflight.MissingImages {
			if img == d.Image {
				c.Installed = false
			}
		}
		// The report is from before the download. While the download and the
		// check that follows it run, an image that arrived is not "not installed".
		if st.Phase == supervisor.PhaseDownloading || st.Phase == supervisor.PhaseChecking {
			if dl := s.downloadFor(d.Image, now); dl != nil && dl.State == pullDone {
				c.Installed = true
			}
		}
		if st.Phase == supervisor.PhaseDownloading {
			c.Download = s.downloadFor(d.Image, now)
		}
		// Facts belong to a live launch; hide them for components that are
		// not running so stale values are never shown.
		switch cs.State {
		case supervisor.ComponentStarting, supervisor.ComponentReady, supervisor.ComponentStale:
			if r := s.runs[d.ID]; r != nil && r.gen == cs.Gen {
				if f := r.facts(); f != nil {
					c.Facts = f
				}
			}
		}
		for _, p := range d.Ports {
			c.Endpoints = append(c.Endpoints, status.Endpoint{
				Name: p.Name, Proto: p.Proto,
				Address: hostPort(sc, p.Port),
			})
		}
		snap.Components = append(snap.Components, c)
	}
	return snap
}
