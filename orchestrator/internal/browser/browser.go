// Package browser opens a URL in the user's default web browser.
package browser

import (
	"fmt"
	"os/exec"
	"runtime"
)

// command returns the program and arguments that open url on goos.
func command(goos, url string) (string, []string, error) {
	switch goos {
	case "linux", "freebsd", "openbsd", "netbsd":
		return "xdg-open", []string{url}, nil
	case "darwin":
		return "open", []string{url}, nil
	case "windows":
		// rundll32 hands the URL to the shell's default handler without
		// going through cmd.exe, so no characters in the URL need escaping.
		return "rundll32", []string{"url.dll,FileProtocolHandler", url}, nil
	}
	return "", nil, fmt.Errorf("browser: don't know how to open a browser on %s", goos)
}

// Open asks the operating system to open url in the default browser. It
// starts the helper program without a shell and does not wait for it, so it
// returns as soon as the helper is running; it is a best effort, and the
// caller should always also print the URL.
func Open(url string) error {
	name, args, err := command(runtime.GOOS, url)
	if err != nil {
		return err
	}
	cmd := exec.Command(name, args...)
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("browser: %w", err)
	}
	// Reap the helper in the background to avoid a zombie.
	go func() { _ = cmd.Wait() }()
	return nil
}
