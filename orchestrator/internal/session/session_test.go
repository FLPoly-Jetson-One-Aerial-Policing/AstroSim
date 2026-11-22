package session

import (
	"context"
	"errors"
	"net"
	"reflect"
	"sync/atomic"
	"testing"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/component"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/health"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime/fake"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/status"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/supervisor"
)

// tickProbe reports one healthy observation every few milliseconds.
type tickProbe struct{}

func (tickProbe) Run(ctx context.Context, report func(health.Observation)) error {
	t := time.NewTicker(5 * time.Millisecond)
	defer t.Stop()
	for {
		report(health.Observation{Healthy: true, Facts: []health.Fact{{Key: "k", Label: "Fact", Value: "v"}}})
		select {
		case <-ctx.Done():
			return nil
		case <-t.C:
		}
	}
}

func testDefs(ports ...component.PortSpec) []component.Definition {
	mk := func(id string, deps ...component.Dependency) component.Definition {
		return component.Definition{
			ID: id, DisplayName: id, Image: "img-" + id, BuildHint: "{runtime} build " + id,
			RestartGroup: "sim", DependsOn: deps,
			Probe:        func(component.StackContext) health.Probe { return tickProbe{} },
			ReadyTimeout: time.Minute, LivenessTimeout: time.Second, StopTimeout: time.Second,
		}
	}
	a := mk("a")
	a.Ports = ports
	return []component.Definition{a, mk("b", component.Dependency{ID: "a", Condition: component.Started})}
}

type rig struct {
	t   *testing.T
	rt  *fake.Runtime
	s   *Session
	bc  *status.Broadcaster
	end chan error
}

// newRig starts a session whose supervisor timers run 20 times faster.
func newRig(t *testing.T, defs []component.Definition) *rig {
	t.Helper()
	return newRigWith(t, defs, func(f *fake.Runtime) runtime.Runtime { return f })
}

// newRigWith is newRig with the runtime wrapped, to inject faults.
func newRigWith(t *testing.T, defs []component.Definition, wrap func(*fake.Runtime) runtime.Runtime) *rig {
	t.Helper()
	return newRigOpts(t, defs, wrap, nil)
}

// newRigOpts is newRigWith with a hook to adjust the session Options.
func newRigOpts(t *testing.T, defs []component.Definition, wrap func(*fake.Runtime) runtime.Runtime, mod func(*Options)) *rig {
	t.Helper()
	rt := fake.New()
	bc := status.NewBroadcaster()
	opts := Options{
		Runtime: wrap(rt), Definitions: defs, StackContext: component.DefaultStackContext(),
		Policy: supervisor.DefaultRestartPolicy, SessionID: "test", Broadcaster: bc,
		AfterFunc: func(d time.Duration, f func()) *time.Timer { return time.AfterFunc(d/20, f) },
	}
	if mod != nil {
		mod(&opts)
	}
	s, err := New(opts)
	if err != nil {
		t.Fatal(err)
	}
	r := &rig{t: t, rt: rt, s: s, bc: bc, end: make(chan error, 1)}
	ctx, cancel := context.WithCancel(context.Background())
	go func() { r.end <- s.Run(ctx); close(r.end) }()
	t.Cleanup(func() {
		s.Shutdown()
		select {
		case <-r.end:
		case <-time.After(5 * time.Second):
			t.Error("Run did not return at cleanup")
		}
		cancel()
	})
	return r
}

func (r *rig) waitFor(desc string, cond func(status.Snapshot) bool) status.Snapshot {
	r.t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if s := r.s.Snapshot(); cond(s) {
			return s
		}
		time.Sleep(2 * time.Millisecond)
	}
	r.t.Fatalf("timed out waiting for %s; last snapshot %+v", desc, r.s.Snapshot())
	return status.Snapshot{}
}

func (r *rig) waitPhase(p supervisor.Phase) status.Snapshot {
	r.t.Helper()
	return r.waitFor("phase "+string(p), func(s status.Snapshot) bool { return s.Phase == string(p) })
}

func (r *rig) containers() []string {
	names, _ := r.rt.ListByLabel(context.Background(), component.LabelManaged, "true")
	return names
}

func (r *rig) installImages() {
	r.rt.SetImage("img-a", true)
	r.rt.SetImage("img-b", true)
}

func TestStartupCheckAndRecheck(t *testing.T) {
	r := newRig(t, testDefs())
	s := r.waitPhase(supervisor.PhaseUnavailable)
	if s.Reason.Code != supervisor.ProblemImagesMissing {
		t.Fatalf("reason = %+v", s.Reason)
	}
	for _, c := range s.Components {
		if c.Installed || c.BuildCommand != "podman build "+c.ID {
			t.Errorf("component %+v: want not installed with substituted build command", c)
		}
	}
	if s.CanStart {
		t.Error("CanStart while unavailable")
	}
	r.installImages()
	s = r.waitPhase(supervisor.PhaseStopped)
	if !s.CanStart || s.Reason.Code != "" {
		t.Fatalf("after install: %+v", s)
	}
}

func TestRuntimeProblems(t *testing.T) {
	rt := fake.New()
	rt.PingErr = errNotRunning()
	bc := status.NewBroadcaster()
	s, err := New(Options{Runtime: rt, Definitions: testDefs(), StackContext: component.DefaultStackContext(), Broadcaster: bc})
	if err != nil {
		t.Fatal(err)
	}
	rep := s.preflight(context.Background())
	if rep.Problem != supervisor.ProblemRuntimeNotRunning {
		t.Fatalf("got %+v", rep)
	}
	rt.PingErr = errNotInstalled()
	if rep := s.preflight(context.Background()); rep.Problem != supervisor.ProblemRuntimeMissing {
		t.Fatalf("got %+v", rep)
	}
}

func TestRuntimeTooOld(t *testing.T) {
	rt := fake.New()
	rt.SetVersion("3.4.4")
	s, err := New(Options{Runtime: rt, Definitions: testDefs(), StackContext: component.DefaultStackContext(), Broadcaster: status.NewBroadcaster()})
	if err != nil {
		t.Fatal(err)
	}
	rep := s.preflight(context.Background())
	if rep.Problem != supervisor.ProblemRuntimeTooOld || rep.Detail != "podman 3.4.4 found; 4.0 or newer is required" {
		t.Fatalf("got %+v", rep)
	}
	rt.SetVersion("4.9.4-rhel")
	if rep := s.preflight(context.Background()); rep.Problem == supervisor.ProblemRuntimeTooOld {
		t.Fatalf("4.9.4 flagged too old: %+v", rep)
	}
}

func TestStartRunStop(t *testing.T) {
	r := newRig(t, testDefs())
	r.installImages()
	r.waitPhase(supervisor.PhaseStopped)

	if err := r.s.Stop(); err != ErrNotAllowed {
		t.Fatalf("Stop while stopped = %v, want ErrNotAllowed", err)
	}
	if err := r.s.Start(); err != nil {
		t.Fatal(err)
	}
	if err := r.s.Start(); err != ErrNotAllowed {
		t.Fatalf("second Start = %v, want ErrNotAllowed", err)
	}
	s := r.waitPhase(supervisor.PhaseRunning)
	for _, c := range s.Components {
		if c.State != "ready" || !c.Installed {
			t.Errorf("component %+v", c)
		}
	}
	if got := r.containers(); len(got) != 2 {
		t.Fatalf("containers = %v", got)
	}
	if got, _ := r.rt.ListNetworksByLabel(context.Background(), component.LabelManaged, "true"); len(got) != 1 || got[0] != "astrosim" {
		t.Errorf("labelled networks = %v, want the astrosim network", got)
	}
	spec, _ := r.rt.Spec("astrosim-a")
	if spec.Labels[component.LabelSession] != "test" || spec.Network != "astrosim" {
		t.Errorf("spec = %+v", spec)
	}
	r.waitFor("facts", func(s status.Snapshot) bool { return len(s.Components[0].Facts) == 1 })

	if err := r.s.Stop(); err != nil {
		t.Fatal(err)
	}
	r.waitPhase(supervisor.PhaseStopped)
	if got := r.containers(); len(got) != 0 {
		t.Fatalf("containers left after Stop: %v", got)
	}
}

func TestRecoversFromExit(t *testing.T) {
	r := newRig(t, testDefs())
	r.installImages()
	r.waitPhase(supervisor.PhaseStopped)
	_ = r.s.Start()
	r.waitPhase(supervisor.PhaseRunning)

	if !r.rt.Exit("astrosim-a", 1) {
		t.Fatal("container a was not running")
	}
	r.waitPhase(supervisor.PhaseRecovering)
	s := r.waitPhase(supervisor.PhaseRunning)
	if s.Attempt < 1 {
		t.Errorf("attempt = %d, want >= 1", s.Attempt)
	}
	runsOfA := 0
	for _, spec := range r.rt.Runs() {
		if spec.Name == "astrosim-a" {
			runsOfA++
		}
	}
	if runsOfA != 2 {
		t.Errorf("a was run %d times, want 2", runsOfA)
	}
}

// vanishingImage makes b's image disappear as b is launched, like an image
// deleted between preflight and launch.
type vanishingImage struct {
	*fake.Runtime
	gone atomic.Bool
}

func (v *vanishingImage) Run(ctx context.Context, spec runtime.RunSpec) (string, error) {
	if spec.Name == "astrosim-b" {
		v.gone.Store(true)
		return "", errors.New("no such image")
	}
	return v.Runtime.Run(ctx, spec)
}

func (v *vanishingImage) ImageExists(ctx context.Context, image string) (bool, error) {
	if v.gone.Load() && image == "img-b" {
		return false, nil
	}
	return v.Runtime.ImageExists(ctx, image)
}

func TestMissingImageAtLaunchIsPermanent(t *testing.T) {
	r := newRigWith(t, testDefs(), func(f *fake.Runtime) runtime.Runtime { return &vanishingImage{Runtime: f} })
	r.installImages()
	r.waitPhase(supervisor.PhaseStopped)
	_ = r.s.Start()
	s := r.waitPhase(supervisor.PhaseFailed)
	if s.Reason.Code != supervisor.ReasonLaunchFailed || !s.CanStart {
		t.Fatalf("snapshot %+v", s)
	}
	if got := r.containers(); len(got) != 0 {
		t.Fatalf("containers left: %v", got)
	}
}

func TestShutdownMakesRunReturn(t *testing.T) {
	r := newRig(t, testDefs())
	r.installImages()
	r.waitPhase(supervisor.PhaseStopped)
	_ = r.s.Start()
	r.waitPhase(supervisor.PhaseRunning)
	r.s.Shutdown()
	select {
	case err := <-r.end:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("Run did not return")
	}
	if got := r.containers(); len(got) != 0 {
		t.Fatalf("containers left: %v", got)
	}
	if err := r.s.Start(); err != ErrClosed {
		t.Fatalf("Start after Run = %v, want ErrClosed", err)
	}
}

func TestPortBusy(t *testing.T) {
	pc, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Skipf("cannot bind a UDP port: %v", err)
	}
	defer pc.Close()
	port := pc.LocalAddr().(*net.UDPAddr).Port
	r := newRig(t, testDefs(component.PortSpec{Name: "p", Proto: "udp", Port: port}))
	r.installImages()
	s := r.waitPhase(supervisor.PhaseUnavailable)
	if s.Reason.Code != supervisor.ProblemPortBusy {
		t.Fatalf("reason = %+v", s.Reason)
	}
	pc.Close()
	r.waitPhase(supervisor.PhaseStopped)
}

func TestCleanupStale(t *testing.T) {
	rt := fake.New()
	rt.SetImage("x", true)
	ctx := context.Background()
	_, _ = rt.Run(ctx, runtimeSpecLabelled("astrosim-old", true))
	_, _ = rt.Run(ctx, runtimeSpecLabelled("by-hand", false))
	removed, err := CleanupStale(ctx, rt)
	if err != nil {
		t.Fatal(err)
	}
	if len(removed) != 1 || removed[0] != "astrosim-old" {
		t.Errorf("removed = %v", removed)
	}
	if _, err := rt.Inspect(ctx, "astrosim-old"); err == nil {
		t.Error("managed container survived")
	}
	if _, err := rt.Inspect(ctx, "by-hand"); err != nil {
		t.Error("unmanaged container was removed")
	}
}

func TestPrune(t *testing.T) {
	ctx := context.Background()
	managed := map[string]string{component.LabelManaged: "true"}
	setup := func() *fake.Runtime {
		rt := fake.New()
		rt.SetImage("x", true)
		_, _ = rt.Run(ctx, runtimeSpecLabelled("astrosim-old", true))
		_, _ = rt.Run(ctx, runtimeSpecLabelled("by-hand", false))
		_ = rt.EnsureNetwork(ctx, "astrosim", managed)
		_ = rt.EnsureNetwork(ctx, "by-hand-net", nil)
		return rt
	}

	t.Run("removes only labelled resources", func(t *testing.T) {
		rt := setup()
		got, err := Prune(ctx, rt)
		if err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(got.Containers, []string{"astrosim-old"}) || !reflect.DeepEqual(got.Networks, []string{"astrosim"}) {
			t.Errorf("pruned = %+v", got)
		}
		if got.Empty() {
			t.Error("Empty() on a non-empty result")
		}
		if _, err := rt.Inspect(ctx, "by-hand"); err != nil {
			t.Error("unlabelled container was removed")
		}
		if n := rt.Networks(); !reflect.DeepEqual(n, []string{"by-hand-net"}) {
			t.Errorf("networks left = %v", n)
		}
	})
	t.Run("nothing to do", func(t *testing.T) {
		got, err := Prune(ctx, fake.New())
		if err != nil || !got.Empty() {
			t.Errorf("Prune on an empty runtime = %+v, %v", got, err)
		}
	})
	t.Run("second run is a no-op", func(t *testing.T) {
		rt := setup()
		_, _ = Prune(ctx, rt)
		if got, err := Prune(ctx, rt); err != nil || !got.Empty() {
			t.Errorf("second Prune = %+v, %v", got, err)
		}
	})
}
