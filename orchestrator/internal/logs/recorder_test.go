package logs

import (
	"archive/zip"
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime/fake"
)

const testID = "0123456789abcdef"

var testStart = time.Date(2026, 11, 22, 18, 26, 58, 0, time.UTC)

func fastCapture(t *testing.T) {
	t.Helper()
	oldB, oldG, oldS, oldM := reconnectBackoff, gapFillTimeout, stopWait, maxReconnects
	reconnectBackoff, gapFillTimeout, stopWait = 10*time.Millisecond, time.Second, 500*time.Millisecond
	t.Cleanup(func() { reconnectBackoff, gapFillTimeout, stopWait, maxReconnects = oldB, oldG, oldS, oldM })
}

type recFixture struct {
	rt      *fake.Runtime
	rec     *Recorder
	console *syncBuf
}

func newFixture(t *testing.T, src func(*fake.Runtime) LogSource, verbose bool) *recFixture {
	t.Helper()
	fastCapture(t)
	rt := fake.New()
	rt.SetImage("img", true)
	f := &recFixture{rt: rt, console: &syncBuf{}}
	var s LogSource = rt
	if src != nil {
		s = src(rt)
	}
	rec, err := Open(Options{Root: t.TempDir(), SessionID: testID, Version: "v1.2.3", Start: testStart,
		Source: s, Console: f.console, Verbose: verbose})
	if err != nil {
		t.Fatal(err)
	}
	f.rec = rec
	t.Cleanup(func() { rec.Close(context.Background()) })
	return f
}

func (f *recFixture) launch(t *testing.T, name string) *Capture {
	t.Helper()
	id, err := f.rt.Run(context.Background(), runtime.RunSpec{Name: "astrosim-" + name, Image: "img"})
	if err != nil {
		t.Fatal(err)
	}
	return f.rec.Begin(Launch{Component: name, Container: "astrosim-" + name, ContainerID: id + "-long-long", Image: "img", Gen: 3})
}

func (f *recFixture) file(t *testing.T, name string) string {
	t.Helper()
	// A snapshot is a barrier that also flushes.
	_, release, err := f.rec.sink.snapshot(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	release()
	b, err := os.ReadFile(filepath.Join(f.rec.Dir(), name))
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for !cond() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(2 * time.Millisecond)
	}
}

func drain(c *Capture, d time.Duration) time.Duration {
	ctx, cancel := context.WithTimeout(context.Background(), d)
	defer cancel()
	start := time.Now()
	c.Drain(ctx)
	return time.Since(start)
}

func TestOpenNamesAndHeader(t *testing.T) {
	f := newFixture(t, nil, false)
	if f.rec.Name() != "20261122T182658Z-"+testID || filepath.Base(f.rec.Dir()) != f.rec.Name() {
		t.Errorf("name=%q dir=%q", f.rec.Name(), f.rec.Dir())
	}
	h := strings.SplitN(f.file(t, "combined.log"), "\n", 2)[0]
	if !strings.HasPrefix(h, "# AstroSim v1.2.3 session "+testID+", started 2026-11-22T18:26:58Z, times are local (UTC") {
		t.Errorf("header = %q", h)
	}
	if _, err := Open(Options{Root: t.TempDir(), SessionID: "short"}); err == nil {
		t.Error("invalid session ID accepted")
	}
}

func TestNilRecorderIsInert(t *testing.T) {
	var r *Recorder
	r.Note("fc", "x")
	if r.Dir() != "" || r.Name() != "" || r.OrchestratorWriter() != nil || r.Close(context.Background()) != nil {
		t.Error("nil recorder not inert")
	}
	c := r.Begin(Launch{})
	c.Drain(context.Background())
	if c != nil || c.Tail(5) != nil {
		t.Error("nil capture not inert")
	}
	if err := r.WriteZip(context.Background(), io.Discard); err == nil {
		t.Error("nil WriteZip should fail")
	}
}

func TestCaptureStdoutStderrAndLaunchMarker(t *testing.T) {
	f := newFixture(t, nil, false)
	c := f.launch(t, "fc")
	f.rt.WriteLog("astrosim-fc", fake.Stdout, "out one\n")
	f.rt.WriteLog("astrosim-fc", fake.Stderr, "err one\n\x1b[31mred\x1b[0m\n")
	f.rt.WriteLog("astrosim-fc", fake.Stdout, "part")
	f.rt.WriteLog("astrosim-fc", fake.Stdout, "ial\n")
	f.rt.Exit("astrosim-fc", 0)
	if d := drain(c, 3*time.Second); d > 2*time.Second {
		t.Fatalf("Drain took %v on a clean exit", d)
	}
	fc := f.file(t, "fc.log")
	for _, want := range []string{
		"==== launch gen=3 container=astrosim-fc id=fake00000001 image=img ====",
		"| out one\n", "| err one\n", "| red\n", "| partial\n",
	} {
		if !strings.Contains(fc, want) {
			t.Errorf("fc.log lacks %q:\n%s", want, fc)
		}
	}
	if strings.Contains(fc, "\x1b") || strings.Contains(fc, "2026-11-22T") && strings.Contains(fc, "Z out") {
		t.Errorf("runtime timestamps or escapes leaked:\n%s", fc)
	}
	comb := f.file(t, "combined.log")
	if !strings.Contains(comb, " fc           | ==== launch gen=3") || !strings.Contains(comb, " fc           | out one") {
		t.Errorf("combined.log:\n%s", comb)
	}
}

func TestCaptureCrashEndsByItself(t *testing.T) {
	f := newFixture(t, nil, false)
	c := f.launch(t, "engine")
	f.rt.WriteLog("astrosim-engine", fake.Stderr, "fatal: boom\n")
	f.rt.Kill("astrosim-engine")
	select {
	case <-c.done:
	case <-time.After(3 * time.Second):
		t.Fatal("capture did not end after the container exited")
	}
	if d := drain(c, time.Second); d > 100*time.Millisecond {
		t.Errorf("Drain after the capture ended took %v", d)
	}
	if got := c.Tail(10); len(got) != 1 || got[0] != "fatal: boom" {
		t.Errorf("Tail = %q", got)
	}
	if strings.Contains(f.file(t, "orchestrator.log"), "WARN") {
		t.Error("a normal exit must not warn")
	}
}

// hangSource simulates an engine whose follow never delivers or ends.
type hangSource struct{ *fake.Runtime }

func (h hangSource) FollowLogs(ctx context.Context, name string, o runtime.LogOptions, out, errw io.Writer) error {
	if o.Follow {
		<-ctx.Done()
		return ctx.Err()
	}
	return h.Runtime.FollowLogs(ctx, name, o, out, errw)
}

func TestDrainHangingFollowerIsBoundedAndGapFilled(t *testing.T) {
	f := newFixture(t, func(rt *fake.Runtime) LogSource { return hangSource{rt} }, false)
	c := f.launch(t, "fc")
	f.rt.WriteLog("astrosim-fc", fake.Stdout, "one\ntwo\nthree\n")
	f.rt.Exit("astrosim-fc", 0)
	d := drain(c, 100*time.Millisecond)
	if d > 3*time.Second {
		t.Fatalf("Drain took %v", d)
	}
	fc := f.file(t, "fc.log")
	for _, want := range []string{"| one\n", "| two\n", "| three\n", "==== 3 lines fetched after the stream did not end ===="} {
		if !strings.Contains(fc, want) {
			t.Errorf("fc.log lacks %q:\n%s", want, fc)
		}
	}
	if got := c.Tail(10); len(got) != 3 {
		t.Errorf("Tail = %q", got)
	}
}

func TestDrainWithFakeFollowHang(t *testing.T) {
	f := newFixture(t, nil, false)
	f.rt.FollowHang = map[string]bool{"astrosim-fc": true}
	c := f.launch(t, "fc")
	f.rt.WriteLog("astrosim-fc", fake.Stdout, "delivered\n")
	f.rt.Exit("astrosim-fc", 0)
	waitFor(t, "line", func() bool { return len(c.Tail(5)) == 1 })
	if d := drain(c, 100*time.Millisecond); d > 3*time.Second {
		t.Fatalf("Drain took %v", d)
	}
	fc := f.file(t, "fc.log")
	if strings.Count(fc, "| delivered\n") != 1 || strings.Contains(fc, "lines fetched") {
		t.Errorf("duplicate or spurious gap-fill:\n%s", fc)
	}
}

func TestCaptureLogDriverNone(t *testing.T) {
	f := newFixture(t, nil, false)
	f.rt.FollowErr = func(string) error {
		return &runtime.CommandError{Args: []string{"logs"}, ExitCode: 125,
			Stderr: "Error: this container is using the \"none\" log driver, which does not support reading"}
	}
	c := f.launch(t, "fc")
	select {
	case <-c.done:
	case <-time.After(3 * time.Second):
		t.Fatal("capture did not end")
	}
	drain(c, time.Second)
	fc := f.file(t, "fc.log")
	if !strings.Contains(fc, "==== capture ended: ") || !strings.Contains(fc, "none") {
		t.Errorf("fc.log:\n%s", fc)
	}
	orch := f.file(t, "orchestrator.log")
	if strings.Count(orch, "log capture for fc stopped") != 1 {
		t.Errorf("want exactly one warning:\n%s", orch)
	}
	if !strings.Contains(f.file(t, "combined.log"), "==== capture ended: ") {
		t.Error("marker missing from combined.log")
	}
}

// flakySource ends the first n follows early (without error) after
// delivering what exists, like an engine that drops the stream.
type flakySource struct {
	*fake.Runtime
	mu    sync.Mutex
	early int
	calls []runtime.LogOptions
}

func (s *flakySource) FollowLogs(ctx context.Context, name string, o runtime.LogOptions, out, errw io.Writer) error {
	s.mu.Lock()
	s.calls = append(s.calls, o)
	early := s.early > 0
	if early {
		s.early--
	}
	s.mu.Unlock()
	if early {
		o.Follow = false
	}
	return s.Runtime.FollowLogs(ctx, name, o, out, errw)
}

func TestCaptureReconnectsWithoutDuplicates(t *testing.T) {
	var src *flakySource
	f := newFixture(t, func(rt *fake.Runtime) LogSource { src = &flakySource{Runtime: rt, early: 2}; return src }, false)
	f.rt.WriteLog("astrosim-fc", fake.Stdout, "x") // no container yet: ignored
	c := f.launch(t, "fc")
	f.rt.WriteLog("astrosim-fc", fake.Stdout, "first\n")
	waitFor(t, "reconnects", func() bool {
		src.mu.Lock()
		defer src.mu.Unlock()
		return len(src.calls) >= 3
	})
	f.rt.WriteLog("astrosim-fc", fake.Stdout, "second\n")
	f.rt.Exit("astrosim-fc", 0)
	drain(c, 3*time.Second)
	fc := f.file(t, "fc.log")
	if strings.Count(fc, "| first\n") != 1 || strings.Count(fc, "| second\n") != 1 {
		t.Errorf("duplicates or losses:\n%s", fc)
	}
	if strings.Count(fc, "==== reconnected") != 2 {
		t.Errorf("want 2 reconnect markers:\n%s", fc)
	}
	src.mu.Lock()
	defer src.mu.Unlock()
	if !src.calls[0].Since.IsZero() || src.calls[1].Since.IsZero() || !src.calls[1].Follow {
		t.Errorf("calls = %+v", src.calls)
	}
}

func TestCaptureGivesUpReconnecting(t *testing.T) {
	maxBefore := maxReconnects
	defer func() { maxReconnects = maxBefore }()
	f := newFixture(t, func(rt *fake.Runtime) LogSource { return &flakySource{Runtime: rt, early: 1000} }, false)
	maxReconnects = 2
	c := f.launch(t, "fc")
	select {
	case <-c.done:
	case <-time.After(5 * time.Second):
		t.Fatal("capture never gave up")
	}
	fc := f.file(t, "fc.log")
	if strings.Count(fc, "==== reconnected") != 2 || !strings.Contains(fc, "==== capture ended: the log stream keeps ending") {
		t.Errorf("fc.log:\n%s", fc)
	}
}

func TestCaptureMissingContainerIsQuiet(t *testing.T) {
	f := newFixture(t, nil, false)
	c := f.rec.Begin(Launch{Component: "fc", Container: "astrosim-gone", Image: "img", Gen: 1})
	select {
	case <-c.done:
	case <-time.After(3 * time.Second):
		t.Fatal("capture did not end")
	}
	if strings.Contains(f.file(t, "orchestrator.log"), "WARN") || strings.Contains(f.file(t, "fc.log"), "capture ended") {
		t.Error("a removed container should end quietly")
	}
}

func TestTailRing(t *testing.T) {
	f := newFixture(t, nil, false)
	c := f.launch(t, "fc")
	var b strings.Builder
	for i := 0; i < 300; i++ {
		fmt.Fprintf(&b, "line %03d\n", i)
	}
	f.rt.WriteLog("astrosim-fc", fake.Stdout, b.String())
	f.rt.Exit("astrosim-fc", 0)
	drain(c, 3*time.Second)
	all := c.Tail(1000)
	if len(all) != tailLines || all[0] != "line 100" || all[len(all)-1] != "line 299" {
		t.Errorf("Tail(1000): len=%d first=%q last=%q", len(all), all[0], all[len(all)-1])
	}
	if got := c.Tail(3); fmt.Sprint(got) != "[line 297 line 298 line 299]" {
		t.Errorf("Tail(3) = %v", got)
	}
	if c.Tail(0) != nil {
		t.Error("Tail(0) should be nil")
	}
}

func TestCloseStopsCaptures(t *testing.T) {
	f := newFixture(t, nil, false)
	c := f.launch(t, "fc")
	f.rt.WriteLog("astrosim-fc", fake.Stdout, "before close\n")
	waitFor(t, "line", func() bool { return len(c.Tail(1)) == 1 })
	if err := f.rec.Close(context.Background()); err != nil {
		t.Fatal(err)
	}
	select {
	case <-c.done:
	default:
		t.Fatal("capture still running after Close")
	}
	b, _ := os.ReadFile(filepath.Join(f.rec.Dir(), "fc.log"))
	if !strings.Contains(string(b), "before close") {
		t.Errorf("fc.log = %s", b)
	}
	if err := f.rec.Close(context.Background()); err != nil {
		t.Errorf("second Close = %v", err)
	}
}

func TestOrchestratorWriterConsoleModes(t *testing.T) {
	for _, verbose := range []bool{false, true} {
		t.Run(fmt.Sprintf("verbose=%v", verbose), func(t *testing.T) {
			f := newFixture(t, nil, verbose)
			w := f.rec.OrchestratorWriter()
			io.WriteString(w, "2026-11-22 13:00:00 INFO hello\n")
			io.WriteString(w, "2026-11-22 13:00:01 INFO split ")
			io.WriteString(w, "line\n")
			f.rec.Note("fc", "launch test")
			if err := f.rec.Close(context.Background()); err != nil {
				t.Fatal(err)
			}
			io.WriteString(w, "2026-11-22 13:00:02 INFO after close\n")

			orch, _ := os.ReadFile(filepath.Join(f.rec.Dir(), "orchestrator.log"))
			if strings.Count(string(orch), "INFO hello") != 1 || !strings.Contains(string(orch), "| 2026-11-22 13:00:01 INFO split line\n") {
				t.Errorf("orchestrator.log:\n%s", orch)
			}
			if strings.Contains(string(orch), "after close") {
				t.Error("line after Close reached the file")
			}
			con := f.console.String()
			// Never printed twice, and the post-close line still shows.
			if strings.Count(con, "INFO hello") != 1 || strings.Count(con, "after close") != 1 {
				t.Errorf("console:\n%s", con)
			}
			if verbose != strings.Contains(con, "| ==== launch test ====") {
				t.Errorf("component marker on console = %v, want %v", !verbose, verbose)
			}
			if verbose && !strings.Contains(con, "orchestrator | 2026-11-22 13:00:00 INFO hello") {
				t.Errorf("verbose console should use the combined format:\n%s", con)
			}
		})
	}
}

func TestWriteZipWhileWriting(t *testing.T) {
	f := newFixture(t, nil, false)
	w := f.rec.OrchestratorWriter()
	stop := make(chan struct{})
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; ; i++ {
			select {
			case <-stop:
				return
			default:
				fmt.Fprintf(w, "2026-11-22 13:00:00 INFO writer line %d\n", i)
			}
		}
	}()
	c := f.launch(t, "fc")
	f.rt.WriteLog("astrosim-fc", fake.Stdout, "from the container\n")
	waitFor(t, "container line", func() bool { return len(c.Tail(1)) == 1 })

	var buf bytes.Buffer
	if err := f.rec.WriteZip(context.Background(), &buf); err != nil {
		t.Fatal(err)
	}
	close(stop)
	wg.Wait()
	zr, err := zip.NewReader(bytes.NewReader(buf.Bytes()), int64(buf.Len()))
	if err != nil {
		t.Fatalf("download is not a valid zip: %v", err)
	}
	got := map[string]string{}
	for _, zf := range zr.File {
		rc, _ := zf.Open()
		b, err := io.ReadAll(rc)
		rc.Close()
		if err != nil {
			t.Fatalf("%s: %v", zf.Name, err)
		}
		got[zf.Name] = string(b)
	}
	pre := f.rec.Name() + "/"
	for _, n := range []string{"combined.log", "orchestrator.log", "fc.log"} {
		if _, ok := got[pre+n]; !ok {
			t.Errorf("zip lacks %s: %v", n, len(got))
		}
	}
	// Every line in the snapshot is whole.
	for _, l := range strings.Split(strings.TrimSuffix(got[pre+"combined.log"], "\n"), "\n")[1:] {
		if !combinedRE.MatchString(l) {
			t.Fatalf("broken line in download: %q", l)
		}
	}
	if !strings.Contains(got[pre+"fc.log"], "from the container") {
		t.Error("container line missing from the download")
	}
}

// blockingWriter lets a test hold a WriteZip open.
type blockingWriter struct {
	entered chan struct{}
	release chan struct{}
	once    sync.Once
}

func (b *blockingWriter) Write(p []byte) (int, error) {
	b.once.Do(func() { close(b.entered) })
	<-b.release
	return len(p), nil
}

func TestWriteZipOneAtATime(t *testing.T) {
	f := newFixture(t, nil, false)
	bw := &blockingWriter{entered: make(chan struct{}), release: make(chan struct{})}
	done := make(chan error, 1)
	go func() { done <- f.rec.WriteZip(context.Background(), bw) }()
	select {
	case <-bw.entered:
	case <-time.After(3 * time.Second):
		t.Fatal("first download never started writing")
	}
	if err := f.rec.WriteZip(context.Background(), io.Discard); !errors.Is(err, ErrBusy) {
		t.Errorf("second download = %v, want ErrBusy", err)
	}
	close(bw.release)
	if err := <-done; err != nil {
		t.Errorf("first download = %v", err)
	}
	if err := f.rec.WriteZip(context.Background(), io.Discard); err != nil {
		t.Errorf("download after release = %v", err)
	}
	f.rec.Close(context.Background())
	if err := f.rec.WriteZip(context.Background(), io.Discard); err == nil {
		t.Error("download after Close should fail")
	}
	if f.rec.sink.holds.Load() != 0 {
		t.Error("rotation hold leaked")
	}
}

func TestRecorderArchiveRoundTrip(t *testing.T) {
	f := newFixture(t, nil, false)
	c := f.launch(t, "fc")
	f.rt.WriteLog("astrosim-fc", fake.Stdout, "hello\n")
	f.rt.Exit("astrosim-fc", 0)
	drain(c, 3*time.Second)
	if err := f.rec.Close(context.Background()); err != nil {
		t.Fatal(err)
	}
	zp, err := Archive(f.rec.Dir())
	if err != nil {
		t.Fatal(err)
	}
	got := zipContents(t, zp)
	if !strings.Contains(got[f.rec.Name()+"/fc.log"], "| hello\n") {
		t.Errorf("archive:\n%v", got)
	}
}
