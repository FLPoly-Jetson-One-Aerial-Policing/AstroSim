package logs

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
)

// Tunables of the capture loop. They are variables so tests can shorten them.
var (
	// maxReconnects is how often a follow that ended while the container
	// still runs is resumed before capture gives up.
	maxReconnects = 5
	// reconnectBackoff is the pause before resuming such a follow.
	reconnectBackoff = time.Second
	// gapFillTimeout bounds the one-shot read in Drain.
	gapFillTimeout = 2 * time.Second
	// stopWait bounds how long Drain waits for a cancelled follower to exit.
	stopWait = time.Second
)

// tailLines is how many recent lines a Capture keeps for Tail.
const tailLines = 200

// Launch describes one started container generation.
type Launch struct {
	// Component is the component ID; it names the component's log file.
	Component string
	// Container is the container name.
	Container string
	// ContainerID is the runtime's container ID, shortened for the marker.
	ContainerID string
	// Image is the image reference.
	Image string
	// Gen is the launch generation, counting restarts.
	Gen uint64
}

// Capture follows one container's output into the session log. Create it
// with Recorder.Begin. A nil *Capture is valid and does nothing.
type Capture struct {
	r      *Recorder
	l      Launch
	cancel context.CancelFunc
	ctx    context.Context
	done   chan struct{}

	mu     sync.Mutex
	ring   []string
	lastTS time.Time
}

// Begin records a launch marker and starts following the container's output
// in the background. The follow replays from container creation, so no
// output is missed if Begin is called right after the container starts.
// The capture ends by itself when the container exits.
func (r *Recorder) Begin(l Launch) *Capture {
	if r == nil {
		return nil
	}
	id := l.ContainerID
	if len(id) > 12 {
		id = id[:12]
	}
	r.Note(l.Component, fmt.Sprintf("launch gen=%d container=%s id=%s image=%s", l.Gen, l.Container, id, l.Image))
	ctx, cancel := context.WithCancel(context.Background())
	c := &Capture{r: r, l: l, ctx: ctx, cancel: cancel, done: make(chan struct{})}
	r.track(c)
	go c.run()
	return c
}

// emitter returns a lineWriter delivering lines to the log and the tail ring.
func (c *Capture) emitter(count *atomic.Int64) *lineWriter {
	return newLineWriter(func(line string) {
		if ts, rest, ok := splitRuntimeTimestamp(line); ok {
			line = rest
			c.mu.Lock()
			if ts.After(c.lastTS) {
				c.lastTS = ts
			}
			c.mu.Unlock()
		}
		c.mu.Lock()
		c.ring = append(c.ring, line)
		if len(c.ring) > tailLines {
			c.ring = append(c.ring[:0], c.ring[len(c.ring)-tailLines:]...)
		}
		c.mu.Unlock()
		if count != nil {
			count.Add(1)
		}
		// The host receive time, not the runtime's, stamps the line (see the
		// package comment); the runtime's is only for --since.
		c.r.sink.logBlocking(entry{Time: c.r.o.Now(), Source: c.l.Component, Line: line})
	})
}

func (c *Capture) since() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.lastTS
}

func (c *Capture) mark(text string) {
	c.r.sink.logBlocking(entry{Time: c.r.o.Now(), Source: c.l.Component, Line: marker(text)})
}

// run follows the container until it exits, the capture is cancelled, or
// following is impossible. A follow that ends while the container still
// runs (some engines do that) is resumed from the last line seen.
func (c *Capture) run() {
	defer close(c.done)
	for attempt := 0; ; attempt++ {
		out, errw := c.emitter(nil), c.emitter(nil)
		err := c.r.o.Source.FollowLogs(c.ctx, c.l.Container,
			runtime.LogOptions{Follow: true, Since: c.since()}, out, errw)
		out.Close()
		errw.Close()
		if c.ctx.Err() != nil {
			return
		}
		if err != nil {
			if !errors.Is(err, runtime.ErrNotFound) {
				c.ended(fmt.Sprintf("capture ended: %v", err))
			}
			return
		}
		st, ierr := c.r.o.Source.Inspect(c.ctx, c.l.Container)
		if c.ctx.Err() != nil || errors.Is(ierr, runtime.ErrNotFound) {
			return
		}
		if ierr != nil {
			c.ended(fmt.Sprintf("capture ended: cannot inspect the container: %v", ierr))
			return
		}
		if !st.Running {
			return
		}
		if attempt >= maxReconnects {
			c.ended("capture ended: the log stream keeps ending while the container runs")
			return
		}
		select {
		case <-time.After(reconnectBackoff):
		case <-c.ctx.Done():
			return
		}
		since := c.since()
		if since.IsZero() {
			c.mark("reconnected")
		} else {
			c.mark("reconnected (since " + since.UTC().Format(time.RFC3339Nano) + ")")
		}
	}
}

// ended records that capture stopped for a reason the user should know: a
// marker in the component's file and one warning from the orchestrator.
func (c *Capture) ended(text string) {
	c.mark(text)
	c.r.sink.log(entry{Time: c.r.o.Now(), Source: orchestratorName,
		Line: fmt.Sprintf("WARN log capture for %s stopped: %s", c.l.Component, strings.TrimPrefix(text, "capture ended: "))})
}

// Drain waits until everything the container wrote has been recorded, or
// until ctx ends. Call it after stopping the container and before removing
// it. If the follow has not finished when ctx ends (some engines never end
// a follow), it is cancelled and one non-follow read fetches what was
// missed, with a marker saying so.
func (c *Capture) Drain(ctx context.Context) {
	if c == nil {
		return
	}
	select {
	case <-c.done:
		return
	case <-ctx.Done():
	}
	c.cancel()
	select {
	case <-c.done:
	case <-time.After(stopWait):
	}
	var n atomic.Int64
	gctx, cancel := context.WithTimeout(context.Background(), gapFillTimeout)
	defer cancel()
	out, errw := c.emitter(&n), c.emitter(&n)
	err := c.r.o.Source.FollowLogs(gctx, c.l.Container, runtime.LogOptions{Since: c.since()}, out, errw)
	out.Close()
	errw.Close()
	switch {
	case n.Load() > 0:
		c.mark(fmt.Sprintf("%d lines fetched after the stream did not end", n.Load()))
	case err != nil && !errors.Is(err, runtime.ErrNotFound):
		c.mark(fmt.Sprintf("could not fetch the remaining output: %v", err))
	}
}

// Tail returns up to n of the most recent lines the container wrote (at
// most 200), oldest first, without timestamps.
func (c *Capture) Tail(n int) []string {
	if c == nil || n <= 0 {
		return nil
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if n > len(c.ring) {
		n = len(c.ring)
	}
	return append([]string(nil), c.ring[len(c.ring)-n:]...)
}
