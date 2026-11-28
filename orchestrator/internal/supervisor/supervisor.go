package supervisor

import (
	"fmt"
	"slices"
	"strings"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/component"
)

type purpose int

const (
	purposeNone purpose = iota
	purposeRecover
	purposeStop
	purposeFail
	purposeShutdown
)

type timerKey struct {
	kind TimerKind
	key  string
}

type comp struct {
	def     component.Definition
	state   ComponentState
	gen     uint64
	exit    int
	hasExit bool
}

// Supervisor is the state machine. It is not safe for concurrent use; the
// driver owns it from a single goroutine.
type Supervisor struct {
	plan   *Plan
	policy RestartPolicy

	phase     Phase
	since     time.Time
	reason    Reason
	preflight PreflightReport
	comps     map[string]*comp
	failures  map[string]int // by group key

	wanted    map[string]bool // components that should be running
	recGroups map[string]bool // groups awaiting a relaunch after recovery

	// Teardown in progress.
	purpose     purpose
	teardown    map[string]bool
	parallel    bool
	captureLogs bool

	// Timers other than ReadyTimeout: a firing is valid only if its Gen is
	// the one stored here. Deleting the entry cancels the timer.
	timers   map[timerKey]uint64
	timerSeq uint64

	preflightGen    uint64
	awaitPreflight  bool
	startAfterCheck bool
	pullGen         uint64
	awaitPull       bool
	pullHold        bool   // a download failed; wait for TimerPullRetry before the next
	pullFailures    int    // consecutive failed downloads
	pullReason      Reason // why the last download failed
	pulled          bool   // the check in flight follows a successful download
	shuttingDown    bool
	exited          bool

	now time.Time
	out []Action
}

// New returns a supervisor for the plan in the stopped phase. A policy with
// a non-positive StableAfter uses the default two minutes.
func New(plan *Plan, policy RestartPolicy) *Supervisor {
	if policy.StableAfter <= 0 {
		policy.StableAfter = DefaultRestartPolicy.StableAfter
	}
	if policy.MaxAttempts < 0 || policy.Unlimited {
		policy.MaxAttempts = 0
	}
	s := &Supervisor{
		plan:      plan,
		policy:    policy,
		phase:     PhaseStopped,
		comps:     map[string]*comp{},
		failures:  map[string]int{},
		wanted:    map[string]bool{},
		recGroups: map[string]bool{},
		teardown:  map[string]bool{},
		timers:    map[timerKey]uint64{},
	}
	for _, id := range plan.order {
		s.comps[id] = &comp{def: plan.defs[id], state: ComponentPending}
	}
	return s
}

// Snapshot returns the current state.
func (s *Supervisor) Snapshot() State {
	st := State{
		Phase:       s.phase,
		Since:       s.since,
		Reason:      s.reason,
		Preflight:   s.preflight,
		MaxAttempts: s.policy.MaxAttempts,
		CanStart:    !s.shuttingDown && (s.phase == PhaseStopped || s.phase == PhaseFailed),
		CanStop: !s.shuttingDown && (s.phase == PhaseStarting || s.phase == PhaseRunning ||
			s.phase == PhaseRecovering || s.phase == PhaseUnavailable || s.phase == PhaseDownloading ||
			(s.phase == PhaseChecking && s.startAfterCheck)),
	}
	for _, n := range s.failures {
		st.Attempt = max(st.Attempt, n)
	}
	if !s.policy.Unlimited {
		st.Attempt = min(st.Attempt, s.policy.MaxAttempts)
	}
	for _, id := range s.plan.order {
		c := s.comps[id]
		cs := ComponentStatus{ID: id, State: c.state, Gen: c.gen}
		if c.hasExit {
			code := c.exit
			cs.LastExitCode = &code
		}
		st.Components = append(st.Components, cs)
	}
	return st
}

// Step applies one event at time now and returns the actions the driver must
// execute, in order. Events that do not apply in the current state, or that
// carry a stale generation, are ignored and return nil.
func (s *Supervisor) Step(ev Event, now time.Time) []Action {
	if s.exited {
		return nil
	}
	s.now, s.out = now, nil
	switch ev.Kind {
	case StartRequested:
		if s.phase == PhaseStopped || s.phase == PhaseFailed {
			s.pullHold = false // an explicit retry
			s.startAfterCheck = true
			s.beginCheck()
		}
	case CheckRequested:
		if s.phase == PhaseStopped {
			s.startAfterCheck = false
			s.beginCheck()
		}
	case StopRequested:
		s.onStop()
	case ShutdownRequested:
		s.onShutdown()
	case PreflightDone:
		s.onPreflightDone(ev)
	case PullDone:
		s.onPullDone(ev)
	case Launched:
		if c := s.current(ev); c != nil && c.state == ComponentLaunching {
			c.state = ComponentStarting
			s.launchReady()
		}
	case LaunchFailed:
		if c := s.current(ev); c != nil && c.state == ComponentLaunching {
			s.fail(c, ComponentFailed, ReasonLaunchFailed, ev.Detail, ev.Permanent)
		}
	case Ready:
		s.onReady(ev)
	case Unhealthy:
		if c := s.current(ev); c != nil && c.state == ComponentReady {
			s.fail(c, ComponentStale, ReasonUnhealthy, ev.Detail, false)
		}
	case Exited:
		s.onExited(ev)
	case Removed:
		if c := s.current(ev); c != nil && c.state == ComponentStopping {
			c.state = ComponentStopped
			s.advanceTeardown()
		}
	case TimerFired:
		s.onTimer(ev)
	}
	return s.out
}

// current returns the component an event refers to if the event's
// generation is the component's current one, else nil.
func (s *Supervisor) current(ev Event) *comp {
	c := s.comps[ev.Component]
	if c == nil || c.gen != ev.Gen {
		return nil
	}
	return c
}

func (s *Supervisor) setPhase(p Phase) {
	if s.phase != p {
		s.phase, s.since = p, s.now
	}
}

func (s *Supervisor) emit(a Action) { s.out = append(s.out, a) }

// armTimer starts (or restarts) a timer; any earlier timer with the same
// kind and key becomes stale.
func (s *Supervisor) armTimer(kind TimerKind, key string, after time.Duration) {
	s.timerSeq++
	s.timers[timerKey{kind, key}] = s.timerSeq
	s.emit(Action{Kind: ActionSetTimer, Timer: kind, Key: key, After: after, Gen: s.timerSeq})
}

func (s *Supervisor) cancelTimers(kind TimerKind) {
	for k := range s.timers {
		if k.kind == kind {
			delete(s.timers, k)
		}
	}
}

// --- checking ---------------------------------------------------------

func (s *Supervisor) beginCheck() {
	s.reason = Reason{}
	s.setPhase(PhaseChecking)
	s.requestPreflight()
}

func (s *Supervisor) requestPreflight() {
	s.preflightGen++
	s.awaitPreflight = true
	s.emit(Action{Kind: ActionPreflight, Gen: s.preflightGen})
}

func (s *Supervisor) onPreflightDone(ev Event) {
	if !s.awaitPreflight || ev.Gen != s.preflightGen {
		return
	}
	s.awaitPreflight = false
	s.preflight = ev.Report
	pulled := s.pulled
	s.pulled = false
	if len(ev.Report.Pullable) > 0 {
		switch {
		case pulled:
			// The download succeeded but the image is still missing:
			// treat it as a failed download rather than looping.
			s.pullFailed(Reason{Code: ProblemPullFailed,
				Detail: "the image is still missing after it was downloaded: " + strings.Join(ev.Report.Pullable, ", ")})
		case s.pullHold:
			s.startAfterCheck = false
			s.reason = s.pullReason
			s.setPhase(PhaseUnavailable)
			s.armTimer(TimerRecheck, "", RecheckInterval)
		default:
			s.cancelTimers(TimerRecheck)
			s.cancelTimers(TimerPullRetry)
			s.reason = Reason{}
			s.pullGen++
			s.awaitPull = true
			s.setPhase(PhaseDownloading)
			s.emit(Action{Kind: ActionPull, Gen: s.pullGen, Images: slices.Clone(ev.Report.Pullable)})
		}
		return
	}
	if pulled {
		s.pullFailures = 0
	}
	if ev.Report.Problem != "" {
		s.startAfterCheck = false
		s.reason = Reason{Code: ev.Report.Problem, Detail: ev.Report.Detail}
		s.setPhase(PhaseUnavailable)
		s.armTimer(TimerRecheck, "", RecheckInterval)
		return
	}
	s.reason = Reason{}
	s.pullHold, s.pullFailures = false, 0
	if s.phase == PhaseChecking && s.startAfterCheck {
		s.startAfterCheck = false
		s.startAll()
		return
	}
	s.setPhase(PhaseStopped)
}

// onPullDone ends a download: success re-checks, failure holds the next
// attempt back for a backoff.
func (s *Supervisor) onPullDone(ev Event) {
	if !s.awaitPull || ev.Gen != s.pullGen {
		return
	}
	s.awaitPull = false
	if ev.Report.Problem == "" {
		s.pulled = true
		s.beginCheck()
		return
	}
	s.pullFailed(Reason{Code: ev.Report.Problem, Detail: ev.Report.Detail})
}

// pullFailed records a failed download: the stack becomes unavailable with
// the pull reason, rechecks go on without downloading, and a retry timer
// ends the hold after the backoff.
func (s *Supervisor) pullFailed(r Reason) {
	s.pullFailures++
	s.pullHold = true
	s.pullReason = r
	s.startAfterCheck = false
	s.reason = r
	s.setPhase(PhaseUnavailable)
	s.armTimer(TimerRecheck, "", RecheckInterval)
	b := PullBackoff
	s.armTimer(TimerPullRetry, "", b[min(s.pullFailures, len(b))-1])
}

// --- starting ---------------------------------------------------------

func (s *Supervisor) startAll() {
	clear(s.failures)
	clear(s.recGroups)
	s.timers = map[timerKey]uint64{}
	for _, id := range s.plan.order {
		s.wanted[id] = true
	}
	s.setPhase(PhaseStarting)
	s.launchReady()
}

// depsSatisfied reports whether every dependency of c is in the state its
// condition demands.
func (s *Supervisor) depsSatisfied(c *comp) bool {
	for _, dep := range c.def.DependsOn {
		d := s.comps[dep.ID]
		switch dep.Condition {
		case component.Started:
			if d.state != ComponentStarting && d.state != ComponentReady {
				return false
			}
		case component.Ready:
			if d.state != ComponentReady {
				return false
			}
		}
	}
	return true
}

// launchReady launches every wanted component whose dependencies are met.
// A launch can satisfy a Started dependency only after its Launched event,
// so one pass in launch order is enough per event.
func (s *Supervisor) launchReady() {
	if s.phase != PhaseStarting {
		return
	}
	for _, id := range s.plan.order {
		c := s.comps[id]
		if !s.wanted[id] || (c.state != ComponentPending && c.state != ComponentStopped) || !s.depsSatisfied(c) {
			continue
		}
		c.gen++
		c.state = ComponentLaunching
		s.emit(Action{Kind: ActionLaunch, Component: id, Gen: c.gen})
		s.emit(Action{Kind: ActionSetTimer, Timer: TimerReadyTimeout, Key: id, Gen: c.gen, After: c.def.ReadyTimeout})
	}
}

func (s *Supervisor) onReady(ev Event) {
	c := s.current(ev)
	if c == nil || (c.state != ComponentLaunching && c.state != ComponentStarting) {
		return
	}
	c.state = ComponentReady
	if s.phase != PhaseStarting {
		return
	}
	s.launchReady()
	for _, id := range s.plan.order {
		if s.comps[id].state != ComponentReady {
			return
		}
	}
	s.reason = Reason{}
	s.setPhase(PhaseRunning)
	for _, g := range s.plan.groups {
		s.armTimer(TimerStable, g, s.policy.StableAfter)
	}
}

// --- failures ---------------------------------------------------------

func (s *Supervisor) onExited(ev Event) {
	c := s.current(ev)
	if c == nil {
		return
	}
	c.exit, c.hasExit = ev.ExitCode, true
	switch c.state {
	case ComponentLaunching, ComponentStarting, ComponentReady:
		permanent := slices.Contains(c.def.PermanentExitCodes, ev.ExitCode)
		code := ReasonComponentExited
		if permanent {
			code = ReasonInvalidConfig
		}
		s.fail(c, ComponentExited, code, fmt.Sprintf("%s exited with code %d", c.def.ID, ev.ExitCode), permanent)
	default:
		// Already failed or being torn down: the exit is expected.
	}
}

// fail records a failure of c and decides between recovery, giving up and
// ignoring (when the phase does not react to failures).
func (s *Supervisor) fail(c *comp, st ComponentState, code, detail string, permanent bool) {
	c.state = st
	if s.phase != PhaseStarting && s.phase != PhaseRunning && s.phase != PhaseRecovering {
		return
	}
	if detail == "" {
		detail = fmt.Sprintf("%s: %s", c.def.ID, code)
	}
	s.reason = Reason{Code: code, Detail: detail, Component: c.def.ID}
	g := s.plan.groupOf[c.def.ID]
	delete(s.timers, timerKey{TimerStable, g})
	if permanent {
		s.beginFinalTeardown()
		return
	}
	s.failures[g]++
	if !s.policy.Unlimited && s.failures[g] > s.policy.MaxAttempts {
		s.reason = Reason{
			Code:      ReasonRestartLimit,
			Detail:    fmt.Sprintf("%s; gave up after %d restart attempts", detail, s.policy.MaxAttempts),
			Component: c.def.ID,
		}
		s.beginFinalTeardown()
		return
	}
	s.beginRecovery(g)
}

func (s *Supervisor) beginRecovery(group string) {
	s.purpose, s.parallel, s.captureLogs = purposeRecover, false, true
	s.recGroups[group] = true
	s.cancelTimers(TimerBackoff)
	for _, id := range s.plan.RestartSet(group) {
		s.teardown[id] = true
		s.wanted[id] = false
	}
	s.setPhase(PhaseRecovering)
	s.advanceTeardown()
}

// beginFinalTeardown tears everything down, keeping logs, before failing.
func (s *Supervisor) beginFinalTeardown() {
	s.purpose, s.parallel, s.captureLogs = purposeFail, false, true
	s.cancelAll()
	for _, id := range s.plan.order {
		s.teardown[id] = true
	}
	s.setPhase(PhaseStopping)
	s.advanceTeardown()
}

// cancelAll drops every pending relaunch and timer.
func (s *Supervisor) cancelAll() {
	s.timers = map[timerKey]uint64{}
	clear(s.recGroups)
	clear(s.wanted)
	s.awaitPreflight = false
	s.pulled = false
}

// --- stopping ---------------------------------------------------------

func (s *Supervisor) onStop() {
	switch s.phase {
	case PhaseUnavailable:
		s.cancelAll()
		s.pullHold, s.pullFailures = false, 0
		s.reason = Reason{}
		s.setPhase(PhaseStopped)
	case PhaseDownloading:
		s.cancelPull()
		s.cancelAll()
		s.startAfterCheck = false
		s.pullHold, s.pullFailures = false, 0
		s.reason = Reason{Code: ReasonPullCancelled}
		s.setPhase(PhaseStopped)
	case PhaseChecking:
		if s.startAfterCheck {
			s.cancelAll()
			s.startAfterCheck = false
			s.setPhase(PhaseStopped)
		}
	case PhaseStarting, PhaseRunning, PhaseRecovering:
		s.purpose, s.parallel, s.captureLogs = purposeStop, false, false
		s.cancelAll()
		for _, id := range s.plan.order {
			s.teardown[id] = true
		}
		s.reason = Reason{}
		s.setPhase(PhaseStopping)
		s.advanceTeardown()
	}
}

func (s *Supervisor) onShutdown() {
	if s.shuttingDown {
		return
	}
	s.shuttingDown = true
	s.startAfterCheck = false
	s.cancelPull()
	s.purpose, s.parallel, s.captureLogs = purposeShutdown, true, false
	s.cancelAll()
	for _, id := range s.plan.order {
		s.teardown[id] = true
	}
	if s.needsTeardown() {
		s.setPhase(PhaseStopping)
	}
	s.advanceTeardown()
}

// cancelPull aborts the download in flight, if any.
func (s *Supervisor) cancelPull() {
	if s.awaitPull {
		s.awaitPull = false
		s.emit(Action{Kind: ActionCancelPull, Gen: s.pullGen})
	}
}

func (s *Supervisor) needsTeardown() bool {
	for id := range s.teardown {
		if !done(s.comps[id]) {
			return true
		}
	}
	return false
}

// done reports whether a component has nothing (left) to tear down.
func done(c *comp) bool { return c.state == ComponentPending || c.state == ComponentStopped }

// advanceTeardown issues Teardown for every flagged component that may go
// now: in ordered mode a component waits until all flagged dependents are
// gone, which yields reverse-topological waves. When nothing is left it
// completes the teardown according to its purpose.
func (s *Supervisor) advanceTeardown() {
	if s.purpose == purposeNone {
		return
	}
	for i := len(s.plan.order) - 1; i >= 0; i-- {
		id := s.plan.order[i]
		c := s.comps[id]
		if !s.teardown[id] || done(c) || c.state == ComponentStopping {
			continue
		}
		if !s.parallel && s.dependentPending(id) {
			continue
		}
		c.state = ComponentStopping
		s.emit(Action{Kind: ActionTeardown, Component: id, Gen: c.gen, CaptureLogs: s.captureLogs, Reason: s.teardownReason()})
	}
	if s.needsTeardown() {
		return
	}
	p := s.purpose
	s.purpose = purposeNone
	clear(s.teardown)
	switch p {
	case purposeRecover:
		for _, g := range s.plan.groups {
			if s.recGroups[g] {
				s.armTimer(TimerBackoff, g, s.backoff(s.failures[g]))
			}
		}
	case purposeStop:
		s.setPhase(PhaseStopped)
	case purposeFail:
		s.setPhase(PhaseFailed)
	case purposeShutdown:
		s.exited = true
		s.emit(Action{Kind: ActionExit, Code: 0})
	}
}

// dependentPending reports whether a flagged dependent of id still exists.
func (s *Supervisor) dependentPending(id string) bool {
	for _, d := range s.plan.dependents[id] {
		if s.teardown[d] && !done(s.comps[d]) {
			return true
		}
	}
	return false
}

func (s *Supervisor) teardownReason() string {
	switch s.purpose {
	case purposeRecover:
		return "restart"
	case purposeStop:
		return "stop"
	case purposeShutdown:
		return "shutdown"
	default:
		return "failed"
	}
}

// backoff returns the wait before restart attempt n (1-based).
func (s *Supervisor) backoff(n int) time.Duration {
	b := s.policy.Backoff
	if len(b) == 0 || n < 1 {
		return 0
	}
	return b[min(n, len(b))-1]
}

// --- timers -----------------------------------------------------------

func (s *Supervisor) onTimer(ev Event) {
	if ev.Timer == TimerReadyTimeout {
		c := s.current(Event{Component: ev.Key, Gen: ev.Gen})
		if c != nil && (c.state == ComponentLaunching || c.state == ComponentStarting) {
			s.fail(c, ComponentFailed, ReasonReadyTimeout,
				fmt.Sprintf("%s was not ready after %s", c.def.ID, c.def.ReadyTimeout), false)
		}
		return
	}
	k := timerKey{ev.Timer, ev.Key}
	if gen, ok := s.timers[k]; !ok || gen != ev.Gen {
		return
	}
	delete(s.timers, k)
	switch ev.Timer {
	case TimerRecheck:
		if s.phase == PhaseUnavailable {
			s.requestPreflight()
		}
	case TimerPullRetry:
		if s.phase == PhaseUnavailable {
			s.pullHold = false
			if !s.awaitPreflight {
				s.requestPreflight()
			}
		}
	case TimerStable:
		s.failures[ev.Key] = 0
	case TimerBackoff:
		delete(s.recGroups, ev.Key)
		for _, id := range s.plan.RestartSet(ev.Key) {
			s.wanted[id] = true
		}
		s.setPhase(PhaseStarting)
		s.launchReady()
	}
}
