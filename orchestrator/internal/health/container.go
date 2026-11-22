package health

import (
	"context"
	"errors"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
)

// DefaultPollInterval is how often the container probes inspect the container.
const DefaultPollInterval = time.Second

// poll runs check every interval (and once immediately) until ctx is done.
func poll(ctx context.Context, interval time.Duration, check func()) {
	if interval <= 0 {
		interval = DefaultPollInterval
	}
	t := time.NewTicker(interval)
	defer t.Stop()
	for {
		check()
		select {
		case <-ctx.Done():
			return
		case <-t.C:
		}
	}
}

// inspect fetches the container state. ok=false means there is nothing to
// report: either the daemon hiccuped (stay silent, the Monitor's liveness
// timeout covers a persistent outage) or the container is gone, in which
// case gone=true.
func inspect(ctx context.Context, rt runtime.Runtime, name string) (st runtime.ContainerState, gone, ok bool) {
	st, err := rt.Inspect(ctx, name)
	switch {
	case err == nil:
		return st, false, true
	case errors.Is(err, runtime.ErrNotFound):
		return st, true, true
	default:
		return st, false, false
	}
}

// ContainerHealth reports a container healthy when it is running and, if its
// image defines a HEALTHCHECK, that check passes. It is for components that
// cannot be probed over the network.
type ContainerHealth struct {
	// Runtime inspects the container.
	Runtime runtime.Runtime
	// Name is the container name.
	Name string
	// Interval is the poll period; zero means DefaultPollInterval.
	Interval time.Duration
}

// Run implements Probe.
func (p *ContainerHealth) Run(ctx context.Context, report func(Observation)) error {
	poll(ctx, p.Interval, func() {
		st, gone, ok := inspect(ctx, p.Runtime, p.Name)
		if !ok {
			return
		}
		if gone {
			report(Observation{Facts: []Fact{{Key: "container", Label: "Container", Value: "missing"}}})
			return
		}
		healthy := st.Running && (st.Health == "" || st.Health == "healthy")
		facts := []Fact{{Key: "container", Label: "Container", Value: st.Status}}
		if st.Health != "" {
			facts = append(facts, Fact{Key: "health", Label: "Health check", Value: st.Health})
		}
		report(Observation{Healthy: healthy, Facts: facts})
	})
	return nil
}

// Running reports a container healthy as soon as it is running, ignoring any
// image health check. It is the weakest probe: "the process is alive".
type Running struct {
	// Runtime inspects the container.
	Runtime runtime.Runtime
	// Name is the container name.
	Name string
	// Interval is the poll period; zero means DefaultPollInterval.
	Interval time.Duration
}

// Run implements Probe.
func (p *Running) Run(ctx context.Context, report func(Observation)) error {
	poll(ctx, p.Interval, func() {
		st, gone, ok := inspect(ctx, p.Runtime, p.Name)
		if !ok {
			return
		}
		if gone {
			report(Observation{Facts: []Fact{{Key: "container", Label: "Container", Value: "missing"}}})
			return
		}
		report(Observation{
			Healthy: st.Running,
			Facts:   []Fact{{Key: "container", Label: "Container", Value: st.Status}},
		})
	})
	return nil
}
