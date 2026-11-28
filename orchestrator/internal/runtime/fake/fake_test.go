package fake

import (
	"context"
	"errors"
	"io"
	"reflect"
	"strings"
	"sync"
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
	_ = r.EnsureNetwork(ctx, "astrosim", map[string]string{"astrosim.managed": "true"})
	if n := r.Networks(); len(n) != 1 || n[0] != "astrosim" {
		t.Errorf("networks = %v", n)
	}
	if n, _ := r.ListNetworksByLabel(ctx, "astrosim.managed", "true"); len(n) != 1 {
		t.Errorf("labelled networks = %v", n)
	}
	if n, _ := r.ListNetworksByLabel(ctx, "astrosim.managed", "false"); len(n) != 0 {
		t.Errorf("networks with other label value = %v", n)
	}
	if err := r.RemoveNetwork(ctx, "astrosim"); err != nil || len(r.Networks()) != 0 {
		t.Errorf("RemoveNetwork: err=%v networks=%v", err, r.Networks())
	}
	if err := r.RemoveNetwork(ctx, "astrosim"); err != nil {
		t.Errorf("second RemoveNetwork = %v, want nil", err)
	}
	if s, ok := r.Spec("a"); !ok || s.Image != "img" {
		t.Errorf("spec = %+v", s)
	}
}

// collect is a goroutine-safe io.Writer.
type collect struct {
	mu sync.Mutex
	b  strings.Builder
}

func (c *collect) Write(p []byte) (int, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.b.Write(p)
}

func (c *collect) String() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.b.String()
}

func started(t *testing.T) (*Runtime, *time.Time) {
	t.Helper()
	r := New()
	now := time.Date(2026, 11, 22, 19, 0, 0, 0, time.UTC)
	r.Now = func() time.Time { return now }
	r.SetImage("img", true)
	if _, err := r.Run(context.Background(), spec("a")); err != nil {
		t.Fatal(err)
	}
	return r, &now
}

func TestFollowLogsReplayAndStreams(t *testing.T) {
	r, _ := started(t)
	r.WriteLog("a", Stdout, "out one\n")
	r.WriteLog("a", Stderr, "err one\n")
	r.WriteLog("a", Stdout, "par")
	r.WriteLog("a", Stdout, "tial\nsecond\n")
	r.Exit("a", 0)

	var out, errb collect
	if err := r.FollowLogs(context.Background(), "a", runtime.LogOptions{Follow: true}, &out, &errb); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), " out one\n") || !strings.Contains(out.String(), "partial\n") || !strings.Contains(out.String(), " second\n") {
		t.Errorf("stdout = %q", out.String())
	}
	if strings.Count(out.String(), "Z ") != 3 {
		t.Errorf("partial line stamped more than once: %q", out.String())
	}
	if !strings.HasSuffix(errb.String(), " err one\n") || strings.Contains(errb.String(), "out") {
		t.Errorf("stderr = %q", errb.String())
	}
	if logs, _ := r.Logs(context.Background(), "a", 5); logs != "out one\nerr one\npartial\nsecond\n" {
		t.Errorf("Logs = %q", logs)
	}
}

func TestFollowLogsSinceIsExclusive(t *testing.T) {
	r, now := started(t)
	r.WriteLog("a", Stdout, "one\n")
	*now = now.Add(time.Second)
	r.WriteLog("a", Stdout, "two\n")
	cut := now.Add(-500 * time.Millisecond)
	var out collect
	if err := r.FollowLogs(context.Background(), "a", runtime.LogOptions{Since: cut}, &out, io.Discard); err != nil {
		t.Fatal(err)
	}
	if got := out.String(); !strings.HasSuffix(got, " two\n") || strings.Contains(got, "one") {
		t.Errorf("got %q", got)
	}
	out = collect{}
	last := *now
	if err := r.FollowLogs(context.Background(), "a", runtime.LogOptions{Since: last}, &out, io.Discard); err != nil || out.String() != "" {
		t.Errorf("since == last line should be empty: %q %v", out.String(), err)
	}
}

func TestFollowLogsLiveAndExit(t *testing.T) {
	r, _ := started(t)
	var out collect
	done := make(chan error, 1)
	go func() {
		done <- r.FollowLogs(context.Background(), "a", runtime.LogOptions{Follow: true}, &out, io.Discard)
	}()
	r.WriteLog("a", Stdout, "live\n")
	select {
	case err := <-done:
		t.Fatalf("follow ended early: %v", err)
	case <-time.After(50 * time.Millisecond):
	}
	r.Exit("a", 1)
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("follow did not end after exit")
	}
	if !strings.Contains(out.String(), "live") {
		t.Errorf("out = %q", out.String())
	}
}

func TestFollowLogsHangErrAndMissing(t *testing.T) {
	r, _ := started(t)
	r.FollowHang = map[string]bool{"a": true}
	r.WriteLog("a", Stdout, "x\n")
	r.Exit("a", 0)
	ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
	defer cancel()
	var out collect
	err := r.FollowLogs(ctx, "a", runtime.LogOptions{Follow: true}, &out, io.Discard)
	if !errors.Is(err, context.DeadlineExceeded) || !strings.Contains(out.String(), "x") {
		t.Fatalf("hang: err=%v out=%q", err, out.String())
	}
	// A non-follow read still returns despite the hang knob.
	if err := r.FollowLogs(context.Background(), "a", runtime.LogOptions{}, io.Discard, io.Discard); err != nil {
		t.Fatalf("non-follow: %v", err)
	}

	boom := errors.New("boom")
	r.FollowErr = func(string) error { return boom }
	if err := r.FollowLogs(context.Background(), "a", runtime.LogOptions{}, io.Discard, io.Discard); !errors.Is(err, boom) {
		t.Fatalf("FollowErr: %v", err)
	}
	r.FollowErr = nil
	if err := r.FollowLogs(context.Background(), "nope", runtime.LogOptions{}, io.Discard, io.Discard); !errors.Is(err, runtime.ErrNotFound) {
		t.Fatalf("missing: %v", err)
	}
}

func TestSetLogsAndOps(t *testing.T) {
	r, _ := started(t)
	r.WriteLog("a", Stderr, "old\n")
	r.SetLogs("a", "fresh\n")
	if got, _ := r.Logs(context.Background(), "a", 1); got != "fresh\n" {
		t.Errorf("Logs = %q", got)
	}
	ctx := context.Background()
	r.Stop(ctx, "a", time.Second)
	r.Remove(ctx, "a")
	want := []string{"run a", "stop a", "remove a"}
	if got := r.Ops(); !reflect.DeepEqual(got, want) {
		t.Errorf("Ops = %q, want %q", got, want)
	}
}

func TestPull(t *testing.T) {
	ctx := context.Background()
	r := New()
	var out collect
	if err := r.Pull(ctx, "ghcr.io/o/x:1", &out); err != nil {
		t.Fatal(err)
	}
	if ok, _ := r.ImageExists(ctx, "ghcr.io/o/x:1"); !ok {
		t.Error("image not present after Pull")
	}
	if out.String() != "pulled ghcr.io/o/x:1\n" {
		t.Errorf("progress = %q", out.String())
	}
	if err := r.Pull(ctx, "ghcr.io/o/y:1", nil); err != nil {
		t.Fatal(err)
	}
	if got := r.Pulls(); !reflect.DeepEqual(got, []string{"ghcr.io/o/x:1", "ghcr.io/o/y:1"}) {
		t.Errorf("Pulls = %v", got)
	}
	if got := r.Ops(); !reflect.DeepEqual(got, []string{"pull ghcr.io/o/x:1", "pull ghcr.io/o/y:1"}) {
		t.Errorf("Ops = %v", got)
	}
}

func TestPullHook(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	r := New()
	boom := errors.New("registry down")
	r.PullHook = func(ctx context.Context, image string, progress io.Writer) error {
		io.WriteString(progress, "Copying blob x\n")
		if image == "bad" {
			return boom
		}
		<-ctx.Done()
		return ctx.Err()
	}
	var out collect
	if err := r.Pull(ctx, "bad", &out); !errors.Is(err, boom) {
		t.Fatalf("Pull = %v", err)
	}
	if ok, _ := r.ImageExists(ctx, "bad"); ok {
		t.Error("failed pull marked the image present")
	}
	cancel()
	if err := r.Pull(ctx, "slow", &out); !errors.Is(err, context.Canceled) {
		t.Fatalf("Pull = %v", err)
	}
	if ok, _ := r.ImageExists(context.Background(), "slow"); ok {
		t.Error("cancelled pull marked the image present")
	}
	if got := r.Pulls(); len(got) != 2 {
		t.Errorf("Pulls = %v", got)
	}
}
