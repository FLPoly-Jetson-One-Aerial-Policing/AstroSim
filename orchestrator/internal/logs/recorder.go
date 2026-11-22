package logs

import (
	"archive/zip"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sync"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
)

// ErrBusy is returned by WriteZip while another WriteZip is running.
var ErrBusy = errors.New("logs: a download is already running")

// defaultFileMax is the per-file size cap when Options.FileMaxBytes is zero.
const defaultFileMax = 64 << 20

// LogSource is the part of the container runtime a Recorder needs to follow
// a container's output. runtime.Runtime satisfies it.
type LogSource interface {
	// FollowLogs streams the container's timestamped output.
	FollowLogs(ctx context.Context, name string, opts runtime.LogOptions, stdout, stderr io.Writer) error
	// Inspect reports whether the container is still running.
	Inspect(ctx context.Context, name string) (runtime.ContainerState, error)
}

// Options configures Open.
type Options struct {
	// Root is the directory holding all sessions; it is created if missing.
	Root string
	// SessionID identifies this run; it must be 16 hex digits, like the
	// orchestrator's session IDs, so the folder is recognised later.
	SessionID string
	// Version is the orchestrator version for the file headers.
	Version string
	// Start is the session start time; zero means now.
	Start time.Time
	// Source follows container logs. It may be nil if Begin is never used.
	Source LogSource
	// Console is the terminal. It receives the orchestrator's own lines
	// always and, with Verbose, every combined line. It may be nil.
	Console io.Writer
	// Verbose mirrors the combined log to Console.
	Verbose bool
	// FileMaxBytes caps each log file (see the package comment); zero means
	// 64 MiB and the minimum is 4 bytes.
	FileMaxBytes int64
	// Now supplies timestamps; nil uses time.Now.
	Now func() time.Time
}

// Recorder collects one session's logs into a folder. A nil *Recorder is a
// valid, disabled recorder: every method does nothing, so callers need no
// checks when logging could not be set up.
type Recorder struct {
	o    Options
	name string
	dir  string
	sink *sink

	downloading sync.Mutex // held by WriteZip; TryLock gives "one at a time"

	mu       sync.Mutex
	closed   bool
	captures []*Capture
	orch     *lineWriter
}

// Open creates the session folder and starts the writer.
func Open(o Options) (*Recorder, error) {
	if o.Now == nil {
		o.Now = time.Now
	}
	if o.Start.IsZero() {
		o.Start = o.Now()
	}
	if o.FileMaxBytes == 0 {
		o.FileMaxBytes = defaultFileMax
	}
	if !IsSessionName(SessionName(o.Start, o.SessionID)) {
		return nil, fmt.Errorf("logs: invalid session ID %q", o.SessionID)
	}
	r := &Recorder{o: o, name: SessionName(o.Start, o.SessionID)}
	r.dir = filepath.Join(o.Root, r.name)
	so := sinkOptions{
		Dir:     r.dir,
		Header:  header(o),
		FileMax: o.FileMaxBytes,
		ErrOut:  o.Console,
		Now:     o.Now,
	}
	if o.Verbose && o.Console != nil {
		so.Console = newConsole(o.Console, o.Now)
	}
	if err := os.MkdirAll(o.Root, 0o700); err != nil {
		return nil, err
	}
	var err error
	if r.sink, err = newSink(so); err != nil {
		return nil, err
	}
	r.orch = newLineWriter(func(line string) {
		r.sink.log(entry{Time: o.Now(), Source: orchestratorName, Line: line})
	})
	return r, nil
}

// header is the first line of every file.
func header(o Options) string {
	return fmt.Sprintf("# AstroSim %s session %s, started %s, times are local (UTC%s)",
		o.Version, o.SessionID, o.Start.UTC().Format(time.RFC3339), o.Now().Local().Format("-07:00"))
}

// Dir returns the session folder, or "" for a nil recorder.
func (r *Recorder) Dir() string {
	if r == nil {
		return ""
	}
	return r.dir
}

// Name returns the session folder name (the base name of Dir), or "" for a
// nil recorder.
func (r *Recorder) Name() string {
	if r == nil {
		return ""
	}
	return r.name
}

// OrchestratorWriter returns a writer for the orchestrator's own log output,
// for logging.New. Lines go to orchestrator.log and combined.log and never
// block the caller for long. Without Verbose they also go to Console as
// before; with Verbose the console sink prints them with the rest, so they
// appear exactly once. After Close only Console is written, so the final
// "stopped" lines still reach the terminal. It returns nil for a nil
// recorder.
func (r *Recorder) OrchestratorWriter() io.Writer {
	if r == nil {
		return nil
	}
	return orchWriter{r}
}

type orchWriter struct{ r *Recorder }

func (w orchWriter) Write(p []byte) (int, error) {
	r := w.r
	r.mu.Lock()
	closed := r.closed
	if !closed {
		r.orch.Write(p)
	}
	r.mu.Unlock()
	if (closed || !r.o.Verbose) && r.o.Console != nil {
		r.o.Console.Write(p)
	}
	return len(p), nil
}

// Note writes a marker line, "==== msg ====", to the component's file and to
// combined.log. The orchestrator calls it for events the container itself
// cannot log, such as its exit code.
func (r *Recorder) Note(component, msg string) {
	if r == nil {
		return
	}
	r.sink.logBlocking(entry{Time: r.o.Now(), Source: component, Line: marker(msg)})
}

// WriteZip writes a zip of the session's files as they are now to w, while
// logging continues. Each file is copied up to the size it had when the
// snapshot was taken, so the zip is consistent even if the file grows, and
// rotation is postponed meanwhile. Only one WriteZip runs at a time; a
// second returns ErrBusy at once.
func (r *Recorder) WriteZip(ctx context.Context, w io.Writer) error {
	if r == nil {
		return errors.New("logs: logging is not enabled")
	}
	if !r.downloading.TryLock() {
		return ErrBusy
	}
	defer r.downloading.Unlock()
	files, release, err := r.sink.snapshot(ctx)
	if err != nil {
		return err
	}
	defer release()
	zw := zip.NewWriter(w)
	for _, f := range files {
		p := filepath.Join(r.dir, f.Rel)
		fi, err := os.Stat(p)
		if err != nil {
			return err
		}
		if err := addFile(zw, p, r.name+"/"+f.Rel, fi, f.Size); err != nil {
			return err
		}
	}
	return zw.Close()
}

// Close stops all captures, writes everything queued, and closes the files,
// waiting at most until ctx ends. The folder is then ready for Archive.
func (r *Recorder) Close(ctx context.Context) error {
	if r == nil {
		return nil
	}
	r.mu.Lock()
	if r.closed {
		r.mu.Unlock()
		return nil
	}
	r.closed = true
	r.orch.Close()
	caps := r.captures
	r.mu.Unlock()
	for _, c := range caps {
		c.cancel()
	}
	for _, c := range caps {
		select {
		case <-c.done:
		case <-ctx.Done():
		}
	}
	return r.sink.close(ctx)
}

func (r *Recorder) track(c *Capture) {
	r.mu.Lock()
	r.captures = append(r.captures, c)
	r.mu.Unlock()
}
