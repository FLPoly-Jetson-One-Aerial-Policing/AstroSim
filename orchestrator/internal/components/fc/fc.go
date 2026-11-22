// Package fc defines the flight computer component: the PX4-based container
// built from fc-container/ that flies the aircraft and publishes telemetry.
package fc

import (
	"fmt"
	"math"
	"strconv"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/component"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/components/engine"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/components/format"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/health"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/wire"
)

const (
	// ID is the component ID, also its network alias.
	ID = "fc"
	// Port is the UDP port the telemetry stream is served on.
	Port = 8766
	// Image is the flight computer image reference.
	Image = "astrosim-fc"
)

// Definition returns the flight computer component.
func Definition() component.Definition {
	return component.Definition{
		ID:          ID,
		DisplayName: "Flight computer",
		Description: "Runs the autopilot that flies the aircraft.",
		Image:       Image,
		BuildHint:   "{runtime} build -t astrosim-fc -f fc-container/Dockerfile .",
		Ports:       []component.PortSpec{{Name: "telemetry", Proto: "udp", Port: Port}},
		Env:         env,
		// Started, not Ready: the pilot script runs on engine simulation
		// time, so the two must come up together.
		DependsOn:    []component.Dependency{{ID: engine.ID, Condition: component.Started}},
		RestartGroup: "sim",
		Probe: func(sc component.StackContext) health.Probe {
			return &health.UDPSubscribe{
				Addr:      sc.ProbeAddr(Port),
				Subscribe: func(seq uint32) []byte { return wire.FCSubscribe{Seq: seq}.Encode() },
				Classify:  Classify,
			}
		},
		ReadyTimeout:       90 * time.Second,
		LivenessTimeout:    5 * time.Second,
		StopTimeout:        3 * time.Second,
		PermanentExitCodes: []int{2},
	}
}

// env points the flight computer at the engine and fixes its telemetry
// port, using the environment overrides documented in fc-container/README.md
// (every config key [section] key is ASTROSIM_<SECTION>_<KEY>).
func env(sc component.StackContext) map[string]string {
	return map[string]string{
		"ASTROSIM_ENGINE_HOST":    sc.ServiceHost(engine.ID),
		"ASTROSIM_ENGINE_PORT":    strconv.Itoa(engine.Port),
		"ASTROSIM_TELEMETRY_PORT": strconv.Itoa(Port),
	}
}

// Classify is the health.UDPSubscribe Classify function for the telemetry
// stream. Healthy means the flight computer is connected to its autopilot.
func Classify(b []byte) (health.Observation, bool) {
	t, err := wire.DecodeTelemetry(b)
	if err != nil {
		return health.Observation{}, false
	}
	return health.Observation{
		Healthy: t.Connected(),
		Facts: []health.Fact{
			{Key: "link", Label: "Autopilot link", Value: yesNo(t.Connected(), "connected", "not connected")},
			{Key: "armed", Label: "Armed", Value: yesNo(t.Status&wire.StatusArmed != 0, "yes", "no")},
			{Key: "mode", Label: "Flight mode", Value: modeText(t)},
			{Key: "gps", Label: "GPS", Value: gpsText(t)},
			{Key: "altitude", Label: "Height above ground", Value: altitudeText(t.AltAGL)},
			{Key: "sim_time", Label: "Simulation time", Value: format.SimTime(t.TimeS)},
		},
	}, true
}

func yesNo(v bool, yes, no string) string {
	if v {
		return yes
	}
	return no
}

func modeText(t wire.Telemetry) string {
	var s string
	switch t.Mode {
	case wire.ModeGPSHold:
		s = "GPS hold"
	case wire.ModeNoGPSHold:
		s = "No-GPS hold"
	default:
		s = fmt.Sprintf("unknown (%d)", t.Mode)
	}
	if t.Status&wire.StatusModeRefused != 0 {
		s += ", requested mode refused"
	}
	return s
}

func gpsText(t wire.Telemetry) string {
	var fix string
	switch t.GPSFix {
	case 0:
		fix = "no fix"
	case 2:
		fix = "2D fix"
	case 3:
		fix = "3D fix"
	default:
		fix = fmt.Sprintf("fix type %d", t.GPSFix)
	}
	return fmt.Sprintf("%s, %d satellites", fix, t.GPSSats)
}

func altitudeText(m float32) string {
	if math.IsNaN(float64(m)) || math.IsInf(float64(m), 0) {
		return "unknown"
	}
	return fmt.Sprintf("%.1f m", m)
}
