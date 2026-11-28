package session

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"sync"
	"time"
	"unicode"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/status"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/supervisor"
)

// Download states of one image.
const (
	pullDownloading = "downloading"
	pullDone        = "done"
	pullFailed      = "failed"
)

const (
	// maxOutputRunes caps the progress line published in a snapshot.
	maxOutputRunes = 200
	// maxLineBytes caps a progress line that never ends, so a runtime that
	// prints no newline cannot grow the buffer without bound.
	maxLineBytes = 4096
)

// pullJob is one ActionPull: the download of a set of images.
type pullJob struct {
	gen    uint64
	cancel context.CancelFunc

	mu     sync.Mutex // guards images; taken after Session.mu, never before
	images map[string]*imagePull
}

// imagePull is the progress of one image.
type imagePull struct {
	state    string // pullDownloading, pullDone or pullFailed
	started  time.Time
	finished time.Time
	output   string // the latest progress line
}

// setOutput records the latest progress line of ref.
func (j *pullJob) setOutput(ref, line string) {
	j.mu.Lock()
	defer j.mu.Unlock()
	if p := j.images[ref]; p != nil {
		p.output = line
	}
}

// finish records the end of ref's download.
func (j *pullJob) finish(ref, state string, at time.Time) {
	j.mu.Lock()
	defer j.mu.Unlock()
	if p := j.images[ref]; p != nil {
		p.state, p.finished = state, at
	}
}

// startPull registers the download synchronously, so a later CancelPull can
// find it, and runs it in a goroutine.
func (s *Session) startPull(a supervisor.Action) {
	ctx, cancel := context.WithCancel(s.root)
	job := &pullJob{gen: a.Gen, cancel: cancel, images: map[string]*imagePull{}}
	now := s.opts.Clock()
	for _, ref := range a.Images {
		job.images[ref] = &imagePull{state: pullDownloading, started: now}
	}
	s.mu.Lock()
	if old := s.pull; old != nil {
		old.cancel()
	}
	s.pull = job
	s.mu.Unlock()
	s.goRun(func() {
		defer cancel()
		s.doPull(ctx, job, a.Images)
	})
}

// cancelPull aborts the download of generation gen, if it is still current.
func (s *Session) cancelPull(gen uint64) {
	s.mu.Lock()
	job := s.pull
	s.mu.Unlock()
	if job != nil && job.gen == gen {
		job.cancel()
	}
}

// doPull downloads every image in parallel, waits for all of them and
// reports the outcome. A cancelled download reports nothing: whoever
// cancelled it has already moved on.
func (s *Session) doPull(ctx context.Context, job *pullJob, images []string) {
	var wg sync.WaitGroup
	errs := make([]error, len(images))
	for i, ref := range images {
		wg.Add(1)
		go func() {
			defer wg.Done()
			errs[i] = s.pullImage(ctx, job, ref)
		}()
	}
	wg.Wait()
	if ctx.Err() != nil {
		return
	}
	rep := supervisor.PreflightReport{}
	var details []string
	for i, err := range errs {
		if err == nil {
			continue
		}
		details = append(details, images[i]+": "+err.Error())
		if p := pullProblem(err); problemRank(p) < problemRank(rep.Problem) {
			rep.Problem = p
		}
	}
	rep.Detail = strings.Join(details, "; ")
	s.post(supervisor.Event{Kind: supervisor.PullDone, Gen: job.gen, Report: rep})
}

// pullImage downloads one image, keeping the job, the logs and the
// orchestrator log informed.
func (s *Session) pullImage(ctx context.Context, job *pullJob, ref string) error {
	start := s.opts.Clock()
	s.log.Info("downloading image", "image", ref)
	s.noteImage(ref, "pull "+ref)
	w := newLineWriter(func(line string) {
		job.setOutput(ref, truncate(line, maxOutputRunes))
		s.lineImage(ref, line)
	})
	err := s.opts.Runtime.Pull(ctx, ref, w)
	w.Close()
	end := s.opts.Clock()
	took := end.Sub(start).Round(time.Second)
	switch {
	case err == nil:
		job.finish(ref, pullDone, end)
		s.log.Info("image downloaded", "image", ref, "took", took)
		s.noteImage(ref, "pull done in "+took.String())
	case ctx.Err() != nil:
		job.finish(ref, pullFailed, end)
		s.log.Info("image download cancelled", "image", ref)
		s.noteImage(ref, "pull cancelled")
		return ctx.Err()
	default:
		job.finish(ref, pullFailed, end)
		s.log.Warn("image download failed", "image", ref, "err", err)
		s.noteImage(ref, fmt.Sprintf("pull failed: %v", err))
	}
	return err
}

// noteImage writes a marker to the log of every component that uses ref.
func (s *Session) noteImage(ref, msg string) {
	for _, id := range s.ids {
		if s.defs[id].Image == ref {
			s.note(id, msg)
		}
	}
}

// lineImage writes a plain line to the log of every component that uses ref.
func (s *Session) lineImage(ref, line string) {
	if s.opts.Logs == nil {
		return
	}
	for _, id := range s.ids {
		if s.defs[id].Image == ref {
			s.opts.Logs.Line(id, line)
		}
	}
}

// pullProblem maps a download error to the problem code the dashboard shows.
func pullProblem(err error) string {
	switch {
	case errors.Is(err, runtime.ErrNoSpace):
		return supervisor.ProblemPullNoSpace
	case errors.Is(err, runtime.ErrDenied):
		return supervisor.ProblemPullDenied
	case errors.Is(err, runtime.ErrNotFound):
		return supervisor.ProblemPullNotFound
	case errors.Is(err, runtime.ErrUnreachable):
		return supervisor.ProblemPullNetwork
	default:
		return supervisor.ProblemPullFailed
	}
}

// problemRank orders problem codes by how actionable they are; lower is
// more actionable. An empty code ranks last.
func problemRank(p string) int {
	for i, c := range []string{
		supervisor.ProblemPullNoSpace, supervisor.ProblemPullDenied, supervisor.ProblemPullNotFound,
		supervisor.ProblemPullNetwork, supervisor.ProblemPullFailed,
	} {
		if p == c {
			return i
		}
	}
	return 1 << 10
}

// truncate shortens s to at most n runes.
func truncate(s string, n int) string {
	if r := []rune(s); len(r) > n {
		return string(r[:n])
	}
	return s
}

// downloadFor returns the published download progress of ref, or nil. The
// caller holds s.mu.
func (s *Session) downloadFor(ref string, now time.Time) *status.Download {
	if s.pull == nil {
		return nil
	}
	s.pull.mu.Lock()
	defer s.pull.mu.Unlock()
	p := s.pull.images[ref]
	if p == nil {
		return nil
	}
	end := now
	if p.state != pullDownloading {
		end = p.finished
	}
	return &status.Download{
		State: p.state, Since: p.started, ElapsedSeconds: max(0, int(end.Sub(p.started).Seconds())), Output: p.output,
	}
}

// lineWriter turns a stream of progress text into lines. Runtimes redraw
// progress with carriage returns, so \r ends a line as well as \n. It is safe
// for concurrent use, because a runtime writes its two streams from two
// goroutines.
type lineWriter struct {
	emit func(string)

	mu  sync.Mutex
	buf []byte
}

// newLineWriter returns a lineWriter that calls emit for every non-empty line.
func newLineWriter(emit func(string)) *lineWriter { return &lineWriter{emit: emit} }

// Write implements io.Writer.
func (w *lineWriter) Write(p []byte) (int, error) {
	w.mu.Lock()
	defer w.mu.Unlock()
	for _, b := range p {
		if b == '\n' || b == '\r' || len(w.buf) >= maxLineBytes {
			w.flushLocked()
			if b == '\n' || b == '\r' {
				continue
			}
		}
		w.buf = append(w.buf, b)
	}
	return len(p), nil
}

// Close emits the unfinished last line, if any.
func (w *lineWriter) Close() {
	w.mu.Lock()
	defer w.mu.Unlock()
	w.flushLocked()
}

// flushLocked emits the buffered line, cleaned and trimmed. The caller holds w.mu.
func (w *lineWriter) flushLocked() {
	line := strings.TrimSpace(strings.Map(func(r rune) rune {
		if r == '\t' {
			return ' '
		}
		if unicode.IsControl(r) {
			return -1
		}
		return r
	}, strings.ToValidUTF8(string(w.buf), "�")))
	w.buf = w.buf[:0]
	if line != "" {
		w.emit(line)
	}
}
