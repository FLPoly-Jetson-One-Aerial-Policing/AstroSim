package component

import (
	"context"
	"strings"
	"testing"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/health"
)

type nopProbe struct{}

func (nopProbe) Run(ctx context.Context, _ func(health.Observation)) error {
	<-ctx.Done()
	return nil
}

func valid() Definition {
	return Definition{
		ID: "engine", Image: "img",
		Probe:        func(StackContext) health.Probe { return nopProbe{} },
		Ports:        []PortSpec{{Name: "state", Proto: "udp", Port: 8765}},
		ReadyTimeout: time.Second, LivenessTimeout: time.Second, StopTimeout: time.Second,
		DependsOn: []Dependency{{ID: "other", Condition: Started}},
	}
}

func TestValidate(t *testing.T) {
	tests := []struct {
		name string
		mod  func(*Definition)
		want string // substring of the error; empty means valid
	}{
		{"valid", func(*Definition) {}, ""},
		{"no id", func(d *Definition) { d.ID = "" }, "empty ID"},
		{"no image", func(d *Definition) { d.Image = "" }, "empty image"},
		{"no probe", func(d *Definition) { d.Probe = nil }, "no probe"},
		{"zero timeout", func(d *Definition) { d.StopTimeout = 0 }, "timeouts"},
		{"bad port", func(d *Definition) { d.Ports[0].Port = 0 }, "invalid port"},
		{"bad proto", func(d *Definition) { d.Ports[0].Proto = "sctp" }, "invalid port"},
		{"dup port", func(d *Definition) { d.Ports = append(d.Ports, d.Ports[0]) }, "duplicate port"},
		{"self dep", func(d *Definition) { d.DependsOn = []Dependency{{ID: "engine", Condition: Ready}} }, "invalid dependency"},
		{"zero condition", func(d *Definition) { d.DependsOn = []Dependency{{ID: "x"}} }, "invalid dependency"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			d := valid()
			tc.mod(&d)
			err := d.Validate()
			switch {
			case tc.want == "" && err != nil:
				t.Fatal(err)
			case tc.want != "" && (err == nil || !strings.Contains(err.Error(), tc.want)):
				t.Fatalf("err = %v, want %q", err, tc.want)
			}
		})
	}
}

func TestStackContext(t *testing.T) {
	s := DefaultStackContext()
	if s.ServiceHost("engine") != "engine" {
		t.Error("bridge service host should be the alias")
	}
	if got := s.ProbeAddr(8765); got != "127.0.0.1:8765" {
		t.Errorf("ProbeAddr = %q", got)
	}
	s.NetworkMode = NetworkHost
	if s.ServiceHost("engine") != "127.0.0.1" {
		t.Error("host-mode service host should be loopback")
	}
}

func TestNamesAndConditions(t *testing.T) {
	if got := (Definition{ID: "fc"}).ContainerName(); got != "astrosim-fc" {
		t.Errorf("ContainerName = %q", got)
	}
	if Started.String() != "started" || Ready.String() != "ready" || Condition(9).String() != "Condition(9)" {
		t.Error("Condition.String")
	}
}
