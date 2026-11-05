package main

import (
	"context"
	"io"
	"log"
	"os"
	"os/signal"
	"sync"
	"syscall"
	"time"

	"astrosim/orchestrator/podman"
	"astrosim/orchestrator/probe"
	"astrosim/orchestrator/proc"
	"astrosim/orchestrator/supervisor"
)

type options struct {
	Image, Root, ViewerPath, Containerfile string
	Port, TickHz                           int
	HostNet, NoDisplay, Verbose            bool
	ReadyTimeout                           time.Duration
}

const containerName = "astrosim-engine"

type session struct {
	o      options
	sup    *supervisor.Supervisor
	events chan supervisor.Event
	ctx    context.Context
	cancel context.CancelFunc

	mu     sync.Mutex
	cont   *proc.Child
	viewer *proc.Child
}

func newSession(o options) *session {
	cfg := supervisor.DefaultConfig()
	cfg.NoDisplay = o.NoDisplay
	ctx, cancel := context.WithCancel(context.Background())
	return &session{o: o, sup: supervisor.New(cfg), events: make(chan supervisor.Event, 32), ctx: ctx, cancel: cancel}
}

func (s *session) post(ev supervisor.Event) {
	select {
	case s.events <- ev:
	case <-s.ctx.Done():
	}
}

// run drives the supervisor until it asks for shutdown; returns the process exit code.
func (s *session) run() int {
	sig := make(chan os.Signal, 2)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	defer signal.Stop(sig)
	go func() {
		select {
		case <-sig:
			s.post(supervisor.Event{Kind: supervisor.Signal})
		case <-s.ctx.Done():
		}
	}()

	pending := s.sup.Step(supervisor.Event{Kind: supervisor.Start}, time.Now())
	for {
		for _, a := range pending {
			switch a.Kind {
			case supervisor.StartContainer:
				s.startContainer(a.Delay)
			case supervisor.StartViewer:
				s.startViewer(a.Delay)
			case supervisor.Shutdown:
				log.Printf("shutting down: %s", a.Reason)
				s.teardown()
				return a.Code
			}
		}
		ev := <-s.events
		pending = s.sup.Step(ev, time.Now())
	}
}

func (s *session) sleep(d time.Duration) bool {
	if d <= 0 {
		return s.ctx.Err() == nil
	}
	select {
	case <-time.After(d):
		return true
	case <-s.ctx.Done():
		return false
	}
}

func (s *session) startContainer(delay time.Duration) {
	go func() {
		if !s.sleep(delay) {
			return
		}
		_ = podman.RemoveForce(containerName) // stale container or name clash from a prior crash
		tail := proc.NewTail(40)
		var out io.Writer = tail
		if s.o.Verbose {
			out = io.MultiWriter(tail, os.Stderr)
		}
		args := podman.RunArgs(podman.RunOpts{Name: containerName, Image: s.o.Image, Port: s.o.Port, HostNet: s.o.HostNet, TickHz: s.o.TickHz})
		log.Printf("starting engine container (%s)", s.o.Image)
		c, err := proc.Start("podman", args, proc.Options{Stdout: out, Stderr: out})
		if err != nil {
			log.Printf("podman start failed: %v", err)
			s.post(supervisor.Event{Kind: supervisor.ContainerExited, Code: -1})
			return
		}
		s.mu.Lock()
		s.cont = c
		s.mu.Unlock()

		cctx, ccancel := context.WithCancel(s.ctx)
		go func() { // container death
			<-c.Done()
			ccancel()
			if s.ctx.Err() == nil {
				log.Printf("engine container exited (code %d); last output:\n%s", c.Code(), tail.String())
				s.post(supervisor.Event{Kind: supervisor.ContainerExited, Code: c.Code()})
			}
		}()
		go func() { // readiness
			rctx, rcancel := context.WithTimeout(cctx, s.o.ReadyTimeout)
			defer rcancel()
			_, err := probe.WaitReady(rctx, "127.0.0.1", s.o.Port, 200*time.Millisecond)
			switch {
			case err == nil:
				log.Printf("engine ready")
				s.post(supervisor.Event{Kind: supervisor.ContainerReady})
			case cctx.Err() == nil: // not cancelled by container exit or shutdown => timeout
				log.Printf("engine not ready after %v; last output:\n%s", s.o.ReadyTimeout, tail.String())
				s.post(supervisor.Event{Kind: supervisor.ReadyTimeout})
			}
		}()
	}()
}

func (s *session) startViewer(delay time.Duration) {
	go func() {
		if !s.sleep(delay) {
			return
		}
		tail := proc.NewTail(20)
		var out io.Writer = tail
		if s.o.Verbose {
			out = io.MultiWriter(tail, os.Stderr)
		}
		log.Printf("starting viewer")
		v, err := proc.Start(s.o.ViewerPath, []string{"127.0.0.1", itoa(s.o.Port)}, proc.Options{Stdout: out, Stderr: out})
		if err != nil {
			log.Printf("viewer start failed: %v", err)
			s.post(supervisor.Event{Kind: supervisor.ViewerExited, Code: -1})
			return
		}
		s.mu.Lock()
		s.viewer = v
		s.mu.Unlock()
		<-v.Done()
		if s.ctx.Err() == nil {
			if v.Code() != 0 {
				log.Printf("viewer exited (code %d); last output:\n%s", v.Code(), tail.String())
			}
			s.post(supervisor.Event{Kind: supervisor.ViewerExited, Code: v.Code()})
		}
	}()
}

func (s *session) teardown() {
	s.cancel()
	s.mu.Lock()
	v, c := s.viewer, s.cont
	s.mu.Unlock()
	if v != nil {
		v.Stop(2 * time.Second)
	}
	if podman.Exists(containerName) {
		_ = podman.Stop(containerName, 2)
	}
	if c != nil {
		c.Stop(3 * time.Second)
	}
	if podman.Exists(containerName) {
		_ = podman.RemoveForce(containerName)
	}
}
