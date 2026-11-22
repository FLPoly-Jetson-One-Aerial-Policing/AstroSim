package fake

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
)

func spec(name string) runtime.RunSpec {
	return runtime.RunSpec{Name: name, Image: "img", Labels: map[string]string{"astrosim.managed": "true"}}
}

func TestLifecycle(t *testing.T) {
	ctx := context.Background()
	r := New()
	if ok, _ := r.ImageExists(ctx, "img"); ok {
		t.Fatal("image should start absent")
	}
	if _, err := r.Run(ctx, spec("a")); !errors.Is(err, runtime.ErrNotFound) {
		t.Fatalf("run without image: %v", err)
	}
	r.SetImage("img", true)
	if _, err := r.Run(ctx, spec("a")); err != nil {
		t.Fatal(err)
	}
	if _, err := r.Run(ctx, spec("a")); err == nil {
		t.Fatal("duplicate name should fail")
	}
	st, _ := r.Inspect(ctx, "a")
	if !st.Running || st.Status != "running" {
		t.Errorf("state = %+v", st)
	}
	names, _ := r.ListByLabel(ctx, "astrosim.managed", "true")
	if len(names) != 1 || names[0] != "a" {
		t.Errorf("names = %v", names)
	}

	done := make(chan int, 1)
	go func() {
		code, _ := r.Wait(ctx, "a")
		done <- code
	}()
	select {
	case <-done:
		t.Fatal("Wait returned while running")
	case <-time.After(20 * time.Millisecond):
	}
	if !r.Kill("a") {
		t.Fatal("kill should report running")
	}
	if code := <-done; code != KillExitCode {
		t.Errorf("code = %d", code)
	}
	st, _ = r.Inspect(ctx, "a")
	if st.Running || st.ExitCode != KillExitCode || st.Status != "exited" {
		t.Errorf("state = %+v", st)
	}
	if r.Kill("a") {
		t.Error("second kill should report not running")
	}
	if err := r.Remove(ctx, "a"); err != nil {
		t.Fatal(err)
	}
	if _, err := r.Inspect(ctx, "a"); !errors.Is(err, runtime.ErrNotFound) {
		t.Errorf("inspect removed: %v", err)
	}
	if err := r.Remove(ctx, "a"); err != nil {
		t.Errorf("remove missing: %v", err)
	}
}

func TestStopReleasesWait(t *testing.T) {
	ctx := context.Background()
	r := New()
	r.SetImage("img", true)
	_, _ = r.Run(ctx, spec("a"))
	done := make(chan int, 1)
	go func() { c, _ := r.Wait(ctx, "a"); done <- c }()
	if err := r.Stop(ctx, "a", time.Second); err != nil {
		t.Fatal(err)
	}
	if c := <-done; c != StopExitCode {
		t.Errorf("code = %d", c)
	}
}

func TestRemoveRunningReleasesWait(t *testing.T) {
	ctx := context.Background()
	r := New()
	r.SetImage("img", true)
	_, _ = r.Run(ctx, spec("a"))
	done := make(chan int, 1)
	go func() { c, _ := r.Wait(ctx, "a"); done <- c }()
	time.Sleep(10 * time.Millisecond)
	_ = r.Remove(ctx, "a")
	if c := <-done; c != KillExitCode {
		t.Errorf("code = %d", c)
	}
}

func TestWaitContext(t *testing.T) {
	r := New()
	r.SetImage("img", true)
	_, _ = r.Run(context.Background(), spec("a"))
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Millisecond)
	defer cancel()
	if _, err := r.Wait(ctx, "a"); !errors.Is(err, context.DeadlineExceeded) {
		t.Errorf("err = %v", err)
	}
	if _, err := r.Wait(context.Background(), "nope"); !errors.Is(err, runtime.ErrNotFound) {
		t.Errorf("err = %v", err)
	}
}

func TestHooks(t *testing.T) {
	ctx := context.Background()
	r := New()
	r.SetImage("img", true)
	r.PingErr = runtime.ErrNotRunning
	if _, err := r.Ping(ctx); !errors.Is(err, runtime.ErrNotRunning) {
		t.Errorf("ping: %v", err)
	}
	boom := errors.New("port busy")
	r.RunErr = func(runtime.RunSpec) error { return boom }
	if _, err := r.Run(ctx, spec("a")); !errors.Is(err, boom) {
		t.Errorf("run: %v", err)
	}
	if len(r.Runs()) != 1 {
		t.Errorf("runs = %v", r.Runs())
	}
	r.RunErr = nil
	_, _ = r.Run(ctx, spec("a"))
	r.SetLogs("a", "hello")
	r.SetHealth("a", "healthy")
	if l, _ := r.Logs(ctx, "a", 5); l != "hello" {
		t.Errorf("logs = %q", l)
	}
	if st, _ := r.Inspect(ctx, "a"); st.Health != "healthy" {
		t.Errorf("health = %q", st.Health)
	}
	_ = r.EnsureNetwork(ctx, "astrosim")
	if n := r.Networks(); len(n) != 1 || n[0] != "astrosim" {
		t.Errorf("networks = %v", n)
	}
	if s, ok := r.Spec("a"); !ok || s.Image != "img" {
		t.Errorf("spec = %+v", s)
	}
}
