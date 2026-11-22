// Package component is the generic model of a simulation component.
//
// A Definition describes everything the orchestrator needs to run one
// container and judge its health, without any component-specific logic in the
// supervisor, session, API or UI. Concrete definitions live in
// internal/components; adding a component is a local source change there.
package component

import (
	"fmt"
	"net"
	"strconv"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/health"
)

// ContainerPrefix prefixes every managed container name.
const ContainerPrefix = "astrosim-"

// Network modes for StackContext.NetworkMode.
const (
	// NetworkBridge puts the components on a user-defined network where
	// they reach each other by service alias and publish ports to the host.
	NetworkBridge = "bridge"
	// NetworkHost shares the host network namespace (Linux only).
	NetworkHost = "host"
)

// Condition says when a dependency counts as satisfied.
type Condition int

const (
	// Started means the dependency's container has been launched.
	Started Condition = iota + 1
	// Ready means the dependency has passed its readiness probe.
	Ready
)

// String returns the condition name.
func (c Condition) String() string {
	switch c {
	case Started:
		return "started"
	case Ready:
		return "ready"
	default:
		return "Condition(" + strconv.Itoa(int(c)) + ")"
	}
}

// Dependency is a requirement on another component.
type Dependency struct {
	// ID is the required component's ID.
	ID string
	// Condition is when the requirement is met.
	Condition Condition
}

// PortSpec is a network port a component exposes.
type PortSpec struct {
	// Name identifies the port, for example "state".
	Name string
	// Proto is "udp" or "tcp".
	Proto string
	// Port is the port number inside the container. In bridge mode it is
	// published on the same host port.
	Port int
}

// StackContext carries the deployment-wide settings that component
// definitions may depend on. Today it is the local network mode; it is where
// a remote host would later be expressed.
type StackContext struct {
	// NetworkMode is NetworkBridge or NetworkHost.
	NetworkMode string
	// NetworkName is the user network name used in bridge mode.
	NetworkName string
	// PublishAddr is the host address published ports bind to in bridge
	// mode, normally "127.0.0.1".
	PublishAddr string
	// ProbeHost is the host the orchestrator contacts to probe components.
	ProbeHost string
}

// DefaultStackContext returns the default local deployment: a user network
// named "astrosim" with ports published on loopback.
func DefaultStackContext() StackContext {
	return StackContext{
		NetworkMode: NetworkBridge,
		NetworkName: "astrosim",
		PublishAddr: "127.0.0.1",
		ProbeHost:   "127.0.0.1",
	}
}

// ServiceHost returns the hostname under which components reach the
// component with the given ID: its network alias in bridge mode, the
// loopback address in host mode.
func (s StackContext) ServiceHost(id string) string {
	if s.NetworkMode == NetworkHost {
		return "127.0.0.1"
	}
	return id
}

// ProbeAddr returns the host:port the orchestrator uses to reach a
// component port.
func (s StackContext) ProbeAddr(port int) string {
	return net.JoinHostPort(s.ProbeHost, strconv.Itoa(port))
}

// Definition describes one component of the simulation stack.
type Definition struct {
	// ID is the unique machine name, also the network alias, for example "engine".
	ID string
	// DisplayName is the human-readable name shown in the dashboard.
	DisplayName string
	// Description is one plain-language sentence about what the component does.
	Description string
	// Image is the container image reference.
	Image string
	// BuildHint is the command a user runs to build the image if it is missing.
	BuildHint string
	// Ports are the ports the component exposes.
	Ports []PortSpec
	// Env returns the container environment for the given stack. May be nil.
	Env func(StackContext) map[string]string
	// DependsOn lists the components that must be up first.
	DependsOn []Dependency
	// RestartGroup names a set of components that must restart together,
	// for example because they share a simulation clock. Empty means the
	// component restarts alone.
	RestartGroup string
	// Probe builds the readiness and liveness probe for the stack.
	Probe func(StackContext) health.Probe
	// ReadyTimeout is how long the component may take to become ready.
	ReadyTimeout time.Duration
	// LivenessTimeout is how long a ready component may go without a
	// healthy observation before it is considered hung.
	LivenessTimeout time.Duration
	// StopTimeout is the grace period given to the container on stop.
	StopTimeout time.Duration
	// PermanentExitCodes are exit codes that mean restarting cannot help,
	// such as an invalid configuration.
	PermanentExitCodes []int
}

// ContainerName returns the name of the component's container.
func (d Definition) ContainerName() string { return ContainerPrefix + d.ID }

// Validate checks that the definition is complete and self-consistent. It
// does not check references to other components; that is the plan's job.
func (d Definition) Validate() error {
	switch {
	case d.ID == "":
		return fmt.Errorf("component: empty ID")
	case d.Image == "":
		return fmt.Errorf("component %s: empty image", d.ID)
	case d.Probe == nil:
		return fmt.Errorf("component %s: no probe", d.ID)
	case d.ReadyTimeout <= 0 || d.LivenessTimeout <= 0 || d.StopTimeout <= 0:
		return fmt.Errorf("component %s: ready, liveness and stop timeouts must be positive", d.ID)
	}
	seen := map[string]bool{}
	for _, p := range d.Ports {
		if p.Name == "" || p.Port < 1 || p.Port > 65535 || (p.Proto != "udp" && p.Proto != "tcp") {
			return fmt.Errorf("component %s: invalid port %+v", d.ID, p)
		}
		if seen[p.Name] {
			return fmt.Errorf("component %s: duplicate port name %q", d.ID, p.Name)
		}
		seen[p.Name] = true
	}
	for _, dep := range d.DependsOn {
		if dep.ID == "" || dep.ID == d.ID || (dep.Condition != Started && dep.Condition != Ready) {
			return fmt.Errorf("component %s: invalid dependency %+v", d.ID, dep)
		}
	}
	return nil
}
