package config

import (
	"errors"
	"flag"
	"io"
	"strings"
	"testing"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/component"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/health"
)

func envMap(m map[string]string) func(string) string { return func(k string) string { return m[k] } }

func fileMap(m map[string]string) ReadFileFunc {
	return func(name string) ([]byte, error) {
		if s, ok := m[name]; ok {
			return []byte(s), nil
		}
		return nil, errors.New("no such file")
	}
}

func load(t *testing.T, args []string, env map[string]string, files map[string]string) (Config, error) {
	t.Helper()
	return LoadTo(io.Discard, args, envMap(env), fileMap(files))
}

func TestDefaults(t *testing.T) {
	c, err := load(t, nil, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	want := Defaults()
	if c.Listen != "127.0.0.1:8770" || c.Runtime != "auto" || c.Network != "bridge" || c.PublishAddr != "127.0.0.1" ||
		c.RestartMax != 3 || c.LogLevel != "info" || c.NoBrowser || c.NoFail || c.Autostart || c.AllowRemote {
		t.Fatalf("defaults = %+v", c)
	}
	if c.Listen != want.Listen {
		t.Fatal("Load without inputs differs from Defaults")
	}
}

func TestPrecedence(t *testing.T) {
	files := map[string]string{"c.json": `{"listen":"127.0.0.1:1111","restartMax":5,"logLevel":"debug","noBrowser":true}`}
	tests := []struct {
		name       string
		args       []string
		env        map[string]string
		listen     string
		restartMax int
		level      string
		noBrowser  bool
	}{
		{"defaults", nil, nil, "127.0.0.1:8770", 3, "info", false},
		{"file beats defaults", []string{"--config", "c.json"}, nil, "127.0.0.1:1111", 5, "debug", true},
		{"env beats file", []string{"--config", "c.json"}, map[string]string{
			"ASTROSIM_ORCH_LISTEN": "127.0.0.1:2222", "ASTROSIM_ORCH_NO_BROWSER": "false"},
			"127.0.0.1:2222", 5, "debug", false},
		{"flag beats env and file", []string{"--config=c.json", "--listen=127.0.0.1:3333", "--restart-max", "7"},
			map[string]string{"ASTROSIM_ORCH_LISTEN": "127.0.0.1:2222", "ASTROSIM_ORCH_RESTART_MAX": "6"},
			"127.0.0.1:3333", 7, "debug", true},
		{"explicit false flag beats file true", []string{"--config", "c.json", "--no-browser=false"}, nil,
			"127.0.0.1:1111", 5, "debug", false},
		{"config path from env", nil, map[string]string{"ASTROSIM_ORCH_CONFIG": "c.json"}, "127.0.0.1:1111", 5, "debug", true},
		{"env alone", nil, map[string]string{"ASTROSIM_ORCH_RESTART_MAX": "9", "ASTROSIM_ORCH_LOG_LEVEL": "warn"},
			"127.0.0.1:8770", 9, "warn", false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			c, err := load(t, tt.args, tt.env, files)
			if err != nil {
				t.Fatal(err)
			}
			if c.Listen != tt.listen || c.RestartMax != tt.restartMax || c.LogLevel != tt.level || c.NoBrowser != tt.noBrowser {
				t.Fatalf("got %+v", c)
			}
		})
	}
}

func TestLogSettings(t *testing.T) {
	files := map[string]string{"c.json": `{"logDir":"/file","logKeep":5,"logMaxMB":50,"logFileMaxMB":8,"verbose":true}`}
	tests := []struct {
		name    string
		args    []string
		env     map[string]string
		dir     string
		keep    int
		maxMB   int
		fileMB  int
		verbose bool
	}{
		{"defaults", nil, nil, "", 20, 500, 64, false},
		{"file", []string{"--config", "c.json"}, nil, "/file", 5, 50, 8, true},
		{"env beats file", []string{"--config", "c.json"}, map[string]string{
			"ASTROSIM_ORCH_LOG_DIR": "/env", "ASTROSIM_ORCH_LOG_KEEP": "6", "ASTROSIM_ORCH_LOG_MAX_MB": "60",
			"ASTROSIM_ORCH_LOG_FILE_MAX_MB": "9", "ASTROSIM_ORCH_VERBOSE": "false"}, "/env", 6, 60, 9, false},
		{"flag beats env", []string{"--config", "c.json", "--log-dir", "/flag", "--log-keep=7", "--log-max-mb=70",
			"--log-file-max-mb=10", "--verbose=false"}, map[string]string{
			"ASTROSIM_ORCH_LOG_DIR": "/env", "ASTROSIM_ORCH_LOG_KEEP": "6", "ASTROSIM_ORCH_VERBOSE": "true"},
			"/flag", 7, 70, 10, false},
		{"verbose flag", []string{"--verbose"}, nil, "", 20, 500, 64, true},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			c, err := load(t, tt.args, tt.env, files)
			if err != nil {
				t.Fatal(err)
			}
			if c.LogDir != tt.dir || c.LogKeep != tt.keep || c.LogMaxMB != tt.maxMB ||
				c.LogFileMaxMB != tt.fileMB || c.Verbose != tt.verbose {
				t.Fatalf("got %+v", c)
			}
		})
	}
}

func TestPrintLogDirIsFlagOnly(t *testing.T) {
	c, err := load(t, []string{"--print-log-dir"}, nil, nil)
	if err != nil || !c.PrintLogDir {
		t.Fatalf("flag: %+v, %v", c, err)
	}
	c, err = load(t, nil, map[string]string{"ASTROSIM_ORCH_PRINT_LOG_DIR": "1"}, nil)
	if err != nil || c.PrintLogDir {
		t.Fatalf("env must not set it: %+v, %v", c, err)
	}
	if _, err = load(t, []string{"--config", "c.json"}, nil, map[string]string{"c.json": `{"printLogDir":true}`}); err == nil {
		t.Fatal("JSON key printLogDir must be rejected")
	}
	// --log-dir keeps taking a value alongside it.
	c, err = load(t, []string{"--print-log-dir", "--log-dir", "/x"}, nil, nil)
	if err != nil || !c.PrintLogDir || c.LogDir != "/x" {
		t.Fatalf("combined: %+v, %v", c, err)
	}
}

func TestAllFlagsAndEnv(t *testing.T) {
	c, err := load(t, []string{"--allow-remote", "--runtime", "docker", "--runtime-path", "/x/docker", "--network", "host",
		"--publish-addr", "0.0.0.0", "--autostart", "--no-fail", "--version"}, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	if !c.AllowRemote || c.Runtime != "docker" || c.RuntimePath != "/x/docker" || c.Network != "host" ||
		c.PublishAddr != "0.0.0.0" || !c.Autostart || !c.NoFail || !c.ShowVersion {
		t.Fatalf("flags: %+v", c)
	}
	c, err = load(t, nil, map[string]string{
		"ASTROSIM_ORCH_ALLOW_REMOTE": "1", "ASTROSIM_ORCH_RUNTIME": "podman", "ASTROSIM_ORCH_RUNTIME_PATH": "/p",
		"ASTROSIM_ORCH_NETWORK": "host", "ASTROSIM_ORCH_PUBLISH_ADDR": "10.0.0.1", "ASTROSIM_ORCH_AUTOSTART": "true",
		"ASTROSIM_ORCH_NO_FAIL": "true"}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if !c.AllowRemote || c.Runtime != "podman" || c.RuntimePath != "/p" || c.Network != "host" ||
		c.PublishAddr != "10.0.0.1" || !c.Autostart || !c.NoFail {
		t.Fatalf("env: %+v", c)
	}
}

func TestLoadErrors(t *testing.T) {
	files := map[string]string{
		"bad.json":     `{"listen":`,
		"unknown.json": `{"lisen":"x"}`,
		"dur.json":     `{"components":{"engine":{"readyTimeout":"soon"}}}`,
		"num.json":     `{"components":{"engine":{"readyTimeout":30}}}`,
		"trail.json":   `{} {}`,
	}
	tests := []struct {
		name string
		args []string
		env  map[string]string
		want string
	}{
		{"unknown flag", []string{"--bogus"}, nil, "bogus"},
		{"stray argument", []string{"extra"}, nil, "unexpected argument"},
		{"missing file", []string{"--config", "nope.json"}, nil, "read config file"},
		{"bad json", []string{"--config", "bad.json"}, nil, "bad.json"},
		{"unknown json key", []string{"--config", "unknown.json"}, nil, "lisen"},
		{"bad duration", []string{"--config", "dur.json"}, nil, "duration"},
		{"duration must be string", []string{"--config", "num.json"}, nil, "duration string"},
		{"trailing data", []string{"--config", "trail.json"}, nil, "unexpected data"},
		{"bad env bool", nil, map[string]string{"ASTROSIM_ORCH_NO_FAIL": "maybe"}, "not a boolean"},
		{"bad env int", nil, map[string]string{"ASTROSIM_ORCH_RESTART_MAX": "x"}, "not a number"},
		{"bad env log keep", nil, map[string]string{"ASTROSIM_ORCH_LOG_KEEP": "many"}, "ASTROSIM_ORCH_LOG_KEEP"},
		{"bad env log max", nil, map[string]string{"ASTROSIM_ORCH_LOG_MAX_MB": "1.5"}, "not a number"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			_, err := load(t, tt.args, tt.env, files)
			if err == nil || !strings.Contains(err.Error(), tt.want) {
				t.Fatalf("err = %v, want containing %q", err, tt.want)
			}
		})
	}
}

func TestHelp(t *testing.T) {
	var out strings.Builder
	_, err := LoadTo(&out, []string{"--help"}, nil, nil)
	if !errors.Is(err, flag.ErrHelp) {
		t.Fatalf("err = %v", err)
	}
	for _, f := range []string{"--listen", "-allow-remote", "-no-fail", "-config", "ASTROSIM_ORCH_"} {
		if !strings.Contains(out.String(), strings.TrimPrefix(f, "--")) {
			t.Errorf("usage lacks %s:\n%s", f, out.String())
		}
	}
}

// defs is a small stack: b depends on a.
func defs() []component.Definition {
	probe := func(component.StackContext) health.Probe { return nil }
	return []component.Definition{
		{ID: "a", Image: "img-a", Probe: probe, ReadyTimeout: time.Second, LivenessTimeout: time.Second, StopTimeout: time.Second,
			Env: func(component.StackContext) map[string]string { return map[string]string{"K": "base", "A": "1"} }},
		{ID: "b", Image: "img-b", Probe: probe, ReadyTimeout: time.Second, LivenessTimeout: time.Second, StopTimeout: time.Second,
			DependsOn: []component.Dependency{{ID: "a", Condition: component.Started}}},
	}
}

func TestValidate(t *testing.T) {
	tests := []struct {
		name string
		mod  func(*Config)
		goos string
		want string // substring of the error; empty means valid
	}{
		{"defaults", func(*Config) {}, "linux", ""},
		{"host on linux", func(c *Config) { c.Network = "host" }, "linux", ""},
		{"host on windows", func(c *Config) { c.Network = "host" }, "windows", "only supported on Linux"},
		{"host on darwin", func(c *Config) { c.Network = "host" }, "darwin", "only supported on Linux"},
		{"unknown network", func(c *Config) { c.Network = "overlay" }, "linux", "bridge or host"},
		{"unknown runtime", func(c *Config) { c.Runtime = "lxc" }, "linux", "--runtime"},
		{"wildcard listen", func(c *Config) { c.Listen = "0.0.0.0:8770" }, "linux", "--allow-remote"},
		{"empty host listen", func(c *Config) { c.Listen = ":8770" }, "linux", "--allow-remote"},
		{"lan listen", func(c *Config) { c.Listen = "192.168.1.5:8770" }, "linux", "--allow-remote"},
		{"remote allowed", func(c *Config) { c.Listen = "0.0.0.0:8770"; c.AllowRemote = true }, "linux", ""},
		{"localhost listen", func(c *Config) { c.Listen = "localhost:9000" }, "linux", ""},
		{"ipv6 loopback", func(c *Config) { c.Listen = "[::1]:9000" }, "linux", ""},
		{"listen without port", func(c *Config) { c.Listen = "127.0.0.1" }, "linux", "--listen"},
		{"listen bad port", func(c *Config) { c.Listen = "127.0.0.1:99999" }, "linux", "invalid port"},
		{"restart-max zero", func(c *Config) { c.RestartMax = 0 }, "linux", "--restart-max"},
		{"log keep zero", func(c *Config) { c.LogKeep = 0 }, "linux", "--log-keep"},
		{"log max zero", func(c *Config) { c.LogMaxMB = 0 }, "linux", "--log-max-mb"},
		{"log file max small", func(c *Config) { c.LogFileMaxMB = 3 }, "linux", "--log-file-max-mb"},
		{"log file max minimum", func(c *Config) { c.LogFileMaxMB = 4 }, "linux", ""},
		{"bad log level", func(c *Config) { c.LogLevel = "loud" }, "linux", "--log-level"},
		{"bad publish addr", func(c *Config) { c.PublishAddr = "localhost" }, "linux", "--publish-addr"},
		{"unknown component", func(c *Config) { c.Components = map[string]ComponentOverride{"zzz": {}} }, "linux", "components.zzz: unknown component (known: a, b)"},
		{"disable leaf", func(c *Config) { c.Components = map[string]ComponentOverride{"b": {Disabled: true}} }, "linux", ""},
		{"disable required", func(c *Config) { c.Components = map[string]ComponentOverride{"a": {Disabled: true}} }, "linux", "b depends on it"},
		{"disable both", func(c *Config) {
			c.Components = map[string]ComponentOverride{"a": {Disabled: true}, "b": {Disabled: true}}
		}, "linux", ""},
		{"negative timeout", func(c *Config) {
			c.Components = map[string]ComponentOverride{"a": {StopTimeout: Duration(-time.Second)}}
		}, "linux", "components.a.stopTimeout"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			c := Defaults()
			tt.mod(&c)
			err := c.Validate(tt.goos, defs())
			switch {
			case tt.want == "" && err != nil:
				t.Fatalf("unexpected error: %v", err)
			case tt.want != "" && (err == nil || !strings.Contains(err.Error(), tt.want)):
				t.Fatalf("err = %v, want containing %q", err, tt.want)
			}
		})
	}
	// Several problems are all reported.
	c := Defaults()
	c.RestartMax, c.LogLevel = 0, "loud"
	err := c.Validate("linux", defs())
	if err == nil || !strings.Contains(err.Error(), "--restart-max") || !strings.Contains(err.Error(), "--log-level") {
		t.Fatalf("joined err = %v", err)
	}
}

func TestApplyOverrides(t *testing.T) {
	orig := defs()
	cfg, err := load(t, []string{"--config", "c.json"}, nil, map[string]string{"c.json": `{"components":{
		"a":{"image":"custom-a:2","env":{"K":"over","NEW":"x"},"readyTimeout":"45s","livenessTimeout":"3s","stopTimeout":"1m"},
		"b":{"disabled":true}}}`})
	if err != nil {
		t.Fatal(err)
	}
	if err := cfg.Validate("linux", orig); err != nil {
		t.Fatal(err)
	}
	out := cfg.ApplyOverrides(orig)
	if len(out) != 1 || out[0].ID != "a" {
		t.Fatalf("out = %+v", out)
	}
	a := out[0]
	if a.Image != "custom-a:2" || a.ReadyTimeout != 45*time.Second || a.LivenessTimeout != 3*time.Second || a.StopTimeout != time.Minute {
		t.Fatalf("a = %+v", a)
	}
	env := a.Env(component.DefaultStackContext())
	if env["K"] != "over" || env["A"] != "1" || env["NEW"] != "x" || len(env) != 3 {
		t.Fatalf("merged env = %v", env)
	}
	// The input definitions are untouched.
	if orig[0].Image != "img-a" || orig[0].ReadyTimeout != time.Second || orig[0].Env(component.StackContext{})["K"] != "base" || len(orig) != 2 {
		t.Fatal("ApplyOverrides modified its input")
	}
	// Overriding a definition without an Env works too.
	cfg.Components = map[string]ComponentOverride{"b": {Env: map[string]string{"X": "1"}}}
	out = cfg.ApplyOverrides(orig)
	if got := out[1].Env(component.StackContext{}); len(got) != 1 || got["X"] != "1" {
		t.Fatalf("env on nil base = %v", got)
	}
	// No overrides: identical definitions.
	if out := Defaults().ApplyOverrides(orig); len(out) != 2 || out[0].Image != "img-a" {
		t.Fatalf("no-op out = %+v", out)
	}
}

func TestDerived(t *testing.T) {
	c := Defaults()
	if sc := c.StackContext(); sc.NetworkMode != "bridge" || sc.PublishAddr != "127.0.0.1" || sc.ProbeHost != "127.0.0.1" || sc.NetworkName != "astrosim" {
		t.Fatalf("sc = %+v", sc)
	}
	c.PublishAddr = "192.168.1.5"
	if sc := c.StackContext(); sc.ProbeHost != "192.168.1.5" {
		t.Fatalf("probe host = %s", sc.ProbeHost)
	}
	c.PublishAddr = "0.0.0.0"
	if sc := c.StackContext(); sc.ProbeHost != "127.0.0.1" {
		t.Fatalf("wildcard probe host = %s", sc.ProbeHost)
	}
	c.RestartMax, c.NoFail = 5, true
	if p := c.RestartPolicy(); p.MaxAttempts != 5 || !p.Unlimited || len(p.Backoff) != 3 {
		t.Fatalf("policy = %+v", p)
	}
}

func TestIsLoopback(t *testing.T) {
	for h, want := range map[string]bool{"127.0.0.1": true, "127.5.5.5": true, "::1": true, "localhost": true, "LOCALHOST": true,
		"": false, "0.0.0.0": false, "::": false, "10.0.0.1": false, "example.com": false} {
		if got := IsLoopback(h); got != want {
			t.Errorf("IsLoopback(%q) = %v", h, got)
		}
	}
}

func TestCleanupAndPrune(t *testing.T) {
	tests := []struct {
		name        string
		args        []string
		env         map[string]string
		files       map[string]string
		cleanup     bool
		prune       bool
		invalidWith string
	}{
		{name: "neither"},
		{name: "cleanup", args: []string{"--cleanup"}, cleanup: true},
		{name: "prune implies cleanup", args: []string{"--prune"}, cleanup: true, prune: true},
		{name: "both", args: []string{"--cleanup", "--prune"}, cleanup: true, prune: true},
		{name: "cleanup with autostart flag", args: []string{"--cleanup", "--autostart"}, cleanup: true, invalidWith: "--autostart"},
		{name: "prune with autostart flag", args: []string{"--prune", "--autostart"}, cleanup: true, prune: true, invalidWith: "--autostart"},
		{name: "prune with autostart from env", args: []string{"--prune"}, env: map[string]string{"ASTROSIM_ORCH_AUTOSTART": "1"},
			cleanup: true, prune: true, invalidWith: "--autostart"},
		{name: "cleanup with autostart from file", args: []string{"--cleanup", "--config", "c.json"},
			files: map[string]string{"c.json": `{"autostart":true}`}, cleanup: true, invalidWith: "--autostart"},
		{name: "no environment variable", env: map[string]string{"ASTROSIM_ORCH_CLEANUP": "1", "ASTROSIM_ORCH_PRUNE": "1"}},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			c, err := load(t, tt.args, tt.env, tt.files)
			if err != nil {
				t.Fatal(err)
			}
			if c.Cleanup != tt.cleanup || c.Prune != tt.prune {
				t.Errorf("Cleanup=%v Prune=%v, want %v %v", c.Cleanup, c.Prune, tt.cleanup, tt.prune)
			}
			err = c.Validate("linux", nil)
			switch {
			case tt.invalidWith == "" && err != nil:
				t.Errorf("Validate = %v", err)
			case tt.invalidWith != "" && (err == nil || !strings.Contains(err.Error(), tt.invalidWith)):
				t.Errorf("Validate = %v, want mention of %s", err, tt.invalidWith)
			}
		})
	}
}
