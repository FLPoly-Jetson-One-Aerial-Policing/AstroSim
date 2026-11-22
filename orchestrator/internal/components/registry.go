// Package components lists the simulation components the orchestrator
// manages. To add a component, create a package here with a Definition
// function and add it to All; nothing else in the orchestrator changes.
package components

import (
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/component"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/components/engine"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/components/fc"
)

// All returns the definitions of every component, in no required order; the
// supervisor derives the launch order from the dependencies.
func All() []component.Definition {
	return []component.Definition{
		engine.Definition(),
		fc.Definition(),
	}
}
