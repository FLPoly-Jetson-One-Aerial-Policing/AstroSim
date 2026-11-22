// Package supervisor is the pure state machine that decides when to launch,
// restart and tear down the components of the simulation stack.
//
// The package performs no I/O, starts no goroutines and never reads the
// clock. A driver (internal/session) feeds Events and the current time into
// Supervisor.Step and executes the returned Actions; results of those
// actions come back as further Events. Because of this the whole lifecycle,
// including every failure path, is covered by fast table tests.
//
// Every Launch bumps the component's generation. Events and timers carry the
// generation they were created for, so anything belonging to a container
// that has since been torn down or relaunched is ignored.
package supervisor

import (
	"fmt"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/component"
)

// Plan is the validated dependency graph of a set of components. It is
// immutable after NewPlan returns and safe for concurrent use.
type Plan struct {
	defs       map[string]component.Definition
	order      []string            // topological launch order
	dependents map[string][]string // direct dependents, in launch order
	groupOf    map[string]string   // component ID -> group key
	members    map[string][]string // group key -> members, in launch order
	groups     []string            // group keys, in order of first member
}

// NewPlan validates the definitions and builds the plan. It rejects invalid
// definitions, duplicate IDs, dependencies on unknown components, dependency
// cycles, and two components publishing the same port and protocol.
func NewPlan(defs []component.Definition) (*Plan, error) {
	p := &Plan{
		defs:       make(map[string]component.Definition, len(defs)),
		dependents: make(map[string][]string, len(defs)),
		groupOf:    make(map[string]string, len(defs)),
		members:    make(map[string][]string),
	}
	var input []string
	for _, d := range defs {
		if err := d.Validate(); err != nil {
			return nil, err
		}
		if _, dup := p.defs[d.ID]; dup {
			return nil, fmt.Errorf("supervisor: duplicate component ID %q", d.ID)
		}
		p.defs[d.ID] = d
		input = append(input, d.ID)
	}

	type portKey struct {
		proto string
		port  int
	}
	ports := map[portKey]string{}
	for _, id := range input {
		d := p.defs[id]
		for _, pt := range d.Ports {
			k := portKey{pt.Proto, pt.Port}
			if other, clash := ports[k]; clash {
				return nil, fmt.Errorf("supervisor: components %q and %q both publish %d/%s", other, id, pt.Port, pt.Proto)
			}
			ports[k] = id
		}
		for _, dep := range d.DependsOn {
			if _, ok := p.defs[dep.ID]; !ok {
				return nil, fmt.Errorf("supervisor: component %q depends on unknown component %q", id, dep.ID)
			}
		}
	}

	// Kahn's algorithm; ties resolve in input order so launch order is
	// deterministic and follows the order of the registry.
	indeg := make(map[string]int, len(input))
	for _, id := range input {
		indeg[id] = len(uniqueDeps(p.defs[id]))
	}
	placed := map[string]bool{}
	for len(p.order) < len(input) {
		progressed := false
		for _, id := range input {
			if placed[id] || indeg[id] != 0 {
				continue
			}
			placed[id] = true
			progressed = true
			p.order = append(p.order, id)
			for _, other := range input {
				for _, dep := range uniqueDeps(p.defs[other]) {
					if dep == id {
						indeg[other]--
					}
				}
			}
			break // restart the scan so input order wins among ready nodes
		}
		if !progressed {
			var stuck []string
			for _, id := range input {
				if !placed[id] {
					stuck = append(stuck, id)
				}
			}
			return nil, fmt.Errorf("supervisor: dependency cycle among components %v", stuck)
		}
	}

	for _, id := range p.order {
		for _, dep := range uniqueDeps(p.defs[id]) {
			p.dependents[dep] = append(p.dependents[dep], id)
		}
		g := groupKey(p.defs[id])
		p.groupOf[id] = g
		if _, seen := p.members[g]; !seen {
			p.groups = append(p.groups, g)
		}
		p.members[g] = append(p.members[g], id)
	}
	return p, nil
}

// groupKey returns the internal restart-group key. Named groups and
// ungrouped components live in separate namespaces so a group name can
// never collide with a component ID.
func groupKey(d component.Definition) string {
	if d.RestartGroup != "" {
		return "group:" + d.RestartGroup
	}
	return "solo:" + d.ID
}

func uniqueDeps(d component.Definition) []string {
	var out []string
	seen := map[string]bool{}
	for _, dep := range d.DependsOn {
		if !seen[dep.ID] {
			seen[dep.ID] = true
			out = append(out, dep.ID)
		}
	}
	return out
}

// Order returns the component IDs in launch order: every component comes
// after everything it depends on.
func (p *Plan) Order() []string { return append([]string(nil), p.order...) }

// Definition returns the definition of the component with the given ID.
func (p *Plan) Definition(id string) (component.Definition, bool) {
	d, ok := p.defs[id]
	return d, ok
}

// Definitions returns all definitions in launch order.
func (p *Plan) Definitions() []component.Definition {
	out := make([]component.Definition, 0, len(p.order))
	for _, id := range p.order {
		out = append(out, p.defs[id])
	}
	return out
}

// GroupOf returns the opaque restart-group key of a component. Components
// with the same RestartGroup share a key; an ungrouped component has a key
// of its own. The key is what events and timers use to name a group.
func (p *Plan) GroupOf(id string) string { return p.groupOf[id] }

// Groups returns every restart-group key.
func (p *Plan) Groups() []string { return append([]string(nil), p.groups...) }

// RestartSet returns the components that must restart when something in the
// given group fails: the group's members plus everything that transitively
// depends on any of them, in launch order. An unknown group yields nil.
func (p *Plan) RestartSet(group string) []string {
	seed := p.members[group]
	if len(seed) == 0 {
		return nil
	}
	in := map[string]bool{}
	var visit func(id string)
	visit = func(id string) {
		if in[id] {
			return
		}
		in[id] = true
		for _, d := range p.dependents[id] {
			visit(d)
		}
	}
	for _, id := range seed {
		visit(id)
	}
	var out []string
	for _, id := range p.order {
		if in[id] {
			out = append(out, id)
		}
	}
	return out
}

// TeardownWaves splits ids into waves for an ordered teardown: the first
// wave holds the components that nothing else in ids depends on, the next
// wave what only the first wave depended on, and so on. Components within a
// wave may be stopped in parallel; a wave is started only after the previous
// one is gone. A nil ids means every component. Unknown IDs are ignored.
func (p *Plan) TeardownWaves(ids []string) [][]string {
	if ids == nil {
		ids = p.order
	}
	left := map[string]bool{}
	for _, id := range ids {
		if _, ok := p.defs[id]; ok {
			left[id] = true
		}
	}
	var waves [][]string
	for len(left) > 0 {
		var wave []string
		for _, id := range p.order {
			if !left[id] {
				continue
			}
			blocked := false
			for _, d := range p.dependents[id] {
				if left[d] {
					blocked = true
					break
				}
			}
			if !blocked {
				wave = append(wave, id)
			}
		}
		for _, id := range wave {
			delete(left, id)
		}
		waves = append(waves, wave)
	}
	return waves
}
