// Package supervisor is the pure restart/backoff/teardown policy for the engine container and the viewer.
// It owns no processes: it consumes Events and returns Actions for a runtime to execute.
package supervisor

import "time"

type EventKind int

const (
	Start          EventKind = iota // begin the session
	ContainerReady                  // gateway reports the engine alive
	ContainerExited
	ViewerExited // Code = process exit code (0 = window closed normally)
	ReadyTimeout
	Signal
)

type Event struct {
	Kind EventKind
	Code int
}

type ActionKind int

const (
	StartContainer ActionKind = iota
	StartViewer
	Shutdown // stop everything, then exit with Code
)

type Action struct {
	Kind   ActionKind
	Delay  time.Duration
	Code   int
	Reason string
}

type Config struct {
	NoDisplay           bool
	MaxViewerCrashes    int // crashes within ViewerWindow before giving up
	ViewerWindow        time.Duration
	MaxContainerCrashes int
	ContainerWindow     time.Duration
}

func DefaultConfig() Config {
	return Config{MaxViewerCrashes: 3, ViewerWindow: 30 * time.Second, MaxContainerCrashes: 3, ContainerWindow: 60 * time.Second}
}

type phase int

const (
	idle phase = iota
	startingContainer
	running
	restartingContainer
	stopping
)

type Supervisor struct {
	cfg           Config
	ph            phase
	viewerStarted bool
	viewerCrashes []time.Time
	contCrashes   []time.Time
}

func New(cfg Config) *Supervisor { return &Supervisor{cfg: cfg} }

func prune(ts []time.Time, now time.Time, window time.Duration) []time.Time {
	out := ts[:0]
	for _, t := range ts {
		if now.Sub(t) <= window {
			out = append(out, t)
		}
	}
	return out
}

// First crash in the window restarts immediately; later ones back off 500ms, 1s, 2s, ... capped at 4s.
func backoff(n int) time.Duration {
	if n <= 1 {
		return 0
	}
	d := 500 * time.Millisecond << (n - 2)
	if d > 4*time.Second {
		d = 4 * time.Second
	}
	return d
}

func (s *Supervisor) stop(code int, reason string) []Action {
	s.ph = stopping
	return []Action{{Kind: Shutdown, Code: code, Reason: reason}}
}

func (s *Supervisor) Step(ev Event, now time.Time) []Action {
	if s.ph == stopping {
		return nil
	}
	if ev.Kind == Signal {
		return s.stop(0, "signal")
	}
	switch ev.Kind {
	case Start:
		if s.ph != idle {
			return nil
		}
		s.ph = startingContainer
		return []Action{{Kind: StartContainer}}

	case ContainerReady:
		if s.ph != startingContainer && s.ph != restartingContainer {
			return nil
		}
		s.ph = running
		if !s.viewerStarted && !s.cfg.NoDisplay {
			s.viewerStarted = true
			return []Action{{Kind: StartViewer}}
		}

	case ReadyTimeout:
		if s.ph == startingContainer || s.ph == restartingContainer {
			return s.stop(1, "engine did not become ready")
		}

	case ContainerExited:
		s.contCrashes = append(prune(s.contCrashes, now, s.cfg.ContainerWindow), now)
		if len(s.contCrashes) >= s.cfg.MaxContainerCrashes {
			return s.stop(1, "engine container crash loop")
		}
		s.ph = restartingContainer
		return []Action{{Kind: StartContainer, Delay: backoff(len(s.contCrashes))}}

	case ViewerExited:
		if ev.Code == 0 {
			return s.stop(0, "viewer closed")
		}
		s.viewerCrashes = append(prune(s.viewerCrashes, now, s.cfg.ViewerWindow), now)
		if len(s.viewerCrashes) >= s.cfg.MaxViewerCrashes {
			return s.stop(1, "viewer crash loop")
		}
		return []Action{{Kind: StartViewer, Delay: backoff(len(s.viewerCrashes))}}
	}
	return nil
}
