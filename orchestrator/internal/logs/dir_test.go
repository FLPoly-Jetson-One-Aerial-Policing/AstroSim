package logs

import (
	"testing"
	"time"
)

func TestDefaultDir(t *testing.T) {
	env := func(m map[string]string) func(string) string {
		return func(k string) string { return m[k] }
	}
	tests := []struct {
		name    string
		goos    string
		env     map[string]string
		home    string
		want    string
		wantErr bool
	}{
		{"linux default", "linux", nil, "/home/u", "/home/u/.local/state/astrosim/logs", false},
		{"linux XDG", "linux", map[string]string{"XDG_STATE_HOME": "/var/state"}, "/home/u", "/var/state/astrosim/logs", false},
		{"linux XDG trailing slash", "linux", map[string]string{"XDG_STATE_HOME": "/var/state/"}, "/home/u", "/var/state/astrosim/logs", false},
		{"linux relative XDG ignored", "linux", map[string]string{"XDG_STATE_HOME": "state"}, "/home/u", "/home/u/.local/state/astrosim/logs", false},
		{"other unix", "freebsd", nil, "/home/u", "/home/u/.local/state/astrosim/logs", false},
		{"linux no home", "linux", nil, "", "", true},
		{"darwin", "darwin", map[string]string{"XDG_STATE_HOME": "/ignored"}, "/Users/u", "/Users/u/Library/Logs/AstroSim", false},
		{"darwin no home", "darwin", nil, "", "", true},
		{"windows", "windows", map[string]string{"LOCALAPPDATA": `C:\Users\u\AppData\Local`}, `C:\Users\u`, `C:\Users\u\AppData\Local\AstroSim\logs`, false},
		{"windows fallback", "windows", nil, `C:\Users\u`, `C:\Users\u\AppData\Local\AstroSim\logs`, false},
		{"windows nothing", "windows", nil, "", "", true},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got, err := DefaultDir(tc.goos, env(tc.env), tc.home)
			if (err != nil) != tc.wantErr || got != tc.want {
				t.Fatalf("got %q, %v; want %q (err %v)", got, err, tc.want, tc.wantErr)
			}
		})
	}
}

func TestSessionName(t *testing.T) {
	start := time.Date(2026, 11, 22, 13, 26, 58, 0, time.FixedZone("EST", -5*3600))
	got := SessionName(start, "3f2a9c1b0d4e5a67")
	if got != "20261122T182658Z-3f2a9c1b0d4e5a67" {
		t.Errorf("SessionName = %q", got)
	}
	if !IsSessionName(got) {
		t.Errorf("IsSessionName(%q) = false", got)
	}
}

func TestIsSessionName(t *testing.T) {
	tests := map[string]bool{
		"20261122T182658Z-3f2a9c1b0d4e5a67":        true,
		"20261122T182658Z-3f2a9c1b0d4e5a6":         false, // 15 digits
		"20261122T182658Z-3f2a9c1b0d4e5a678":       false, // 17 digits
		"20261122T182658Z-3F2A9C1B0D4E5A67":        false, // upper case
		"20261122T182658-3f2a9c1b0d4e5a67":         false, // no Z
		"x20261122T182658Z-3f2a9c1b0d4e5a67":       false,
		"20261122T182658Z-3f2a9c1b0d4e5a67.zip":    false,
		"20261122T182658Z-3f2a9c1b0d4e5a67/../etc": false,
		"20261122T182658Z-3f2a9c1b0d4e5a67\n":      false,
		"":                                         false,
		"notes":                                    false,
	}
	for name, want := range tests {
		if got := IsSessionName(name); got != want {
			t.Errorf("IsSessionName(%q) = %v, want %v", name, got, want)
		}
	}
}

func TestDefaultHost(t *testing.T) {
	if d, err := Default(); err != nil || d == "" {
		t.Skipf("no home directory here: %q, %v", d, err)
	}
}
