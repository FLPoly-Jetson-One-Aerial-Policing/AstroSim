package session

import (
	"context"
	"errors"
	"fmt"
	"net"
	"strconv"
	"strings"
	"time"

	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/component"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/health"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/runtime"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/supervisor"
)

// hostPort is the host:port a component port is reachable on.
func hostPort(sc component.StackContext, port int) string {
	return sc.ProbeAddr(port)
}

// CleanupStale removes every container carrying the managed label. main
// calls it once, after taking the single-instance lock, to clear leftovers
// of a previous crashed run.
func CleanupStale(ctx context.Context, rt runtime.Runtime) error {
	names, err := rt.ListByLabel(ctx, component.LabelManaged, "true")
	if err != nil {
		return fmt.Errorf("list stale containers: %w", err)
	}
	var errs []error
	for _, n := range names {
		if err := rt.Remove(ctx, n); err != nil {
			errs = append(errs, fmt.Errorf("remove %s: %w", n, err))
		}
	}
	return errors.Join(errs...)
}

// --- preflight --------------------------------------------------------

func (s *Session) doPreflight(gen uint64) {
	ctx, cancel := context.WithTimeout(s.root, preflightTimeout)
	defer cancel()
	rep := s.preflight(ctx)
	if rep.Problem != "" {
		s.log.Info("preflight found a problem", "problem", rep.Problem, "detail", rep.Detail)
	}
	s.post(supervisor.Event{Kind: supervisor.PreflightDone, Gen: gen, Report: rep})
}

// preflight checks the runtime, then the images, then the host ports. It
// reports the first class of problem found, so the user fixes one thing at a time.
func (s *Session) preflight(ctx context.Context) supervisor.PreflightReport {
	info, err := s.opts.Runtime.Ping(ctx)
	if err == nil {
		if verr := runtime.CheckMinVersion(info, s.log); verr != nil {
			return supervisor.PreflightReport{Problem: supervisor.ProblemRuntimeTooOld, Detail: verr.Error()}
		}
	}
	if err != nil {
		switch {
		case errors.Is(err, runtime.ErrNotInstalled):
			return supervisor.PreflightReport{Problem: supervisor.ProblemRuntimeMissing, Detail: err.Error()}
		default:
			return supervisor.PreflightReport{Problem: supervisor.ProblemRuntimeNotRunning, Detail: err.Error()}
		}
	}
	var rep supervisor.PreflightReport
	for _, id := range s.ids {
		d := s.defs[id]
		ok, err := s.opts.Runtime.ImageExists(ctx, d.Image)
		if err != nil {
			return supervisor.PreflightReport{
				Problem: supervisor.ProblemRuntimeNotRunning,
				Detail:  fmt.Sprintf("checking image %s: %v", d.Image, err),
			}
		}
		if !ok {
			rep.MissingImages = append(rep.MissingImages, d.Image)
		}
	}
	if len(rep.MissingImages) > 0 {
		rep.Problem = supervisor.ProblemImagesMissing
		rep.Detail = "missing images: " + strings.Join(rep.MissingImages, ", ")
		return rep
	}
	for _, id := range s.ids {
		for _, p := range s.defs[id].Ports {
			if p.Proto != "udp" {
				continue
			}
			addr := net.JoinHostPort(s.opts.StackContext.PublishAddr, strconv.Itoa(p.Port))
			// Briefly bind the port to learn whether something else holds it.
			pc, err := net.ListenPacket("udp", addr)
			if err != nil {
				rep.BusyPorts = append(rep.BusyPorts, fmt.Sprintf("%d/udp", p.Port))
				continue
			}
			_ = pc.Close()
		}
	}
	if len(rep.BusyPorts) > 0 {
		rep.Problem = supervisor.ProblemPortBusy
		rep.Detail = "ports in use: " + strings.Join(rep.BusyPorts, ", ")
	}
	return rep
}

// --- launch -----------------------------------------------------------

// startLaunch registers the run synchronously (so a later Teardown can find
// it) and executes the launch in a goroutine.
func (s *Session) startLaunch(a supervisor.Action) {
	genCtx, cancel := context.WithCancel(s.root)
	monCtx, stopMon := context.WithCancel(genCtx)
	r := &run{gen: a.Gen, launched: make(chan struct{}), cancel: cancel, stopMon: stopMon}
	s.mu.Lock()
	s.runs[a.Component] = r
	s.mu.Unlock()
	d := s.defs[a.Component]
	s.goRun(func() {
		defer close(r.launched)
		s.doLaunch(d, a.Gen, r, genCtx, monCtx)
	})
}

func (s *Session) doLaunch(d component.Definition, gen uint64, r *run, genCtx, monCtx context.Context) {
	rt, sc := s.opts.Runtime, s.opts.StackContext
	name := d.ContainerName()
	fail := func(permanent bool, err error) {
		s.log.Warn("launch failed", "component", d.ID, "err", err)
		s.post(supervisor.Event{
			Kind: supervisor.LaunchFailed, Component: d.ID, Gen: gen, Permanent: permanent, Detail: err.Error(),
		})
	}
	ctx, cancel := context.WithTimeout(s.root, opTimeout)
	defer cancel()

	// A container left over from an earlier attempt would block the name.
	_ = rt.Remove(ctx, name)
	if sc.NetworkMode != component.NetworkHost {
		if err := rt.EnsureNetwork(ctx, sc.NetworkName, map[string]string{component.LabelManaged: "true"}); err != nil {
			fail(false, fmt.Errorf("create network %s: %w", sc.NetworkName, err))
			return
		}
	}
	if _, err := rt.Run(ctx, d.RunSpec(sc, s.opts.SessionID)); err != nil {
		// A missing image cannot be fixed by retrying.
		ok, ierr := rt.ImageExists(ctx, d.Image)
		fail(ierr == nil && !ok, fmt.Errorf("start %s: %w", name, err))
		return
	}
	s.post(supervisor.Event{Kind: supervisor.Launched, Component: d.ID, Gen: gen})
	s.goRun(func() { s.watchExit(genCtx, d, gen) })
	s.goRun(func() { s.watchHealth(monCtx, d, gen, r) })
}

// watchExit posts Exited when the container ends. It falls back to polling
// Inspect if Wait itself fails, so an exit is never missed.
func (s *Session) watchExit(ctx context.Context, d component.Definition, gen uint64) {
	name := d.ContainerName()
	code, err := s.opts.Runtime.Wait(ctx, name)
	for err != nil {
		if ctx.Err() != nil {
			return
		}
		select {
		case <-ctx.Done():
			return
		case <-time.After(inspectPoll):
		}
		st, ierr := s.opts.Runtime.Inspect(ctx, name)
		switch {
		case errors.Is(ierr, runtime.ErrNotFound):
			return // removed by teardown
		case ierr == nil && !st.Running:
			code, err = st.ExitCode, nil
		}
	}
	s.post(supervisor.Event{Kind: supervisor.Exited, Component: d.ID, Gen: gen, ExitCode: code})
}

// watchHealth runs a fresh Monitor for this launch.
func (s *Session) watchHealth(ctx context.Context, d component.Definition, gen uint64, r *run) {
	probe := d.Probe(s.opts.StackContext)
	if probe == nil {
		return
	}
	unhealthy := func(detail string) {
		s.post(supervisor.Event{Kind: supervisor.Unhealthy, Component: d.ID, Gen: gen, Detail: detail})
	}
	m := health.NewMonitor(probe, health.MonitorConfig{
		LivenessTimeout: d.LivenessTimeout,
		OnReady:         func() { s.post(supervisor.Event{Kind: supervisor.Ready, Component: d.ID, Gen: gen}) },
		OnUnhealthy: func() {
			unhealthy(fmt.Sprintf("%s stopped answering for %s", d.ID, d.LivenessTimeout))
		},
		Now: s.opts.Clock,
	})
	r.mu.Lock()
	r.monitor = m
	r.mu.Unlock()
	if err := m.Run(ctx); err != nil && ctx.Err() == nil {
		s.log.Warn("health probe failed", "component", d.ID, "err", err)
		unhealthy(fmt.Sprintf("health probe failed: %v", err))
	}
}

// --- teardown ---------------------------------------------------------

func (s *Session) doTeardown(a supervisor.Action, r *run) {
	d := s.defs[a.Component]
	name := d.ContainerName()
	if r != nil {
		// A launch of this generation may still be creating the container;
		// let it finish so the container is not leaked.
		<-r.launched
		r.stopMon()
	}
	ctx, cancel := context.WithTimeout(s.root, opTimeout+d.StopTimeout)
	defer cancel()
	if a.CaptureLogs {
		if out, err := s.opts.Runtime.Logs(ctx, name, logTail); err == nil {
			s.mu.Lock()
			s.lastLogs[d.ID] = splitLines(out)
			s.mu.Unlock()
		}
	}
	if err := s.opts.Runtime.Stop(ctx, name, d.StopTimeout); err != nil && !errors.Is(err, runtime.ErrNotFound) {
		s.log.Warn("stop failed", "component", d.ID, "err", err)
	}
	if err := s.opts.Runtime.Remove(ctx, name); err != nil {
		s.log.Warn("remove failed", "component", d.ID, "err", err)
	}
	if r != nil {
		r.cancel()
	}
	s.post(supervisor.Event{Kind: supervisor.Removed, Component: a.Component, Gen: a.Gen})
}

func splitLines(s string) []string {
	s = strings.TrimRight(s, "\r\n")
	if s == "" {
		return nil
	}
	lines := strings.Split(s, "\n")
	for i := range lines {
		lines[i] = strings.TrimRight(lines[i], "\r")
	}
	return lines
}
