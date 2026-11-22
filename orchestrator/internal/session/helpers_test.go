package session

import (
	"fmt"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/component"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
)

func errNotRunning() error   { return fmt.Errorf("daemon down: %w", runtime.ErrNotRunning) }
func errNotInstalled() error { return fmt.Errorf("no binary: %w", runtime.ErrNotInstalled) }

func runtimeSpecLabelled(name string, managed bool) runtime.RunSpec {
	spec := runtime.RunSpec{Name: name, Image: "x", Labels: map[string]string{}}
	if managed {
		spec.Labels[component.LabelManaged] = "true"
	}
	return spec
}
