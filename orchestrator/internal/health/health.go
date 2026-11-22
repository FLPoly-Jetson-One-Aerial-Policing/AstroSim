// Package health defines how the orchestrator decides a component is up.
//
// A Probe watches one component and reports Observations. Probes are generic
// (UDP subscribe, container state); the component-specific rule for what
// "healthy" means is a Classify function supplied by the component's
// definition. A Monitor turns the stream of observations into the two edges
// the supervisor cares about: the component became ready, and a ready
// component went silent or unhealthy.
//
// This package must not import package component: component depends on
// health, not the other way round.
package health

import "context"

// Fact is one human-facing status detail of a component, such as
// "engine state: fresh". Facts are what the dashboard shows beside the state.
type Fact struct {
	// Key is a stable machine identifier, for example "engine_ok".
	Key string `json:"key"`
	// Label is the short human-readable name, for example "Engine state".
	Label string `json:"label"`
	// Value is the current value rendered as text, for example "fresh".
	Value string `json:"value"`
}

// Observation is one probe result.
type Observation struct {
	// Healthy reports whether the component currently meets its readiness
	// rule. Only healthy observations count as proof of life.
	Healthy bool
	// Facts are the current status details; they replace the previous set.
	Facts []Fact
}

// Probe watches a component until its context is cancelled.
type Probe interface {
	// Run starts watching and calls report for each observation worth
	// passing on. It blocks until ctx is done (returning nil) or the probe
	// fails irrecoverably (returning the error). report must not be retained
	// after Run returns and may be called from Run's own goroutine only.
	Run(ctx context.Context, report func(Observation)) error
}
