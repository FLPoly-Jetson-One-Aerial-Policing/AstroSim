package session

import (
	"context"
	"reflect"
	"strings"
	"sync"
	"testing"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime/fake"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/status"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/supervisor"
)

// fakeLogs is a LogCapture that records what the session asks of it.
type fakeLogs struct {
	rt   *fake.Runtime
	tail []string // returned by every capture's Tail

	mu     sync.Mutex
	begun  []string
	notes  []string
	lines  []string
	drains map[string]int // component -> length of the runtime journal at Drain
}

func (f *fakeLogs) Begin(component string, gen uint64, container, containerID, image string) Capture {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.begun = append(f.begun, component+" "+container+" "+image)
	return &fakeCapture{f: f, container: container}
}

func (f *fakeLogs) Note(component, msg string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.notes = append(f.notes, component+": "+msg)
}

func (f *fakeLogs) Line(component, line string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.lines = append(f.lines, component+": "+line)
}

type fakeCapture struct {
	f         *fakeLogs
	container string
}

func (c *fakeCapture) Tail(int) []string { return c.f.tail }

func (c *fakeCapture) Drain(context.Context) {
	ops := c.f.rt.Ops()
	c.f.mu.Lock()
	defer c.f.mu.Unlock()
	if c.f.drains == nil {
		c.f.drains = map[string]int{}
	}
	c.f.drains[c.container] = len(ops)
}

// logRig runs a session with fl as its log capture and brings it to running.
func logRig(t *testing.T, fl *fakeLogs) *rig {
	t.Helper()
	r := newRigOpts(t, testDefs(), func(f *fake.Runtime) runtime.Runtime { fl.rt = f; return f },
		func(o *Options) { o.Logs = fl; o.LogDir = "/logs/dir" })
	r.installImages()
	r.waitPhase(supervisor.PhaseStopped)
	if err := r.s.Start(); err != nil {
		t.Fatal(err)
	}
	r.waitPhase(supervisor.PhaseRunning)
	return r
}

func TestTeardownDrainsBetweenStopAndRemove(t *testing.T) {
	fl := &fakeLogs{tail: []string{"one", "two"}}
	r := logRig(t, fl)
	if got := r.s.Snapshot().LogDir; got != "/logs/dir" {
		t.Errorf("LogDir = %q", got)
	}
	if err := r.s.Stop(); err != nil {
		t.Fatal(err)
	}
	r.waitPhase(supervisor.PhaseStopped)

	fl.mu.Lock()
	defer fl.mu.Unlock()
	if len(fl.begun) != 2 {
		t.Fatalf("Begin calls = %v", fl.begun)
	}
	ops := r.rt.Ops()
	for _, c := range []string{"astrosim-a", "astrosim-b"} {
		n, ok := fl.drains[c]
		if !ok {
			t.Fatalf("%s was never drained", c)
		}
		// stop < drain < remove: the journal was cut right after the stop
		// and the remove follows it directly.
		if n < 1 || ops[n-1] != "stop "+c || n >= len(ops) || ops[n] != "remove "+c {
			t.Errorf("%s: drain at journal position %d; journal %v", c, n, ops)
		}
	}
}

func TestLastLogFromCaptureTail(t *testing.T) {
	fl := &fakeLogs{tail: []string{"from", "capture"}}
	r := logRig(t, fl)
	r.rt.SetLogs("astrosim-a", "from runtime\n")
	r.rt.Exit("astrosim-a", 1)
	s := r.waitFor("lastLog", func(s status.Snapshot) bool { return len(s.Components[0].LastLog) > 0 })
	if got := s.Components[0].LastLog; !reflect.DeepEqual(got, fl.tail) {
		t.Errorf("LastLog = %q, want %q", got, fl.tail)
	}
	fl.mu.Lock()
	defer fl.mu.Unlock()
	found := false
	for _, n := range fl.notes {
		found = found || n == "a: exited code=1"
	}
	if !found {
		t.Errorf("no exit marker in %q", fl.notes)
	}
}

func TestLastLogFallsBackToRuntime(t *testing.T) {
	fl := &fakeLogs{} // empty tail
	r := logRig(t, fl)
	r.rt.SetLogs("astrosim-a", "fallback line\n")
	r.rt.Exit("astrosim-a", 1)
	s := r.waitFor("lastLog", func(s status.Snapshot) bool { return len(s.Components[0].LastLog) > 0 })
	if got := strings.Join(s.Components[0].LastLog, "|"); !strings.Contains(got, "fallback line") {
		t.Errorf("LastLog = %q", got)
	}
}

func TestNoLogsConfigured(t *testing.T) {
	r := newRig(t, testDefs())
	r.installImages()
	r.waitPhase(supervisor.PhaseStopped)
	_ = r.s.Start()
	r.waitPhase(supervisor.PhaseRunning)
	r.rt.SetLogs("astrosim-a", "plain\n")
	r.rt.Exit("astrosim-a", 1)
	s := r.waitFor("lastLog", func(s status.Snapshot) bool { return len(s.Components[0].LastLog) > 0 })
	if s.LogDir != "" {
		t.Errorf("LogDir = %q, want empty", s.LogDir)
	}
}

func TestLaunchFailureIsNoted(t *testing.T) {
	fl := &fakeLogs{}
	r := newRigOpts(t, testDefs(), func(f *fake.Runtime) runtime.Runtime { fl.rt = f; return &vanishingImage{Runtime: f} },
		func(o *Options) { o.Logs = fl })
	r.installImages()
	r.waitPhase(supervisor.PhaseStopped)
	_ = r.s.Start()
	r.waitPhase(supervisor.PhaseFailed)
	fl.mu.Lock()
	defer fl.mu.Unlock()
	for _, n := range fl.notes {
		if strings.HasPrefix(n, "b: launch failed") {
			return
		}
	}
	t.Errorf("no launch failure marker in %q", fl.notes)
}
