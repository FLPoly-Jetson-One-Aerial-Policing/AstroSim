package runtime

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"testing"
)

// byBinary is a Commander whose results depend on the executable path.
type byBinary struct {
	results map[string]Result
	tried   []string
}

func (b *byBinary) Run(_ context.Context, name string, _ ...string) (Result, error) {
	b.tried = append(b.tried, name)
	r, ok := b.results[name]
	if !ok {
		return Result{}, errors.New("no such executable")
	}
	return r, nil
}

func lookIn(paths map[string]string) func(string) (string, error) {
	return func(file string) (string, error) {
		if p, ok := paths[file]; ok {
			return p, nil
		}
		return "", fmt.Errorf("%s not on PATH", file)
	}
}

func TestDetect(t *testing.T) {
	up := Result{Stdout: "1.2.3\n"}
	down := Result{ExitCode: 1, Stderr: "cannot connect"}
	both := map[string]string{"podman": "/bin/podman", "docker": "/bin/docker"}

	tests := []struct {
		name      string
		opts      DetectOptions
		path      map[string]string
		results   map[string]Result
		wantName  string
		wantPath  string
		wantErr   error
		wantTried []string
	}{
		{"auto prefers podman", DetectOptions{}, both,
			map[string]Result{"/bin/podman": up, "/bin/docker": up}, "podman", "/bin/podman", nil, []string{"/bin/podman"}},
		{"auto falls back to docker when podman is down", DetectOptions{Name: "auto"}, both,
			map[string]Result{"/bin/podman": down, "/bin/docker": up}, "docker", "/bin/docker", nil, []string{"/bin/podman", "/bin/docker"}},
		{"auto only docker installed", DetectOptions{}, map[string]string{"docker": "/bin/docker"},
			map[string]Result{"/bin/docker": up}, "docker", "/bin/docker", nil, []string{"/bin/docker"}},
		{"explicit docker skips podman", DetectOptions{Name: "docker"}, both,
			map[string]Result{"/bin/podman": up, "/bin/docker": up}, "docker", "/bin/docker", nil, []string{"/bin/docker"}},
		{"explicit path with name", DetectOptions{Name: "podman", Path: "/opt/p/podman-custom"}, nil,
			map[string]Result{"/opt/p/podman-custom": up}, "podman", "/opt/p/podman-custom", nil, []string{"/opt/p/podman-custom"}},
		{"path infers dialect", DetectOptions{Path: `C:\Docker\docker.exe`}, nil,
			map[string]Result{`C:\Docker\docker.exe`: up}, "docker", `C:\Docker\docker.exe`, nil, nil},
		{"nothing installed", DetectOptions{}, nil, nil, "", "", ErrNotInstalled, nil},
		{"installed but down", DetectOptions{}, both,
			map[string]Result{"/bin/podman": down, "/bin/docker": down}, "", "", ErrNotRunning, nil},
		{"explicit runtime missing", DetectOptions{Name: "podman"}, map[string]string{"docker": "/bin/docker"},
			map[string]Result{"/bin/docker": up}, "", "", ErrNotInstalled, nil},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			cmd := &byBinary{results: tc.results}
			opts := tc.opts
			opts.Commander, opts.LookPath = cmd, lookIn(tc.path)
			rt, info, err := Detect(context.Background(), opts)
			if tc.wantErr != nil {
				if !errors.Is(err, tc.wantErr) {
					t.Fatalf("err = %v, want %v", err, tc.wantErr)
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			if info.Name != tc.wantName || info.Path != tc.wantPath || info.Version != "1.2.3" || rt.Info() != info {
				t.Errorf("info = %+v", info)
			}
			if tc.wantTried != nil && strings.Join(cmd.tried, ",") != strings.Join(tc.wantTried, ",") {
				t.Errorf("tried %v, want %v", cmd.tried, tc.wantTried)
			}
		})
	}
}

func TestDetectBadOptions(t *testing.T) {
	look := lookIn(nil)
	if _, _, err := Detect(context.Background(), DetectOptions{Name: "lxc", LookPath: look}); err == nil {
		t.Error("unknown runtime name should fail")
	}
	if _, _, err := Detect(context.Background(), DetectOptions{Path: "/bin/mystery", LookPath: look}); err == nil {
		t.Error("uninferable path should fail")
	}
}

func TestDetectCancelled(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	cmd := cancelCommander{}
	_, _, err := Detect(ctx, DetectOptions{Commander: cmd, LookPath: lookIn(map[string]string{"podman": "/bin/podman"})})
	if !errors.Is(err, context.Canceled) {
		t.Errorf("err = %v", err)
	}
}

type cancelCommander struct{}

func (cancelCommander) Run(ctx context.Context, _ string, _ ...string) (Result, error) {
	return Result{}, ctx.Err()
}
