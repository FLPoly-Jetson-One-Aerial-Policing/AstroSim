package components

import (
	"reflect"
	"strings"
	"testing"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/component"
)

const testRepo = "ghcr.io/flpoly-jetson-one-aerial-policing"

func TestImagesApplyDevIsIdentity(t *testing.T) {
	defs := All()
	got := (Images{}).Apply(defs)
	if !reflect.DeepEqual(images(got), images(defs)) || (Images{}).Release() {
		t.Errorf("dev images = %v", images(got))
	}
}

func TestImagesApplyRelease(t *testing.T) {
	defs := All()
	before := images(defs)
	im := Images{Repository: testRepo, Tag: "0.1.0"}
	got := im.Apply(defs)
	want := map[string]string{
		"engine": testRepo + "/astrosim-engine:0.1.0",
		"fc":     testRepo + "/astrosim-fc:0.1.0",
	}
	if !im.Release() || !reflect.DeepEqual(images(got), want) {
		t.Errorf("release images = %v, want %v", images(got), want)
	}
	if !reflect.DeepEqual(images(defs), before) {
		t.Error("Apply modified its input")
	}
}

func TestImagesValidate(t *testing.T) {
	for _, tc := range []struct {
		name string
		im   Images
		bad  string // substring of the error; empty means valid
	}{
		{"dev", Images{}, ""},
		{"release", Images{testRepo, "0.1.0"}, ""},
		{"nightly tag", Images{testRepo, "sha-1a2b3c4"}, ""},
		{"registry with port", Images{"localhost:5000/astrosim", "dev"}, ""},
		{"repository only", Images{Repository: testRepo}, "image tag is not"},
		{"tag only", Images{Tag: "0.1.0"}, "image repository is not"},
		{"trailing slash", Images{testRepo + "/", "1"}, "slash"},
		{"uppercase", Images{"ghcr.io/FLPoly-Jetson", "1"}, "lowercase"},
		{"odd characters", Images{"ghcr.io/a b", "1"}, "characters"},
		{"no registry host", Images{"flpoly/astrosim", "1"}, "registry host"},
		{"localhost without port", Images{"localhost/astrosim", "1"}, "registry host"},
		{"bad tag", Images{testRepo, "v 1"}, "not a valid tag"},
		{"tag starts with dot", Images{testRepo, ".1"}, "not a valid tag"},
		{"tag too long", Images{testRepo, strings.Repeat("a", 129)}, "not a valid tag"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			err := tc.im.Validate()
			switch {
			case tc.bad == "" && err != nil:
				t.Errorf("Validate = %v, want nil", err)
			case tc.bad != "" && (err == nil || !strings.Contains(err.Error(), tc.bad)):
				t.Errorf("Validate = %v, want it to mention %q", err, tc.bad)
			}
		})
	}
}

// images maps each definition's ID to its image reference.
func images(defs []component.Definition) map[string]string {
	m := map[string]string{}
	for _, d := range defs {
		m[d.ID] = d.Image
	}
	return m
}
