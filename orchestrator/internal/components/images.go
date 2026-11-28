package components

import (
	"errors"
	"fmt"
	"regexp"
	"strings"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/component"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
)

// Images says where the component images come from. The zero value is a
// development build, whose images are the locally built bare names (such as
// "astrosim-engine") and are never downloaded. A release build names the
// registry repository and the tag its images were published under, which
// makes the references downloadable.
type Images struct {
	// Repository is the registry path the component images live under, for
	// example "ghcr.io/flpoly-jetson-one-aerial-policing". Empty for a
	// development build.
	Repository string
	// Tag is the image tag of this release, for example "0.1.0". Empty for a
	// development build.
	Tag string
}

// tagPattern is the syntax of an image tag.
var tagPattern = regexp.MustCompile(`^[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}$`)

// repositoryPattern is the characters a lowercase registry repository may
// hold: a host (with an optional port) followed by path components.
var repositoryPattern = regexp.MustCompile(`^[a-z0-9][a-z0-9._:/-]*$`)

// Release reports whether these are the settings of a release build.
func (im Images) Release() bool { return im.Repository != "" }

// Validate checks the settings. Repository and Tag must be both set or both
// empty. A Repository must be lowercase (GHCR rejects capitals), have no
// trailing slash and start with a registry host, so that a typo cannot make
// the runtime fall back to docker.io. A Tag must be a valid image tag.
func (im Images) Validate() error {
	switch {
	case im.Repository == "" && im.Tag == "":
		return nil
	case im.Repository == "":
		return errors.New("image tag is set but the image repository is not")
	case im.Tag == "":
		return errors.New("image repository is set but the image tag is not")
	}
	if strings.HasSuffix(im.Repository, "/") {
		return fmt.Errorf("image repository %q must not end with a slash", im.Repository)
	}
	if im.Repository != strings.ToLower(im.Repository) {
		return fmt.Errorf("image repository %q must be lowercase", im.Repository)
	}
	if !repositoryPattern.MatchString(im.Repository) {
		return fmt.Errorf("image repository %q has characters a repository cannot hold", im.Repository)
	}
	if !runtime.Pullable(im.Repository + "/x") {
		return fmt.Errorf("image repository %q must start with a registry host such as ghcr.io", im.Repository)
	}
	if !tagPattern.MatchString(im.Tag) {
		return fmt.Errorf("image tag %q is not a valid tag", im.Tag)
	}
	return nil
}

// Apply returns defs with the image references of these settings. For a
// development build the definitions are returned unchanged. For a release
// build each bare image name becomes "<Repository>/<name>:<Tag>". The input
// slice is not modified. Call Validate first.
func (im Images) Apply(defs []component.Definition) []component.Definition {
	if !im.Release() {
		return defs
	}
	out := make([]component.Definition, len(defs))
	for i, d := range defs {
		d.Image = im.Repository + "/" + d.Image + ":" + im.Tag
		out[i] = d
	}
	return out
}
