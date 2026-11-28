package runtime

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Dialect captures where the podman and docker command lines differ. The
// shared subset (run, stop, rm, wait, inspect, ps, logs) is common code.
type Dialect struct {
	// Name is "podman" or "docker".
	Name string
	// ImageExistsArgs returns the command that tests for a local image.
	ImageExistsArgs func(image string) []string
	// RunLogArgs are extra `run` arguments that select the container log
	// driver, or nil for the engine's default.
	RunLogArgs []string
	// ImageMissing decides, from a failed ImageExistsArgs command, whether
	// the image is simply absent (true) or the engine misbehaved (false).
	ImageMissing func(res Result) bool
}

// Podman is the podman dialect. `podman image exists` exits 1 for a missing
// image and 125 for engine errors.
//
// Containers use the k8s-file log driver: podman's default depends on the
// host (journald on many Linux distributions), and journald is slower and
// has had `logs --follow` bugs, whereas k8s-file behaves the same everywhere.
var Podman = Dialect{
	Name:            "podman",
	RunLogArgs:      []string{"--log-driver", "k8s-file"},
	ImageExistsArgs: func(image string) []string { return []string{"image", "exists", image} },
	ImageMissing:    func(res Result) bool { return res.ExitCode == 1 },
}

// Docker is the docker dialect. `docker image inspect` exits 1 both for a
// missing image and for an unreachable daemon, so the stderr text decides.
var Docker = Dialect{
	Name: "docker",
	ImageExistsArgs: func(image string) []string {
		return []string{"image", "inspect", "--format", "{{.Id}}", image}
	},
	ImageMissing: func(res Result) bool {
		return res.ExitCode == 1 && isNotFoundText(res.Stderr)
	},
}

// CLI is a Runtime that drives a podman or docker executable.
type CLI struct {
	bin     string
	dialect Dialect
	cmd     Commander
	info    Info
}

// NewCLI returns a Runtime running bin through cmd with the given dialect.
// A nil cmd uses ExecCommander.
func NewCLI(bin string, d Dialect, cmd Commander) *CLI {
	if cmd == nil {
		cmd = ExecCommander{}
	}
	return &CLI{bin: bin, dialect: d, cmd: cmd, info: Info{Name: d.Name, Path: bin}}
}

// CommandError is returned when the runtime executable exits non-zero.
type CommandError struct {
	// Args are the arguments passed to the executable.
	Args []string
	// ExitCode is the process exit code.
	ExitCode int
	// Stderr is the trimmed standard error output.
	Stderr string
}

// Error implements error.
func (e *CommandError) Error() string {
	cmd := ""
	if len(e.Args) > 0 {
		cmd = e.Args[0]
	}
	return fmt.Sprintf("runtime: %s failed (exit %d): %s", cmd, e.ExitCode, e.Stderr)
}

// isNotFoundText recognises the many spellings podman and docker use for a
// missing object.
func isNotFoundText(s string) bool {
	s = strings.ToLower(s)
	for _, m := range []string{"no such", "not found", "not known", "no container with name"} {
		if strings.Contains(s, m) {
			return true
		}
	}
	return false
}

// do runs the executable and converts a non-zero exit into a CommandError,
// or ErrNotFound (wrapped) when the failure text says the object is missing.
func (c *CLI) do(ctx context.Context, args ...string) (Result, error) {
	res, err := c.cmd.Run(ctx, c.bin, args...)
	if err != nil {
		return res, err
	}
	if res.ExitCode != 0 {
		ce := &CommandError{Args: args, ExitCode: res.ExitCode, Stderr: strings.TrimSpace(res.Stderr)}
		if isNotFoundText(res.Stderr) {
			return res, fmt.Errorf("%w: %w", ErrNotFound, ce)
		}
		return res, ce
	}
	return res, nil
}

// Info implements Runtime.
func (c *CLI) Info() Info { return c.info }

// Ping implements Runtime. `version --format {{.Server.Version}}` needs a
// live engine in both podman and docker, unlike the client-only `--version`.
func (c *CLI) Ping(ctx context.Context) (Info, error) {
	res, err := c.do(ctx, "version", "--format", "{{.Server.Version}}")
	if err != nil {
		var ce *CommandError
		if errors.As(err, &ce) {
			return Info{}, fmt.Errorf("%w: %w", ErrNotRunning, err)
		}
		return Info{}, err
	}
	c.info.Version = strings.TrimSpace(res.Stdout)
	return c.info, nil
}

// ImageExists implements Runtime.
func (c *CLI) ImageExists(ctx context.Context, image string) (bool, error) {
	args := c.dialect.ImageExistsArgs(image)
	res, err := c.cmd.Run(ctx, c.bin, args...)
	if err != nil {
		return false, err
	}
	switch {
	case res.ExitCode == 0:
		return true, nil
	case c.dialect.ImageMissing(res):
		return false, nil
	default:
		return false, &CommandError{Args: args, ExitCode: res.ExitCode, Stderr: strings.TrimSpace(res.Stderr)}
	}
}

// EnsureNetwork implements Runtime. Podman and docker share the
// `network create --label k=v` syntax.
func (c *CLI) EnsureNetwork(ctx context.Context, name string, labels map[string]string) error {
	_, err := c.do(ctx, "network", "inspect", name)
	if err == nil {
		return nil
	}
	if !errors.Is(err, ErrNotFound) {
		return err
	}
	args := []string{"network", "create"}
	for _, k := range sortedKeys(labels) {
		args = append(args, "--label", k+"="+labels[k])
	}
	_, err = c.do(ctx, append(args, name)...)
	return err
}

// ListNetworksByLabel implements Runtime.
func (c *CLI) ListNetworksByLabel(ctx context.Context, key, value string) ([]string, error) {
	res, err := c.do(ctx, "network", "ls", "--filter", "label="+key+"="+value, "--format", "{{.Name}}")
	if err != nil {
		return nil, err
	}
	return splitNames(res.Stdout), nil
}

// RemoveNetwork implements Runtime. A missing network is success.
func (c *CLI) RemoveNetwork(ctx context.Context, name string) error {
	_, err := c.do(ctx, "network", "rm", name)
	if errors.Is(err, ErrNotFound) {
		return nil
	}
	return err
}

// RunArgs returns the argument list for starting spec. The output is
// deterministic (sorted env and labels) so it can be compared in tests.
// With host networking, port publishing and aliases are omitted because the
// container shares the host's network stack.
func (c *CLI) RunArgs(spec RunSpec) []string {
	args := []string{"run", "-d", "--name", spec.Name}
	args = append(args, c.dialect.RunLogArgs...)
	host := spec.Network == HostNetwork
	if spec.Network != "" {
		args = append(args, "--network", spec.Network)
	}
	if !host {
		for _, a := range spec.Aliases {
			args = append(args, "--network-alias", a)
		}
		for _, p := range spec.Publish {
			args = append(args, "-p", p.mapping())
		}
	}
	for _, k := range sortedKeys(spec.Env) {
		args = append(args, "-e", k+"="+spec.Env[k])
	}
	for _, k := range sortedKeys(spec.Labels) {
		args = append(args, "--label", k+"="+spec.Labels[k])
	}
	return append(args, spec.Image)
}

// mapping renders the -p argument: [addr:]hostPort:containerPort/proto.
func (p Publish) mapping() string {
	proto := p.Proto
	if proto == "" {
		proto = "tcp"
	}
	s := strconv.Itoa(p.HostPort) + ":" + strconv.Itoa(p.ContainerPort) + "/" + proto
	if p.HostAddr != "" {
		s = p.HostAddr + ":" + s
	}
	return s
}

func sortedKeys(m map[string]string) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}

// Run implements Runtime.
func (c *CLI) Run(ctx context.Context, spec RunSpec) (string, error) {
	res, err := c.do(ctx, c.RunArgs(spec)...)
	if err != nil {
		return "", err
	}
	return strings.TrimSpace(res.Stdout), nil
}

// Stop implements Runtime. A container that is already stopped is success.
func (c *CLI) Stop(ctx context.Context, name string, grace time.Duration) error {
	secs := int(grace.Round(time.Second) / time.Second)
	if secs < 0 {
		secs = 0
	}
	_, err := c.do(ctx, "stop", "-t", strconv.Itoa(secs), name)
	return err
}

// removeBackoff is the wait before each re-check of a container whose
// `rm -f` failed. The total (3.75 s) bounds how long Remove can stall.
var removeBackoff = []time.Duration{
	250 * time.Millisecond, 500 * time.Millisecond, time.Second, 2 * time.Second,
}

// sleep waits for d or until ctx ends, returning ctx's error in the latter
// case. It is a variable so tests can replace the real wait.
var sleep = func(ctx context.Context, d time.Duration) error {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-t.C:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

// Remove implements Runtime. A missing container is success, and so is a
// failed `rm -f` after which the container is gone anyway: rootless podman
// can delete the container and then fail tidying its network namespace
// ("rootless netns: kill network process: permission denied", typically
// Ubuntu's AppArmor profile restricting podman's network helper), exiting
// 125. Podman may finish the removal a moment later, so after a failure
// Remove inspects and retries `rm -f` on the removeBackoff schedule. If the
// container never disappears it returns the first error, wrapped.
func (c *CLI) Remove(ctx context.Context, name string) error {
	_, err := c.do(ctx, "rm", "-f", name)
	if err == nil || errors.Is(err, ErrNotFound) {
		return nil
	}
	for _, d := range removeBackoff {
		if serr := sleep(ctx, d); serr != nil {
			return fmt.Errorf("remove %s: %w (waiting: %w)", name, err, serr)
		}
		_, ierr := c.Inspect(ctx, name)
		if errors.Is(ierr, ErrNotFound) {
			slog.Warn("container removed although the runtime reported an error",
				"container", name, "err", err)
			return nil
		}
		if ierr != nil {
			// Existence is unknown; do not hide the removal failure.
			continue
		}
		if _, rerr := c.do(ctx, "rm", "-f", name); rerr == nil || errors.Is(rerr, ErrNotFound) {
			slog.Warn("container removed on retry after the runtime reported an error",
				"container", name, "err", err)
			return nil
		}
	}
	return fmt.Errorf("remove %s: %w", name, err)
}

// Wait implements Runtime.
func (c *CLI) Wait(ctx context.Context, name string) (int, error) {
	res, err := c.do(ctx, "wait", name)
	if err != nil {
		return 0, err
	}
	code, perr := strconv.Atoi(strings.TrimSpace(res.Stdout))
	if perr != nil {
		return 0, fmt.Errorf("runtime: parse wait output %q: %w", res.Stdout, perr)
	}
	return code, nil
}

// stateJSON mirrors `inspect --format {{json .State}}`. Podman 4 reports the
// health check under "Healthcheck"; podman 5 and docker use "Health".
type stateJSON struct {
	Status      string                   `json:"Status"`
	Running     bool                     `json:"Running"`
	ExitCode    int                      `json:"ExitCode"`
	OOMKilled   bool                     `json:"OOMKilled"`
	Error       string                   `json:"Error"`
	StartedAt   string                   `json:"StartedAt"`
	FinishedAt  string                   `json:"FinishedAt"`
	Health      *struct{ Status string } `json:"Health"`
	Healthcheck *struct{ Status string } `json:"Healthcheck"`
}

// parseState converts inspect JSON into a ContainerState.
func parseState(data []byte) (ContainerState, error) {
	var j stateJSON
	if err := json.Unmarshal(data, &j); err != nil {
		return ContainerState{}, fmt.Errorf("runtime: parse inspect state: %w", err)
	}
	st := ContainerState{
		Status:    j.Status,
		Running:   j.Running,
		ExitCode:  j.ExitCode,
		OOMKilled: j.OOMKilled,
		Error:     j.Error,
	}
	switch {
	case j.Health != nil:
		st.Health = j.Health.Status
	case j.Healthcheck != nil:
		st.Health = j.Healthcheck.Status
	}
	var err error
	if st.StartedAt, err = parseTime(j.StartedAt); err != nil {
		return ContainerState{}, err
	}
	if st.FinishedAt, err = parseTime(j.FinishedAt); err != nil {
		return ContainerState{}, err
	}
	return st, nil
}

// parseTime parses an RFC 3339 timestamp; empty and the runtimes' "never"
// value (year 1) give the zero time.
func parseTime(s string) (time.Time, error) {
	if s == "" {
		return time.Time{}, nil
	}
	t, err := time.Parse(time.RFC3339Nano, s)
	if err != nil {
		return time.Time{}, fmt.Errorf("runtime: parse time %q: %w", s, err)
	}
	if t.Year() <= 1 {
		return time.Time{}, nil
	}
	return t, nil
}

// Inspect implements Runtime.
func (c *CLI) Inspect(ctx context.Context, name string) (ContainerState, error) {
	res, err := c.do(ctx, "inspect", "--format", "{{json .State}}", name)
	if err != nil {
		return ContainerState{}, err
	}
	return parseState([]byte(strings.TrimSpace(res.Stdout)))
}

// ListByLabel implements Runtime.
func (c *CLI) ListByLabel(ctx context.Context, key, value string) ([]string, error) {
	res, err := c.do(ctx, "ps", "-a", "--filter", "label="+key+"="+value, "--format", "{{.Names}}")
	if err != nil {
		return nil, err
	}
	return splitNames(res.Stdout), nil
}

// splitNames returns the sorted non-empty lines of a `--format {{.Name}}`
// listing.
func splitNames(out string) []string {
	var names []string
	for _, line := range strings.Split(out, "\n") {
		if line = strings.TrimSpace(line); line != "" {
			names = append(names, line)
		}
	}
	sort.Strings(names)
	return names
}

// Logs implements Runtime. Container stderr arrives on the command's stderr,
// so both streams are combined.
func (c *CLI) Logs(ctx context.Context, name string, tail int) (string, error) {
	res, err := c.do(ctx, "logs", "--tail", strconv.Itoa(tail), name)
	if err != nil {
		return "", err
	}
	return res.Stdout + res.Stderr, nil
}

// maxErrText is how much of the command's stderr FollowLogs keeps to explain
// a failure.
const maxErrText = 4 << 10

// tailWriter forwards writes to w and remembers the last maxErrText bytes.
type tailWriter struct {
	mu  sync.Mutex
	w   io.Writer
	buf []byte
}

func (t *tailWriter) Write(p []byte) (int, error) {
	t.mu.Lock()
	t.buf = append(t.buf, p...)
	if over := len(t.buf) - maxErrText; over > 0 {
		t.buf = append(t.buf[:0], t.buf[over:]...)
	}
	t.mu.Unlock()
	return t.w.Write(p)
}

func (t *tailWriter) text() string {
	t.mu.Lock()
	defer t.mu.Unlock()
	return strings.TrimSpace(string(t.buf))
}

// FollowLogs implements Runtime. Both podman and docker accept
// `logs --timestamps [--follow] [--since T] name` and send the container's
// stderr to the command's stderr. Verified against podman 5.8: the follow
// ends by itself when the container exits, --since is exclusive and takes a
// UTC RFC 3339 timestamp with nanoseconds, and each line starts with a
// timestamp in the host's local offset (docker uses UTC); all parse as
// RFC 3339.
func (c *CLI) FollowLogs(ctx context.Context, name string, opts LogOptions, stdout, stderr io.Writer) error {
	s, ok := c.cmd.(Streamer)
	if !ok {
		return fmt.Errorf("runtime: %T cannot stream command output", c.cmd)
	}
	args := []string{"logs", "--timestamps"}
	if opts.Follow {
		args = append(args, "--follow")
	}
	if !opts.Since.IsZero() {
		args = append(args, "--since", opts.Since.UTC().Format(time.RFC3339Nano))
	}
	args = append(args, name)
	tw := &tailWriter{w: stderr}
	code, err := s.Stream(ctx, stdout, tw, c.bin, args...)
	if err != nil {
		return err
	}
	if code == 0 {
		return nil
	}
	ce := &CommandError{Args: args, ExitCode: code, Stderr: tw.text()}
	if isNotFoundText(ce.Stderr) {
		return fmt.Errorf("%w: %w", ErrNotFound, ce)
	}
	return ce
}

// syncWriter serialises writes to w, so that the two output streams of one
// command can share a destination.
type syncWriter struct {
	mu sync.Mutex
	w  io.Writer
}

func (s *syncWriter) Write(p []byte) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.w.Write(p)
}

// Pull implements Runtime. Both podman and docker accept `pull <image>`.
// Podman writes its progress to stderr and docker to stdout, so both streams
// go to progress; stderr is also kept (by tailWriter) to explain a failure.
func (c *CLI) Pull(ctx context.Context, image string, progress io.Writer) error {
	s, ok := c.cmd.(Streamer)
	if !ok {
		return fmt.Errorf("runtime: %T cannot stream command output", c.cmd)
	}
	if progress == nil {
		progress = io.Discard
	}
	out := &syncWriter{w: progress}
	tw := &tailWriter{w: out}
	args := []string{"pull", image}
	code, err := s.Stream(ctx, out, tw, c.bin, args...)
	if err != nil {
		return err
	}
	if code == 0 {
		return nil
	}
	return classifyPull(&CommandError{Args: args, ExitCode: code, Stderr: tw.text()})
}

// pullClasses maps fragments of a failed pull's error text to the sentinel
// error they mean. The first class with a matching fragment wins, so the
// order matters: a DNS failure says "no such host", which must not be
// mistaken for a missing image (isNotFoundText would do exactly that).
var pullClasses = []struct {
	err       error
	fragments []string
}{
	{ErrNoSpace, []string{"no space left on device"}},
	{ErrDenied, []string{"unauthorized", "denied", "authentication required", "403 forbidden"}},
	{ErrUnreachable, []string{
		"dial tcp", "no such host", "i/o timeout", "connection refused", "network is unreachable",
		"tls handshake timeout", "temporary failure in name resolution", "connection reset",
	}},
	{ErrNotFound, []string{
		"manifest unknown", "name unknown", "not found", "no matching manifest",
		"no image found in manifest list", "no image found in image index",
	}},
}

// classifyPull wraps ce with the sentinel error its text indicates (see
// pullClasses), or returns ce itself when the text is not recognised. The
// result matches both the sentinel (errors.Is) and *CommandError (errors.As).
func classifyPull(ce *CommandError) error {
	text := strings.ToLower(ce.Stderr)
	for _, class := range pullClasses {
		for _, f := range class.fragments {
			if strings.Contains(text, f) {
				return fmt.Errorf("%w: %w", class.err, ce)
			}
		}
	}
	return ce
}
