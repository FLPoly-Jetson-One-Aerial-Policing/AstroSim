// Package podman is a thin wrapper over the podman CLI.
package podman

import (
	"fmt"
	"os/exec"
	"strings"
)

func run(args ...string) (string, error) {
	out, err := exec.Command("podman", args...).CombinedOutput()
	return strings.TrimSpace(string(out)), err
}

func Available() error {
	_, err := exec.LookPath("podman")
	return err
}

func ImageExists(image string) bool {
	return exec.Command("podman", "image", "exists", image).Run() == nil
}

func Build(image, containerfile, contextDir string) (string, error) {
	return run("build", "-t", image, "-f", containerfile, contextDir)
}

// Exists reports whether a container with this name exists in any state.
func Exists(name string) bool {
	return exec.Command("podman", "container", "exists", name).Run() == nil
}

func Running(name string) bool {
	out, err := run("container", "inspect", "-f", "{{.State.Running}}", name)
	return err == nil && out == "true"
}

func Stop(name string, graceSecs int) error {
	_, err := run("stop", "-t", fmt.Sprint(graceSecs), name)
	return err
}

func RemoveForce(name string) error {
	_, err := run("rm", "-f", name)
	return err
}

type RunOpts struct {
	Name     string
	Image    string
	Port     int
	HostNet  bool
	TickHz   int
	ExtraEnv []string
}

// RunArgs returns args for an attached `podman run` (the process lives as long as the container).
func RunArgs(o RunOpts) []string {
	a := []string{"run", "--rm", "--name", o.Name}
	if o.HostNet {
		a = append(a, "--network=host")
	} else {
		a = append(a, "-p", fmt.Sprintf("%d:8765/udp", o.Port))
	}
	if o.TickHz > 0 {
		a = append(a, "-e", fmt.Sprintf("TICK_HZ=%d", o.TickHz))
	}
	for _, e := range o.ExtraEnv {
		a = append(a, "-e", e)
	}
	return append(a, o.Image)
}
