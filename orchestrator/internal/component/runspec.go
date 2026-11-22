package component

import "github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"

// Labels the orchestrator puts on every container it creates. The managed
// label is what stale-container cleanup selects on.
const (
	// LabelManaged marks a container as owned by the orchestrator.
	LabelManaged = "astrosim.managed"
	// LabelSession identifies the orchestrator session that created it.
	LabelSession = "astrosim.session"
	// LabelComponent holds the component ID.
	LabelComponent = "astrosim.component"
	// LabelGroup holds the restart group, when the component has one.
	LabelGroup = "astrosim.group"
)

// RunSpec builds the container description for the component in the given
// stack. In bridge mode the container joins the stack's user network under
// its ID as alias and each port is published to sc.PublishAddr on the same
// host port; in host mode it shares the host network. Per-component
// overrides (image, environment) are applied to the Definition before
// calling this, not here.
func (d Definition) RunSpec(sc StackContext, sessionID string) runtime.RunSpec {
	spec := runtime.RunSpec{
		Name:  d.ContainerName(),
		Image: d.Image,
		Labels: map[string]string{
			LabelManaged:   "true",
			LabelSession:   sessionID,
			LabelComponent: d.ID,
		},
	}
	if d.RestartGroup != "" {
		spec.Labels[LabelGroup] = d.RestartGroup
	}
	if d.Env != nil {
		spec.Env = d.Env(sc)
	}
	if sc.NetworkMode == NetworkHost {
		spec.Network = runtime.HostNetwork
		return spec
	}
	spec.Network = sc.NetworkName
	spec.Aliases = []string{d.ID}
	for _, p := range d.Ports {
		spec.Publish = append(spec.Publish, runtime.Publish{
			HostAddr: sc.PublishAddr, HostPort: p.Port, ContainerPort: p.Port, Proto: p.Proto,
		})
	}
	return spec
}
