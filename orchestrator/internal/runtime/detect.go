package runtime

import (
	"context"
	"errors"
	"fmt"
	"os/exec"
	"path/filepath"
	"strings"
)

// Auto selects the first reachable runtime, trying podman then docker.
const Auto = "auto"

// DetectOptions controls Detect.
type DetectOptions struct {
	// Name is "podman", "docker", or "auto" (also the empty string).
	Name string
	// Path overrides the executable location. With Name "auto" the dialect
	// is inferred from the executable's base name.
	Path string
	// Commander runs the executable; nil uses ExecCommander.
	Commander Commander
	// LookPath finds executables on PATH; nil uses exec.LookPath.
	LookPath func(file string) (string, error)
}

// Detect finds a working container runtime and returns it with its Info.
//
// In auto mode it tries podman, then docker, and returns the first that
// answers a Ping. An explicit Name restricts the search to that runtime.
// When nothing works the error wraps ErrNotInstalled if no executable was
// found at all, otherwise ErrNotRunning, and its text lists what was tried.
func Detect(ctx context.Context, opts DetectOptions) (Runtime, Info, error) {
	look := opts.LookPath
	if look == nil {
		look = exec.LookPath
	}
	type candidate struct {
		dialect Dialect
		path    string
	}
	var cands []candidate
	switch strings.ToLower(opts.Name) {
	case "", Auto:
		if opts.Path != "" {
			d, err := dialectFromPath(opts.Path)
			if err != nil {
				return nil, Info{}, err
			}
			cands = append(cands, candidate{d, opts.Path})
		} else {
			cands = append(cands, candidate{Podman, ""}, candidate{Docker, ""})
		}
	case "podman":
		cands = append(cands, candidate{Podman, opts.Path})
	case "docker":
		cands = append(cands, candidate{Docker, opts.Path})
	default:
		return nil, Info{}, fmt.Errorf("runtime: unknown runtime %q (want auto, podman or docker)", opts.Name)
	}

	var notes []string
	found := false
	for _, c := range cands {
		path := c.path
		if path == "" {
			p, err := look(c.dialect.Name)
			if err != nil {
				notes = append(notes, c.dialect.Name+": not found on PATH")
				continue
			}
			path = p
		}
		found = true
		rt := NewCLI(path, c.dialect, opts.Commander)
		info, err := rt.Ping(ctx)
		if err != nil {
			if ctx.Err() != nil {
				return nil, Info{}, ctx.Err()
			}
			notes = append(notes, fmt.Sprintf("%s (%s): %v", c.dialect.Name, path, err))
			continue
		}
		return rt, info, nil
	}
	base := ErrNotInstalled
	if found {
		base = ErrNotRunning
	}
	return nil, Info{}, fmt.Errorf("%w (%s)", base, strings.Join(notes, "; "))
}

// dialectFromPath infers the dialect from an executable name such as
// "/usr/bin/podman" or "docker.exe".
func dialectFromPath(path string) (Dialect, error) {
	base := strings.ToLower(filepath.Base(path))
	base = strings.TrimSuffix(base, ".exe")
	switch {
	case strings.Contains(base, "podman"):
		return Podman, nil
	case strings.Contains(base, "docker"):
		return Docker, nil
	}
	return Dialect{}, errors.New("runtime: cannot infer runtime from path " + path + "; also set the runtime name")
}
