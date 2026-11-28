package main

import (
	"bytes"
	"context"
	"errors"
	"io"
	"log/slog"
	"net"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/component"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/components"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/config"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/health"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/instance"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/logging"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/logs"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime/fake"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/session"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/status"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/supervisor"
)

func tcp(s string) net.Addr {
	a, err := net.ResolveTCPAddr("tcp", s)
	if err != nil {
		panic(err)
	}
	return a
}

func TestDashboardURL(t *testing.T) {
	for in, want := range map[string]string{
		"127.0.0.1:8770": "http://127.0.0.1:8770",
		"[::1]:9000":     "http://[::1]:9000",
		"0.0.0.0:8770":   "http://127.0.0.1:8770",
		"[::]:8770":      "http://127.0.0.1:8770",
		"10.1.2.3:80":    "http://10.1.2.3:80",
	} {
		if got := dashboardURL(tcp(in)); got != want {
			t.Errorf("dashboardURL(%s) = %s, want %s", in, got, want)
		}
	}
}

func TestAllowedHosts(t *testing.T) {
	host := func() (string, error) { return "Sim-Box", nil }
	ifaces := func() ([]net.Addr, error) {
		return []net.Addr{&net.IPNet{IP: net.ParseIP("192.168.1.5"), Mask: net.CIDRMask(24, 32)}, &net.IPAddr{}}, nil
	}
	tests := []struct {
		name string
		addr string
		want []string
	}{
		{"loopback", "127.0.0.1:8770", []string{"127.0.0.1:8770", "localhost:8770"}},
		{"specific address", "10.0.0.2:8770", []string{"10.0.0.2:8770", "localhost:8770", "127.0.0.1:8770"}},
		{"wildcard", "0.0.0.0:8770", []string{"0.0.0.0:8770", "localhost:8770", "127.0.0.1:8770", "sim-box:8770", "192.168.1.5:8770"}},
	}
	for _, tt := range tests {
		got := allowedHosts(tcp(tt.addr), host, ifaces)
		if !reflect.DeepEqual(got, tt.want) {
			t.Errorf("%s: got %q, want %q", tt.name, got, tt.want)
		}
	}
	// Lookup failures just mean fewer names.
	got := allowedHosts(tcp("0.0.0.0:1"), func() (string, error) { return "", errors.New("x") },
		func() ([]net.Addr, error) { return nil, errors.New("x") })
	if len(got) != 3 {
		t.Errorf("got %q", got)
	}
}

func TestRunExitCodes(t *testing.T) {
	var out, errb strings.Builder
	env := func(string) string { return "" }
	if code := run([]string{"--version"}, env, &out, &errb); code != 0 || out.String() != "astrosim dev\n" {
		t.Errorf("--version: %d %q", code, out.String())
	}
	if code := run([]string{"--help"}, env, &out, &errb); code != 0 {
		t.Errorf("--help: %d", code)
	}
	for _, args := range [][]string{{"--bogus"}, {"--restart-max=0"}, {"--listen=0.0.0.0:1"}, {"--network=overlay"}} {
		errb.Reset()
		if code := run(args, env, &out, &errb); code != 2 || !strings.Contains(errb.String(), "astrosim") {
			t.Errorf("%v: code %d, stderr %q", args, code, errb.String())
		}
	}
}

func TestCleanupCommand(t *testing.T) {
	ctx := context.Background()
	managed := map[string]string{"astrosim.managed": "true"}
	setup := func() *fake.Runtime {
		rt := fake.New()
		rt.SetImage("img", true)
		_, _ = rt.Run(ctx, runtime.RunSpec{Name: "astrosim-engine", Image: "img", Labels: managed})
		_, _ = rt.Run(ctx, runtime.RunSpec{Name: "by-hand", Image: "img"})
		_ = rt.EnsureNetwork(ctx, "astrosim", managed)
		return rt
	}
	tests := []struct {
		name     string
		prune    bool
		pingErr  error
		wantCode int
		wantOut  string
		wantNets int
	}{
		{"cleanup keeps the network", false, nil, exitOK, "removed container astrosim-engine\n", 1},
		{"prune removes the network", true, nil, exitOK, "removed container astrosim-engine\nremoved network astrosim\n", 0},
		{"no runtime", false, runtime.ErrNotRunning, exitFailure, "", 1},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			rt := setup()
			rt.PingErr = tt.pingErr
			var out, errb bytes.Buffer
			code := cleanup(rt, tt.prune, &out, logging.New(&errb, slog.LevelInfo))
			if code != tt.wantCode || out.String() != tt.wantOut {
				t.Errorf("code=%d out=%q, want %d %q (log %q)", code, out.String(), tt.wantCode, tt.wantOut, errb.String())
			}
			if n := len(rt.Networks()); n != tt.wantNets {
				t.Errorf("networks = %d, want %d", n, tt.wantNets)
			}
			if _, err := rt.Inspect(ctx, "by-hand"); err != nil {
				t.Error("unlabelled container was touched")
			}
		})
	}
	t.Run("nothing to clean up", func(t *testing.T) {
		var out bytes.Buffer
		if code := cleanup(fake.New(), true, &out, logging.New(&bytes.Buffer{}, slog.LevelInfo)); code != exitOK || out.String() != "nothing to clean up\n" {
			t.Errorf("code=%d out=%q", code, out.String())
		}
	})
}

func TestRefuseCleanup(t *testing.T) {
	path := filepath.Join(t.TempDir(), "lock")
	lock, err := instance.Acquire(path)
	if err != nil {
		t.Fatal(err)
	}
	defer lock.Release()
	if err := lock.Publish("http://127.0.0.1:8770"); err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	code := refuseCleanup(path, &out, logging.New(&bytes.Buffer{}, slog.LevelInfo))
	if code != exitFailure || out.String() != "AstroSim is running at http://127.0.0.1:8770; stop it before cleaning up\n" {
		t.Errorf("code=%d out=%q", code, out.String())
	}
}

func TestNewSessionID(t *testing.T) {
	if id := newSessionID(); !logs.IsSessionName("20260102T030405Z-" + id) {
		t.Errorf("session ID %q does not make a valid log folder name", id)
	}
}

func TestLogRoot(t *testing.T) {
	env := func(m map[string]string) func(string) string { return func(k string) string { return m[k] } }
	home := t.TempDir()
	t.Setenv("HOME", home)
	t.Setenv("USERPROFILE", home)
	abs, _ := filepath.Abs("rel/logs")
	tests := []struct {
		name string
		goos string
		dir  string
		env  map[string]string
		want string
	}{
		{"linux default", "linux", "", nil, filepath.Join(home, ".local/state/astrosim/logs")},
		{"linux xdg", "linux", "", map[string]string{"XDG_STATE_HOME": "/state"}, "/state/astrosim/logs"},
		{"darwin default", "darwin", "", nil, home + "/Library/Logs/AstroSim"},
		{"windows default", "windows", "", map[string]string{"LOCALAPPDATA": `C:\Users\u\AppData\Local`}, `C:\Users\u\AppData\Local\AstroSim\logs`},
		{"override", "linux", "/custom", nil, "/custom"},
		{"relative override is made absolute", "darwin", "rel/logs", nil, abs},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			cfg := config.Defaults()
			cfg.LogDir = tt.dir
			got, err := logRoot(cfg, tt.goos, env(tt.env))
			if err != nil || got != tt.want {
				t.Errorf("logRoot = %q, %v; want %q", got, err, tt.want)
			}
		})
	}
}

func TestPrintLogDir(t *testing.T) {
	state := t.TempDir()
	cfgFile := filepath.Join(t.TempDir(), "c.json")
	if err := os.WriteFile(cfgFile, []byte(`{"logDir":"/from/json"}`), 0o600); err != nil {
		t.Fatal(err)
	}
	tests := []struct {
		name string
		args []string
		env  map[string]string
		want string
	}{
		{"default", nil, map[string]string{"XDG_STATE_HOME": state}, filepath.Join(state, "astrosim", "logs")},
		{"json", []string{"--config", cfgFile}, nil, "/from/json"},
		{"env beats json", []string{"--config", cfgFile}, map[string]string{"ASTROSIM_ORCH_LOG_DIR": "/from/env"}, "/from/env"},
		{"flag beats env", []string{"--log-dir", "/from/flag"}, map[string]string{"ASTROSIM_ORCH_LOG_DIR": "/from/env"}, "/from/flag"},
		{"flag with equals", []string{"--log-dir=/from/flag"}, nil, "/from/flag"},
		// Nothing else is checked or run: an invalid setting does not matter.
		{"wins over validation", []string{"--restart-max=0", "--autostart", "--cleanup"}, map[string]string{"XDG_STATE_HOME": state},
			filepath.Join(state, "astrosim", "logs")},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			var out, errb bytes.Buffer
			env := func(k string) string { return tt.env[k] }
			code := run(append([]string{"--print-log-dir"}, tt.args...), env, &out, &errb)
			if code != exitOK || out.String() != tt.want+"\n" || errb.Len() != 0 {
				t.Errorf("code=%d out=%q err=%q, want %q", code, out.String(), errb.String(), tt.want)
			}
		})
	}
	if _, err := os.Stat(filepath.Join(state, "astrosim")); err == nil {
		t.Error("--print-log-dir created the directory")
	}
}

// newRecorder opens a recorder under root and writes a line to it.
func newRecorder(t *testing.T, root, id string) *logs.Recorder {
	t.Helper()
	rec, err := logs.Open(logs.Options{Root: root, SessionID: id, Version: "test", Source: fake.New()})
	if err != nil {
		t.Fatal(err)
	}
	rec.Note("fc", "hello")
	return rec
}

func zipsIn(t *testing.T, root string) (zips, folders []string) {
	t.Helper()
	es, err := os.ReadDir(root)
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range es {
		if e.IsDir() {
			folders = append(folders, e.Name())
		} else if strings.HasSuffix(e.Name(), ".zip") {
			zips = append(zips, e.Name())
		}
	}
	return
}

func TestFinishLogsArchivesTheSession(t *testing.T) {
	root := t.TempDir()
	// An old archive beyond the keep limit goes after the new one is written.
	old := filepath.Join(root, "20200101T000000Z-0000000000000001.zip")
	if err := os.WriteFile(old, []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	rec := newRecorder(t, root, "0123456789abcdef")
	recovered := make(chan struct{})
	close(recovered)
	cfg := config.Defaults()
	cfg.LogKeep = 1
	var errb bytes.Buffer
	finishLogs(rec, recovered, cfg, &errb, logging.New(&errb, slog.LevelInfo))

	zips, folders := zipsIn(t, root)
	if len(zips) != 1 || len(folders) != 0 || zips[0] != rec.Name()+".zip" {
		t.Fatalf("zips %v folders %v", zips, folders)
	}
	if !strings.Contains(errb.String(), "logs saved to "+filepath.Join(root, rec.Name()+".zip")) {
		t.Errorf("stderr = %q", errb.String())
	}
}

func TestRecoverLogsSkipsTheLiveSession(t *testing.T) {
	root := t.TempDir()
	stray := newRecorder(t, root, "aaaaaaaaaaaaaaaa")
	if err := stray.Close(context.Background()); err != nil { // as if killed after its files were flushed
		t.Fatal(err)
	}
	live := newRecorder(t, root, "bbbbbbbbbbbbbbbb")
	defer live.Close(context.Background())
	var errb bytes.Buffer
	recoverLogs(root, live.Name(), config.Defaults(), logging.New(&errb, slog.LevelInfo))
	zips, folders := zipsIn(t, root)
	if len(zips) != 1 || zips[0] != stray.Name()+".zip" || len(folders) != 1 || folders[0] != live.Name() {
		t.Fatalf("zips %v folders %v", zips, folders)
	}
}

func TestArchiveStrayLogsForPrune(t *testing.T) {
	root := t.TempDir()
	a := newRecorder(t, root, "aaaaaaaaaaaaaaaa")
	if err := a.Close(context.Background()); err != nil {
		t.Fatal(err)
	}
	var out, errb bytes.Buffer
	if !archiveStrayLogs(root, &out, logging.New(&errb, slog.LevelInfo)) {
		t.Fatalf("failed: %s", errb.String())
	}
	zips, folders := zipsIn(t, root)
	if len(zips) != 1 || len(folders) != 0 || !strings.HasPrefix(out.String(), "archived logs ") {
		t.Errorf("zips %v folders %v out %q", zips, folders, out.String())
	}
	// A second prune finds nothing and deletes nothing.
	out.Reset()
	archiveStrayLogs(root, &out, logging.New(&errb, slog.LevelInfo))
	if zips2, _ := zipsIn(t, root); len(zips2) != 1 || out.Len() != 0 {
		t.Errorf("second prune: zips %v out %q", zips2, out.String())
	}
}

func TestSessionLogsAdapter(t *testing.T) {
	if logCapture(nil) != nil {
		t.Error("no recorder must mean no capture")
	}
	rt := fake.New()
	rt.SetImage("img", true)
	root := t.TempDir()
	rec, err := logs.Open(logs.Options{Root: root, SessionID: "0123456789abcdef", Version: "t", Source: rt})
	if err != nil {
		t.Fatal(err)
	}
	lc := logCapture(rec)
	id, _ := rt.Run(context.Background(), runtime.RunSpec{Name: "astrosim-fc", Image: "img"})
	c := lc.Begin("fc", 2, "astrosim-fc", id, "img")
	lc.Note("fc", "exited code=1")
	lc.Line("fc", "Copying blob x")
	rt.Exit("astrosim-fc", 1)
	c.Drain(context.Background())
	if err := rec.Close(context.Background()); err != nil {
		t.Fatal(err)
	}
	b, err := os.ReadFile(filepath.Join(rec.Dir(), "fc.log"))
	if err != nil || !strings.Contains(string(b), "==== launch gen=2 container=astrosim-fc") ||
		!strings.Contains(string(b), "==== exited code=1 ====") || !strings.Contains(string(b), "| Copying blob x\n") {
		t.Errorf("fc.log = %q, %v", b, err)
	}
}

// setImages sets the build-time image variables for one test.
func setImages(t *testing.T, repository, tag string) {
	t.Helper()
	oldRepo, oldTag := imageRepository, imageTag
	imageRepository, imageTag = repository, tag
	t.Cleanup(func() { imageRepository, imageTag = oldRepo, oldTag })
}

func TestVersionDevHasOneLine(t *testing.T) {
	setImages(t, "", "")
	var out, errb strings.Builder
	if code := run([]string{"--version"}, func(string) string { return "" }, &out, &errb); code != 0 || out.String() != "astrosim dev\n" {
		t.Errorf("--version: %d %q", code, out.String())
	}
}

func TestVersionReleaseListsImages(t *testing.T) {
	setImages(t, "ghcr.io/flpoly-jetson-one-aerial-policing", "0.1.0")
	var out, errb strings.Builder
	code := run([]string{"--version"}, func(string) string { return "" }, &out, &errb)
	want := "astrosim dev\nimages ghcr.io/flpoly-jetson-one-aerial-policing/astrosim-engine:0.1.0 " +
		"ghcr.io/flpoly-jetson-one-aerial-policing/astrosim-fc:0.1.0\n"
	if code != 0 || out.String() != want {
		t.Errorf("--version: %d %q, want %q", code, out.String(), want)
	}
}

func TestBrokenBuildImagesExitFailure(t *testing.T) {
	setImages(t, "ghcr.io/o", "")
	for _, args := range [][]string{{"--version"}, {}} {
		var out, errb strings.Builder
		code := run(args, func(string) string { return "" }, &out, &errb)
		if code != exitFailure || !strings.Contains(errb.String(), "this build's image settings are invalid") {
			t.Errorf("%v: code %d, stderr %q", args, code, errb.String())
		}
	}
}

func TestComponentDefs(t *testing.T) {
	release := components.Images{Repository: "ghcr.io/o", Tag: "1"}
	imageOf := func(defs []component.Definition, id string) string {
		for _, d := range defs {
			if d.ID == id {
				return d.Image
			}
		}
		return ""
	}

	t.Run("dev", func(t *testing.T) {
		defs, err := componentDefs(config.Defaults(), components.Images{})
		if err != nil || imageOf(defs, "engine") != "astrosim-engine" {
			t.Errorf("defs = %v, err = %v", defs, err)
		}
	})
	t.Run("release", func(t *testing.T) {
		defs, err := componentDefs(config.Defaults(), release)
		if err != nil || imageOf(defs, "engine") != "ghcr.io/o/astrosim-engine:1" || imageOf(defs, "fc") != "ghcr.io/o/astrosim-fc:1" {
			t.Errorf("defs = %v, err = %v", defs, err)
		}
	})
	t.Run("json image override wins", func(t *testing.T) {
		cfg := config.Defaults()
		cfg.Components = map[string]config.ComponentOverride{"engine": {Image: "localhost/astrosim-engine:dev"}}
		defs, err := componentDefs(cfg, release)
		if err != nil {
			t.Fatal(err)
		}
		got := cfg.ApplyOverrides(defs)
		if imageOf(got, "engine") != "localhost/astrosim-engine:dev" || imageOf(got, "fc") != "ghcr.io/o/astrosim-fc:1" {
			t.Errorf("images = %q, %q", imageOf(got, "engine"), imageOf(got, "fc"))
		}
	})
	t.Run("broken build", func(t *testing.T) {
		_, err := componentDefs(config.Defaults(), components.Images{Tag: "1"})
		var be *buildError
		if !errors.As(err, &be) {
			t.Errorf("err = %v, want a *buildError", err)
		}
	})
	t.Run("invalid configuration", func(t *testing.T) {
		cfg := config.Defaults()
		cfg.Components = map[string]config.ComponentOverride{"nope": {}}
		_, err := componentDefs(cfg, release)
		var be *buildError
		if err == nil || errors.As(err, &be) {
			t.Errorf("err = %v, want a configuration error", err)
		}
	})
}

// autostartSession runs a session whose two images are downloadable and
// whose download blocks until cancelled.
func autostartSession(t *testing.T, hook func(ctx context.Context, image string, progress io.Writer) error) (*session.Session, *status.Broadcaster) {
	t.Helper()
	mk := func(id string) component.Definition {
		return component.Definition{
			ID: id, DisplayName: id, Image: "ghcr.io/t/" + id + ":1", RestartGroup: "sim",
			Probe:        func(component.StackContext) health.Probe { return nil },
			ReadyTimeout: time.Minute, LivenessTimeout: time.Second, StopTimeout: time.Second,
		}
	}
	rt := fake.New()
	rt.PullHook = hook
	bc := status.NewBroadcaster()
	sess, err := session.New(session.Options{
		Runtime: rt, Definitions: []component.Definition{mk("a")}, StackContext: component.DefaultStackContext(),
		Policy: supervisor.DefaultRestartPolicy, Broadcaster: bc, PullMissing: true,
	})
	if err != nil {
		t.Fatal(err)
	}
	done := make(chan struct{})
	go func() { _ = sess.Run(context.Background()); close(done) }()
	t.Cleanup(func() { sess.Shutdown(); <-done })
	return sess, bc
}

func waitSnapshot(t *testing.T, bc *status.Broadcaster, cond func(status.Snapshot) bool) status.Snapshot {
	t.Helper()
	for deadline := time.Now().Add(5 * time.Second); time.Now().Before(deadline); time.Sleep(2 * time.Millisecond) {
		if s, ok := bc.Latest(); ok && cond(s) {
			return s
		}
	}
	s, _ := bc.Latest()
	t.Fatalf("timed out; last snapshot phase %q reason %+v", s.Phase, s.Reason)
	return s
}

func TestAutostartWaitsForTheDownload(t *testing.T) {
	release := make(chan struct{})
	sess, bc := autostartSession(t, func(ctx context.Context, _ string, _ io.Writer) error {
		select {
		case <-release:
			return nil
		case <-ctx.Done():
			return ctx.Err()
		}
	})
	waitSnapshot(t, bc, func(s status.Snapshot) bool { return s.Phase == "downloading" })
	done := make(chan struct{})
	go func() {
		autostart(sess, bc, context.Background(), logging.New(&bytes.Buffer{}, slog.LevelInfo))
		close(done)
	}()
	time.Sleep(50 * time.Millisecond)
	select {
	case <-done:
		t.Fatal("autostart gave up while downloading")
	default:
	}
	close(release)
	<-done
	waitSnapshot(t, bc, func(s status.Snapshot) bool { return s.Phase == "starting" || s.Phase == "running" })
}

func TestAutostartDoesNotRestartACancelledDownload(t *testing.T) {
	sess, bc := autostartSession(t, func(ctx context.Context, _ string, _ io.Writer) error {
		<-ctx.Done()
		return ctx.Err()
	})
	waitSnapshot(t, bc, func(s status.Snapshot) bool { return s.Phase == "downloading" })
	if err := sess.Stop(); err != nil {
		t.Fatal(err)
	}
	waitSnapshot(t, bc, func(s status.Snapshot) bool { return s.Phase == "stopped" })
	var out bytes.Buffer
	done := make(chan struct{})
	go func() {
		autostart(sess, bc, context.Background(), logging.New(&out, slog.LevelInfo))
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("autostart did not return")
	}
	time.Sleep(50 * time.Millisecond)
	if s, _ := bc.Latest(); s.Phase != "stopped" || s.Reason.Code != "pull_cancelled" {
		t.Errorf("autostart changed the state: %+v", s)
	}
	if !strings.Contains(out.String(), "cancelled with the download") {
		t.Errorf("log = %q", out.String())
	}
}
