package session

import (
	"context"
	"errors"
	"fmt"
	"io"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/component"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime/fake"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/status"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/supervisor"
)

const (
	refA = "ghcr.io/t/img-a:1"
	refB = "ghcr.io/t/img-b:1"
)

// pullDefs is testDefs with images that name a registry.
func pullDefs() []component.Definition {
	defs := testDefs()
	defs[0].Image, defs[1].Image = refA, refB
	return defs
}

// pullRig runs a session over pullDefs with downloads enabled and hook as
// the fake runtime's PullHook.
func pullRig(t *testing.T, hook func(ctx context.Context, image string, progress io.Writer) error, mod func(*Options)) *rig {
	t.Helper()
	return newRigOpts(t, pullDefs(), func(f *fake.Runtime) runtime.Runtime { f.PullHook = hook; return f },
		func(o *Options) {
			o.PullMissing = true
			if mod != nil {
				mod(o)
			}
		})
}

// gate lets a test hold downloads open and release them per image.
type gate struct {
	open map[string]chan struct{}
}

func newGate(images ...string) *gate {
	g := &gate{open: map[string]chan struct{}{}}
	for _, i := range images {
		g.open[i] = make(chan struct{})
	}
	return g
}

func (g *gate) release(image string) { close(g.open[image]) }

// hook writes a progress line, then waits for the image's gate or ctx.
func (g *gate) hook(ctx context.Context, image string, progress io.Writer) error {
	fmt.Fprintf(progress, "Copying blob %s\r", image)
	select {
	case <-g.open[image]:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func component0(s status.Snapshot, id string) status.Component {
	for _, c := range s.Components {
		if c.ID == id {
			return c
		}
	}
	return status.Component{}
}

func TestPullsMissingImagesAtStartup(t *testing.T) {
	g := newGate(refA, refB)
	t.Cleanup(func() { // free a hook that is still waiting if the test fails early
		for _, ch := range g.open {
			select {
			case <-ch:
			default:
				close(ch)
			}
		}
	})
	r := pullRig(t, g.hook, nil)
	s := r.waitFor("download progress", func(s status.Snapshot) bool {
		a := component0(s, "a")
		return s.Phase == "downloading" && a.Download != nil && strings.HasPrefix(a.Download.Output, "Copying blob")
	})
	if s.Reason.Code != "" || s.CanStart || !s.CanStop {
		t.Errorf("downloading: reason %+v canStart %v canStop %v", s.Reason, s.CanStart, s.CanStop)
	}
	a := component0(s, "a")
	if a.Installed || a.Download.State != "downloading" || a.Download.Output != "Copying blob "+refA {
		t.Errorf("a = %+v download %+v", a, a.Download)
	}
	if a.PullCommand != "podman pull "+refA {
		t.Errorf("PullCommand = %q", a.PullCommand)
	}

	// Once an image has arrived it is installed and says "done" while the
	// other one is still going, with no flicker back to "not installed".
	g.release(refA)
	s = r.waitFor("a done", func(s status.Snapshot) bool {
		a := component0(s, "a")
		return a.Download != nil && a.Download.State == "done"
	})
	if !component0(s, "a").Installed || component0(s, "b").Installed || component0(s, "b").Download.State != "downloading" {
		t.Errorf("snapshot while b downloads: %+v", s.Components)
	}

	g.release(refB)
	s = r.waitPhase(supervisor.PhaseStopped)
	for _, c := range s.Components {
		if !c.Installed || c.Download != nil {
			t.Errorf("after the download: %+v", c)
		}
	}
	if s.Reason.Code != "" {
		t.Errorf("reason = %+v", s.Reason)
	}
	if got := r.rt.Pulls(); len(got) != 2 {
		t.Errorf("pulls = %v", got)
	}
	if got := r.containers(); len(got) != 0 {
		t.Errorf("a check started containers: %v", got)
	}
}

func TestStartDownloadsThenRuns(t *testing.T) {
	r := pullRig(t, nil, nil)
	r.waitPhase(supervisor.PhaseStopped) // downloaded at startup
	r.rt.SetImage(refA, false)
	r.rt.SetImage(refB, false)
	if err := r.s.Start(); err != nil {
		t.Fatal(err)
	}
	r.waitPhase(supervisor.PhaseRunning)
	if got := r.rt.Pulls(); len(got) != 4 {
		t.Errorf("pulls = %v, want both images twice", got)
	}
}

func TestOnlyMissingImagesArePulled(t *testing.T) {
	r := newRigOpts(t, pullDefs(), func(f *fake.Runtime) runtime.Runtime { f.SetImage(refA, true); return f },
		func(o *Options) { o.PullMissing = true })
	r.waitPhase(supervisor.PhaseStopped)
	if got := r.rt.Pulls(); len(got) != 1 || got[0] != refB {
		t.Errorf("pulls = %v, want only %s", got, refB)
	}
}

func TestSharedImageIsPulledOnce(t *testing.T) {
	defs := pullDefs()
	defs[1].Image = refA
	r := newRigOpts(t, defs, func(f *fake.Runtime) runtime.Runtime { return f }, func(o *Options) { o.PullMissing = true })
	r.waitPhase(supervisor.PhaseStopped)
	if got := r.rt.Pulls(); len(got) != 1 {
		t.Errorf("pulls = %v", got)
	}
}

func TestDevImagesNeverPulled(t *testing.T) {
	r := newRigOpts(t, testDefs(), func(f *fake.Runtime) runtime.Runtime { return f }, func(o *Options) { o.PullMissing = true })
	s := r.waitPhase(supervisor.PhaseUnavailable)
	if s.Reason.Code != supervisor.ProblemImagesMissing {
		t.Fatalf("reason = %+v", s.Reason)
	}
	if got := r.rt.Pulls(); len(got) != 0 {
		t.Errorf("pulls = %v", got)
	}
	for _, c := range s.Components {
		if c.PullCommand != "" || c.BuildCommand != "podman build "+c.ID || c.Download != nil {
			t.Errorf("component %+v", c)
		}
	}
}

func TestPullNever(t *testing.T) {
	r := newRigOpts(t, pullDefs(), func(f *fake.Runtime) runtime.Runtime { return f }, nil)
	s := r.waitPhase(supervisor.PhaseUnavailable)
	if s.Reason.Code != supervisor.ProblemImagesMissing {
		t.Fatalf("reason = %+v", s.Reason)
	}
	if got := r.rt.Pulls(); len(got) != 0 {
		t.Errorf("pulls = %v", got)
	}
	if c := component0(s, "a"); c.Installed || c.PullCommand != "podman pull "+refA {
		t.Errorf("component %+v", c)
	}
}

func TestPullProblemCodes(t *testing.T) {
	other := errors.New("boom")
	for _, tc := range []struct {
		err  error
		want string
	}{
		{runtime.ErrNoSpace, supervisor.ProblemPullNoSpace},
		{runtime.ErrDenied, supervisor.ProblemPullDenied},
		{runtime.ErrNotFound, supervisor.ProblemPullNotFound},
		{runtime.ErrUnreachable, supervisor.ProblemPullNetwork},
		{fmt.Errorf("wrapped: %w", runtime.ErrNoSpace), supervisor.ProblemPullNoSpace},
		{other, supervisor.ProblemPullFailed},
	} {
		if got := pullProblem(tc.err); got != tc.want {
			t.Errorf("pullProblem(%v) = %s, want %s", tc.err, got, tc.want)
		}
	}
}

func TestPullFailureThenRetry(t *testing.T) {
	var fail atomic.Bool
	fail.Store(true)
	r := pullRig(t, func(_ context.Context, image string, _ io.Writer) error {
		if fail.Load() {
			return fmt.Errorf("%s: %w", image, runtime.ErrNotFound)
		}
		return nil
	}, nil)
	s := r.waitPhase(supervisor.PhaseUnavailable)
	if s.Reason.Code != supervisor.ProblemPullNotFound || !strings.Contains(s.Reason.Detail, refA) || !strings.Contains(s.Reason.Detail, refB) {
		t.Fatalf("reason = %+v", s.Reason)
	}
	for _, c := range s.Components {
		if c.Download != nil {
			t.Errorf("download in unavailable: %+v", c)
		}
	}
	// The hold keeps the 5 s recheck from downloading again, then the
	// backoff timer (15 s, run 20 times faster) ends it.
	fail.Store(false)
	r.waitPhase(supervisor.PhaseStopped)
	if got := len(r.rt.Pulls()); got != 4 {
		t.Errorf("pulls = %d, want 2 failed + 2 successful", got)
	}
}

func TestMostActionableFailureWins(t *testing.T) {
	r := pullRig(t, func(_ context.Context, image string, _ io.Writer) error {
		if image == refA {
			return runtime.ErrUnreachable
		}
		return runtime.ErrNoSpace
	}, nil)
	s := r.waitPhase(supervisor.PhaseUnavailable)
	if s.Reason.Code != supervisor.ProblemPullNoSpace {
		t.Errorf("reason = %+v", s.Reason)
	}
}

func TestStopCancelsPull(t *testing.T) {
	var calls atomic.Int32
	cancelled := make(chan error, 2)
	r := pullRig(t, func(ctx context.Context, image string, _ io.Writer) error {
		if calls.Add(1) > 2 {
			return nil // the second download, after Start
		}
		<-ctx.Done()
		cancelled <- ctx.Err()
		return ctx.Err()
	}, nil)
	r.waitPhase(supervisor.PhaseDownloading)
	if err := r.s.Stop(); err != nil {
		t.Fatal(err)
	}
	s := r.waitPhase(supervisor.PhaseStopped)
	if s.Reason.Code != supervisor.ReasonPullCancelled || !s.CanStart {
		t.Errorf("after Stop: %+v", s)
	}
	for i := 0; i < 2; i++ {
		select {
		case err := <-cancelled:
			if !errors.Is(err, context.Canceled) {
				t.Errorf("hook saw %v", err)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("the download was not cancelled")
		}
	}
	// The cancelled downloads leave no trace in the status.
	time.Sleep(50 * time.Millisecond)
	if s := r.s.Snapshot(); s.Phase != "stopped" || s.Reason.Code != supervisor.ReasonPullCancelled {
		t.Errorf("late result changed the state: %+v", s)
	}
	// Start downloads again and runs.
	if err := r.s.Start(); err != nil {
		t.Fatal(err)
	}
	r.waitPhase(supervisor.PhaseRunning)
}

func TestShutdownCancelsPull(t *testing.T) {
	started := make(chan struct{}, 2)
	r := pullRig(t, func(ctx context.Context, _ string, _ io.Writer) error {
		started <- struct{}{}
		<-ctx.Done()
		return ctx.Err()
	}, nil)
	<-started
	r.s.Shutdown()
	select {
	case err := <-r.end:
		if err != nil {
			t.Errorf("Run = %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("Run did not return while downloading")
	}
}

func TestDownloadElapsedTimeAdvances(t *testing.T) {
	var offset atomic.Int64
	g := newGate(refA, refB)
	t.Cleanup(func() {
		for _, ch := range g.open {
			select {
			case <-ch:
			default:
				close(ch)
			}
		}
	})
	r := pullRig(t, g.hook, func(o *Options) {
		o.Clock = func() time.Time { return time.Now().Add(time.Duration(offset.Load())) }
	})
	r.waitPhase(supervisor.PhaseDownloading)
	offset.Store(int64(75 * time.Second))
	// No event happens; the periodic republish must show the time passing.
	s := r.waitFor("elapsed time", func(s status.Snapshot) bool {
		a := component0(s, "a")
		return a.Download != nil && a.Download.ElapsedSeconds >= 75
	})
	if component0(s, "a").Download.Since.IsZero() {
		t.Error("Since is zero")
	}
}

func TestPullIsLogged(t *testing.T) {
	fl := &fakeLogs{}
	var fail atomic.Bool
	r := newRigOpts(t, pullDefs(), func(f *fake.Runtime) runtime.Runtime {
		fl.rt = f
		f.PullHook = func(_ context.Context, image string, progress io.Writer) error {
			io.WriteString(progress, "Copying blob x\nWriting manifest\n")
			if fail.Load() {
				return runtime.ErrDenied
			}
			return nil
		}
		return f
	}, func(o *Options) { o.PullMissing, o.Logs = true, fl })
	r.waitPhase(supervisor.PhaseStopped)
	fl.mu.Lock()
	notes, lines := strings.Join(fl.notes, "\n"), strings.Join(fl.lines, "\n")
	fl.mu.Unlock()
	for _, want := range []string{"a: pull " + refA, "a: pull done in ", "b: pull " + refB, "b: pull done in "} {
		if !strings.Contains(notes, want) {
			t.Errorf("notes lack %q:\n%s", want, notes)
		}
	}
	for _, want := range []string{"a: Copying blob x", "a: Writing manifest", "b: Copying blob x"} {
		if !strings.Contains(lines, want) {
			t.Errorf("lines lack %q:\n%s", want, lines)
		}
	}
	if strings.Contains(lines, "b: pull") || strings.Contains(notes, "a: pull "+refB) {
		t.Error("a component's log got another image's output")
	}

	// A failure and a cancellation are marked too.
	fail.Store(true)
	r.rt.SetImage(refA, false)
	_ = r.s.Start()
	r.waitPhase(supervisor.PhaseUnavailable)
	fl.mu.Lock()
	defer fl.mu.Unlock()
	if !strings.Contains(strings.Join(fl.notes, "\n"), "a: pull failed: "+runtime.ErrDenied.Error()) {
		t.Errorf("no failure marker:\n%s", strings.Join(fl.notes, "\n"))
	}
}

func TestCancelIsLogged(t *testing.T) {
	fl := &fakeLogs{}
	r := pullRig(t, func(ctx context.Context, _ string, _ io.Writer) error { <-ctx.Done(); return ctx.Err() },
		func(o *Options) { o.Logs = fl })
	r.waitPhase(supervisor.PhaseDownloading)
	_ = r.s.Stop()
	r.waitPhase(supervisor.PhaseStopped)
	r.waitFor("cancel marker", func(status.Snapshot) bool {
		fl.mu.Lock()
		defer fl.mu.Unlock()
		return strings.Contains(strings.Join(fl.notes, "\n"), "a: pull cancelled")
	})
}

func TestLineWriter(t *testing.T) {
	var got []string
	w := newLineWriter(func(l string) { got = append(got, l) })
	io.WriteString(w, "one\ntw")
	io.WriteString(w, "o\r\r  three \x1b[0m\t\n\n")
	io.WriteString(w, "last")
	w.Close()
	w.Close()
	want := []string{"one", "two", "three [0m", "last"}
	if strings.Join(got, "|") != strings.Join(want, "|") {
		t.Errorf("lines = %q, want %q", got, want)
	}
	// A line that never ends is cut instead of growing without bound.
	got = nil
	io.WriteString(w, strings.Repeat("x", 3*maxLineBytes))
	w.Close()
	if len(got) != 3 || len(got[0]) != maxLineBytes {
		t.Errorf("long line: %d pieces", len(got))
	}
	// Concurrent writers do not race.
	var wg sync.WaitGroup
	for range 4 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for range 100 {
				io.WriteString(w, "line\n")
			}
		}()
	}
	wg.Wait()
}

func TestTruncate(t *testing.T) {
	if got := truncate("héllo", 3); got != "hél" {
		t.Errorf("got %q", got)
	}
	if got := truncate("ab", 3); got != "ab" {
		t.Errorf("got %q", got)
	}
}
