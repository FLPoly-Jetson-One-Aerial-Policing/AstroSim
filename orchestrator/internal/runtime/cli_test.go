package runtime

import (
	"context"
	"errors"
	"io"
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
				want := tc.want
				if d.Name == "podman" { // podman pins the k8s-file log driver
					want = append(append(append([]string(nil), want[:4]...),
						"--log-driver", "k8s-file"), want[4:]...)
				}
				for i := 0; i < 20; i++ { // map iteration order must not leak
					if got := cli.RunArgs(tc.spec); !reflect.DeepEqual(got, want) {
						t.Fatalf("got  %q\nwant %q", got, want)
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
	fakeSleep(t, 0, nil)
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

// seqCommander replays a fixed sequence of results per command key
// (the arguments joined by spaces); the last result repeats.
type seqCommander struct {
	seq   map[string][]Result
	calls []string
}

func (s *seqCommander) Run(_ context.Context, _ string, args ...string) (Result, error) {
	key := strings.Join(args, " ")
	s.calls = append(s.calls, key)
	q := s.seq[key]
	if len(q) == 0 {
		return Result{}, nil
	}
	r := q[0]
	if len(q) > 1 {
		s.seq[key] = q[1:]
	}
	return r, nil
}

func (s *seqCommander) count(key string) int {
	n := 0
	for _, c := range s.calls {
		if c == key {
			n++
		}
	}
	return n
}

// fakeSleep replaces sleep for a test and records the requested waits.
// A non-nil failAt ends the wait with that error on the n-th call (1-based).
func fakeSleep(t *testing.T, failAt int, err error) *[]time.Duration {
	t.Helper()
	old := sleep
	t.Cleanup(func() { sleep = old })
	var waits []time.Duration
	sleep = func(_ context.Context, d time.Duration) error {
		waits = append(waits, d)
		if failAt > 0 && len(waits) == failAt {
			return err
		}
		return nil
	}
	return &waits
}

// TestRemoveGoneAfterFailure covers rootless podman failing to tidy the
// network after it already deleted the container.
func TestRemoveGoneAfterFailure(t *testing.T) {
	const (
		rm  = "rm -f c"
		ins = "inspect --format {{json .State}} c"
	)
	netns := Result{ExitCode: 125, Stderr: fixture(t, "podman_rm_netns_error.log")}
	notFound := Result{ExitCode: 125, Stderr: "Error: no such object: \"c\""}
	running := Result{Stdout: fixture(t, "podman_running.json")}
	unknown := Result{ExitCode: 1, Stderr: "cannot connect to the engine"}
	full := len(removeWaits)

	tests := []struct {
		name      string
		seq       map[string][]Result
		wantErr   bool
		wantWaits int
		wantRm    int
	}{
		{"rm succeeds", map[string][]Result{rm: {{}}}, false, 0, 1},
		{"rm reports not found", map[string][]Result{rm: {notFound}}, false, 0, 1},
		{"gone on the first poll", map[string][]Result{rm: {netns}, ins: {notFound}}, false, 1, 1},
		{"gone after five polls, no rm retry", map[string][]Result{
			rm: {netns}, ins: {running, running, running, running, notFound},
		}, false, 5, 1},
		{"still there, final rm succeeds", map[string][]Result{rm: {netns, {}}, ins: {running}}, false, full, 2},
		{"still there, final rm reports not found", map[string][]Result{rm: {netns, notFound}, ins: {running}}, false, full, 2},
		{"never gone", map[string][]Result{rm: {netns}, ins: {running}}, true, full, 2},
		{"inspect unknown, then gone", map[string][]Result{rm: {netns}, ins: {unknown, unknown, notFound}}, false, 3, 1},
		{"inspect always unknown", map[string][]Result{rm: {netns}, ins: {unknown}}, true, full, 2},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			waits := fakeSleep(t, 0, nil)
			sc := &seqCommander{seq: tt.seq}
			err := NewCLI("podman", Podman, sc).Remove(context.Background(), "c")
			if (err != nil) != tt.wantErr {
				t.Fatalf("err = %v, wantErr %v", err, tt.wantErr)
			}
			if tt.wantErr {
				var ce *CommandError
				if !errors.As(err, &ce) || ce.ExitCode != 125 || !strings.Contains(err.Error(), "kill network process") {
					t.Errorf("err = %v, want the original rm failure", err)
				}
			}
			if len(*waits) != tt.wantWaits {
				t.Errorf("waits = %d, want %d", len(*waits), tt.wantWaits)
			}
			if n := sc.count(rm); n != tt.wantRm {
				t.Errorf("rm calls = %d, want %d", n, tt.wantRm)
			}
		})
	}
}

// TestRemoveWaitsBounded checks the poll schedule totals between 10 s and 16 s.
func TestRemoveWaitsBounded(t *testing.T) {
	var total time.Duration
	for _, d := range removeWaits {
		total += d
	}
	if total < 10*time.Second || total > 16*time.Second {
		t.Errorf("total wait %v, want within [10s, 16s]", total)
	}
}

// TestRemoveContextCancelled checks a context ending during the wait stops
// the polling, issues no further rm and reports the original failure.
func TestRemoveContextCancelled(t *testing.T) {
	fakeSleep(t, 2, context.Canceled)
	netns := Result{ExitCode: 125, Stderr: fixture(t, "podman_rm_netns_error.log")}
	running := Result{Stdout: fixture(t, "podman_running.json")}
	sc := &seqCommander{seq: map[string][]Result{
		"rm -f c": {netns}, "inspect --format {{json .State}} c": {running},
	}}
	err := NewCLI("podman", Podman, sc).Remove(context.Background(), "c")
	if !errors.Is(err, context.Canceled) {
		t.Errorf("err = %v, want context.Canceled", err)
	}
	var ce *CommandError
	if !errors.As(err, &ce) {
		t.Errorf("err = %v, want the rm failure wrapped", err)
	}
	if n := sc.count("inspect --format {{json .State}} c"); n != 1 {
		t.Errorf("inspect calls = %d, want 1", n)
	}
	if n := sc.count("rm -f c"); n != 1 {
		t.Errorf("rm calls = %d, want 1", n)
	}
}

// TestSleepHonoursContext checks the real wait returns early on cancel.
func TestSleepHonoursContext(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := sleep(ctx, time.Hour); !errors.Is(err, context.Canceled) {
		t.Errorf("err = %v, want context.Canceled", err)
	}
	if err := sleep(context.Background(), time.Millisecond); err != nil {
		t.Errorf("err = %v", err)
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
	labels := map[string]string{"astrosim.managed": "true", "a": "b"}
	t.Run("exists", func(t *testing.T) {
		sc := &scripted{}
		if err := NewCLI("rt", Podman, sc).EnsureNetwork(ctx, "astrosim", labels); err != nil {
			t.Fatal(err)
		}
		if len(sc.calls) != 1 {
			t.Errorf("calls = %v", sc.calls)
		}
	})
	t.Run("created", func(t *testing.T) {
		sc := &scripted{results: map[string]Result{
			"network inspect": {ExitCode: 125, Stderr: "Error: network not found: astrosim"}}}
		if err := NewCLI("rt", Podman, sc).EnsureNetwork(ctx, "astrosim", labels); err != nil {
			t.Fatal(err)
		}
		if want := []string{"network", "create", "--label", "a=b", "--label", "astrosim.managed=true", "astrosim"}; !reflect.DeepEqual(sc.last(), want) {
			t.Errorf("args = %q", sc.last())
		}
	})
	t.Run("inspect fails for another reason", func(t *testing.T) {
		sc := &scripted{results: map[string]Result{"network inspect": {ExitCode: 1, Stderr: "boom"}}}
		if err := NewCLI("rt", Docker, sc).EnsureNetwork(ctx, "astrosim", labels); err == nil {
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

func TestListNetworksByLabel(t *testing.T) {
	for _, d := range []Dialect{Podman, Docker} {
		sc := &scripted{results: map[string]Result{"network ls": {Stdout: "zeta\nastrosim\n\n"}}}
		got, err := NewCLI("rt", d, sc).ListNetworksByLabel(context.Background(), "astrosim.managed", "true")
		if err != nil {
			t.Fatal(err)
		}
		if want := []string{"astrosim", "zeta"}; !reflect.DeepEqual(got, want) {
			t.Errorf("got %q, want %q", got, want)
		}
		want := []string{"network", "ls", "--filter", "label=astrosim.managed=true", "--format", "{{.Name}}"}
		if !reflect.DeepEqual(sc.last(), want) {
			t.Errorf("args = %q", sc.last())
		}
	}
}

func TestRemoveNetwork(t *testing.T) {
	ctx := context.Background()
	tests := []struct {
		name    string
		res     Result
		wantErr bool
	}{
		{"removed", Result{}, false},
		{"podman not found", Result{ExitCode: 1, Stderr: "Error: unable to find network with name or ID x: network not found"}, false},
		{"docker not found", Result{ExitCode: 1, Stderr: "Error response from daemon: network x not found"}, false},
		{"in use", Result{ExitCode: 1, Stderr: "network has active endpoints"}, true},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			sc := &scripted{results: map[string]Result{"network rm": tt.res}}
			err := NewCLI("rt", Podman, sc).RemoveNetwork(ctx, "x")
			if (err != nil) != tt.wantErr {
				t.Errorf("err = %v, wantErr %v", err, tt.wantErr)
			}
			if want := []string{"network", "rm", "x"}; !reflect.DeepEqual(sc.last(), want) {
				t.Errorf("args = %q", sc.last())
			}
		})
	}
}

// streamer is a Commander that is also a Streamer, replaying scripted output.
type streamer struct {
	scripted
	out, err string
	code     int
	runErr   error
}

func (s *streamer) Stream(_ context.Context, stdout, stderr io.Writer, name string, args ...string) (int, error) {
	s.calls = append(s.calls, call{name, append([]string(nil), args...)})
	if s.runErr != nil {
		return 0, s.runErr
	}
	io.WriteString(stdout, s.out)
	io.WriteString(stderr, s.err)
	return s.code, nil
}

func TestFollowLogsArgs(t *testing.T) {
	since := time.Date(2026, 11, 22, 14, 0, 0, 123456789, time.FixedZone("EST", -5*3600))
	tests := []struct {
		name string
		opts LogOptions
		want []string
	}{
		{"plain", LogOptions{}, []string{"logs", "--timestamps", "c"}},
		{"follow", LogOptions{Follow: true}, []string{"logs", "--timestamps", "--follow", "c"}},
		{"since is UTC", LogOptions{Follow: true, Since: since},
			[]string{"logs", "--timestamps", "--follow", "--since", "2026-11-22T19:00:00.123456789Z", "c"}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			st := &streamer{out: "o\n", err: "e\n"}
			var out, errb strings.Builder
			if err := NewCLI("rt", Podman, st).FollowLogs(context.Background(), "c", tc.opts, &out, &errb); err != nil {
				t.Fatal(err)
			}
			if !reflect.DeepEqual(st.last(), tc.want) {
				t.Errorf("args = %q, want %q", st.last(), tc.want)
			}
			if out.String() != "o\n" || errb.String() != "e\n" {
				t.Errorf("out=%q err=%q", out.String(), errb.String())
			}
		})
	}
}

func TestFollowLogsErrors(t *testing.T) {
	ctx := context.Background()
	t.Run("not found", func(t *testing.T) {
		st := &streamer{code: 125, err: "Error: no container with name or ID \"c\" found: no such container\n"}
		err := NewCLI("rt", Podman, st).FollowLogs(ctx, "c", LogOptions{}, io.Discard, io.Discard)
		var ce *CommandError
		if !errors.Is(err, ErrNotFound) || !errors.As(err, &ce) || ce.ExitCode != 125 {
			t.Fatalf("err = %v", err)
		}
	})
	t.Run("other failure keeps stderr tail", func(t *testing.T) {
		st := &streamer{code: 1, err: strings.Repeat("x", 10000) + "boom\n"}
		err := NewCLI("rt", Podman, st).FollowLogs(ctx, "c", LogOptions{}, io.Discard, io.Discard)
		var ce *CommandError
		if !errors.As(err, &ce) || errors.Is(err, ErrNotFound) {
			t.Fatalf("err = %v", err)
		}
		if len(ce.Stderr) > maxErrText || !strings.HasSuffix(ce.Stderr, "boom") {
			t.Errorf("stderr tail len=%d %q", len(ce.Stderr), ce.Stderr[len(ce.Stderr)-10:])
		}
	})
	t.Run("run error", func(t *testing.T) {
		want := errors.New("cancelled")
		err := NewCLI("rt", Podman, &streamer{runErr: want}).FollowLogs(ctx, "c", LogOptions{}, io.Discard, io.Discard)
		if !errors.Is(err, want) {
			t.Fatalf("err = %v", err)
		}
	})
	t.Run("commander cannot stream", func(t *testing.T) {
		err := NewCLI("rt", Podman, &scripted{}).FollowLogs(ctx, "c", LogOptions{}, io.Discard, io.Discard)
		if err == nil || !strings.Contains(err.Error(), "cannot stream") {
			t.Fatalf("err = %v", err)
		}
	})
}

// TestPullArgs checks the command line and that both output streams reach
// the progress writer, whichever one a dialect uses for its progress.
func TestPullArgs(t *testing.T) {
	for _, d := range []Dialect{Podman, Docker} {
		t.Run(d.Name, func(t *testing.T) {
			st := &streamer{out: "Pulling fs layer\n", err: "Copying blob x\n"}
			var progress strings.Builder
			if err := NewCLI("rt", d, st).Pull(context.Background(), "ghcr.io/o/x:1", &progress); err != nil {
				t.Fatal(err)
			}
			if got, want := st.last(), []string{"pull", "ghcr.io/o/x:1"}; !reflect.DeepEqual(got, want) {
				t.Errorf("args = %v, want %v", got, want)
			}
			if got := progress.String(); !strings.Contains(got, "Pulling fs layer\n") || !strings.Contains(got, "Copying blob x\n") {
				t.Errorf("progress = %q", got)
			}
		})
	}
	t.Run("nil progress", func(t *testing.T) {
		st := &streamer{out: "o\n", err: "e\n"}
		if err := NewCLI("rt", Podman, st).Pull(context.Background(), "ghcr.io/o/x:1", nil); err != nil {
			t.Fatal(err)
		}
	})
}

// TestPullErrors classifies real podman failures (captured by running podman
// 5.8 against ghcr.io, docker.io and an .invalid host) and docker failures
// (hand-written, as docker was not available; see the README). The
// podman_pull_no_space fixture is hand-written too: a full disk could not be
// provoked safely. The fixtures end in .log because the repository ignores .txt.
func TestPullErrors(t *testing.T) {
	for _, tc := range []struct {
		fixture string
		want    error
	}{
		{"podman_pull_manifest_unknown.log", ErrNotFound},
		{"podman_pull_no_platform.log", ErrNotFound},
		{"podman_pull_no_such_host.log", ErrUnreachable},
		{"podman_pull_forbidden.log", ErrDenied},
		{"podman_pull_denied.log", ErrDenied},
		{"podman_pull_no_space.log", ErrNoSpace},
		{"docker_pull_manifest_unknown.log", ErrNotFound},
		{"docker_pull_no_platform.log", ErrNotFound},
		{"docker_pull_no_such_host.log", ErrUnreachable},
		{"docker_pull_unauthorized.log", ErrDenied},
		{"docker_pull_no_space.log", ErrNoSpace},
	} {
		t.Run(tc.fixture, func(t *testing.T) {
			dialect := Podman
			if strings.HasPrefix(tc.fixture, "docker") {
				dialect = Docker
			}
			st := &streamer{code: 125, err: fixture(t, tc.fixture)}
			err := NewCLI("rt", dialect, st).Pull(context.Background(), "ghcr.io/o/x:1", io.Discard)
			if !errors.Is(err, tc.want) {
				t.Fatalf("err = %v, want %v", err, tc.want)
			}
			for _, other := range []error{ErrNotFound, ErrDenied, ErrNoSpace, ErrUnreachable} {
				if other != tc.want && errors.Is(err, other) {
					t.Errorf("err also matches %v", other)
				}
			}
			var ce *CommandError
			if !errors.As(err, &ce) || ce.ExitCode != 125 || ce.Stderr == "" {
				t.Errorf("no usable CommandError in %v", err)
			}
		})
	}
}

func TestPullOtherErrors(t *testing.T) {
	ctx := context.Background()
	t.Run("unrecognised", func(t *testing.T) {
		st := &streamer{code: 1, err: "something odd\n"}
		err := NewCLI("rt", Podman, st).Pull(ctx, "ghcr.io/o/x:1", nil)
		var ce *CommandError
		if !errors.As(err, &ce) || ce.Stderr != "something odd" {
			t.Fatalf("err = %v", err)
		}
		for _, s := range []error{ErrNotFound, ErrDenied, ErrNoSpace, ErrUnreachable} {
			if errors.Is(err, s) {
				t.Errorf("err matches %v", s)
			}
		}
	})
	t.Run("run error", func(t *testing.T) {
		want := errors.New("cancelled")
		err := NewCLI("rt", Podman, &streamer{runErr: want}).Pull(ctx, "ghcr.io/o/x:1", nil)
		if !errors.Is(err, want) {
			t.Fatalf("err = %v", err)
		}
	})
	t.Run("not a streamer", func(t *testing.T) {
		if err := NewCLI("rt", Podman, &scripted{}).Pull(ctx, "ghcr.io/o/x:1", nil); err == nil {
			t.Fatal("want an error")
		}
	})
}
