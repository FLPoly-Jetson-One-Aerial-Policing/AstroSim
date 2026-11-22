package health

import (
	"context"
	"errors"
	"net"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime/fake"
)

// Toy protocol: the client sends "SUB<seq>", the server answers datagrams
// "OK" (healthy), "BAD" (unhealthy) or anything else (malformed).

func toySubscribe(seq uint32) []byte { return []byte{'S', 'U', 'B', byte(seq)} }

func toyClassify(b []byte) (Observation, bool) {
	switch string(b) {
	case "OK":
		return Observation{Healthy: true, Facts: []Fact{{Key: "s", Label: "State", Value: "ok"}}}, true
	case "BAD":
		return Observation{Healthy: false, Facts: []Fact{{Key: "s", Label: "State", Value: "bad"}}}, true
	}
	return Observation{}, false
}

// server is a loopback fake UDP publisher.
type server struct {
	t    *testing.T
	conn *net.UDPConn
	mu   sync.Mutex
	subs int
	peer *net.UDPAddr
	// autoReply, when non-empty, is sent back for every Subscribe.
	autoReply atomic.Value
	first     chan struct{}
}

func newServer(t *testing.T) *server {
	t.Helper()
	c, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	s := &server{t: t, conn: c, first: make(chan struct{})}
	s.autoReply.Store("")
	t.Cleanup(func() { c.Close() })
	go func() {
		buf := make([]byte, 64)
		for {
			n, from, err := c.ReadFromUDP(buf)
			if err != nil {
				return
			}
			if n < 3 || string(buf[:3]) != "SUB" {
				continue
			}
			s.mu.Lock()
			s.subs++
			if s.subs == 1 {
				close(s.first)
			}
			s.peer = from
			s.mu.Unlock()
			if r := s.autoReply.Load().(string); r != "" {
				_, _ = c.WriteToUDP([]byte(r), from)
			}
		}
	}()
	return s
}

func (s *server) addr() string { return s.conn.LocalAddr().String() }

func (s *server) subscribes() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.subs
}

// send pushes a datagram to the subscriber.
func (s *server) send(msg string) {
	s.mu.Lock()
	p := s.peer
	s.mu.Unlock()
	if p != nil {
		_, _ = s.conn.WriteToUDP([]byte(msg), p)
	}
}

func (s *server) probe(interval, minFact time.Duration) *UDPSubscribe {
	return &UDPSubscribe{
		Addr: s.addr(), Subscribe: toySubscribe, Classify: toyClassify,
		Interval: interval, MinFactInterval: minFact,
	}
}

func eventually(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(2 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

func TestUDPSubscribeResends(t *testing.T) {
	s := newServer(t)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- s.probe(10*time.Millisecond, 0).Run(ctx, func(Observation) {}) }()
	select {
	case <-s.first:
	case <-time.After(time.Second):
		t.Fatal("no immediate Subscribe")
	}
	eventually(t, "3 subscribes", func() bool { return s.subscribes() >= 3 })
	cancel()
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	n := s.subscribes()
	time.Sleep(50 * time.Millisecond)
	if s.subscribes() != n {
		t.Error("probe kept sending after cancel")
	}
}

func TestUDPSubscribeIgnoresMalformed(t *testing.T) {
	s := newServer(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	var reports atomic.Int32
	go s.probe(10*time.Millisecond, 0).Run(ctx, func(Observation) { reports.Add(1) })
	<-s.first
	for i := 0; i < 20; i++ {
		s.send("garbage")
		s.send("")
		time.Sleep(time.Millisecond)
	}
	s.send("OK") // a valid datagram after the junk proves the loop survived
	eventually(t, "valid report", func() bool { return reports.Load() == 1 })
}

func TestUDPSubscribeRateLimitAndEdges(t *testing.T) {
	s := newServer(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	var mu sync.Mutex
	var got []bool
	p := s.probe(time.Second, time.Hour) // unchanged health: effectively never again
	go p.Run(ctx, func(o Observation) { mu.Lock(); got = append(got, o.Healthy); mu.Unlock() })
	<-s.first
	count := func() int { mu.Lock(); defer mu.Unlock(); return len(got) }

	for i := 0; i < 50; i++ {
		s.send("OK")
	}
	eventually(t, "first report", func() bool { return count() >= 1 })
	time.Sleep(50 * time.Millisecond)
	if n := count(); n != 1 {
		t.Fatalf("rate limit: %d reports for 50 healthy datagrams, want 1", n)
	}
	// An edge goes through immediately despite the huge rate-limit window.
	s.send("BAD")
	eventually(t, "unhealthy edge", func() bool { return count() == 2 })
	s.send("OK")
	eventually(t, "healthy edge", func() bool { return count() == 3 })
	mu.Lock()
	defer mu.Unlock()
	if got[0] != true || got[1] != false || got[2] != true {
		t.Errorf("reports = %v", got)
	}
}

func TestUDPSubscribeRateLimitWithClock(t *testing.T) {
	s := newServer(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	var clock atomic.Int64 // nanoseconds
	p := s.probe(time.Second, 250*time.Millisecond)
	p.Now = func() time.Time { return time.Unix(0, clock.Load()) }
	var reports atomic.Int32
	go p.Run(ctx, func(Observation) { reports.Add(1) })
	<-s.first
	s.send("OK")
	eventually(t, "1", func() bool { return reports.Load() == 1 })
	clock.Store(int64(100 * time.Millisecond))
	s.send("OK")
	time.Sleep(30 * time.Millisecond)
	if reports.Load() != 1 {
		t.Fatal("report inside the window")
	}
	clock.Store(int64(300 * time.Millisecond))
	s.send("OK")
	eventually(t, "2", func() bool { return reports.Load() == 2 })
}

func TestUDPSubscribeConfigErrors(t *testing.T) {
	if err := (&UDPSubscribe{Addr: "127.0.0.1:1"}).Run(context.Background(), nil); err == nil {
		t.Error("missing Subscribe/Classify should fail")
	}
	p := &UDPSubscribe{Addr: "not an address", Subscribe: toySubscribe, Classify: toyClassify}
	if err := p.Run(context.Background(), nil); err == nil {
		t.Error("bad address should fail")
	}
}

// TestMonitorOverUDP exercises probe and monitor together: ready edge, then
// the unhealthy edge once the server goes silent.
func TestMonitorOverUDP(t *testing.T) {
	s := newServer(t)
	s.autoReply.Store("OK")
	ready := make(chan struct{}, 4)
	unhealthy := make(chan struct{}, 4)
	m := NewMonitor(s.probe(10*time.Millisecond, time.Millisecond), MonitorConfig{
		LivenessTimeout: 150 * time.Millisecond,
		OnReady:         func() { ready <- struct{}{} },
		OnUnhealthy:     func() { unhealthy <- struct{}{} },
	})
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- m.Run(ctx) }()

	select {
	case <-ready:
	case <-time.After(2 * time.Second):
		t.Fatal("never ready")
	}
	if f := m.Facts(); len(f) != 1 || f[0].Value != "ok" {
		t.Errorf("facts = %v", f)
	}
	select {
	case <-unhealthy:
		t.Fatal("unhealthy while the server is answering")
	case <-time.After(300 * time.Millisecond):
	}
	s.autoReply.Store("") // the engine goes silent
	select {
	case <-unhealthy:
	case <-time.After(2 * time.Second):
		t.Fatal("no unhealthy edge after silence")
	}
	cancel()
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	if len(ready) != 0 || len(unhealthy) != 0 {
		t.Error("callbacks fired more than once")
	}
}

func TestMonitorNeverReadyOnMalformed(t *testing.T) {
	s := newServer(t)
	s.autoReply.Store("junk")
	var fired atomic.Int32
	m := NewMonitor(s.probe(10*time.Millisecond, 0), MonitorConfig{
		LivenessTimeout: 30 * time.Millisecond,
		OnReady:         func() { fired.Add(1) },
		OnUnhealthy:     func() { fired.Add(1) },
	})
	ctx, cancel := context.WithTimeout(context.Background(), 200*time.Millisecond)
	defer cancel()
	_ = m.Run(ctx)
	if fired.Load() != 0 || m.Ready() || len(m.Facts()) != 0 {
		t.Errorf("fired=%d ready=%v facts=%v", fired.Load(), m.Ready(), m.Facts())
	}
}

// fakeClock is a manually advanced clock.
type fakeClock struct{ t time.Time }

func (c *fakeClock) now() time.Time          { return c.t }
func (c *fakeClock) advance(d time.Duration) { c.t = c.t.Add(d) }

func TestMonitorStateMachine(t *testing.T) {
	clk := &fakeClock{t: time.Unix(1000, 0)}
	var ready, unhealthy int
	m := NewMonitor(nil, MonitorConfig{
		LivenessTimeout: 2 * time.Second,
		OnReady:         func() { ready++ },
		OnUnhealthy:     func() { unhealthy++ },
		Now:             clk.now,
	})
	healthy := Observation{Healthy: true, Facts: []Fact{{Key: "a", Value: "1"}}}
	bad := Observation{Facts: []Fact{{Key: "a", Value: "0"}}}

	// Silence before ready never triggers the unhealthy edge.
	clk.advance(time.Hour)
	m.Check()
	if unhealthy != 0 {
		t.Fatal("unhealthy before ready")
	}
	// An unhealthy observation updates facts but does not make it ready.
	m.Observe(bad)
	if ready != 0 || m.Ready() || m.Facts()[0].Value != "0" {
		t.Fatalf("ready=%d facts=%v", ready, m.Facts())
	}
	m.Observe(healthy)
	m.Observe(healthy)
	if ready != 1 || !m.Ready() {
		t.Fatalf("ready fired %d times", ready)
	}
	// Healthy observations refresh liveness.
	clk.advance(1500 * time.Millisecond)
	m.Observe(healthy)
	clk.advance(1500 * time.Millisecond)
	m.Check()
	if unhealthy != 0 {
		t.Fatal("unhealthy despite recent healthy observation")
	}
	// Unhealthy observations do not refresh liveness.
	m.Observe(bad)
	clk.advance(600 * time.Millisecond)
	m.Check()
	if unhealthy != 1 {
		t.Fatalf("unhealthy fired %d times, want 1", unhealthy)
	}
	// Edge fires once only.
	clk.advance(time.Hour)
	m.Check()
	m.Observe(healthy)
	if unhealthy != 1 || ready != 1 {
		t.Errorf("ready=%d unhealthy=%d", ready, unhealthy)
	}
}

func TestMonitorFactsAreCopied(t *testing.T) {
	m := NewMonitor(nil, MonitorConfig{LivenessTimeout: time.Second})
	in := []Fact{{Key: "k", Value: "v"}}
	m.Observe(Observation{Facts: in})
	in[0].Value = "mutated"
	out := m.Facts()
	if out[0].Value != "v" {
		t.Error("input aliasing")
	}
	out[0].Value = "mutated"
	if m.Facts()[0].Value != "v" {
		t.Error("output aliasing")
	}
}

type erroringProbe struct{}

func (erroringProbe) Run(context.Context, func(Observation)) error { return errors.New("boom") }

func TestMonitorReturnsProbeError(t *testing.T) {
	m := NewMonitor(erroringProbe{}, MonitorConfig{LivenessTimeout: time.Second})
	if err := m.Run(context.Background()); err == nil || err.Error() != "boom" {
		t.Errorf("err = %v", err)
	}
}

func collect(t *testing.T, p Probe) (<-chan Observation, context.CancelFunc) {
	t.Helper()
	ctx, cancel := context.WithCancel(context.Background())
	ch := make(chan Observation, 64)
	go func() {
		_ = p.Run(ctx, func(o Observation) {
			select {
			case ch <- o:
			default:
			}
		})
	}()
	return ch, cancel
}

func next(t *testing.T, ch <-chan Observation, want bool) Observation {
	t.Helper()
	timeout := time.After(2 * time.Second)
	for {
		select {
		case o := <-ch:
			if o.Healthy == want {
				return o
			}
		case <-timeout:
			t.Fatalf("no observation with Healthy=%v", want)
		}
	}
}

func TestContainerProbes(t *testing.T) {
	ctx := context.Background()
	rt := fake.New()
	rt.SetImage("img", true)
	_, _ = rt.Run(ctx, runtime.RunSpec{Name: "c", Image: "img"})

	ch, cancel := collect(t, &ContainerHealth{Runtime: rt, Name: "c", Interval: 5 * time.Millisecond})
	defer cancel()
	next(t, ch, true)
	rt.SetHealth("c", "unhealthy")
	o := next(t, ch, false)
	if len(o.Facts) != 2 {
		t.Errorf("facts = %v", o.Facts)
	}
	rt.SetHealth("c", "healthy")
	next(t, ch, true)
	rt.Kill("c")
	next(t, ch, false)

	// Running ignores the image health check.
	rt2 := fake.New()
	rt2.SetImage("img", true)
	_, _ = rt2.Run(ctx, runtime.RunSpec{Name: "r", Image: "img"})
	rt2.SetHealth("r", "unhealthy")
	ch2, cancel2 := collect(t, &Running{Runtime: rt2, Name: "r", Interval: 5 * time.Millisecond})
	defer cancel2()
	next(t, ch2, true)
	rt2.Kill("r")
	next(t, ch2, false)
}

func TestContainerProbeMissingContainer(t *testing.T) {
	rt := fake.New()
	ch, cancel := collect(t, &Running{Runtime: rt, Name: "nope", Interval: 5 * time.Millisecond})
	defer cancel()
	o := next(t, ch, false)
	if len(o.Facts) != 1 || o.Facts[0].Value != "missing" {
		t.Errorf("facts = %v", o.Facts)
	}
}
