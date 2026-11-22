package main

import (
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/logs"
	"github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/orchestrator/internal/session"
)

// sessionLogs adapts a logs.Recorder to the session's LogCapture interface,
// which cannot name the logs package's types. A nil recorder is harmless:
// the logs package treats it as disabled.
type sessionLogs struct{ r *logs.Recorder }

// Begin implements session.LogCapture.
func (s sessionLogs) Begin(component string, gen uint64, container, containerID, image string) session.Capture {
	return s.r.Begin(logs.Launch{
		Component: component, Container: container, ContainerID: containerID, Image: image, Gen: gen,
	})
}

// Note implements session.LogCapture.
func (s sessionLogs) Note(component, msg string) { s.r.Note(component, msg) }
