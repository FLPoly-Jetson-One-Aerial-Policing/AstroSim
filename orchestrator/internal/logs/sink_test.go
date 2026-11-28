package logs

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"
)

var combinedRE = regexp.MustCompile(`^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3} \S+\s* \| .*$`)

func testSink(t *testing.T, mut func(*sinkOptions)) (*sink, string) {
	t.Helper()
	dir := filepath.Join(t.TempDir(), "session")
	o := sinkOptions{Dir: dir, Header: "# header", FileMax: 1 << 20}
	if mut != nil {
		mut(&o)
	}
	s, err := newSink(o)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.close(context.Background()) })
	return s, dir
}

func closeSink(t *testing.T, s *sink) {
	t.Helper()
	if err := s.close(context.Background()); err != nil {
		t.Fatal(err)
	}
}

func readLines(t *testing.T, path string) []string {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return strings.Split(strings.TrimSuffix(string(b), "\n"), "\n")
}

func TestSinkFilesAndFormat(t *testing.T) {
	s, dir := testSink(t, nil)
	ts := time.Date(2026, 11, 22, 13, 26, 58, 123_000_000, time.Local)
	s.logBlocking(entry{Time: ts, Source: "fc", Line: "hello"})
	s.log(entry{Time: ts, Source: "orchestrator", Line: "started"})
	s.logBlocking(entry{Time: ts, Source: "weird/../name", Line: "x"})
	s.logBlocking(entry{Time: ts, Source: "combined", Line: "y"})
	closeSink(t, s)

	comb := readLines(t, filepath.Join(dir, "combined.log"))
	want := []string{
		"# header",
		"2026-11-22 13:26:58.123 fc           | hello",
		"2026-11-22 13:26:58.123 orchestrator | started",
		"2026-11-22 13:26:58.123 weird/../name | x",
		"2026-11-22 13:26:58.123 combined     | y",
	}
	if strings.Join(comb, "\n") != strings.Join(want, "\n") {
		t.Errorf("combined.log:\n%s", strings.Join(comb, "\n"))
	}
	if got := readLines(t, filepath.Join(dir, "fc.log")); strings.Join(got, "\n") != "# header\n2026-11-22 13:26:58.123 | hello" {
		t.Errorf("fc.log = %q", got)
	}
	if got := readLines(t, filepath.Join(dir, "orchestrator.log")); len(got) != 2 || !strings.HasSuffix(got[1], "| started") {
		t.Errorf("orchestrator.log = %q", got)
	}
	// Hostile and reserved names stay inside the folder and apart from combined.log.
	for _, f := range []string{"weird_.._name.log", "_combined.log"} {
		if _, err := os.Stat(filepath.Join(dir, f)); err != nil {
			t.Errorf("missing %s: %v", f, err)
		}
	}
}

func TestSinkPermissions(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("POSIX permissions")
	}
	s, dir := testSink(t, nil)
	s.logBlocking(entry{Source: "fc", Line: "x"})
	closeSink(t, s)
	for path, want := range map[string]os.FileMode{dir: 0o700, filepath.Join(dir, "fc.log"): 0o600, filepath.Join(dir, "combined.log"): 0o600} {
		fi, err := os.Stat(path)
		if err != nil || fi.Mode().Perm() != want {
			t.Errorf("%s mode = %v, %v; want %v", path, fi.Mode().Perm(), err, want)
		}
	}
}

func TestSinkConcurrentWritersNeverBreakLines(t *testing.T) {
	s, dir := testSink(t, nil)
	const writers, per = 8, 400
	var wg sync.WaitGroup
	for w := 0; w < writers; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			src := fmt.Sprintf("comp%d", w%3)
			for i := 0; i < per; i++ {
				line := fmt.Sprintf("w%d-%04d %s", w, i, strings.Repeat("x", i%50))
				if w%2 == 0 {
					s.logBlocking(entry{Source: src, Line: line})
				} else {
					s.log(entry{Source: src, Line: line})
				}
			}
		}()
	}
	wg.Wait()
	closeSink(t, s)

	got := readLines(t, filepath.Join(dir, "combined.log"))[1:]
	if len(got) != writers*per {
		t.Fatalf("combined has %d lines, want %d", len(got), writers*per)
	}
	seen := map[string]bool{}
	for _, l := range got {
		if !combinedRE.MatchString(l) {
			t.Fatalf("broken line %q", l)
		}
		seen[strings.TrimSpace(l[strings.Index(l, "| ")+2:])] = true
	}
	if len(seen) != writers*per {
		t.Errorf("distinct lines = %d", len(seen))
	}
	total := 0
	for i := 0; i < 3; i++ {
		total += len(readLines(t, filepath.Join(dir, fmt.Sprintf("comp%d.log", i)))) - 1
	}
	if total != writers*per {
		t.Errorf("component files hold %d lines", total)
	}
}

func partNames(t *testing.T, dir, base string) []string {
	t.Helper()
	m, err := filepath.Glob(filepath.Join(dir, base+"*.log"))
	if err != nil {
		t.Fatal(err)
	}
	for i := range m {
		m[i] = filepath.Base(m[i])
	}
	return m
}

func TestSinkRotationKeepsFirstAndLastParts(t *testing.T) {
	// FileMax 2000 gives 500-byte parts.
	s, dir := testSink(t, func(o *sinkOptions) { o.FileMax = 2000 })
	const n = 400
	for i := 0; i < n; i++ {
		s.logBlocking(entry{Source: "fc", Line: fmt.Sprintf("line-%04d", i)})
	}
	closeSink(t, s)

	parts := partNames(t, dir, "fc")
	if len(parts) != 3 || parts[0] != "fc.log" {
		t.Fatalf("parts = %v, want first part plus two", parts)
	}
	first := readLines(t, filepath.Join(dir, "fc.log"))
	if first[0] != "# header" || !strings.HasSuffix(first[1], "| line-0000") {
		t.Errorf("first part does not show the start: %q", first[:2])
	}
	if !strings.HasPrefix(first[len(first)-1], "==== part ") || !strings.Contains(first[len(first)-1], "removed") {
		t.Errorf("first part lacks the removal marker: %q", first[len(first)-1])
	}
	var all []string
	for _, p := range parts[1:] {
		all = append(all, readLines(t, filepath.Join(dir, p))...)
	}
	if !strings.HasSuffix(all[len(all)-1], fmt.Sprintf("| line-%04d", n-1)) {
		t.Errorf("tail does not end with the last line: %q", all[len(all)-1])
	}
	for _, p := range parts {
		fi, _ := os.Stat(filepath.Join(dir, p))
		// One part, plus one line of slack and the removal markers in part 1.
		if fi.Size() > 500+200+40*int64(n) { // loose sanity bound
			t.Errorf("%s is %d bytes", p, fi.Size())
		}
	}
	var total int64
	for _, p := range parts {
		fi, _ := os.Stat(filepath.Join(dir, p))
		total += fi.Size()
	}
	if total > 3*500+3*100+40*int64(n)/4 && total > 3000 {
		t.Errorf("total %d bytes exceeds the cap by too much", total)
	}
}

func TestSinkRotationPostponedDuringSnapshot(t *testing.T) {
	s, dir := testSink(t, func(o *sinkOptions) { o.FileMax = 800 }) // 200-byte parts
	s.logBlocking(entry{Source: "fc", Line: "before"})
	files, release, err := s.snapshot(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	sizes := map[string]int64{}
	for _, f := range files {
		sizes[f.Rel] = f.Size
	}
	if sizes["fc.log"] == 0 || sizes["combined.log"] == 0 || sizes["orchestrator.log"] == 0 {
		t.Fatalf("snapshot = %+v", files)
	}
	for i := 0; i < 100; i++ {
		s.logBlocking(entry{Source: "fc", Line: fmt.Sprintf("during-%03d", i)})
	}
	// Wait for the writer to catch up with a second snapshot-free barrier.
	deadline := time.Now().Add(2 * time.Second)
	for len(s.in) > 0 && time.Now().Before(deadline) {
		time.Sleep(time.Millisecond)
	}
	time.Sleep(20 * time.Millisecond)
	if p := partNames(t, dir, "fc"); len(p) != 1 {
		t.Fatalf("rotated during a snapshot: %v", p)
	}
	// The snapshotted prefix is intact: sizes only grew by appends.
	b, _ := os.ReadFile(filepath.Join(dir, "fc.log"))
	if int64(len(b)) < sizes["fc.log"] || !strings.HasSuffix(strings.Split(string(b[:sizes["fc.log"]]), "\n")[1], "| before") {
		t.Errorf("snapshotted bytes changed")
	}
	release()
	s.logBlocking(entry{Source: "fc", Line: "after"})
	closeSink(t, s)
	if p := partNames(t, dir, "fc"); len(p) < 2 {
		t.Errorf("rotation did not resume after release: %v", p)
	}
}

func TestSinkSnapshotAfterClose(t *testing.T) {
	s, _ := testSink(t, nil)
	closeSink(t, s)
	if _, _, err := s.snapshot(context.Background()); err == nil {
		t.Fatal("snapshot after close should fail")
	}
	if s.holds.Load() != 0 {
		t.Errorf("hold leaked: %d", s.holds.Load())
	}
}

func TestSinkOrchestratorDropsAreCountedAndAnnounced(t *testing.T) {
	// Without a writer goroutine the queue fills and log must give up.
	stalled := &sink{
		o:        sinkOptions{DropWait: 5 * time.Millisecond},
		in:       make(chan any, 1),
		finished: make(chan struct{}),
	}
	start := time.Now()
	stalled.log(entry{Line: "fits"})
	stalled.log(entry{Line: "dropped 1"})
	stalled.log(entry{Line: "dropped 2"})
	if got := stalled.dropped.Load(); got != 2 {
		t.Fatalf("dropped = %d", got)
	}
	if time.Since(start) > 2*time.Second {
		t.Error("log blocked too long")
	}

	s, dir := testSink(t, nil)
	s.dropped.Store(7)
	s.log(entry{Source: "orchestrator", Line: "next"})
	closeSink(t, s)
	got := strings.Join(readLines(t, filepath.Join(dir, "orchestrator.log")), "\n")
	if !strings.Contains(got, "==== 7 orchestrator log lines dropped") || strings.Index(got, "dropped") > strings.Index(got, "| next") {
		t.Errorf("orchestrator.log:\n%s", got)
	}
}

func TestSinkWriteFailureWarnsOnceAndNeverBlocks(t *testing.T) {
	var warn syncBuf
	s, dir := testSink(t, func(o *sinkOptions) { o.ErrOut = &warn })
	s.logBlocking(entry{Source: "fc", Line: "ok"})
	// A snapshot is a barrier: "ok" is written before it returns.
	if _, release, err := s.snapshot(context.Background()); err != nil {
		t.Fatal(err)
	} else {
		release()
	}
	// A directory squatting on the name of a new source's first part file
	// makes creating that file fail on every OS. (Deleting the session
	// folder would not: Windows refuses while a file in it is open.)
	if err := os.Mkdir(filepath.Join(dir, "new0.log"), 0o700); err != nil {
		t.Fatal(err)
	}
	done := make(chan struct{})
	go func() {
		defer close(done)
		for i := 0; i < 2000; i++ { // far more than the queue holds
			s.logBlocking(entry{Source: fmt.Sprintf("new%d", i%3), Line: "x"})
		}
	}()
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("writers blocked after a file error")
	}
	closeSink(t, s)
	if n := strings.Count(warn.String(), "file logging stopped"); n != 1 {
		t.Errorf("warned %d times: %q", n, warn.String())
	}
}

func TestSinkFeedsConsole(t *testing.T) {
	var out syncBuf
	c := newConsole(&out, time.Now)
	s, _ := testSink(t, func(o *sinkOptions) { o.Console = c })
	s.logBlocking(entry{Source: "fc", Line: "one"})
	s.log(entry{Source: "orchestrator", Line: "two"})
	closeSink(t, s)
	lines := strings.Split(strings.TrimSpace(out.String()), "\n")
	if len(lines) != 2 || !strings.Contains(lines[0], "fc           | one") || !strings.Contains(lines[1], "| two") {
		t.Errorf("console:\n%s", out.String())
	}
}

func TestSinkCloseDiscardsLateEntriesWithoutBlocking(t *testing.T) {
	s, dir := testSink(t, nil)
	s.logBlocking(entry{Source: "fc", Line: "early"})
	closeSink(t, s)
	done := make(chan struct{})
	go func() {
		for i := 0; i < 1000; i++ {
			s.logBlocking(entry{Source: "fc", Line: "late"})
			s.log(entry{Source: "fc", Line: "late"})
		}
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("late writers blocked")
	}
	if got := strings.Join(readLines(t, filepath.Join(dir, "fc.log")), "\n"); strings.Contains(got, "late") || !strings.Contains(got, "early") {
		t.Errorf("fc.log = %q", got)
	}
}

func TestFileName(t *testing.T) {
	tests := map[string]string{
		"fc": "fc", "engine-2": "engine-2", "a b": "a_b", "../x": ".._x", "combined": "_combined",
		"": "_", ".": "_.", "..": "_..", "a/b\\c": "a_b_c", "é": "__",
	}
	for in, want := range tests {
		if got := fileName(in); got != want {
			t.Errorf("fileName(%q) = %q, want %q", in, got, want)
		}
	}
}

type syncBuf struct {
	mu sync.Mutex
	b  strings.Builder
}

func (s *syncBuf) Write(p []byte) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.Write(p)
}

func (s *syncBuf) String() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.String()
}
