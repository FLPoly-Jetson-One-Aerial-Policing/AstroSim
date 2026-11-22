package health

import (
	"context"
	"sync"
	"time"
)

// MonitorConfig configures a Monitor.
type MonitorConfig struct {
	// LivenessTimeout is how long a ready component may go without a
	// healthy observation before OnUnhealthy fires. Must be positive.
	LivenessTimeout time.Duration
	// OnReady is called once, on the first healthy observation. May be nil.
	OnReady func()
	// OnUnhealthy is called once, when a component that became ready has had
	// no healthy observation for LivenessTimeout. May be nil.
	OnUnhealthy func()
	// Now supplies the time; nil means time.Now. Tests inject a fake clock
	// and drive Check themselves.
	Now func() time.Time
}

// Monitor turns a Probe's observations into readiness and liveness edges.
//
// A Monitor serves one launch of one component: after OnUnhealthy the
// supervisor tears the container down and creates a new Monitor for the
// replacement, so both callbacks fire at most once. Callbacks run on the
// goroutine that detected the edge, with no Monitor lock held, so they may
// call back into the Monitor.
type Monitor struct {
	probe Probe
	cfg   MonitorConfig

	mu          sync.Mutex
	ready       bool
	unhealthy   bool
	lastHealthy time.Time
	facts       []Fact
}

// NewMonitor returns a Monitor for probe.
func NewMonitor(probe Probe, cfg MonitorConfig) *Monitor {
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	return &Monitor{probe: probe, cfg: cfg}
}

// Run runs the probe and the liveness check until ctx is done. It returns
// the probe's error, if it failed, or nil on cancellation.
func (m *Monitor) Run(ctx context.Context) error {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()

	// Check often enough that the unhealthy edge is late by at most a
	// quarter of the timeout.
	period := m.cfg.LivenessTimeout / 4
	if period < time.Millisecond {
		period = time.Millisecond
	}
	done := make(chan struct{})
	go func() {
		defer close(done)
		t := time.NewTicker(period)
		defer t.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-t.C:
				m.Check()
			}
		}
	}()
	err := m.probe.Run(ctx, m.Observe)
	cancel()
	<-done
	return err
}

// Observe feeds one observation into the Monitor. It is what Run passes to
// the probe, and it is exported so tests can drive the state machine directly.
func (m *Monitor) Observe(o Observation) {
	m.mu.Lock()
	m.facts = append([]Fact(nil), o.Facts...)
	fire := false
	if o.Healthy {
		m.lastHealthy = m.cfg.Now()
		if !m.ready {
			m.ready, fire = true, true
		}
	}
	m.mu.Unlock()
	if fire && m.cfg.OnReady != nil {
		m.cfg.OnReady()
	}
}

// Check evaluates liveness against the clock and fires OnUnhealthy if a
// ready component has been without a healthy observation for too long.
func (m *Monitor) Check() {
	m.mu.Lock()
	fire := m.ready && !m.unhealthy && m.cfg.Now().Sub(m.lastHealthy) >= m.cfg.LivenessTimeout
	if fire {
		m.unhealthy = true
	}
	m.mu.Unlock()
	if fire && m.cfg.OnUnhealthy != nil {
		m.cfg.OnUnhealthy()
	}
}

// Facts returns a copy of the facts from the latest observation.
func (m *Monitor) Facts() []Fact {
	m.mu.Lock()
	defer m.mu.Unlock()
	return append([]Fact(nil), m.facts...)
}

// Ready reports whether a healthy observation has been seen.
func (m *Monitor) Ready() bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.ready
}
