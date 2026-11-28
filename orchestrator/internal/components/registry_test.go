package components

import (
	"reflect"
	"strings"
	"testing"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/component"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/supervisor"
)

func TestAllFormsAValidPlan(t *testing.T) {
	p, err := supervisor.NewPlan(All())
	if err != nil {
		t.Fatal(err)
	}
	if got, want := p.Order(), []string{"engine", "fc"}; !reflect.DeepEqual(got, want) {
		t.Errorf("order = %v, want %v", got, want)
	}
	// Engine and FC restart together.
	if got, want := p.RestartSet(p.GroupOf("engine")), []string{"engine", "fc"}; !reflect.DeepEqual(got, want) {
		t.Errorf("restart set = %v, want %v", got, want)
	}
	if p.GroupOf("engine") != p.GroupOf("fc") {
		t.Error("engine and fc are in different restart groups")
	}
}

func TestRunSpecs(t *testing.T) {
	byID := map[string]component.Definition{}
	for _, d := range All() {
		byID[d.ID] = d
	}
	bridge := component.DefaultStackContext()
	host := bridge
	host.NetworkMode = component.NetworkHost

	eng := byID["engine"].RunSpec(bridge, "s1")
	if eng.Name != "astrosim-engine" || eng.Image != "astrosim-engine" || eng.Network != "astrosim" ||
		!reflect.DeepEqual(eng.Aliases, []string{"engine"}) || len(eng.Env) != 0 {
		t.Errorf("engine bridge spec = %+v", eng)
	}
	if len(eng.Publish) != 1 || eng.Publish[0].HostAddr != "127.0.0.1" || eng.Publish[0].HostPort != 8765 ||
		eng.Publish[0].ContainerPort != 8765 || eng.Publish[0].Proto != "udp" {
		t.Errorf("engine publish = %+v", eng.Publish)
	}
	wantLabels := map[string]string{"astrosim.managed": "true", "astrosim.session": "s1", "astrosim.component": "engine", "astrosim.group": "sim"}
	if !reflect.DeepEqual(eng.Labels, wantLabels) {
		t.Errorf("labels = %v", eng.Labels)
	}

	fcb := byID["fc"].RunSpec(bridge, "s1")
	if fcb.Env["ASTROSIM_ENGINE_HOST"] != "engine" || fcb.Publish[0].HostPort != 8766 {
		t.Errorf("fc bridge spec = %+v", fcb)
	}

	enh := byID["engine"].RunSpec(host, "s1")
	if enh.Network != "host" || enh.Publish != nil || enh.Aliases != nil ||
		enh.Env["GZ_PARTITION"] != "astrosim" || enh.Env["GZ_IP"] != "127.0.0.1" {
		t.Errorf("engine host spec = %+v", enh)
	}
	fch := byID["fc"].RunSpec(host, "s1")
	if fch.Network != "host" || fch.Env["ASTROSIM_ENGINE_HOST"] != "127.0.0.1" {
		t.Errorf("fc host spec = %+v", fch)
	}
}

func TestBuildHintsMentionRuntimePlaceholder(t *testing.T) {
	for _, d := range All() {
		if d.BuildHint == "" || d.DisplayName == "" || d.Description == "" {
			t.Errorf("%s: incomplete presentation fields", d.ID)
		}
	}
}

// TestBuiltInImagesAreNotPullable guards the development default: the
// built-in image names are bare, so the orchestrator never tries to download
// them (which could only reach docker.io).
func TestBuiltInImagesAreNotPullable(t *testing.T) {
	for _, d := range All() {
		if runtime.Pullable(d.Image) {
			t.Errorf("%s: built-in image %q is pullable", d.ID, d.Image)
		}
		if strings.ContainsAny(d.Image, "/:") {
			t.Errorf("%s: built-in image %q is not a bare name", d.ID, d.Image)
		}
	}
}
