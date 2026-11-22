package supervisor

import (
	"fmt"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/component"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/health"
)

func def(id, group string, deps ...component.Dependency) component.Definition {
	return component.Definition{
		ID: id, Image: "img-" + id, RestartGroup: group, DependsOn: deps,
		Probe:        func(component.StackContext) health.Probe { return nil },
		ReadyTimeout: 30 * time.Second, LivenessTimeout: time.Second, StopTimeout: time.Second,
		PermanentExitCodes: []int{2},
	}
}

func started(id string) component.Dependency {
	return component.Dependency{ID: id, Condition: component.Started}
}
func ready(id string) component.Dependency {
	return component.Dependency{ID: id, Condition: component.Ready}
}

// chainDefs is the synthetic plan: b needs a Started, c needs b Ready. a and
// b are in separate restart groups, c restarts alone.
func chainDefs() []component.Definition {
	return []component.Definition{def("a", "ga"), def("b", "gb", started("a")), def("c", "", ready("b"))}
}

// sharedDefs puts a and b in one group.
func sharedDefs() []component.Definition {
	return []component.Definition{def("a", "sim"), def("b", "sim", started("a")), def("c", "", ready("b"))}
}

func mustPlan(t *testing.T, defs []component.Definition) *Plan {
	t.Helper()
	p, err := NewPlan(defs)
	if err != nil {
		t.Fatal(err)
	}
	return p
}

// rig drives a supervisor and records a readable trace of every action.
type rig struct {
	t   *testing.T
	s   *Supervisor
	now time.Time
}

func newRig(t *testing.T, defs []component.Definition, pol RestartPolicy) *rig {
	return &rig{t: t, s: New(mustPlan(t, defs), pol), now: time.Unix(1000, 0)}
}

func fmtAction(a Action) string {
	switch a.Kind {
	case ActionPreflight:
		return fmt.Sprintf("preflight#%d", a.Gen)
	case ActionLaunch:
		return fmt.Sprintf("launch:%s#%d", a.Component, a.Gen)
	case ActionTeardown:
		return fmt.Sprintf("teardown:%s#%d:%s:logs=%v", a.Component, a.Gen, a.Reason, a.CaptureLogs)
	case ActionSetTimer:
		kind := map[TimerKind]string{TimerReadyTimeout: "ready", TimerBackoff: "backoff", TimerStable: "stable", TimerRecheck: "recheck"}[a.Timer]
		return fmt.Sprintf("timer:%s:%s@%s", kind, a.Key, a.After)
	case ActionExit:
		return fmt.Sprintf("exit:%d", a.Code)
	}
	return "?"
}

// step feeds one event and returns the formatted actions.
func (r *rig) step(ev Event) []string {
	r.now = r.now.Add(time.Second)
	var out []string
	for _, a := range r.s.Step(ev, r.now) {
		out = append(out, fmtAction(a))
	}
	return out
}

func (r *rig) want(got []string, want ...string) {
	r.t.Helper()
	if !reflect.DeepEqual(got, want) {
		r.t.Fatalf("actions:\n got  %q\n want %q", got, want)
	}
}

func (r *rig) comp(id string) ComponentStatus {
	for _, c := range r.s.Snapshot().Components {
		if c.ID == id {
			return c
		}
	}
	r.t.Fatalf("no component %s", id)
	return ComponentStatus{}
}

func (r *rig) phase(p Phase) {
	r.t.Helper()
	if got := r.s.Snapshot().Phase; got != p {
		r.t.Fatalf("phase = %s, want %s", got, p)
	}
}

func (r *rig) state(id string, st ComponentState) {
	r.t.Helper()
	if got := r.comp(id).State; got != st {
		r.t.Fatalf("%s state = %s, want %s", id, got, st)
	}
}

// ev builds a component event with the component's current generation.
func (r *rig) ev(kind EventKind, id string) Event {
	return Event{Kind: kind, Component: id, Gen: r.comp(id).Gen}
}

func (r *rig) exit(id string, code int) []string {
	e := r.ev(Exited, id)
	e.ExitCode = code
	return r.step(e)
}

// timer fires a timer exactly as the driver would, taking the generation
// from the action that set it.
func (r *rig) timer(kind TimerKind, key string, gen uint64) []string {
	return r.step(Event{Kind: TimerFired, Timer: kind, Key: key, Gen: gen})
}

// lastTimerGen finds the generation of the most recent SetTimer of a kind.
func lastTimerGen(acts []Action, kind TimerKind, key string) uint64 {
	var g uint64
	for _, a := range acts {
		if a.Kind == ActionSetTimer && a.Timer == kind && a.Key == key {
			g = a.Gen
		}
	}
	return g
}

// stepRaw is step but returns the raw actions too.
func (r *rig) stepRaw(ev Event) ([]Action, []string) {
	r.now = r.now.Add(time.Second)
	acts := r.s.Step(ev, r.now)
	var out []string
	for _, a := range acts {
		out = append(out, fmtAction(a))
	}
	return acts, out
}

// bringUp starts the stack and walks it to running; it returns the stable
// timer actions' raw form.
func (r *rig) bringUp() []Action {
	r.t.Helper()
	r.step(Event{Kind: StartRequested})
	r.step(Event{Kind: PreflightDone, Gen: r.s.preflightGen})
	r.step(r.ev(Launched, "a"))
	r.step(r.ev(Launched, "b"))
	r.step(r.ev(Ready, "a"))
	r.step(r.ev(Ready, "b"))
	r.step(r.ev(Launched, "c"))
	acts, _ := r.stepRaw(r.ev(Ready, "c"))
	r.phase(PhaseRunning)
	return acts
}

func TestNewPlanValidation(t *testing.T) {
	withPort := func(id string, port int, proto string) component.Definition {
		d := def(id, "")
		d.Ports = []component.PortSpec{{Name: "p", Proto: proto, Port: port}}
		return d
	}
	bad := def("x", "")
	bad.Image = ""
	tests := []struct {
		name string
		defs []component.Definition
		err  string // substring; empty means success
	}{
		{"ok", chainDefs(), ""},
		{"same port different proto", []component.Definition{withPort("a", 80, "udp"), withPort("b", 80, "tcp")}, ""},
		{"invalid definition", []component.Definition{bad}, "empty image"},
		{"duplicate id", []component.Definition{def("a", ""), def("a", "")}, "duplicate component ID"},
		{"unknown dependency", []component.Definition{def("a", "", started("zzz"))}, "unknown component"},
		{"self cycle via validate", []component.Definition{def("a", "", started("a"))}, "invalid dependency"},
		{"two cycle", []component.Definition{def("a", "", started("b")), def("b", "", ready("a"))}, "cycle"},
		{"three cycle", []component.Definition{def("a", "", started("c")), def("b", "", started("a")), def("c", "", started("b")), def("d", "")}, "cycle"},
		{"port clash", []component.Definition{withPort("a", 80, "udp"), withPort("b", 80, "udp")}, "both publish 80/udp"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			_, err := NewPlan(tc.defs)
			if tc.err == "" {
				if err != nil {
					t.Fatalf("unexpected error: %v", err)
				}
				return
			}
			if err == nil || !strings.Contains(err.Error(), tc.err) {
				t.Fatalf("error = %v, want containing %q", err, tc.err)
			}
		})
	}
}

func TestPlanOrderAndSets(t *testing.T) {
	// Declared out of order on purpose.
	p := mustPlan(t, []component.Definition{def("c", "", ready("b")), def("b", "gb", started("a")), def("a", "ga")})
	if got, want := p.Order(), []string{"a", "b", "c"}; !reflect.DeepEqual(got, want) {
		t.Fatalf("order = %v, want %v", got, want)
	}
	ga, gb, gc := p.GroupOf("a"), p.GroupOf("b"), p.GroupOf("c")
	for _, tc := range []struct {
		group string
		want  []string
	}{{ga, []string{"a", "b", "c"}}, {gb, []string{"b", "c"}}, {gc, []string{"c"}}, {"nope", nil}} {
		if got := p.RestartSet(tc.group); !reflect.DeepEqual(got, tc.want) {
			t.Errorf("RestartSet(%s) = %v, want %v", tc.group, got, tc.want)
		}
	}
	if got, want := p.TeardownWaves(nil), [][]string{{"c"}, {"b"}, {"a"}}; !reflect.DeepEqual(got, want) {
		t.Errorf("waves = %v, want %v", got, want)
	}
	if got, want := p.TeardownWaves([]string{"a", "c"}), [][]string{{"a", "c"}}; !reflect.DeepEqual(got, want) {
		t.Errorf("waves(a,c) = %v, want %v", got, want)
	}
	// A shared group closes over dependents of every member.
	q := mustPlan(t, sharedDefs())
	if got, want := q.RestartSet(q.GroupOf("a")), []string{"a", "b", "c"}; !reflect.DeepEqual(got, want) {
		t.Errorf("shared RestartSet = %v, want %v", got, want)
	}
	// A group named like another component's ID does not collide.
	r := mustPlan(t, []component.Definition{def("x", ""), def("y", "x")})
	if r.GroupOf("x") == r.GroupOf("y") {
		t.Error("group name collided with component ID")
	}
}

func TestStartupOrdering(t *testing.T) {
	r := newRig(t, chainDefs(), DefaultRestartPolicy)
	r.phase(PhaseStopped)
	if st := r.s.Snapshot(); !st.CanStart || st.CanStop {
		t.Fatalf("stopped: CanStart/CanStop = %v/%v", st.CanStart, st.CanStop)
	}
	r.want(r.step(Event{Kind: StartRequested}), "preflight#1")
	r.phase(PhaseChecking)
	if st := r.s.Snapshot(); st.CanStart || !st.CanStop {
		t.Fatalf("checking+start: CanStart/CanStop = %v/%v", st.CanStart, st.CanStop)
	}
	// Only a is launchable first.
	r.want(r.step(Event{Kind: PreflightDone, Gen: 1}), "launch:a#1", "timer:ready:a@30s")
	r.phase(PhaseStarting)
	// a Launched satisfies b's Started dependency.
	r.want(r.step(r.ev(Launched, "a")), "launch:b#1", "timer:ready:b@30s")
	// b Launched does NOT satisfy c's Ready dependency.
	r.want(r.step(r.ev(Launched, "b")))
	r.want(r.step(r.ev(Ready, "a")))
	r.want(r.step(r.ev(Ready, "b")), "launch:c#1", "timer:ready:c@30s")
	r.want(r.step(r.ev(Launched, "c")))
	r.phase(PhaseStarting)
	acts, got := r.stepRaw(r.ev(Ready, "c"))
	r.phase(PhaseRunning)
	r.want(got, "timer:stable:group:ga@2m0s", "timer:stable:group:gb@2m0s", "timer:stable:solo:c@2m0s")
	if lastTimerGen(acts, TimerStable, "group:ga") == 0 {
		t.Fatal("stable timer has no generation")
	}
	if st := r.s.Snapshot(); st.CanStart || !st.CanStop {
		t.Fatalf("running: CanStart/CanStop = %v/%v", st.CanStart, st.CanStop)
	}
	// Repeated or irrelevant events change nothing.
	r.want(r.step(Event{Kind: StartRequested}))
	r.want(r.step(r.ev(Ready, "c")))
}

func TestPreflightProblemAndRecheck(t *testing.T) {
	r := newRig(t, chainDefs(), DefaultRestartPolicy)
	r.want(r.step(Event{Kind: StartRequested}), "preflight#1")
	rep := PreflightReport{Problem: ProblemImagesMissing, MissingImages: []string{"img-a"}, Detail: "img-a not found"}
	acts, got := r.stepRaw(Event{Kind: PreflightDone, Gen: 1, Report: rep})
	r.want(got, "timer:recheck:@5s")
	r.phase(PhaseUnavailable)
	st := r.s.Snapshot()
	if st.Reason.Code != ProblemImagesMissing || st.Reason.Detail != "img-a not found" || st.Preflight.MissingImages[0] != "img-a" {
		t.Fatalf("reason/preflight = %+v / %+v", st.Reason, st.Preflight)
	}
	if st.CanStart || !st.CanStop {
		t.Fatalf("unavailable: CanStart/CanStop = %v/%v", st.CanStart, st.CanStop)
	}
	// Start is not honoured while unavailable.
	r.want(r.step(Event{Kind: StartRequested}))

	// A stale recheck timer is ignored.
	r.want(r.timer(TimerRecheck, "", 999))
	// The real one reruns preflight; still bad: a new recheck is armed.
	g1 := lastTimerGen(acts, TimerRecheck, "")
	r.want(r.timer(TimerRecheck, "", g1), "preflight#2")
	r.phase(PhaseUnavailable)
	acts, got = r.stepRaw(Event{Kind: PreflightDone, Gen: 2, Report: PreflightReport{Problem: ProblemPortBusy, BusyPorts: []string{"8765/udp"}}})
	r.want(got, "timer:recheck:@5s")
	if r.s.Snapshot().Reason.Code != ProblemPortBusy {
		t.Fatalf("reason = %+v", r.s.Snapshot().Reason)
	}
	// The consumed timer cannot fire again.
	r.want(r.timer(TimerRecheck, "", g1))
	// A clean recheck returns to stopped, not to starting.
	g2 := lastTimerGen(acts, TimerRecheck, "")
	r.want(r.timer(TimerRecheck, "", g2), "preflight#3")
	r.want(r.step(Event{Kind: PreflightDone, Gen: 3}))
	r.phase(PhaseStopped)
	if r.s.Snapshot().Reason.Code != "" {
		t.Fatalf("reason not cleared: %+v", r.s.Snapshot().Reason)
	}
}

func TestCheckRequested(t *testing.T) {
	r := newRig(t, chainDefs(), DefaultRestartPolicy)
	r.want(r.step(Event{Kind: CheckRequested}), "preflight#1")
	r.phase(PhaseChecking)
	if st := r.s.Snapshot(); st.CanStart || st.CanStop {
		t.Fatalf("pure check: CanStart/CanStop = %v/%v", st.CanStart, st.CanStop)
	}
	r.want(r.step(Event{Kind: StopRequested})) // nothing to stop
	r.want(r.step(Event{Kind: PreflightDone, Gen: 1}))
	r.phase(PhaseStopped) // a clean check never launches anything
	r.want(r.step(Event{Kind: CheckRequested}), "preflight#2")
	r.want(r.step(Event{Kind: PreflightDone, Gen: 2, Report: PreflightReport{Problem: ProblemRuntimeMissing}}), "timer:recheck:@5s")
	r.phase(PhaseUnavailable)
	r.want(r.step(Event{Kind: CheckRequested})) // only honoured in stopped
}

func TestStopDuringPreflight(t *testing.T) {
	r := newRig(t, chainDefs(), DefaultRestartPolicy)
	r.step(Event{Kind: StartRequested})
	r.want(r.step(Event{Kind: StopRequested}))
	r.phase(PhaseStopped)
	// The late result of the abandoned preflight must not start anything.
	r.want(r.step(Event{Kind: PreflightDone, Gen: 1}))
	r.phase(PhaseStopped)
}

func TestStopFromUnavailable(t *testing.T) {
	r := newRig(t, chainDefs(), DefaultRestartPolicy)
	r.step(Event{Kind: StartRequested})
	acts, _ := r.stepRaw(Event{Kind: PreflightDone, Gen: 1, Report: PreflightReport{Problem: ProblemPortBusy}})
	g := lastTimerGen(acts, TimerRecheck, "")
	r.want(r.step(Event{Kind: StopRequested}))
	r.phase(PhaseStopped)
	r.want(r.timer(TimerRecheck, "", g)) // cancelled
	r.phase(PhaseStopped)
}

func TestStopRunningInReverseWaves(t *testing.T) {
	r := newRig(t, chainDefs(), DefaultRestartPolicy)
	r.bringUp()
	// Only the last wave member goes first; the next wave waits for Removed.
	r.want(r.step(Event{Kind: StopRequested}), "teardown:c#1:stop:logs=false")
	r.phase(PhaseStopping)
	if st := r.s.Snapshot(); st.CanStart || st.CanStop {
		t.Fatalf("stopping: CanStart/CanStop = %v/%v", st.CanStart, st.CanStop)
	}
	r.want(r.step(Event{Kind: StopRequested})) // idempotent
	// An exit during deliberate teardown is expected, not a failure.
	r.want(r.exit("c", 143))
	r.phase(PhaseStopping)
	if c := r.comp("c"); c.LastExitCode == nil || *c.LastExitCode != 143 {
		t.Fatalf("exit code not recorded: %+v", c)
	}
	r.want(r.step(r.ev(Removed, "c")), "teardown:b#1:stop:logs=false")
	r.want(r.step(r.ev(Removed, "b")), "teardown:a#1:stop:logs=false")
	r.phase(PhaseStopping)
	r.want(r.step(r.ev(Removed, "a")))
	r.phase(PhaseStopped)
	for _, id := range []string{"a", "b", "c"} {
		r.state(id, ComponentStopped)
	}
	// And it can start again, with a fresh preflight and new generations.
	r.want(r.step(Event{Kind: StartRequested}), "preflight#2")
	r.want(r.step(Event{Kind: PreflightDone, Gen: 2}), "launch:a#2", "timer:ready:a@30s")
}

func TestStopMidStart(t *testing.T) {
	r := newRig(t, chainDefs(), DefaultRestartPolicy)
	r.step(Event{Kind: StartRequested})
	r.step(Event{Kind: PreflightDone, Gen: 1})
	r.step(r.ev(Launched, "a"))
	// b is launching (no Launched yet), c still pending.
	r.state("b", ComponentLaunching)
	r.state("c", ComponentPending)
	r.want(r.step(Event{Kind: StopRequested}), "teardown:b#1:stop:logs=false")
	// The late Launched of the abandoned launch is ignored; state stays stopping.
	r.want(r.step(r.ev(Launched, "b")))
	r.state("b", ComponentStopping)
	r.want(r.step(r.ev(Removed, "b")), "teardown:a#1:stop:logs=false")
	r.want(r.step(r.ev(Removed, "a")))
	r.phase(PhaseStopped)
	r.state("c", ComponentPending)
}

func TestRecoveryAndBackoff(t *testing.T) {
	r := newRig(t, chainDefs(), DefaultRestartPolicy)
	r.bringUp()
	wantBackoffs := []string{"2s", "5s", "10s"}
	for i, bo := range wantBackoffs {
		attempt := i + 1
		if i > 0 {
			// Walk back up so the next failure is a fresh failure of a running stack.
			r.want(r.timer(TimerBackoff, "group:gb", 0)) // stale: ignored
		}
		// b exits. Its restart set is {b, c}: c first, then b, both with logs.
		got := r.exit("b", 1)
		r.want(got, "teardown:c#"+fmt.Sprint(r.comp("c").Gen)+":restart:logs=true")
		r.phase(PhaseRecovering)
		st := r.s.Snapshot()
		if st.Attempt != attempt || st.MaxAttempts != 3 || st.Reason.Code != ReasonComponentExited || st.Reason.Component != "b" {
			t.Fatalf("attempt %d: state = %+v", attempt, st)
		}
		r.state("a", ComponentReady) // a is outside the set and keeps running
		r.want(r.step(r.ev(Removed, "c")), fmt.Sprintf("teardown:b#%d:restart:logs=true", r.comp("b").Gen))
		acts, got := r.stepRaw(r.ev(Removed, "b"))
		r.want(got, "timer:backoff:group:gb@"+bo)
		r.phase(PhaseRecovering)
		gen := lastTimerGen(acts, TimerBackoff, "group:gb")
		// Backoff fires: relaunch in dependency order, with new generations.
		bg := r.comp("b").Gen + 1
		r.want(r.timer(TimerBackoff, "group:gb", gen), fmt.Sprintf("launch:b#%d", bg), fmt.Sprintf("timer:ready:b@30s"))
		r.phase(PhaseStarting)
		r.want(r.step(r.ev(Launched, "b")))
		cg := r.comp("c").Gen + 1
		r.want(r.step(r.ev(Ready, "b")), fmt.Sprintf("launch:c#%d", cg), "timer:ready:c@30s")
		r.step(r.ev(Launched, "c"))
		r.step(r.ev(Ready, "c"))
		r.phase(PhaseRunning)
		if r.comp("a").Gen != 1 {
			t.Fatalf("a was relaunched")
		}
	}
	// Fourth consecutive failure: give up, tearing everything down with logs.
	r.exit("b", 1)
	r.step(r.ev(Removed, "c"))
	r.step(r.ev(Removed, "b"))
	r.phase(PhaseStopping)
	// Everything (including a) is torn down because the failure was final.
	r.state("a", ComponentStopping)
	r.step(r.ev(Removed, "a"))
	r.phase(PhaseFailed)
	st := r.s.Snapshot()
	if st.Reason.Code != ReasonRestartLimit || !strings.Contains(st.Reason.Detail, "gave up after 3") {
		t.Fatalf("reason = %+v", st.Reason)
	}
	if st.Attempt != 3 || !st.CanStart || st.CanStop {
		t.Fatalf("failed state = %+v", st)
	}
}

func TestGiveUpTearsDownEverythingWithLogs(t *testing.T) {
	r := newRig(t, chainDefs(), RestartPolicy{MaxAttempts: 0})
	r.bringUp()
	r.want(r.exit("b", 1), "teardown:c#1:failed:logs=true")
	r.phase(PhaseStopping)
	r.want(r.step(r.ev(Removed, "c")), "teardown:b#1:failed:logs=true")
	r.want(r.step(r.ev(Removed, "b")), "teardown:a#1:failed:logs=true")
	r.want(r.step(r.ev(Removed, "a")))
	r.phase(PhaseFailed)
	// Start from failed works and resets the failure count.
	r.want(r.step(Event{Kind: StartRequested}), "preflight#2")
	r.step(Event{Kind: PreflightDone, Gen: 2})
	if r.s.Snapshot().Attempt != 0 {
		t.Fatal("attempt not reset by a new start")
	}
}

func TestSharedGroupRestartsBothAndRelaunchesInOrder(t *testing.T) {
	r := newRig(t, sharedDefs(), DefaultRestartPolicy)
	r.bringUp()
	r.want(r.exit("a", 1), "teardown:c#1:restart:logs=true")
	r.want(r.step(r.ev(Removed, "c")), "teardown:b#1:restart:logs=true")
	r.want(r.step(r.ev(Removed, "b")), "teardown:a#1:restart:logs=true")
	acts, got := r.stepRaw(r.ev(Removed, "a"))
	r.want(got, "timer:backoff:group:sim@2s")
	// Only a is launchable first; b follows a's Launched.
	r.want(r.timer(TimerBackoff, "group:sim", lastTimerGen(acts, TimerBackoff, "group:sim")), "launch:a#2", "timer:ready:a@30s")
	r.want(r.step(r.ev(Launched, "a")), "launch:b#2", "timer:ready:b@30s")
}

func TestStaleGenerationEventsIgnored(t *testing.T) {
	r := newRig(t, chainDefs(), DefaultRestartPolicy)
	r.bringUp()
	// Restart b once so generation 1 events become stale.
	r.exit("b", 1)
	r.step(r.ev(Removed, "c"))
	acts, _ := r.stepRaw(r.ev(Removed, "b"))
	r.timer(TimerBackoff, "group:gb", lastTimerGen(acts, TimerBackoff, "group:gb"))
	if r.comp("b").Gen != 2 {
		t.Fatalf("gen = %d", r.comp("b").Gen)
	}
	old := func(k EventKind) Event { return Event{Kind: k, Component: "b", Gen: 1, ExitCode: 1} }
	for _, k := range []EventKind{Launched, LaunchFailed, Ready, Unhealthy, Exited, Removed} {
		r.want(r.step(old(k)))
		r.state("b", ComponentLaunching)
		r.phase(PhaseStarting)
	}
	// The ready-timeout of the old generation is stale too.
	r.want(r.step(Event{Kind: TimerFired, Timer: TimerReadyTimeout, Key: "b", Gen: 1}))
	r.state("b", ComponentLaunching)
	// The current generation's timeout does fire.
	r.want(r.step(Event{Kind: TimerFired, Timer: TimerReadyTimeout, Key: "b", Gen: 2}), "teardown:b#2:restart:logs=true")
	r.phase(PhaseRecovering)
	// Unknown components are ignored.
	r.want(r.step(Event{Kind: Ready, Component: "zzz", Gen: 1}))
}

func TestStableResetsFailureCount(t *testing.T) {
	r := newRig(t, chainDefs(), DefaultRestartPolicy)
	acts := r.bringUp()
	stable := lastTimerGen(acts, TimerStable, "group:gb")
	r.exit("b", 1)
	if r.s.Snapshot().Attempt != 1 {
		t.Fatal("attempt != 1")
	}
	// The old stable timer was cancelled by the failure.
	r.want(r.timer(TimerStable, "group:gb", stable))
	if r.s.Snapshot().Attempt != 1 {
		t.Fatal("stale stable timer reset the count")
	}
	r.step(r.ev(Removed, "c"))
	bo, _ := r.stepRaw(r.ev(Removed, "b"))
	r.timer(TimerBackoff, "group:gb", lastTimerGen(bo, TimerBackoff, "group:gb"))
	r.step(r.ev(Launched, "b"))
	r.step(r.ev(Ready, "b"))
	r.step(r.ev(Launched, "c"))
	acts, _ = r.stepRaw(r.ev(Ready, "c"))
	r.phase(PhaseRunning)
	r.want(r.timer(TimerStable, "group:gb", lastTimerGen(acts, TimerStable, "group:gb")))
	if got := r.s.Snapshot().Attempt; got != 0 {
		t.Fatalf("attempt after stable = %d, want 0", got)
	}
	// With the count reset the next failure restarts with the first backoff.
	r.exit("b", 1)
	r.step(r.ev(Removed, "c"))
	_, got := r.stepRaw(r.ev(Removed, "b"))
	r.want(got, "timer:backoff:group:gb@2s")
}

func TestBackoffClampsToLastEntry(t *testing.T) {
	pol := RestartPolicy{MaxAttempts: 5, Backoff: []time.Duration{time.Second, 3 * time.Second}}
	s := New(mustPlan(t, chainDefs()), pol)
	for n, want := range map[int]time.Duration{0: 0, 1: time.Second, 2: 3 * time.Second, 4: 3 * time.Second} {
		if got := s.backoff(n); got != want {
			t.Errorf("backoff(%d) = %s, want %s", n, got, want)
		}
	}
	if s.policy.StableAfter != 2*time.Minute {
		t.Errorf("StableAfter default = %s", s.policy.StableAfter)
	}
}

func TestPermanentExitGoesStraightToFailed(t *testing.T) {
	r := newRig(t, chainDefs(), DefaultRestartPolicy)
	r.bringUp()
	r.want(r.exit("a", 2), "teardown:c#1:failed:logs=true")
	r.phase(PhaseStopping)
	r.want(r.step(r.ev(Removed, "c")), "teardown:b#1:failed:logs=true")
	r.want(r.step(r.ev(Removed, "b")), "teardown:a#1:failed:logs=true")
	r.want(r.step(r.ev(Removed, "a")))
	r.phase(PhaseFailed)
	st := r.s.Snapshot()
	if st.Reason.Code != ReasonInvalidConfig || st.Reason.Component != "a" || st.Attempt != 0 {
		t.Fatalf("state = %+v", st)
	}
	if c := r.comp("a"); c.LastExitCode == nil || *c.LastExitCode != 2 {
		t.Fatalf("a exit = %v", c.LastExitCode)
	}
}

func TestFailureKinds(t *testing.T) {
	tests := []struct {
		name  string
		event func(r *rig) Event
		code  string
		final bool // permanent: no recovery
	}{
		{"unhealthy", func(r *rig) Event { e := r.ev(Unhealthy, "c"); e.Detail = "silent"; return e }, ReasonUnhealthy, false},
		{"exited", func(r *rig) Event { e := r.ev(Exited, "c"); e.ExitCode = 137; return e }, ReasonComponentExited, false},
		{"launch failed", func(r *rig) Event { e := r.ev(LaunchFailed, "c"); e.Detail = "boom"; return e }, ReasonLaunchFailed, false},
		{"permanent launch failure", func(r *rig) Event { e := r.ev(LaunchFailed, "c"); e.Permanent = true; return e }, ReasonLaunchFailed, true},
		{"ready timeout", func(r *rig) Event {
			return Event{Kind: TimerFired, Timer: TimerReadyTimeout, Key: "c", Gen: r.comp("c").Gen}
		}, ReasonReadyTimeout, false},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			r := newRig(t, chainDefs(), DefaultRestartPolicy)
			r.step(Event{Kind: StartRequested})
			r.step(Event{Kind: PreflightDone, Gen: 1})
			r.step(r.ev(Launched, "a"))
			r.step(r.ev(Launched, "b"))
			r.step(r.ev(Ready, "b"))
			// c is launching. (Unhealthy only applies to ready components.)
			if tc.name == "unhealthy" {
				r.step(r.ev(Launched, "c"))
				r.step(r.ev(Ready, "c"))
			}
			acts := r.step(tc.event(r))
			st := r.s.Snapshot()
			if st.Reason.Code != tc.code || st.Reason.Component != "c" {
				t.Fatalf("reason = %+v", st.Reason)
			}
			if tc.final {
				r.phase(PhaseStopping)
			} else {
				r.phase(PhaseRecovering)
			}
			if len(acts) == 0 || !strings.HasPrefix(acts[0], "teardown:c#1:") || !strings.HasSuffix(acts[0], "logs=true") {
				t.Fatalf("actions = %q", acts)
			}
		})
	}
}

func TestUnhealthyWhileStartingIsIgnored(t *testing.T) {
	r := newRig(t, chainDefs(), DefaultRestartPolicy)
	r.step(Event{Kind: StartRequested})
	r.step(Event{Kind: PreflightDone, Gen: 1})
	r.step(r.ev(Launched, "a"))
	r.want(r.step(r.ev(Unhealthy, "a")))
	r.phase(PhaseStarting)
}

func TestStopMidRecovering(t *testing.T) {
	r := newRig(t, chainDefs(), DefaultRestartPolicy)
	r.bringUp()
	r.exit("b", 1) // c is being torn down
	r.phase(PhaseRecovering)
	// Stop now: c is already stopping; a and b still need teardown. b may
	// only go after c is removed, a after b.
	r.want(r.step(Event{Kind: StopRequested}))
	r.phase(PhaseStopping)
	r.want(r.step(r.ev(Removed, "c")), "teardown:b#1:stop:logs=false")
	r.want(r.step(r.ev(Removed, "b")), "teardown:a#1:stop:logs=false")
	r.want(r.step(r.ev(Removed, "a")))
	r.phase(PhaseStopped)
	if r.s.Snapshot().Reason.Code != "" {
		t.Fatal("stop left a reason behind")
	}
}

func TestStopDuringBackoffCancelsRelaunch(t *testing.T) {
	r := newRig(t, chainDefs(), DefaultRestartPolicy)
	r.bringUp()
	r.exit("b", 1)
	r.step(r.ev(Removed, "c"))
	acts, _ := r.stepRaw(r.ev(Removed, "b"))
	gen := lastTimerGen(acts, TimerBackoff, "group:gb")
	r.want(r.step(Event{Kind: StopRequested}), "teardown:a#1:stop:logs=false")
	r.want(r.step(r.ev(Removed, "a")))
	r.phase(PhaseStopped)
	r.want(r.timer(TimerBackoff, "group:gb", gen)) // stale: nothing launches
	r.phase(PhaseStopped)
}

func TestSecondGroupFailureDuringRecoveryExtendsIt(t *testing.T) {
	r := newRig(t, chainDefs(), DefaultRestartPolicy)
	r.bringUp()
	r.exit("b", 1)         // restart set {b, c}; c stopping
	r.want(r.exit("a", 1)) // a (group ga) fails too: set grows to {a, b, c}; c already stopping
	r.want(r.step(r.ev(Removed, "c")), "teardown:b#1:restart:logs=true")
	r.want(r.step(r.ev(Removed, "b")), "teardown:a#1:restart:logs=true")
	_, got := r.stepRaw(r.ev(Removed, "a"))
	r.want(got, "timer:backoff:group:ga@2s", "timer:backoff:group:gb@2s")
}

func TestShutdownIsParallel(t *testing.T) {
	r := newRig(t, chainDefs(), DefaultRestartPolicy)
	r.bringUp()
	r.want(r.step(Event{Kind: ShutdownRequested}),
		"teardown:c#1:shutdown:logs=false", "teardown:b#1:shutdown:logs=false", "teardown:a#1:shutdown:logs=false")
	r.phase(PhaseStopping)
	if st := r.s.Snapshot(); st.CanStart || st.CanStop {
		t.Fatalf("shutdown: CanStart/CanStop = %v/%v", st.CanStart, st.CanStop)
	}
	r.want(r.step(Event{Kind: ShutdownRequested})) // repeated signal: no new actions
	r.want(r.step(Event{Kind: StartRequested}))
	r.want(r.step(r.ev(Removed, "b")))
	r.want(r.step(r.ev(Removed, "a")))
	r.want(r.step(r.ev(Removed, "c")), "exit:0")
	r.want(r.step(Event{Kind: StartRequested})) // after Exit nothing happens
}

func TestShutdownWhileIdleExitsImmediately(t *testing.T) {
	for _, setup := range []func(r *rig){
		func(r *rig) {},
		func(r *rig) { r.step(Event{Kind: StartRequested}) }, // preflight in flight
		func(r *rig) {
			r.step(Event{Kind: StartRequested})
			r.step(Event{Kind: PreflightDone, Gen: 1, Report: PreflightReport{Problem: ProblemPortBusy}})
		},
	} {
		r := newRig(t, chainDefs(), DefaultRestartPolicy)
		setup(r)
		r.want(r.step(Event{Kind: ShutdownRequested}), "exit:0")
	}
}

func TestShutdownMidStartOnlyTearsDownLaunched(t *testing.T) {
	r := newRig(t, chainDefs(), DefaultRestartPolicy)
	r.step(Event{Kind: StartRequested})
	r.step(Event{Kind: PreflightDone, Gen: 1})
	r.step(r.ev(Launched, "a")) // a up, b launching, c pending
	r.want(r.step(Event{Kind: ShutdownRequested}), "teardown:b#1:shutdown:logs=false", "teardown:a#1:shutdown:logs=false")
	r.want(r.step(r.ev(Removed, "a")))
	r.want(r.step(r.ev(Removed, "b")), "exit:0")
}

func TestShutdownDuringStopAndFailureTeardown(t *testing.T) {
	r := newRig(t, chainDefs(), DefaultRestartPolicy)
	r.bringUp()
	r.step(Event{Kind: StopRequested}) // c stopping, b and a waiting
	got := r.step(Event{Kind: ShutdownRequested})
	r.want(got, "teardown:b#1:shutdown:logs=false", "teardown:a#1:shutdown:logs=false")
	r.step(r.ev(Removed, "c"))
	r.step(r.ev(Removed, "b"))
	r.want(r.step(r.ev(Removed, "a")), "exit:0")
}

func TestSnapshotComponents(t *testing.T) {
	r := newRig(t, chainDefs(), DefaultRestartPolicy)
	r.bringUp()
	st := r.s.Snapshot()
	var ids []string
	for _, c := range st.Components {
		ids = append(ids, c.ID)
		if c.State != ComponentReady || c.Gen != 1 || c.LastExitCode != nil {
			t.Errorf("component %+v", c)
		}
	}
	if !reflect.DeepEqual(ids, []string{"a", "b", "c"}) {
		t.Errorf("ids = %v", ids)
	}
	if st.Since.IsZero() {
		t.Error("Since not set")
	}
}
