package runtime

import (
	"bytes"
	"errors"
	"log/slog"
	"strings"
	"testing"
)

func TestParseVersion(t *testing.T) {
	tests := []struct {
		in   string
		want Version
		ok   bool
	}{
		{"5.8.9", Version{5, 8, 9}, true},
		{"24.0.7", Version{24, 0, 7}, true},
		{"4.9.4-rhel", Version{4, 9, 4}, true},
		{"28.1.1+dfsg", Version{28, 1, 1}, true},
		{" v20.10\n", Version{20, 10, 0}, true},
		{"4", Version{4, 0, 0}, true},
		{"3.4.4-dev.1", Version{3, 4, 4}, true},
		{"", Version{}, false},
		{"unknown", Version{}, false},
		{"-1.2", Version{}, false},
	}
	for _, tt := range tests {
		got, ok := ParseVersion(tt.in)
		if got != tt.want || ok != tt.ok {
			t.Errorf("ParseVersion(%q) = %v, %v; want %v, %v", tt.in, got, ok, tt.want, tt.ok)
		}
	}
}

func TestCheckMinVersion(t *testing.T) {
	tests := []struct {
		name    string
		info    Info
		wantErr string
		warn    bool
	}{
		{"podman new", Info{Name: "podman", Version: "5.8.9"}, "", false},
		{"podman exact", Info{Name: "podman", Version: "4.0.0"}, "", false},
		{"podman old", Info{Name: "podman", Version: "3.4.4"}, "podman 3.4.4 found; 4.0 or newer is required", false},
		{"docker new", Info{Name: "docker", Version: "28.1.1+dfsg"}, "", false},
		{"docker exact", Info{Name: "docker", Version: "20.10.0"}, "", false},
		{"docker old", Info{Name: "docker", Version: "19.03.15"}, "docker 19.03.15 found; 20.10 or newer is required", false},
		{"unparseable passes with a warning", Info{Name: "docker", Version: "weird"}, "", true},
		{"empty passes with a warning", Info{Name: "podman"}, "", true},
		{"unknown runtime passes", Info{Name: "nerdctl", Version: "0.1"}, "", false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			var buf bytes.Buffer
			log := slog.New(slog.NewTextHandler(&buf, nil))
			err := CheckMinVersion(tt.info, log)
			if tt.wantErr == "" {
				if err != nil {
					t.Fatalf("err = %v", err)
				}
			} else if !errors.Is(err, ErrTooOld) || !strings.Contains(err.Error(), tt.wantErr) {
				t.Fatalf("err = %v, want too old containing %q", err, tt.wantErr)
			}
			if warned := buf.Len() > 0; warned != tt.warn {
				t.Errorf("warned = %v, want %v (%s)", warned, tt.warn, buf.String())
			}
		})
	}
	if err := CheckMinVersion(Info{Name: "podman", Version: "x"}, nil); err != nil {
		t.Errorf("nil logger: %v", err)
	}
}
