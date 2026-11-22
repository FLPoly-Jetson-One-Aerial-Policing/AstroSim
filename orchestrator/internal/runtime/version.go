package runtime

import (
	"errors"
	"fmt"
	"log/slog"
	"strconv"
	"strings"
)

// ErrTooOld means the runtime is older than the minimum the orchestrator
// supports. Match it with errors.Is.
var ErrTooOld = errors.New("runtime: container runtime is too old")

// TooOldError is the error CheckMinVersion returns. It matches ErrTooOld.
type TooOldError struct {
	// Name is the runtime name, "podman" or "docker".
	Name string
	// Found is the version string the runtime reported.
	Found string
	// Min is the oldest supported version.
	Min Version
}

// Error describes the problem in a sentence fit for the dashboard.
func (e *TooOldError) Error() string {
	return fmt.Sprintf("%s %s found; %s or newer is required", e.Name, e.Found, e.Min)
}

// Is makes errors.Is(err, ErrTooOld) true.
func (e *TooOldError) Is(target error) bool { return target == ErrTooOld }

// Version is a numeric major.minor.patch version. Missing parts are zero.
type Version struct {
	// Major, Minor and Patch are the numeric components.
	Major, Minor, Patch int
}

// String formats v as "major.minor" when the patch is zero, else
// "major.minor.patch".
func (v Version) String() string {
	if v.Patch == 0 {
		return fmt.Sprintf("%d.%d", v.Major, v.Minor)
	}
	return fmt.Sprintf("%d.%d.%d", v.Major, v.Minor, v.Patch)
}

// Less reports whether v is older than o.
func (v Version) Less(o Version) bool {
	if v.Major != o.Major {
		return v.Major < o.Major
	}
	if v.Minor != o.Minor {
		return v.Minor < o.Minor
	}
	return v.Patch < o.Patch
}

// minVersions holds the oldest supported release of each runtime.
//
// podman 4.0 introduced netavark and aardvark-dns, which provide the DNS
// name resolution between containers on a user network that the "bridge"
// network mode relies on. docker 20.10 is the oldest release still
// documented as supported by the --network-alias and user-network behaviour
// used here, and the first with the stable Server.Version template field
// across platforms.
var minVersions = map[string]Version{
	"podman": {Major: 4, Minor: 0},
	"docker": {Major: 20, Minor: 10},
}

// MinVersion returns the minimum supported version of the named runtime.
func MinVersion(name string) (Version, bool) {
	v, ok := minVersions[name]
	return v, ok
}

// ParseVersion leniently parses a version string such as "5.8.9", "v24.0.7",
// "4.9.4-rhel" or "28.1.1+dfsg": an optional "v", then up to three
// dot-separated numbers, ignoring any suffix. It reports false when the
// string does not start with a number.
func ParseVersion(s string) (Version, bool) {
	s = strings.TrimPrefix(strings.TrimSpace(s), "v")
	var parts [3]int
	n := 0
	for n < 3 {
		end := 0
		for end < len(s) && s[end] >= '0' && s[end] <= '9' {
			end++
		}
		if end == 0 {
			break
		}
		parts[n], _ = strconv.Atoi(s[:end])
		n++
		s = s[end:]
		if !strings.HasPrefix(s, ".") {
			break
		}
		s = s[1:]
	}
	if n == 0 {
		return Version{}, false
	}
	return Version{parts[0], parts[1], parts[2]}, true
}

// CheckMinVersion verifies that info describes a runtime at or above the
// supported minimum. It returns a *TooOldError (matching ErrTooOld) when the
// runtime is too old. An unknown runtime or an unparseable version passes,
// with a warning on log (which may be nil):
// refusing to start over a version string we cannot read would be worse than
// the risk it guards against.
func CheckMinVersion(info Info, log *slog.Logger) error {
	min, ok := MinVersion(info.Name)
	if !ok {
		return nil
	}
	got, ok := ParseVersion(info.Version)
	if !ok {
		if log != nil {
			log.Warn("cannot parse container runtime version; skipping the minimum version check",
				"runtime", info.Name, "version", info.Version)
		}
		return nil
	}
	if got.Less(min) {
		return &TooOldError{Name: info.Name, Found: strings.TrimSpace(info.Version), Min: min}
	}
	return nil
}
