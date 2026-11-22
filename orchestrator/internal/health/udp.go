package health

import (
	"context"
	"errors"
	"fmt"
	"net"
	"sync"
	"time"
)

// Defaults for UDPSubscribe.
const (
	// DefaultSubscribeInterval resends Subscribe once per second, the
	// minimum keepalive rate both wire protocols require.
	DefaultSubscribeInterval = time.Second
	// DefaultMinFactInterval caps unchanged-health reports at about 4 per second.
	DefaultMinFactInterval = 250 * time.Millisecond
	// maxDatagram comfortably exceeds the largest protocol message (117 bytes).
	maxDatagram = 2048
)

// UDPSubscribe probes a UDP publisher that streams data to subscribers, as
// both the engine and the flight computer do. It uses one ephemeral UDP
// socket: it sends a Subscribe datagram immediately and every Interval (the
// keepalive), and classifies every datagram it receives.
type UDPSubscribe struct {
	// Addr is the publisher's host:port as reachable from the orchestrator.
	Addr string
	// Subscribe builds the Subscribe datagram for the given sequence number
	// (starting at 1 and incrementing per send).
	Subscribe func(seq uint32) []byte
	// Classify decodes one received datagram. It returns ok=false for
	// anything malformed, which is dropped without any report.
	Classify func(datagram []byte) (obs Observation, ok bool)
	// Interval is the Subscribe resend period; zero means
	// DefaultSubscribeInterval.
	Interval time.Duration
	// MinFactInterval is the minimum time between reports while the health
	// value is unchanged; zero means DefaultMinFactInterval. A change of
	// Healthy is always reported at once.
	MinFactInterval time.Duration
	// Now supplies the time for rate limiting; nil means time.Now.
	Now func() time.Time
}

// Run implements Probe.
func (p *UDPSubscribe) Run(ctx context.Context, report func(Observation)) error {
	if p.Subscribe == nil || p.Classify == nil {
		return errors.New("health: UDPSubscribe needs Subscribe and Classify")
	}
	interval := p.Interval
	if interval <= 0 {
		interval = DefaultSubscribeInterval
	}
	minGap := p.MinFactInterval
	if minGap <= 0 {
		minGap = DefaultMinFactInterval
	}
	now := p.Now
	if now == nil {
		now = time.Now
	}

	// A connected socket gets only datagrams from the publisher's address.
	conn, err := net.Dial("udp", p.Addr)
	if err != nil {
		return fmt.Errorf("health: dial %s: %w", p.Addr, err)
	}

	var wg sync.WaitGroup
	stop := make(chan struct{})
	// Closing the connection is what unblocks the reader on cancellation.
	wg.Add(1)
	go func() {
		defer wg.Done()
		select {
		case <-ctx.Done():
		case <-stop:
		}
		conn.Close()
	}()
	// The sender keeps the subscription alive. Send errors (for example a
	// connection-refused echo while the container is still starting) are
	// expected and ignored: the next tick retries.
	wg.Add(1)
	go func() {
		defer wg.Done()
		t := time.NewTicker(interval)
		defer t.Stop()
		var seq uint32
		for {
			seq++
			_, _ = conn.Write(p.Subscribe(seq))
			select {
			case <-ctx.Done():
				return
			case <-stop:
				return
			case <-t.C:
			}
		}
	}()
	defer func() { close(stop); wg.Wait() }()

	var (
		lastHealthy bool
		lastReport  time.Time
		reported    bool
		buf         = make([]byte, maxDatagram)
	)
	for {
		n, err := conn.Read(buf)
		if err != nil {
			if ctx.Err() != nil || errors.Is(err, net.ErrClosed) {
				return nil
			}
			// ECONNREFUSED and friends arrive as read errors on a
			// connected UDP socket; they are transient.
			continue
		}
		obs, ok := p.Classify(buf[:n])
		if !ok {
			continue
		}
		t := now()
		if reported && obs.Healthy == lastHealthy && t.Sub(lastReport) < minGap {
			continue
		}
		reported, lastHealthy, lastReport = true, obs.Healthy, t
		report(obs)
	}
}
