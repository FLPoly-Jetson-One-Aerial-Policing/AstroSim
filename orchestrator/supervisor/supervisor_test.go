package supervisor

import (
	"testing"
	"time"
)

var t0 = time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC)

func kinds(as []Action) []ActionKind {
	var k []ActionKind
	for _, a := range as {
		k = append(k, a.Kind)
	}
	return k
}

func only(t *testing.T, as []Action, kind ActionKind) Action {
	t.Helper()
	if len(as) != 1 || as[0].Kind != kind {
		t.Fatalf("want exactly one action of kind %v, got %+v", kind, as)
	}
	return as[0]
}

func started(t *testing.T, cfg Config) *Supervisor {
	s := New(cfg)
	only(t, s.Step(Event{Kind: Start}, t0), StartContainer)
	as := s.Step(Event{Kind: ContainerReady}, t0)
	if cfg.NoDisplay {
		if len(as) != 0 {
			t.Fatalf("no-display must not start viewer: %+v", as)
		}
	} else {
		only(t, as, StartViewer)
	}
	return s
}

func TestStartThenViewerOnReady(t *testing.T) { started(t, DefaultConfig()) }

func TestNoDisplayNeverStartsViewer(t *testing.T) {
	cfg := DefaultConfig()
	cfg.NoDisplay = true
	s := started(t, cfg)
	if as := s.Step(Event{Kind: ContainerReady}, t0); len(as) != 0 {
		t.Fatalf("got %+v", as)
	}
}

func TestWindowCloseShutsDownCleanly(t *testing.T) {
	s := started(t, DefaultConfig())
	a := only(t, s.Step(Event{Kind: ViewerExited, Code: 0}, t0), Shutdown)
	if a.Code != 0 {
		t.Fatalf("code %d", a.Code)
	}
}

func TestViewerCrashRestartsWithBackoffThenGivesUp(t *testing.T) {
	s := started(t, DefaultConfig())
	a := only(t, s.Step(Event{Kind: ViewerExited, Code: -1}, t0), StartViewer)
	if a.Delay != 0 {
		t.Fatalf("first restart must be immediate, got %v", a.Delay)
	}
	a = only(t, s.Step(Event{Kind: ViewerExited, Code: -1}, t0.Add(time.Second)), StartViewer)
	if a.Delay != 500*time.Millisecond {
		t.Fatalf("second restart delay %v", a.Delay)
	}
	a = only(t, s.Step(Event{Kind: ViewerExited, Code: 139}, t0.Add(2*time.Second)), Shutdown)
	if a.Code != 1 {
		t.Fatalf("crash loop must exit nonzero, got %d", a.Code)
	}
}

func TestViewerCrashCountExpires(t *testing.T) {
	s := started(t, DefaultConfig())
	for i := 0; i < 10; i++ {
		a := only(t, s.Step(Event{Kind: ViewerExited, Code: 1}, t0.Add(time.Duration(i)*time.Minute)), StartViewer)
		if a.Delay != 0 {
			t.Fatalf("spaced crashes must restart immediately, iteration %d delay %v", i, a.Delay)
		}
	}
}

func TestContainerDeathRestartsContainerNotViewer(t *testing.T) {
	s := started(t, DefaultConfig())
	a := only(t, s.Step(Event{Kind: ContainerExited, Code: 137}, t0), StartContainer)
	if a.Delay != 0 {
		t.Fatalf("delay %v", a.Delay)
	}
	if as := s.Step(Event{Kind: ContainerReady}, t0.Add(10*time.Second)); len(as) != 0 {
		t.Fatalf("viewer must keep running across engine restart, got %+v", as)
	}
}

func TestContainerCrashLoopGivesUp(t *testing.T) {
	s := started(t, DefaultConfig())
	only(t, s.Step(Event{Kind: ContainerExited}, t0), StartContainer)
	a := only(t, s.Step(Event{Kind: ContainerExited}, t0.Add(time.Second)), StartContainer)
	if a.Delay != 500*time.Millisecond {
		t.Fatalf("delay %v", a.Delay)
	}
	a = only(t, s.Step(Event{Kind: ContainerExited}, t0.Add(2*time.Second)), Shutdown)
	if a.Code != 1 {
		t.Fatalf("code %d", a.Code)
	}
}

func TestContainerDiesDuringStartup(t *testing.T) {
	s := New(DefaultConfig())
	s.Step(Event{Kind: Start}, t0)
	only(t, s.Step(Event{Kind: ContainerExited, Code: 125}, t0), StartContainer)
}

func TestReadyTimeoutShutsDown(t *testing.T) {
	s := New(DefaultConfig())
	s.Step(Event{Kind: Start}, t0)
	a := only(t, s.Step(Event{Kind: ReadyTimeout}, t0), Shutdown)
	if a.Code != 1 {
		t.Fatalf("code %d", a.Code)
	}
}

func TestStaleReadyTimeoutIgnoredOnceRunning(t *testing.T) {
	s := started(t, DefaultConfig())
	if as := s.Step(Event{Kind: ReadyTimeout}, t0); len(as) != 0 {
		t.Fatalf("got %+v", as)
	}
}

func TestSignalDuringEveryPhase(t *testing.T) {
	// during startup
	s := New(DefaultConfig())
	s.Step(Event{Kind: Start}, t0)
	only(t, s.Step(Event{Kind: Signal}, t0), Shutdown)
	// while running
	s = started(t, DefaultConfig())
	only(t, s.Step(Event{Kind: Signal}, t0), Shutdown)
	// while engine restarting
	s = started(t, DefaultConfig())
	s.Step(Event{Kind: ContainerExited}, t0)
	only(t, s.Step(Event{Kind: Signal}, t0), Shutdown)
}

func TestEventsAfterShutdownIgnored(t *testing.T) {
	s := started(t, DefaultConfig())
	only(t, s.Step(Event{Kind: Signal}, t0), Shutdown)
	for _, k := range []EventKind{ViewerExited, ContainerExited, ContainerReady, Signal, ReadyTimeout} {
		if as := s.Step(Event{Kind: k, Code: 1}, t0); len(as) != 0 {
			t.Fatalf("kind %v produced %v", k, kinds(as))
		}
	}
}

func TestViewerCrashWhileEngineRestarting(t *testing.T) {
	s := started(t, DefaultConfig())
	s.Step(Event{Kind: ContainerExited}, t0)
	only(t, s.Step(Event{Kind: ViewerExited, Code: -1}, t0), StartViewer)
}

func TestDoubleStartIgnored(t *testing.T) {
	s := New(DefaultConfig())
	only(t, s.Step(Event{Kind: Start}, t0), StartContainer)
	if as := s.Step(Event{Kind: Start}, t0); len(as) != 0 {
		t.Fatalf("got %+v", as)
	}
}
