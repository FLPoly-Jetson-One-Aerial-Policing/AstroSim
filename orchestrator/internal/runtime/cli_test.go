package runtime

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"
)

// call is one recorded Commander invocation.
type call struct {
	name string
	args []string
}

// scripted is a Commander that records argv and replays results. Results are
// matched by the first argument(s) joined with spaces (longest prefix wins);
// unmatched commands exit 0 with no output.
type scripted struct {
	calls   []call
	results map[string]Result
	err     error
}

func (s *scripted) Run(_ context.Context, name string, args ...string) (Result, error) {
	s.calls = append(s.calls, call{name, append([]string(nil), args...)})
	if s.err != nil {
		return Result{}, s.err
	}
	best, bestLen := Result{}, -1
	for k, r := range s.results {
		if strings.HasPrefix(strings.Join(args, " "), k) && len(k) > bestLen {
			best, bestLen = r, len(k)
		}
	}
	return best, nil
}

func (s *scripted) last() []string { return s.calls[len(s.calls)-1].args }

func fixture(t *testing.T, name string) string {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("testdata", name))
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

func TestRunArgs(t *testing.T) {
	base := RunSpec{
		Name:    "astrosim-fc",
		Image:   "astrosim-fc:latest",
		Network: "astrosim",
		Aliases: []string{"fc"},
		Publish: []Publish{
			{HostAddr: "127.0.0.1", HostPort: 8766, ContainerPort: 8766, Proto: "udp"},
			{HostPort: 80, ContainerPort: 8080},
		},
		Env:    map[string]string{"B": "2", "A": "1"},
		Labels: map[string]string{"z": "last", "astrosim.managed": "true"},
	}
	host := base
	host.Network = HostNetwork
	noNet := RunSpec{Name: "n", Image: "i"}

	tests := []struct {
		name string
		spec RunSpec
		want []string
	}{
		{"bridge", base, []string{
			"run", "-d", "--name", "astrosim-fc", "--network", "astrosim",
			"--network-alias", "fc",
			"-p", "127.0.0.1:8766:8766/udp", "-p", "80:8080/tcp",
			"-e", "A=1", "-e", "B=2",
			"--label", "astrosim.managed=true", "--label", "z=last",
			"astrosim-fc:latest"}},
		{"host omits publish and aliases", host, []string{
			"run", "-d", "--name", "astrosim-fc", "--network", "host",
			"-e", "A=1", "-e", "B=2",
			"--label", "astrosim.managed=true", "--label", "z=last",
			"astrosim-fc:latest"}},
		{"minimal", noNet, []string{"run", "-d", "--name", "n", "i"}},
	}
	for _, d := range []Dialect{Podman, Docker} {
		cli := NewCLI("rt", d, &scripted{})
		for _, tc := range tests {
			t.Run(d.Name+"/"+tc.name, func(t *testing.T) {
				for i := 0; i < 20; i++ { // map iteration order must not leak
					if got := cli.RunArgs(tc.spec); !reflect.DeepEqual(got, tc.want) {
						t.Fatalf("got  %q\nwant %q", got, tc.want)
					}
				}
			})
		}
	}
}

func TestRunNoRm(t *testing.T) {
	sc := &scripted{results: map[string]Result{"run": {Stdout: "abc123\n"}}}
	cli := NewCLI("podman", Podman, sc)
	id, err := cli.Run(context.Background(), RunSpec{Name: "n", Image: "i"})
	if err != nil || id != "abc123" {
		t.Fatalf("id=%q err=%v", id, err)
	}
	for _, a := range sc.last() {
		if a == "--rm" {
			t.Error("run must not pass --rm")
		}
	}
}

func TestRunFailure(t *testing.T) {
	sc := &scripted{results: map[string]Result{"run": {ExitCode: 125, Stderr: "Error: port in use\n"}}}
	_, err := NewCLI("podman", Podman, sc).Run(context.Background(), RunSpec{Name: "n", Image: "i"})
	var ce *CommandError
	if !errors.As(err, &ce) || ce.ExitCode != 125 || ce.Stderr != "Error: port in use" {
		t.Fatalf("err = %v", err)
	}
	if errors.Is(err, ErrNotFound) {
		t.Error("should not be not-found")
	}
}

func TestImageExists(t *testing.T) {
	tests := []struct {
		name    string
		dialect Dialect
		res     Result
		args    []string
		want    bool
		wantErr bool
	}{
		{"podman present", Podman, Result{}, []string{"image", "exists", "img"}, true, false},
		{"podman missing", Podman, Result{ExitCode: 1}, nil, false, false},
		{"podman engine error", Podman, Result{ExitCode: 125, Stderr: "Error: cannot connect"}, nil, false, true},
		{"docker present", Docker, Result{Stdout: "sha256:abc"}, []string{"image", "inspect", "--format", "{{.Id}}", "img"}, true, false},
		{"docker missing", Docker, Result{ExitCode: 1, Stderr: "Error response from daemon: No such image: img"}, nil, false, false},
		{"docker daemon down", Docker, Result{ExitCode: 1, Stderr: "Cannot connect to the Docker daemon at unix:///var/run/docker.sock. Is the docker daemon running?"}, nil, false, true},
		{"docker permission denied", Docker, Result{ExitCode: 1, Stderr: "permission denied while trying to connect to the Docker daemon socket"}, nil, false, true},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			sc := &scripted{results: map[string]Result{"image": tc.res}}
			got, err := NewCLI("rt", tc.dialect, sc).ImageExists(context.Background(), "img")
			if got != tc.want || (err != nil) != tc.wantErr {
				t.Fatalf("got %v, %v", got, err)
			}
			if tc.args != nil && !reflect.DeepEqual(sc.last(), tc.args) {
				t.Errorf("args = %q", sc.last())
			}
		})
	}
}

func TestImageExistsCommanderError(t *testing.T) {
	boom := errors.New("boom")
	_, err := NewCLI("rt", Podman, &scripted{err: boom}).ImageExists(context.Background(), "i")
	if !errors.Is(err, boom) {
		t.Fatalf("err = %v", err)
	}
}

func TestInspect(t *testing.T) {
	ts := func(s string) time.Time {
		v, err := time.Parse(time.RFC3339Nano, s)
		if err != nil {
			t.Fatal(err)
		}
		return v
	}
	tests := []struct {
		file    string
		dialect Dialect
		want    ContainerState
	}{
		{"podman_running.json", Podman, ContainerState{
			Status: "running", Running: true, StartedAt: ts("2026-11-22T12:54:57.320398968-05:00")}},
		{"podman_exited.json", Podman, ContainerState{
			Status: "exited", ExitCode: 137,
			StartedAt:  ts("2026-11-22T12:54:57.320398968-05:00"),
			FinishedAt: ts("2026-11-22T12:54:57.409547198-05:00")}},
		{"podman_healthy.json", Podman, ContainerState{
			Status: "running", Running: true, Health: "healthy", StartedAt: ts("2026-11-22T13:02:11.512003114-05:00")}},
		{"podman4_unhealthy.json", Podman, ContainerState{
			Status: "running", Running: true, Health: "unhealthy", StartedAt: ts("2026-11-22T13:02:11.512003114-05:00")}},
		{"docker_running.json", Docker, ContainerState{
			Status: "running", Running: true, StartedAt: ts("2026-11-22T18:05:42.318276541Z")}},
		{"docker_exited.json", Docker, ContainerState{
			Status: "exited", ExitCode: 137, OOMKilled: true,
			StartedAt:  ts("2026-11-22T18:05:42.318276541Z"),
			FinishedAt: ts("2026-11-22T18:07:01.902114873Z")}},
		{"docker_healthy.json", Docker, ContainerState{
			Status: "running", Running: true, Health: "healthy", StartedAt: ts("2026-11-22T18:05:42.318276541Z")}},
	}
	for _, tc := range tests {
		t.Run(tc.file, func(t *testing.T) {
			sc := &scripted{results: map[string]Result{"inspect": {Stdout: fixture(t, tc.file)}}}
			got, err := NewCLI("rt", tc.dialect, sc).Inspect(context.Background(), "c")
			if err != nil {
				t.Fatal(err)
			}
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("got  %+v\nwant %+v", got, tc.want)
			}
			want := []string{"inspect", "--format", "{{json .State}}", "c"}
			if !reflect.DeepEqual(sc.last(), want) {
				t.Errorf("args = %q", sc.last())
			}
		})
	}
}

func TestInspectErrors(t *testing.T) {
	tests := []struct {
		name         string
		res          Result
		wantNotFound bool
	}{
		{"podman missing", Result{ExitCode: 125, Stderr: `Error: no such object: "c"`}, true},
		{"docker missing", Result{ExitCode: 1, Stderr: "Error: No such object: c"}, true},
		{"daemon down", Result{ExitCode: 1, Stderr: "Cannot connect to the Docker daemon"}, false},
		{"bad json", Result{Stdout: "not json"}, false},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			sc := &scripted{results: map[string]Result{"inspect": tc.res}}
			_, err := NewCLI("rt", Docker, sc).Inspect(context.Background(), "c")
			if err == nil {
				t.Fatal("want error")
			}
			if errors.Is(err, ErrNotFound) != tc.wantNotFound {
				t.Errorf("ErrNotFound = %v for %v", !tc.wantNotFound, err)
			}
		})
	}
}

func TestStopRemoveWait(t *testing.T) {
	sc := &scripted{results: map[string]Result{
		"wait ok":      {Stdout: "143\n"},
		"wait garbage": {Stdout: "x"},
		"rm -f gone":   {ExitCode: 1, Stderr: "Error: No such container: gone"},
		"rm -f bad":    {ExitCode: 1, Stderr: "permission denied"},
	}}
	cli := NewCLI("docker", Docker, sc)
	ctx := context.Background()

	if err := cli.Stop(ctx, "c", 3*time.Second); err != nil {
		t.Fatal(err)
	}
	if want := []string{"stop", "-t", "3", "c"}; !reflect.DeepEqual(sc.last(), want) {
		t.Errorf("stop args = %q", sc.last())
	}
	if code, err := cli.Wait(ctx, "ok"); err != nil || code != 143 {
		t.Errorf("wait = %d, %v", code, err)
	}
	if _, err := cli.Wait(ctx, "garbage"); err == nil {
		t.Error("want parse error")
	}
	if err := cli.Remove(ctx, "gone"); err != nil {
		t.Errorf("remove missing: %v", err)
	}
	if err := cli.Remove(ctx, "bad"); err == nil {
		t.Error("want error for real failure")
	}
}

func TestListByLabel(t *testing.T) {
	sc := &scripted{results: map[string]Result{"ps": {Stdout: "astrosim-fc\n\nastrosim-engine\n"}}}
	got, err := NewCLI("rt", Podman, sc).ListByLabel(context.Background(), "astrosim.managed", "true")
	if err != nil {
		t.Fatal(err)
	}
	if want := []string{"astrosim-engine", "astrosim-fc"}; !reflect.DeepEqual(got, want) {
		t.Errorf("got %q", got)
	}
	want := []string{"ps", "-a", "--filter", "label=astrosim.managed=true", "--format", "{{.Names}}"}
	if !reflect.DeepEqual(sc.last(), want) {
		t.Errorf("args = %q", sc.last())
	}
}

func TestLogs(t *testing.T) {
	sc := &scripted{results: map[string]Result{"logs": {Stdout: "out\n", Stderr: "err\n"}}}
	got, err := NewCLI("rt", Podman, sc).Logs(context.Background(), "c", 20)
	if err != nil || got != "out\nerr\n" {
		t.Fatalf("got %q, %v", got, err)
	}
	if want := []string{"logs", "--tail", "20", "c"}; !reflect.DeepEqual(sc.last(), want) {
		t.Errorf("args = %q", sc.last())
	}
}

func TestEnsureNetwork(t *testing.T) {
	ctx := context.Background()
	t.Run("exists", func(t *testing.T) {
		sc := &scripted{}
		if err := NewCLI("rt", Podman, sc).EnsureNetwork(ctx, "astrosim"); err != nil {
			t.Fatal(err)
		}
		if len(sc.calls) != 1 {
			t.Errorf("calls = %v", sc.calls)
		}
	})
	t.Run("created", func(t *testing.T) {
		sc := &scripted{results: map[string]Result{
			"network inspect": {ExitCode: 125, Stderr: "Error: network not found: astrosim"}}}
		if err := NewCLI("rt", Podman, sc).EnsureNetwork(ctx, "astrosim"); err != nil {
			t.Fatal(err)
		}
		if want := []string{"network", "create", "astrosim"}; !reflect.DeepEqual(sc.last(), want) {
			t.Errorf("args = %q", sc.last())
		}
	})
	t.Run("inspect fails for another reason", func(t *testing.T) {
		sc := &scripted{results: map[string]Result{"network inspect": {ExitCode: 1, Stderr: "boom"}}}
		if err := NewCLI("rt", Docker, sc).EnsureNetwork(ctx, "astrosim"); err == nil {
			t.Fatal("want error")
		}
		if len(sc.calls) != 1 {
			t.Error("must not try to create")
		}
	})
}

func TestPing(t *testing.T) {
	ctx := context.Background()
	sc := &scripted{results: map[string]Result{"version": {Stdout: "5.8.9\n"}}}
	info, err := NewCLI("/usr/bin/podman", Podman, sc).Ping(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if info != (Info{Name: "podman", Path: "/usr/bin/podman", Version: "5.8.9"}) {
		t.Errorf("info = %+v", info)
	}
	sc = &scripted{results: map[string]Result{"version": {ExitCode: 1, Stderr: "Cannot connect"}}}
	if _, err := NewCLI("docker", Docker, sc).Ping(ctx); !errors.Is(err, ErrNotRunning) {
		t.Errorf("err = %v", err)
	}
}
