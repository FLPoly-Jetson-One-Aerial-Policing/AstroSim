//go:build !windows

package runtime

import "os/exec"

// hideConsole is a no-op: only Windows pops up console windows.
func hideConsole(*exec.Cmd) {}
