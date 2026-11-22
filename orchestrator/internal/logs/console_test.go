package logs

import (
	"errors"
	"fmt"
	"regexp"
	"strings"
	"testing"
	"time"
)

func TestConsoleIdenticalLines(t *testing.T) {
	var out syncBuf
	c := newConsole(&out, time.Now)
	for i := 0; i < 100; i++ {
		c.offer(fmt.Sprintf("line %d", i))
	}
	c.close(time.Second)
	got := strings.Split(strings.TrimSuffix(out.String(), "\n"), "\n")
	if len(got) != 100 {
		t.Fatalf("got %d lines", len(got))
	}
	for i, l := range got {
		if l != fmt.Sprintf("line %d", i) {
			t.Fatalf("line %d = %q", i, l)
		}
	}
}

// gate is a writer that blocks until released, like a stalled terminal.
type gate struct {
	syncBuf
	open chan struct{}
}

func (g *gate) Write(p []byte) (int, error) {
	<-g.open
	return g.syncBuf.Write(p)
}

func TestConsoleDropsWithNotice(t *testing.T) {
	g := &gate{open: make(chan struct{})}
	c := newConsole(g, time.Now)
	start := time.Now()
	const sent = consoleQueue + 500
	for i := 0; i < sent; i++ {
		c.offer(fmt.Sprintf("line %d", i))
	}
	if time.Since(start) > time.Second {
		t.Fatal("offer blocked on a stalled terminal")
	}
	close(g.open)
	c.close(5 * time.Second)

	out := g.String()
	re := regexp.MustCompile(`\[(\d+) lines not shown on the console; see combined\.log\]`)
	m := re.FindStringSubmatch(out)
	if m == nil {
		t.Fatalf("no drop notice in output")
	}
	var dropped int
	fmt.Sscan(m[1], &dropped)
	shown := strings.Count(out, "line ")
	if dropped < 400 || shown+dropped != sent {
		t.Errorf("shown=%d dropped=%d sent=%d", shown, dropped, sent)
	}
	if !strings.Contains(out, " astrosim     | [") {
		t.Errorf("notice not in the combined format: %q", out[strings.Index(out, "20"):][:60])
	}
}

type failWriter struct{ calls int }

func (f *failWriter) Write([]byte) (int, error) { f.calls++; return 0, errors.New("EIO") }

func TestConsoleDeadTerminalIsAbandoned(t *testing.T) {
	w := &failWriter{}
	c := newConsole(w, time.Now)
	c.offer("first")
	deadline := time.Now().Add(2 * time.Second)
	for !c.dead.Load() && time.Now().Before(deadline) {
		time.Sleep(time.Millisecond)
	}
	if !c.dead.Load() {
		t.Fatal("console not marked dead after a write error")
	}
	start := time.Now()
	for i := 0; i < 100000; i++ {
		c.offer("more")
	}
	c.close(time.Second)
	if time.Since(start) > 2*time.Second || w.calls != 1 {
		t.Errorf("elapsed=%v writes=%d", time.Since(start), w.calls)
	}
}

func TestConsoleCloseBoundedOnHungTerminal(t *testing.T) {
	g := &gate{open: make(chan struct{})} // never opened
	c := newConsole(g, time.Now)
	c.offer("x")
	start := time.Now()
	c.close(50 * time.Millisecond)
	if time.Since(start) > time.Second {
		t.Error("close did not honour its timeout")
	}
	c.offer("after close") // must not panic
	c.close(time.Millisecond)
	close(g.open)
}
