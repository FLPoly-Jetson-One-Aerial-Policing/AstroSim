// Package runtime abstracts the container runtime (podman or docker) that
// hosts the simulation components.
//
// The Runtime interface is what the rest of the orchestrator consumes. The
// production implementation, CLI, drives the podman or docker command-line
// tool through a Commander seam, so the same code works on Linux, macOS and
// Windows and unit tests never need a real container engine. Package
// runtime/fake provides an in-memory Runtime for session tests.
package runtime

import (
	"context"
	"errors"
	"time"
)

// Sentinel errors. Callers match them with errors.Is.
var (
	// ErrNotFound means the named container (or other object) does not exist.
	ErrNotFound = errors.New("runtime: not found")
	// ErrNotInstalled means no container runtime executable was found.
	ErrNotInstalled = errors.New("runtime: no container runtime installed")
	// ErrNotRunning means a runtime executable exists but its engine or
	// daemon does not answer (for example the docker daemon is stopped).
	ErrNotRunning = errors.New("runtime: container runtime is not running")
)

// Info describes a detected, reachable runtime.
type Info struct {
	// Name is the dialect name, "podman" or "docker".
	Name string `json:"name"`
	// Path is the executable used to reach the runtime.
	Path string `json:"path"`
	// Version is the engine (server) version reported by the runtime.
	Version string `json:"version"`
}

// ContainerState is the subset of a container's inspected state the
// orchestrator consumes.
type ContainerState struct {
	// Status is the runtime's status word, such as "created", "running",
	// "paused", "exited" or "dead".
	Status string
	// Running is true while the container's main process is alive.
	Running bool
	// ExitCode is the main process's exit code once it has exited.
	ExitCode int
	// OOMKilled is true if the kernel killed the container for memory.
	OOMKilled bool
	// Health is the image HEALTHCHECK status ("starting", "healthy",
	// "unhealthy"), or empty when the image defines no health check.
	Health string
	// StartedAt is when the container last started; zero if never.
	StartedAt time.Time
	// FinishedAt is when the container last exited; zero while running.
	FinishedAt time.Time
	// Error is the runtime's error text for the container, if any.
	Error string
}

// Publish maps a container UDP/TCP port onto a host address.
type Publish struct {
	// HostAddr is the host interface to bind, for example "127.0.0.1".
	// Empty binds all interfaces.
	HostAddr string
	// HostPort is the host port.
	HostPort int
	// ContainerPort is the port inside the container.
	ContainerPort int
	// Proto is "udp" or "tcp"; empty means "tcp".
	Proto string
}

// RunSpec describes a detached container to start.
type RunSpec struct {
	// Name is the container name.
	Name string
	// Image is the image reference.
	Image string
	// Network is a user network name or "host". Empty uses the runtime's
	// default network.
	Network string
	// Aliases are DNS aliases on the user network. They are ignored with
	// host networking.
	Aliases []string
	// Publish lists the published ports. It is ignored with host networking.
	Publish []Publish
	// Env is the container environment.
	Env map[string]string
	// Labels are the container labels.
	Labels map[string]string
}

// HostNetwork is the Network value selecting the host network namespace.
const HostNetwork = "host"

// Runtime is the container engine as the orchestrator uses it. All methods
// honour context cancellation. Implementations must be safe for concurrent
// use.
type Runtime interface {
	// Info returns what Ping or Detect learned about the runtime.
	Info() Info
	// Ping checks that the engine answers and returns its description.
	Ping(ctx context.Context) (Info, error)
	// ImageExists reports whether the image is present locally. A missing
	// image is (false, nil); a daemon error is a non-nil error.
	ImageExists(ctx context.Context, image string) (bool, error)
	// EnsureNetwork creates the user network, carrying labels, if it does
	// not exist. Labels are applied only on creation; an existing network
	// is left as it is.
	EnsureNetwork(ctx context.Context, name string, labels map[string]string) error
	// ListNetworksByLabel returns the names of all networks carrying the
	// label "key=value".
	ListNetworksByLabel(ctx context.Context, key, value string) ([]string, error)
	// RemoveNetwork removes the network. A missing network is not an error.
	// A network still in use by a container is.
	RemoveNetwork(ctx context.Context, name string) error
	// Run starts a detached container (without --rm, so its exit code and
	// logs stay inspectable) and returns its ID.
	Run(ctx context.Context, spec RunSpec) (string, error)
	// Stop asks the container to stop, killing it after grace.
	// Stopping a container that is already stopped succeeds.
	Stop(ctx context.Context, name string, grace time.Duration) error
	// Remove force-removes the container. A missing container is not an error.
	Remove(ctx context.Context, name string) error
	// Wait blocks until the container exits and returns its exit code.
	Wait(ctx context.Context, name string) (int, error)
	// Inspect returns the container's state, or ErrNotFound.
	Inspect(ctx context.Context, name string) (ContainerState, error)
	// ListByLabel returns the names of all containers (running or not)
	// carrying the label "key=value".
	ListByLabel(ctx context.Context, key, value string) ([]string, error)
	// Logs returns the last tail lines of the container's output
	// (stdout and stderr combined).
	Logs(ctx context.Context, name string, tail int) (string, error)
}
