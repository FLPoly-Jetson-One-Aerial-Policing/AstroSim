// Package status defines the snapshot of the whole stack that the dashboard
// consumes, and the Broadcaster that fans snapshots out to subscribers.
//
// # JSON schema
//
// A Snapshot is served as JSON by GET /api/v1/status and as the data of each
// "status" server-sent event. All keys are camelCase.
//
//	{
//	  "version": "1.2.3",            // orchestrator version
//	  "phase": "running",            // checking|unavailable|stopped|starting|running|recovering|stopping|failed
//	  "since": "2026-01-02T03:04:05Z", // when the phase was entered
//	  "reason": {                    // why, after a failure or preflight problem; code is "" otherwise
//	    "code": "images_missing",    // a supervisor Reason or Problem code
//	    "detail": "...",             // technical explanation
//	    "component": "engine"        // component concerned, may be ""
//	  },
//	  "attempt": 1, "maxAttempts": 3, // restart budget use
//	  "canStart": false, "canStop": true,
//	  "runtime": {"name": "podman", "version": "5.0.1", "network": "bridge"},
//	  "logDir": "/home/u/.local/state/astrosim/logs/20260102T030405Z-0123456789abcdef", // "" when file logging is off
//	  "components": [{
//	    "id": "engine", "name": "Simulation engine", "description": "...",
//	    "image": "astrosim-engine",
//	    "installed": true,           // false when the image is not built
//	    "buildCommand": "podman build ...", // "{runtime}" already substituted
//	    "state": "ready",            // pending|launching|starting|ready|stale|stopping|stopped|exited|failed
//	    "facts": [{"key": "...", "label": "...", "value": "..."}],
//	    "exitCode": null,            // last exit status or null
//	    "lastLog": ["..."],          // log tail captured when the component failed
//	    "endpoints": [{"name": "state", "proto": "udp", "address": "127.0.0.1:8765"}]
//	  }]
//	}
//
// Slices are never null in the JSON: an empty list is [].
package status

import (
	"sync"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/health"
)

// Reason explains the current phase.
type Reason struct {
	// Code is a supervisor reason or problem code; empty when nothing needs explaining.
	Code string `json:"code"`
	// Detail is a technical explanation.
	Detail string `json:"detail"`
	// Component is the ID of the component concerned, if any.
	Component string `json:"component"`
}

// Runtime describes the container runtime in use.
type Runtime struct {
	// Name is "podman" or "docker".
	Name string `json:"name"`
	// Version is the engine version.
	Version string `json:"version"`
	// Network is the network mode, "bridge" or "host".
	Network string `json:"network"`
}

// Endpoint is a port a component can be reached on from the host.
type Endpoint struct {
	// Name is the port's name, for example "state".
	Name string `json:"name"`
	// Proto is "udp" or "tcp".
	Proto string `json:"proto"`
	// Address is the host:port to connect to.
	Address string `json:"address"`
}

// Component is the status of one component.
type Component struct {
	// ID is the component ID.
	ID string `json:"id"`
	// Name is the display name.
	Name string `json:"name"`
	// Description is a plain-language sentence about the component.
	Description string `json:"description"`
	// Image is the container image reference.
	Image string `json:"image"`
	// Installed is false when the image is known to be missing.
	Installed bool `json:"installed"`
	// BuildCommand is the command that builds the image, with the runtime name filled in.
	BuildCommand string `json:"buildCommand"`
	// State is the supervisor's component state.
	State string `json:"state"`
	// Facts are live status details from the health probe.
	Facts []health.Fact `json:"facts"`
	// ExitCode is the last observed exit status, or null.
	ExitCode *int `json:"exitCode"`
	// LastLog is the container's log tail captured when it failed.
	LastLog []string `json:"lastLog"`
	// Endpoints lists the host-reachable ports.
	Endpoints []Endpoint `json:"endpoints"`
}

// Snapshot is the complete status of the stack at one moment. See the
// package documentation for the JSON schema.
type Snapshot struct {
	// Version is the orchestrator version.
	Version string `json:"version"`
	// Phase is the overall phase.
	Phase string `json:"phase"`
	// Since is when the phase was entered.
	Since time.Time `json:"since"`
	// Reason explains the phase.
	Reason Reason `json:"reason"`
	// Attempt is the number of recovery attempts used.
	Attempt int `json:"attempt"`
	// MaxAttempts is the recovery budget.
	MaxAttempts int `json:"maxAttempts"`
	// CanStart reports whether a start request would be accepted.
	CanStart bool `json:"canStart"`
	// CanStop reports whether a stop request would be accepted.
	CanStop bool `json:"canStop"`
	// Runtime describes the container runtime.
	Runtime Runtime `json:"runtime"`
	// LogDir is the folder holding this session's logs; "" when file
	// logging is off.
	LogDir string `json:"logDir"`
	// Components lists every component in launch order.
	Components []Component `json:"components"`
}

// Broadcaster delivers the latest Snapshot to any number of subscribers.
// Publishing never blocks: each subscriber has a one-slot mailbox and a new
// snapshot replaces an unread older one, so a slow consumer only ever skips
// intermediate states.
type Broadcaster struct {
	mu     sync.Mutex
	latest *Snapshot
	subs   map[*subscription]struct{}
}

type subscription struct{ ch chan Snapshot }

// NewBroadcaster returns an empty Broadcaster.
func NewBroadcaster() *Broadcaster {
	return &Broadcaster{subs: map[*subscription]struct{}{}}
}

// Publish makes s the latest snapshot and hands it to every subscriber,
// replacing any value they have not read yet.
func (b *Broadcaster) Publish(s Snapshot) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.latest = &s
	for sub := range b.subs {
		// Publish is serialised by mu and only Publish and Subscribe write
		// to the channel, so drain-then-send cannot block.
		select {
		case <-sub.ch:
		default:
		}
		sub.ch <- s
	}
}

// Latest returns the most recently published snapshot, if any.
func (b *Broadcaster) Latest() (Snapshot, bool) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.latest == nil {
		return Snapshot{}, false
	}
	return *b.latest, true
}

// Subscribe returns a channel that receives snapshots and a function that
// cancels the subscription. If a snapshot was already published, the channel
// holds it immediately. The channel is never closed; stop reading after cancel.
func (b *Broadcaster) Subscribe() (<-chan Snapshot, func()) {
	sub := &subscription{ch: make(chan Snapshot, 1)}
	b.mu.Lock()
	if b.latest != nil {
		sub.ch <- *b.latest
	}
	b.subs[sub] = struct{}{}
	b.mu.Unlock()
	return sub.ch, func() {
		b.mu.Lock()
		delete(b.subs, sub)
		b.mu.Unlock()
	}
}
