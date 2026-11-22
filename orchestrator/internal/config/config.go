// Package config holds the orchestrator's settings and how they are loaded.
//
// Values come from four layers, later ones overriding earlier ones:
//
//  1. built-in defaults (Defaults)
//  2. an optional JSON file named by --config
//  3. ASTROSIM_ORCH_<KEY> environment variables
//  4. command-line flags
//
// Only the JSON file can carry per-component overrides. The one-shot
// commands (--cleanup, --prune, --version) exist only as flags.
package config

import (
	"bytes"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"slices"
	"strconv"
	"strings"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/component"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/logging"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/supervisor"
)

// EnvPrefix prefixes the environment variable of every setting, for
// example ASTROSIM_ORCH_LISTEN.
const EnvPrefix = "ASTROSIM_ORCH_"

// Duration is a time.Duration that JSON-decodes from a Go duration string
// such as "30s" or "2m".
type Duration time.Duration

// UnmarshalJSON implements json.Unmarshaler.
func (d *Duration) UnmarshalJSON(b []byte) error {
	var s string
	if err := json.Unmarshal(b, &s); err != nil {
		return errors.New(`want a duration string such as "30s"`)
	}
	v, err := time.ParseDuration(s)
	if err != nil {
		return err
	}
	*d = Duration(v)
	return nil
}

// MarshalJSON implements json.Marshaler.
func (d Duration) MarshalJSON() ([]byte, error) {
	return json.Marshal(time.Duration(d).String())
}

// ComponentOverride adjusts one built-in component. Zero values leave the
// built-in setting alone.
type ComponentOverride struct {
	// Image replaces the container image reference.
	Image string `json:"image,omitempty"`
	// Env is merged over the component's own environment; a key set here wins.
	Env map[string]string `json:"env,omitempty"`
	// ReadyTimeout replaces how long the component may take to become ready.
	ReadyTimeout Duration `json:"readyTimeout,omitempty"`
	// LivenessTimeout replaces how long a ready component may stay silent.
	LivenessTimeout Duration `json:"livenessTimeout,omitempty"`
	// StopTimeout replaces the grace period given on stop.
	StopTimeout Duration `json:"stopTimeout,omitempty"`
	// Disabled removes the component from the stack.
	Disabled bool `json:"disabled,omitempty"`
}

// Config is the complete orchestrator configuration.
type Config struct {
	// Listen is the dashboard's host:port.
	Listen string `json:"listen"`
	// AllowRemote permits a non-loopback Listen address. There is no
	// authentication yet, so this exposes the Start/Stop controls to the network.
	AllowRemote bool `json:"allowRemote"`
	// Runtime is "auto", "podman" or "docker".
	Runtime string `json:"runtime"`
	// RuntimePath overrides the runtime executable location.
	RuntimePath string `json:"runtimePath"`
	// Network is the container network mode, "bridge" or "host" (Linux only).
	Network string `json:"network"`
	// PublishAddr is the host address component ports are published on in bridge mode.
	PublishAddr string `json:"publishAddr"`
	// NoBrowser stops the orchestrator opening the dashboard in a browser.
	NoBrowser bool `json:"noBrowser"`
	// Autostart starts the stack as soon as the first check passes.
	Autostart bool `json:"autostart"`
	// RestartMax is how many consecutive failures are repaired by restarting.
	RestartMax int `json:"restartMax"`
	// NoFail retries forever instead of giving up after RestartMax.
	NoFail bool `json:"noFail"`
	// LogLevel is "debug", "info", "warn" or "error".
	LogLevel string `json:"logLevel"`
	// LogDir is the folder session logs are kept in. Empty means the
	// operating system's default location; main makes a relative path absolute.
	LogDir string `json:"logDir"`
	// LogKeep is how many session log archives to keep.
	LogKeep int `json:"logKeep"`
	// LogMaxMB is the total size, in MB, the kept archives may take up.
	LogMaxMB int `json:"logMaxMB"`
	// LogFileMaxMB caps each live log file, in MB; a longer log keeps its
	// beginning and its most recent part.
	LogFileMaxMB int `json:"logFileMaxMB"`
	// Verbose mirrors the combined log, every component's output as well as
	// the orchestrator's own lines, to the terminal.
	Verbose bool `json:"verbose"`
	// Components holds per-component overrides by component ID. Only the
	// JSON file can set it.
	Components map[string]ComponentOverride `json:"components,omitempty"`

	// ConfigFile is the --config path. It is not part of the JSON file.
	ConfigFile string `json:"-"`
	// ShowVersion is set by --version. It is not part of the JSON file.
	ShowVersion bool `json:"-"`
	// Cleanup is set by --cleanup: remove leftover managed containers and
	// exit, without serving the dashboard. It is a one-shot command, so it
	// is a flag only: not in the JSON file and with no environment
	// variable, where a stray setting would silently turn every launch
	// into a cleanup.
	Cleanup bool `json:"-"`
	// Prune is set by --prune: everything --cleanup does, plus removing the
	// managed network. It implies Cleanup. Like Cleanup it is a flag only.
	Prune bool `json:"-"`
	// PrintLogDir is set by --print-log-dir: print the log folder and exit.
	// Like Cleanup it is a flag only, so a stray setting cannot turn every
	// launch into a print.
	PrintLogDir bool `json:"-"`
}

// Defaults returns the built-in configuration.
func Defaults() Config {
	return Config{
		Listen:       "127.0.0.1:8770",
		Runtime:      runtime.Auto,
		Network:      component.NetworkBridge,
		PublishAddr:  "127.0.0.1",
		RestartMax:   supervisor.DefaultRestartPolicy.MaxAttempts,
		LogLevel:     "info",
		LogKeep:      20,
		LogMaxMB:     500,
		LogFileMaxMB: 64,
	}
}

// ReadFileFunc reads a file; it is os.ReadFile in production.
type ReadFileFunc func(name string) ([]byte, error)

// Load builds the configuration from command-line arguments (without the
// program name), an environment lookup and a file reader, in the precedence
// order described in the package comment. Usage and flag errors are printed
// to stderr; flag.ErrHelp is returned for -h and --help. Load does not
// validate: call Validate on the result.
func Load(args []string, env func(string) string, readFile ReadFileFunc) (Config, error) {
	return LoadTo(os.Stderr, args, env, readFile)
}

// LoadTo is Load with the usage and flag-error output going to w.
func LoadTo(w io.Writer, args []string, env func(string) string, readFile ReadFileFunc) (Config, error) {
	if env == nil {
		env = func(string) string { return "" }
	}
	if readFile == nil {
		readFile = os.ReadFile
	}
	cfg := Defaults()
	fs := flag.NewFlagSet("astrosim", flag.ContinueOnError)
	fs.SetOutput(w)
	// Flags are registered with the defaults for the --help text; which
	// were actually given is learned from fs.Visit, so an unset flag never
	// overrides a lower layer.
	var f Config
	def := Defaults()
	fs.StringVar(&f.Listen, "listen", def.Listen, "dashboard address, host:port")
	fs.BoolVar(&f.AllowRemote, "allow-remote", def.AllowRemote, "allow --listen on a non-loopback address (no authentication!)")
	fs.StringVar(&f.Runtime, "runtime", def.Runtime, "container runtime: auto, podman or docker")
	fs.StringVar(&f.RuntimePath, "runtime-path", def.RuntimePath, "path of the podman or docker executable")
	fs.StringVar(&f.Network, "network", def.Network, "container network mode: bridge, or host (Linux only)")
	fs.StringVar(&f.PublishAddr, "publish-addr", def.PublishAddr, "host address that container ports are published on")
	fs.BoolVar(&f.NoBrowser, "no-browser", def.NoBrowser, "do not open the dashboard in a browser")
	fs.BoolVar(&f.Autostart, "autostart", def.Autostart, "start the simulation as soon as the checks pass")
	fs.IntVar(&f.RestartMax, "restart-max", def.RestartMax, "automatic restarts before giving up (at least 1)")
	fs.BoolVar(&f.NoFail, "no-fail", def.NoFail, "never give up restarting after a failure, retry every 10 s until stopped")
	fs.StringVar(&f.LogLevel, "log-level", def.LogLevel, "log level: debug, info, warn or error")
	fs.StringVar(&f.LogDir, "log-dir", def.LogDir, "folder for session logs (default: the operating system's log location; see --print-log-dir)")
	fs.IntVar(&f.LogKeep, "log-keep", def.LogKeep, "session log archives to keep (at least 1)")
	fs.IntVar(&f.LogMaxMB, "log-max-mb", def.LogMaxMB, "total size of kept log archives in MB (at least 1)")
	fs.IntVar(&f.LogFileMaxMB, "log-file-max-mb", def.LogFileMaxMB, "size limit of each live log file in MB (at least 4)")
	fs.BoolVar(&f.Verbose, "verbose", def.Verbose, "also show every component's output on the terminal")
	fs.BoolVar(&f.PrintLogDir, "print-log-dir", false, "print the log folder and exit")
	fs.StringVar(&f.ConfigFile, "config", "", "JSON configuration file")
	fs.BoolVar(&f.ShowVersion, "version", false, "print the version and exit")
	fs.BoolVar(&f.Cleanup, "cleanup", false, "remove leftover AstroSim containers and exit (refused while AstroSim is running)")
	fs.BoolVar(&f.Prune, "prune", false, "like --cleanup, and also remove the AstroSim network")
	fs.Usage = func() {
		fmt.Fprintf(w, "Usage: astrosim [flags]\n\nStarts the AstroSim dashboard. Every setting except the per-component\noverrides and the one-shot --cleanup, --prune, --print-log-dir and --version flags also has\nan environment variable, %s<NAME> (for example\n%sALLOW_REMOTE); flags win over the environment, which wins over --config.\n\nFlags:\n", EnvPrefix, EnvPrefix)
		fs.PrintDefaults()
	}
	if err := fs.Parse(args); err != nil {
		return Config{}, err
	}
	if fs.NArg() > 0 {
		return Config{}, fmt.Errorf("unexpected argument %q", fs.Arg(0))
	}
	set := map[string]bool{}
	fs.Visit(func(fl *flag.Flag) { set[fl.Name] = true })

	path := f.ConfigFile
	if !set["config"] {
		path = env(EnvPrefix + "CONFIG")
	}
	if path != "" {
		b, err := readFile(path)
		if err != nil {
			return Config{}, fmt.Errorf("read config file: %w", err)
		}
		dec := json.NewDecoder(bytes.NewReader(b))
		dec.DisallowUnknownFields()
		if err := dec.Decode(&cfg); err != nil {
			return Config{}, fmt.Errorf("config file %s: %w", path, err)
		}
		if dec.More() {
			return Config{}, fmt.Errorf("config file %s: unexpected data after the JSON object", path)
		}
	}
	cfg.ConfigFile = path

	if err := applyEnv(&cfg, env); err != nil {
		return Config{}, err
	}

	if set["listen"] {
		cfg.Listen = f.Listen
	}
	if set["allow-remote"] {
		cfg.AllowRemote = f.AllowRemote
	}
	if set["runtime"] {
		cfg.Runtime = f.Runtime
	}
	if set["runtime-path"] {
		cfg.RuntimePath = f.RuntimePath
	}
	if set["network"] {
		cfg.Network = f.Network
	}
	if set["publish-addr"] {
		cfg.PublishAddr = f.PublishAddr
	}
	if set["no-browser"] {
		cfg.NoBrowser = f.NoBrowser
	}
	if set["autostart"] {
		cfg.Autostart = f.Autostart
	}
	if set["restart-max"] {
		cfg.RestartMax = f.RestartMax
	}
	if set["no-fail"] {
		cfg.NoFail = f.NoFail
	}
	if set["log-level"] {
		cfg.LogLevel = f.LogLevel
	}
	if set["log-dir"] {
		cfg.LogDir = f.LogDir
	}
	if set["log-keep"] {
		cfg.LogKeep = f.LogKeep
	}
	if set["log-max-mb"] {
		cfg.LogMaxMB = f.LogMaxMB
	}
	if set["log-file-max-mb"] {
		cfg.LogFileMaxMB = f.LogFileMaxMB
	}
	if set["verbose"] {
		cfg.Verbose = f.Verbose
	}
	cfg.PrintLogDir = f.PrintLogDir
	cfg.ShowVersion = f.ShowVersion
	cfg.Prune = f.Prune
	// Pruning does everything cleaning up does, so callers only test Cleanup
	// to know a one-shot cleanup was asked for.
	cfg.Cleanup = f.Cleanup || f.Prune
	return cfg, nil
}

// applyEnv overlays the ASTROSIM_ORCH_* variables.
func applyEnv(cfg *Config, env func(string) string) error {
	str := func(key string, dst *string) {
		if v := env(EnvPrefix + key); v != "" {
			*dst = v
		}
	}
	boolean := func(key string, dst *bool) error {
		v := env(EnvPrefix + key)
		if v == "" {
			return nil
		}
		b, err := strconv.ParseBool(v)
		if err != nil {
			return fmt.Errorf("%s%s: %q is not a boolean", EnvPrefix, key, v)
		}
		*dst = b
		return nil
	}
	integer := func(key string, dst *int) error {
		v := env(EnvPrefix + key)
		if v == "" {
			return nil
		}
		n, err := strconv.Atoi(v)
		if err != nil {
			return fmt.Errorf("%s%s: %q is not a number", EnvPrefix, key, v)
		}
		*dst = n
		return nil
	}
	str("LISTEN", &cfg.Listen)
	str("RUNTIME", &cfg.Runtime)
	str("RUNTIME_PATH", &cfg.RuntimePath)
	str("NETWORK", &cfg.Network)
	str("PUBLISH_ADDR", &cfg.PublishAddr)
	str("LOG_LEVEL", &cfg.LogLevel)
	str("LOG_DIR", &cfg.LogDir)
	for _, b := range []struct {
		key string
		dst *bool
	}{
		{"ALLOW_REMOTE", &cfg.AllowRemote}, {"NO_BROWSER", &cfg.NoBrowser},
		{"AUTOSTART", &cfg.Autostart}, {"NO_FAIL", &cfg.NoFail}, {"VERBOSE", &cfg.Verbose},
	} {
		if err := boolean(b.key, b.dst); err != nil {
			return err
		}
	}
	for _, n := range []struct {
		key string
		dst *int
	}{
		{"RESTART_MAX", &cfg.RestartMax}, {"LOG_KEEP", &cfg.LogKeep},
		{"LOG_MAX_MB", &cfg.LogMaxMB}, {"LOG_FILE_MAX_MB", &cfg.LogFileMaxMB},
	} {
		if err := integer(n.key, n.dst); err != nil {
			return err
		}
	}
	return nil
}

// Validate checks the configuration for the operating system goos (a
// runtime.GOOS value) and the available component definitions. Every
// problem found is reported, joined into one error.
func (c Config) Validate(goos string, defs []component.Definition) error {
	var errs []error
	bad := func(format string, a ...any) { errs = append(errs, fmt.Errorf(format, a...)) }

	switch strings.ToLower(c.Runtime) {
	case runtime.Auto, "podman", "docker":
	default:
		bad("--runtime %q: want auto, podman or docker", c.Runtime)
	}
	switch c.Network {
	case component.NetworkBridge:
	case component.NetworkHost:
		if goos != "linux" {
			bad("--network host is only supported on Linux, not %s", goos)
		}
	default:
		bad("--network %q: want bridge or host", c.Network)
	}
	if c.RestartMax < 1 {
		bad("--restart-max must be at least 1, got %d", c.RestartMax)
	}
	if c.LogKeep < 1 {
		bad("--log-keep must be at least 1, got %d", c.LogKeep)
	}
	if c.LogMaxMB < 1 {
		bad("--log-max-mb must be at least 1, got %d", c.LogMaxMB)
	}
	if c.LogFileMaxMB < 4 {
		bad("--log-file-max-mb must be at least 4, got %d", c.LogFileMaxMB)
	}
	if _, err := logging.ParseLevel(c.LogLevel); err != nil {
		bad("--log-level: %v", err)
	}
	if ip := net.ParseIP(c.PublishAddr); c.PublishAddr == "" || ip == nil {
		bad("--publish-addr %q is not an IP address", c.PublishAddr)
	}
	host, port, err := net.SplitHostPort(c.Listen)
	switch {
	case err != nil:
		bad("--listen %q: %v", c.Listen, err)
	default:
		if p, perr := strconv.Atoi(port); perr != nil || p < 0 || p > 65535 {
			bad("--listen %q: invalid port %q", c.Listen, port)
		}
		if !IsLoopback(host) && !c.AllowRemote {
			bad("--listen %q is not a loopback address: the dashboard has no authentication, "+
				"so anyone who can reach it could start and stop the simulation; "+
				"add --allow-remote if you really want that", c.Listen)
		}
	}
	if (c.Cleanup || c.Prune) && c.Autostart {
		// Autostart may also come from the config file or the environment.
		bad("--cleanup and --prune remove containers and exit, so they cannot be combined with "+
			"--autostart (also check --config and %sAUTOSTART)", EnvPrefix)
	}
	errs = append(errs, c.validateComponents(defs)...)
	return errors.Join(errs...)
}

func (c Config) validateComponents(defs []component.Definition) []error {
	var errs []error
	known := map[string]component.Definition{}
	for _, d := range defs {
		known[d.ID] = d
	}
	ids := make([]string, 0, len(c.Components))
	for id := range c.Components {
		ids = append(ids, id)
	}
	slices.Sort(ids)
	for _, id := range ids {
		o := c.Components[id]
		if _, ok := known[id]; !ok {
			errs = append(errs, fmt.Errorf("components.%s: unknown component (known: %s)", id, knownList(defs)))
			continue
		}
		for _, t := range []struct {
			name string
			d    Duration
		}{{"readyTimeout", o.ReadyTimeout}, {"livenessTimeout", o.LivenessTimeout}, {"stopTimeout", o.StopTimeout}} {
			if t.d < 0 {
				errs = append(errs, fmt.Errorf("components.%s.%s must be positive", id, t.name))
			}
		}
	}
	for _, d := range defs {
		if c.Components[d.ID].Disabled {
			continue
		}
		for _, dep := range d.DependsOn {
			if c.Components[dep.ID].Disabled {
				errs = append(errs, fmt.Errorf("components.%s is disabled but %s depends on it", dep.ID, d.ID))
			}
		}
	}
	return errs
}

func knownList(defs []component.Definition) string {
	ids := make([]string, 0, len(defs))
	for _, d := range defs {
		ids = append(ids, d.ID)
	}
	return strings.Join(ids, ", ")
}

// IsLoopback reports whether host, as found in a host:port address, only
// accepts connections from the local machine: "localhost" or a loopback IP.
// An empty host (all interfaces) and unspecified addresses are not loopback.
func IsLoopback(host string) bool {
	if strings.EqualFold(host, "localhost") {
		return true
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}

// ApplyOverrides returns copies of defs with the component overrides
// applied and disabled components removed. The input is not modified. Call
// Validate first: an override for an unknown component is ignored here.
func (c Config) ApplyOverrides(defs []component.Definition) []component.Definition {
	out := make([]component.Definition, 0, len(defs))
	for _, d := range defs {
		o, ok := c.Components[d.ID]
		if ok && o.Disabled {
			continue
		}
		if ok {
			if o.Image != "" {
				d.Image = o.Image
			}
			if o.ReadyTimeout > 0 {
				d.ReadyTimeout = time.Duration(o.ReadyTimeout)
			}
			if o.LivenessTimeout > 0 {
				d.LivenessTimeout = time.Duration(o.LivenessTimeout)
			}
			if o.StopTimeout > 0 {
				d.StopTimeout = time.Duration(o.StopTimeout)
			}
			if len(o.Env) > 0 {
				base, over := d.Env, cloneMap(o.Env)
				d.Env = func(sc component.StackContext) map[string]string {
					m := map[string]string{}
					if base != nil {
						for k, v := range base(sc) {
							m[k] = v
						}
					}
					for k, v := range over {
						m[k] = v
					}
					return m
				}
			}
		}
		out = append(out, d)
	}
	return out
}

// cloneMap returns a private copy of m, so later changes to the config do not
// leak into a definition.
func cloneMap(m map[string]string) map[string]string {
	c := make(map[string]string, len(m))
	for k, v := range m {
		c[k] = v
	}
	return c
}

// StackContext derives the deployment settings the components see.
func (c Config) StackContext() component.StackContext {
	sc := component.DefaultStackContext()
	sc.NetworkMode = c.Network
	sc.PublishAddr = c.PublishAddr
	// The probes run on the host. A specific publish address is where the
	// ports are reachable; a wildcard or host networking means loopback.
	sc.ProbeHost = "127.0.0.1"
	if ip := net.ParseIP(c.PublishAddr); c.Network == component.NetworkBridge && ip != nil && !ip.IsUnspecified() {
		sc.ProbeHost = c.PublishAddr
	}
	return sc
}

// RestartPolicy derives the supervisor's restart policy.
func (c Config) RestartPolicy() supervisor.RestartPolicy {
	p := supervisor.DefaultRestartPolicy
	p.MaxAttempts = c.RestartMax
	p.Unlimited = c.NoFail
	return p
}
