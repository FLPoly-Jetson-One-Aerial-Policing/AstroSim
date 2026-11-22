// Package engine defines the physics engine component: the Gazebo-based
// container built from gazebo-container/ that publishes simulation state.
package engine

import (
	"fmt"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/component"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/components/format"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/health"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/wire"
)

const (
	// ID is the component ID, also its network alias.
	ID = "engine"
	// Port is the UDP port the engine's state stream is served on.
	Port = 8765
	// Image is the engine image reference.
	Image = "astrosim-engine"

	// partition is the gz-transport partition used in host network mode.
	partition = "astrosim"

	// rateWindow is the minimum measuring interval for the update rate.
	rateWindow = time.Second
)

// Definition returns the engine component.
func Definition() component.Definition {
	return component.Definition{
		ID:          ID,
		DisplayName: "Physics engine",
		Description: "Simulates the world and the aircraft's physics.",
		Image:       Image,
		BuildHint:   "{runtime} build -t astrosim-engine -f gazebo-container/Dockerfile .",
		Ports:       []component.PortSpec{{Name: "state", Proto: "udp", Port: Port}},
		Env:         env,
		// Engine and flight computer restart together: PX4 cannot survive
		// the engine's simulation clock resetting.
		RestartGroup: "sim",
		Probe: func(sc component.StackContext) health.Probe {
			return &health.UDPSubscribe{
				Addr:      sc.ProbeAddr(Port),
				Subscribe: func(seq uint32) []byte { return wire.EngineSubscribe{Seq: seq}.Encode() },
				Classify:  NewClassifier(time.Now),
			}
		},
		ReadyTimeout:    60 * time.Second,
		LivenessTimeout: 3 * time.Second,
		StopTimeout:     3 * time.Second,
		// Exit status 2 is the container's "invalid configuration" status.
		PermanentExitCodes: []int{2},
	}
}

// env returns the container environment. In bridge mode the image's own
// defaults apply and nothing is set.
//
// In host mode the container shares the host's network stack, and
// gz-transport (the engine's internal messaging between gz-sim-server and
// the gateway) would then do its discovery by multicast on the host's real
// interfaces and join whatever partition matches "<hostname>:<user>". Any
// other Gazebo on the host or the LAN with the same default partition could
// then see this engine's topics, and the engine would see theirs. Pinning a
// private partition and the loopback address keeps the traffic inside the
// host; both processes are in this one container, so they still find each
// other. In bridge mode the container's own network namespace already
// confines this traffic, so nothing needs setting. (The image does not set
// either variable; this is deduced from gz-transport's documented defaults,
// not from an observed capture.)
func env(sc component.StackContext) map[string]string {
	if sc.NetworkMode != component.NetworkHost {
		return nil
	}
	return map[string]string{
		"GZ_PARTITION": partition,
		"GZ_IP":        "127.0.0.1",
	}
}

// NewClassifier returns a health.UDPSubscribe Classify function for the
// engine's State stream. Healthy means the engine reports ENGINE_OK. The
// returned function keeps the receive-rate estimate, so use one per probe;
// it is not safe for concurrent use. now supplies the clock.
func NewClassifier(now func() time.Time) func([]byte) (health.Observation, bool) {
	var (
		windowStart time.Time
		count       int
		rate        = "measuring"
	)
	return func(b []byte) (health.Observation, bool) {
		st, err := wire.DecodeState(b)
		if err != nil {
			return health.Observation{}, false
		}
		t := now()
		if windowStart.IsZero() {
			windowStart = t
		}
		count++
		if el := t.Sub(windowStart); el >= rateWindow {
			rate = fmt.Sprintf("%.0f per second", float64(count)/el.Seconds())
			windowStart, count = t, 0
		}
		return health.Observation{
			Healthy: st.EngineOK(),
			Facts: []health.Fact{
				{Key: "sim_time", Label: "Simulation time", Value: format.SimTime(st.SimTime)},
				{Key: "state_rate", Label: "Updates received", Value: rate},
			},
		}, true
	}
}
