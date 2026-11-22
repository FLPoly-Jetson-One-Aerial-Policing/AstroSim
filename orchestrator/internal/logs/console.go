package logs

import (
	"fmt"
	"io"
	"sync"
	"sync/atomic"
	"time"
)

// consoleQueue is how many lines may wait for the terminal before further
// lines are dropped.
const consoleQueue = 1024

// console mirrors log lines to a terminal (--verbose) without ever slowing
// the logger down. Lines go through a bounded queue with a non-blocking
// offer; a terminal that cannot keep up makes lines drop, and the next line
// shown is preceded by a notice saying how many were not shown. A terminal
// that fails a write (closed, hung up) is abandoned: everything is discarded
// from then on.
type console struct {
	w       io.Writer
	now     func() time.Time
	q       chan string
	dropped atomic.Int64
	dead    atomic.Bool
	done    chan struct{}

	mu     sync.Mutex // guards closed and sending on q
	closed bool
}

// newConsole starts the writer goroutine. now supplies the timestamp of the
// drop notice.
func newConsole(w io.Writer, now func() time.Time) *console {
	c := &console{w: w, now: now, q: make(chan string, consoleQueue), done: make(chan struct{})}
	go c.run()
	return c
}

// offer queues line for the terminal and never blocks.
func (c *console) offer(line string) {
	if c.dead.Load() {
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return
	}
	select {
	case c.q <- line:
	default:
		c.dropped.Add(1)
	}
}

func (c *console) run() {
	defer close(c.done)
	for line := range c.q {
		if c.dead.Load() {
			continue
		}
		c.notice()
		c.write(line)
	}
	c.notice()
}

// notice reports lines dropped since the last one shown.
func (c *console) notice() {
	if n := c.dropped.Swap(0); n > 0 && !c.dead.Load() {
		c.write(formatCombined(c.now(), "astrosim",
			fmt.Sprintf("[%d lines not shown on the console; see combined.log]", n)))
	}
}

func (c *console) write(line string) {
	if _, err := io.WriteString(c.w, line+"\n"); err != nil {
		c.dead.Store(true)
	}
}

// close stops accepting lines and waits up to timeout for the queue to be
// written. A terminal that never returns from a write is left behind.
func (c *console) close(timeout time.Duration) {
	c.mu.Lock()
	if !c.closed {
		c.closed = true
		close(c.q)
	}
	c.mu.Unlock()
	select {
	case <-c.done:
	case <-time.After(timeout):
	}
}
