package logs

import (
	"errors"
	"fmt"
	"os"
	"regexp"
	"runtime"
	"strings"
	"time"
)

// DefaultDir returns the default log root for the given operating system
// (a runtime.GOOS value). getenv looks up environment variables and home is
// the user's home directory; both are parameters so every OS can be tested
// on any host.
//
//	linux and others: $XDG_STATE_HOME/astrosim/logs, or ~/.local/state/astrosim/logs
//	darwin:           ~/Library/Logs/AstroSim
//	windows:          %LOCALAPPDATA%\AstroSim\logs
//
// The result uses the separator of the target OS. XDG_STATE_HOME is ignored
// unless it is an absolute path, as the XDG specification requires.
func DefaultDir(goos string, getenv func(string) string, home string) (string, error) {
	switch goos {
	case "windows":
		base := getenv("LOCALAPPDATA")
		if base == "" {
			if home == "" {
				return "", errors.New("logs: cannot find LOCALAPPDATA or the home directory")
			}
			base = home + `\AppData\Local`
		}
		return strings.TrimRight(base, `\/`) + `\AstroSim\logs`, nil
	case "darwin":
		if home == "" {
			return "", errors.New("logs: cannot find the home directory")
		}
		return strings.TrimRight(home, "/") + "/Library/Logs/AstroSim", nil
	default:
		if x := getenv("XDG_STATE_HOME"); strings.HasPrefix(x, "/") {
			return strings.TrimRight(x, "/") + "/astrosim/logs", nil
		}
		if home == "" {
			return "", errors.New("logs: cannot find the home directory")
		}
		return strings.TrimRight(home, "/") + "/.local/state/astrosim/logs", nil
	}
}

// Default returns DefaultDir for the host operating system and environment.
func Default() (string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		home = ""
	}
	return DefaultDir(runtime.GOOS, os.Getenv, home)
}

// sessionNameLayout is the UTC timestamp prefix of a session folder name.
const sessionNameLayout = "20060102T150405Z"

// sessionNameRE matches exactly the names SessionName produces for the
// 16-hex-digit session IDs the orchestrator generates. Archiving, recovery
// and retention only ever touch names that match, so a user's other files in
// the log root are safe.
var sessionNameRE = regexp.MustCompile(`^\d{8}T\d{6}Z-[0-9a-f]{16}$`)

// SessionName returns the folder name for a session: the UTC start time and
// the session ID, such as "20261122T182658Z-3f2a9c1b0d4e5a67". Names sort
// chronologically.
func SessionName(start time.Time, sessionID string) string {
	return fmt.Sprintf("%s-%s", start.UTC().Format(sessionNameLayout), sessionID)
}

// IsSessionName reports whether name is a session folder name (without any
// ".zip" or ".zip.partial" suffix).
func IsSessionName(name string) bool { return sessionNameRE.MatchString(name) }
