package logs

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"sort"
	"sync/atomic"
	"time"
)

// Names of the files every session has.
const (
	combinedBase     = "combined"
	orchestratorName = "orchestrator"
)

// sourceWidth pads the source column of combined.log.
const sourceWidth = 12

// timeLayout is the timestamp format of every log line (local time).
const timeLayout = "2006-01-02 15:04:05.000"

// formatCombined renders a line of combined.log.
func formatCombined(t time.Time, source, line string) string {
	return fmt.Sprintf("%s %-*s | %s", t.Local().Format(timeLayout), sourceWidth, source, line)
}

// formatComponent renders a line of a per-source file.
func formatComponent(t time.Time, line string) string {
	return t.Local().Format(timeLayout) + " | " + line
}

// fileName maps a source name to a safe file base name. Component IDs come
// from configuration, so anything outside [A-Za-z0-9._-] is replaced and the
// result can never collide with combined.log or escape the folder.
func fileName(source string) string {
	b := []byte(source)
	for i, c := range b {
		switch {
		case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z', c >= '0' && c <= '9', c == '.', c == '-', c == '_':
		default:
			b[i] = '_'
		}
	}
	s := string(b)
	if s == "" || s == "." || s == ".." || s == combinedBase {
		s = "_" + s
	}
	return s
}

// entry is one log line on its way to the files.
type entry struct {
	Time   time.Time
	Source string // "orchestrator" or a component ID
	Line   string // cleaned text; markers are ordinary lines starting "===="
}

// marker wraps text as a marker line.
func marker(text string) string { return "==== " + text + " ====" }

// queue sizes and timings of the sink.
const (
	sinkQueue = 256
	// defaultDropWait is how long an orchestrator line waits for queue room
	// before it is dropped. The orchestrator must never stall on its logs.
	defaultDropWait = 100 * time.Millisecond
)

// sinkOptions configures newSink.
type sinkOptions struct {
	// Dir is the session folder; it is created.
	Dir string
	// Header is the first line of every file.
	Header string
	// FileMax caps each file (see partFile); it must be at least 4.
	FileMax int64
	// Console, if non-nil, receives every combined.log line.
	Console *console
	// ErrOut receives the one warning if a log file cannot be written.
	ErrOut io.Writer
	// Now stamps entries that carry no time; nil uses time.Now.
	Now func() time.Time
	// DropWait overrides defaultDropWait.
	DropWait time.Duration
}

// sink is the single owner of the session's log files. One goroutine does
// all the writing, so lines from every source stay whole and ordered.
// Capture entries block when the queue is full (backpressure on the
// container's log pipe, which the runtime buffers); orchestrator entries
// wait briefly and are then dropped and counted.
type sink struct {
	o        sinkOptions
	in       chan any
	stop     chan struct{}
	finished chan struct{}
	dropped  atomic.Int64
	holds    atomic.Int32

	// Owned by the writer goroutine.
	combined *partFile
	files    map[string]*partFile
	failed   bool
}

// snapshotReq asks the writer to flush and report the files.
type snapshotReq struct {
	reply chan snapshotReply
}

type snapshotReply struct {
	files []fileInfo
	err   error
}

// newSink creates the folder and the combined and orchestrator files and
// starts the writer.
func newSink(o sinkOptions) (*sink, error) {
	if o.Now == nil {
		o.Now = time.Now
	}
	if o.DropWait == 0 {
		o.DropWait = defaultDropWait
	}
	if o.ErrOut == nil {
		o.ErrOut = io.Discard
	}
	if o.FileMax < 4 {
		return nil, errors.New("logs: file size limit too small")
	}
	if err := os.MkdirAll(o.Dir, 0o700); err != nil {
		return nil, err
	}
	s := &sink{
		o:        o,
		in:       make(chan any, sinkQueue),
		stop:     make(chan struct{}),
		finished: make(chan struct{}),
		files:    map[string]*partFile{},
	}
	var err error
	if s.combined, err = s.newFile(combinedBase); err != nil {
		return nil, err
	}
	if _, err = s.file(orchestratorName); err != nil {
		s.combined.close()
		return nil, err
	}
	go s.run()
	return s, nil
}

func (s *sink) holding() bool { return s.holds.Load() > 0 }

func (s *sink) newFile(base string) (*partFile, error) {
	return newPartFile(s.o.Dir, base, s.o.Header, s.o.FileMax/4, s.holding)
}

// file returns the per-source file, creating it on first use.
func (s *sink) file(source string) (*partFile, error) {
	base := fileName(source)
	if f, ok := s.files[base]; ok {
		return f, nil
	}
	f, err := s.newFile(base)
	if err != nil {
		return nil, err
	}
	s.files[base] = f
	return f, nil
}

// logBlocking queues e, waiting for room. It is for container output, whose
// producer can simply be slowed down. After close it discards.
func (s *sink) logBlocking(e entry) {
	select {
	case s.in <- e:
	case <-s.finished:
	}
}

// log queues e from the orchestrator itself: it waits at most DropWait for
// room, then drops the line and counts it. The drop is announced in the log.
func (s *sink) log(e entry) {
	select {
	case s.in <- e:
		return
	default:
	}
	t := time.NewTimer(s.o.DropWait)
	defer t.Stop()
	select {
	case s.in <- e:
	case <-t.C:
		s.dropped.Add(1)
	case <-s.finished:
	}
}

// snapshot flushes the files and returns them with their current sizes, and
// postpones rotation until release is called, so a reader can copy exactly
// those bytes. Call release exactly once.
func (s *sink) snapshot(ctx context.Context) (files []fileInfo, release func(), err error) {
	req := snapshotReq{reply: make(chan snapshotReply, 1)}
	s.holds.Add(1) // before the request, so no rotation can slip in between
	release = func() { s.holds.Add(-1) }
	select {
	case s.in <- req:
	case <-s.finished:
		release()
		return nil, nil, errors.New("logs: recorder is closed")
	case <-ctx.Done():
		release()
		return nil, nil, ctx.Err()
	}
	select {
	case r := <-req.reply:
		if r.err != nil {
			release()
			return nil, nil, r.err
		}
		return r.files, release, nil
	case <-s.finished:
		// The writer exited. It may have answered just before; if not, the
		// request was left in the queue unread.
		select {
		case r := <-req.reply:
			if r.err == nil {
				return r.files, release, nil
			}
		default:
		}
		release()
		return nil, nil, errors.New("logs: recorder is closed")
	case <-ctx.Done():
		// The writer will still answer into the buffered channel.
		release()
		return nil, nil, ctx.Err()
	}
}

// close stops accepting entries, writes what is queued, closes the files and
// the console, and waits for the writer or ctx.
func (s *sink) close(ctx context.Context) error {
	select {
	case <-s.stop:
	default:
		close(s.stop)
	}
	select {
	case <-s.finished:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func (s *sink) run() {
	defer close(s.finished)
	for {
		select {
		case it := <-s.in:
			s.handle(it)
			if len(s.in) == 0 {
				s.flushAll()
			}
		case <-s.stop:
			for {
				select {
				case it := <-s.in:
					s.handle(it)
				default:
					s.announceDrops()
					s.shutdown()
					return
				}
			}
		}
	}
}

func (s *sink) handle(it any) {
	switch v := it.(type) {
	case entry:
		s.announceDrops()
		s.write(v)
	case snapshotReq:
		files, err := s.snapshotFiles()
		v.reply <- snapshotReply{files, err}
	}
}

// announceDrops records, in the log itself, how many orchestrator lines were
// dropped since the last time.
func (s *sink) announceDrops() {
	if n := s.dropped.Swap(0); n > 0 {
		s.write(entry{Source: orchestratorName,
			Line: marker(fmt.Sprintf("%d orchestrator log lines dropped: the log writer fell behind", n))})
	}
}

func (s *sink) write(e entry) {
	if e.Time.IsZero() {
		e.Time = s.o.Now()
	}
	cl := formatCombined(e.Time, e.Source, e.Line)
	if s.o.Console != nil {
		s.o.Console.offer(cl)
	}
	if s.failed {
		return
	}
	f, err := s.file(e.Source)
	if err == nil {
		err = f.writeLine(formatComponent(e.Time, e.Line))
	}
	if err == nil {
		err = s.combined.writeLine(cl)
	}
	if err != nil {
		// Warn once and stop writing files. Container output keeps flowing
		// (and is discarded) so no producer ever blocks on a full disk.
		s.failed = true
		fmt.Fprintf(s.o.ErrOut, "astrosim: log files can no longer be written, file logging stopped: %v\n", err)
	}
}

func (s *sink) flushAll() {
	if s.failed {
		return
	}
	var err error
	for _, f := range s.all() {
		if ferr := f.flush(); err == nil {
			err = ferr
		}
	}
	if err != nil {
		s.failed = true
		fmt.Fprintf(s.o.ErrOut, "astrosim: log files can no longer be written, file logging stopped: %v\n", err)
	}
}

// all returns every open file, combined first, then the rest by name.
func (s *sink) all() []*partFile {
	out := []*partFile{s.combined}
	names := make([]string, 0, len(s.files))
	for n := range s.files {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		out = append(out, s.files[n])
	}
	return out
}

func (s *sink) snapshotFiles() ([]fileInfo, error) {
	var out []fileInfo
	for _, f := range s.all() {
		fi, err := f.infos()
		if err != nil {
			return nil, err
		}
		out = append(out, fi...)
	}
	return out, nil
}

func (s *sink) shutdown() {
	for _, f := range s.all() {
		f.close()
	}
	if s.o.Console != nil {
		s.o.Console.close(200 * time.Millisecond)
	}
}
