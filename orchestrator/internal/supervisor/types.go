package supervisor

import "time"

// Phase is the overall state of the stack.
type Phase string

// Phases of the supervisor.
const (
	// PhaseChecking means a preflight check is running.
	PhaseChecking Phase = "checking"
	// PhaseUnavailable means preflight found a problem; the stack cannot
	// start until it is fixed. The check is repeated automatically.
	PhaseUnavailable Phase = "unavailable"
	// PhaseStopped means nothing is running and Start is allowed.
	PhaseStopped Phase = "stopped"
	// PhaseStarting means components are being launched and are not all ready.
	PhaseStarting Phase = "starting"
	// PhaseRunning means every component is ready.
	PhaseRunning Phase = "running"
	// PhaseRecovering means a failure is being repaired: the affected
	// components are torn down and will be relaunched after a backoff.
	PhaseRecovering Phase = "recovering"
	// PhaseStopping means components are being torn down, because of a
	// Stop or shutdown request or before giving up.
	PhaseStopping Phase = "stopping"
	// PhaseFailed means the stack gave up. Start is allowed again.
	PhaseFailed Phase = "failed"
)

// ComponentState is the state of one component.
type ComponentState string

// Component states.
const (
	// ComponentPending means the component has not been launched yet.
	ComponentPending ComponentState = "pending"
	// ComponentLaunching means a Launch action was issued and its result is awaited.
	ComponentLaunching ComponentState = "launching"
	// ComponentStarting means the container runs but is not ready yet.
	ComponentStarting ComponentState = "starting"
	// ComponentReady means the readiness probe passed.
	ComponentReady ComponentState = "ready"
	// ComponentStale means a ready component stopped answering or turned unhealthy.
	ComponentStale ComponentState = "stale"
	// ComponentStopping means a Teardown action was issued and its result is awaited.
	ComponentStopping ComponentState = "stopping"
	// ComponentStopped means the container is gone (or never existed after a teardown).
	ComponentStopped ComponentState = "stopped"
	// ComponentExited means the container exited by itself.
	ComponentExited ComponentState = "exited"
	// ComponentFailed means the component could not be launched or never became ready.
	ComponentFailed ComponentState = "failed"
)

// Reason codes explaining the current phase.
const (
	// ReasonComponentExited: a container exited unexpectedly.
	ReasonComponentExited = "component_exited"
	// ReasonUnhealthy: a ready component stopped answering or reported unhealthy.
	ReasonUnhealthy = "component_unhealthy"
	// ReasonReadyTimeout: a component did not become ready in time.
	ReasonReadyTimeout = "ready_timeout"
	// ReasonLaunchFailed: the container could not be created or started.
	ReasonLaunchFailed = "launch_failed"
	// ReasonInvalidConfig: a component exited with a permanent exit code.
	ReasonInvalidConfig = "invalid_config"
	// ReasonRestartLimit: the restart budget of a group is used up.
	ReasonRestartLimit = "restart_limit"
)

// Problem codes a PreflightReport may carry.
const (
	// ProblemRuntimeMissing: no container runtime is installed.
	ProblemRuntimeMissing = "runtime_missing"
	// ProblemRuntimeNotRunning: the runtime is installed but not answering.
	ProblemRuntimeNotRunning = "runtime_not_running"
	// ProblemRuntimeTooOld: the runtime version is unsupported.
	ProblemRuntimeTooOld = "runtime_too_old"
	// ProblemImagesMissing: one or more images are not built.
	ProblemImagesMissing = "images_missing"
	// ProblemPortBusy: a port a component needs is already in use.
	ProblemPortBusy = "port_busy"
)

// RecheckInterval is how often an unavailable stack repeats its preflight.
const RecheckInterval = 5 * time.Second

// RestartPolicy bounds automatic recovery.
type RestartPolicy struct {
	// MaxAttempts is how many consecutive failures of one group are
	// repaired by restarting; the failure after that makes the stack give
	// up. Zero means never restart.
	MaxAttempts int
	// Backoff is the wait before restart attempt n (index n-1); attempts
	// beyond the list use the last entry. Empty means no wait.
	Backoff []time.Duration
	// StableAfter is how long a group must run without a failure for its
	// failure count to reset. Zero or negative means the default of 2 minutes.
	StableAfter time.Duration
}

// DefaultRestartPolicy is three restarts after 2 s, 5 s and 10 s, with the
// failure count forgotten after two minutes of stable running.
var DefaultRestartPolicy = RestartPolicy{
	MaxAttempts: 3,
	Backoff:     []time.Duration{2 * time.Second, 5 * time.Second, 10 * time.Second},
	StableAfter: 2 * time.Minute,
}

// EventKind identifies an Event.
type EventKind int

// Event kinds.
const (
	// StartRequested asks to start the stack. Honoured in stopped and
	// failed; it runs a preflight first.
	StartRequested EventKind = iota + 1
	// StopRequested asks to stop the stack. Honoured in starting, running,
	// recovering and unavailable (and while a requested start is still in
	// its preflight).
	StopRequested
	// ShutdownRequested asks to tear everything down in parallel and exit.
	// Honoured in every phase.
	ShutdownRequested
	// CheckRequested runs a preflight without starting anything, so the UI
	// can show "not installed" before the user presses Start. The session
	// injects it once at startup, because the initial phase is stopped.
	// Honoured in stopped. A clean result returns to stopped, a problem
	// moves to unavailable.
	CheckRequested
	// PreflightDone reports the result of a Preflight action (Gen, Report).
	PreflightDone
	// Launched reports that a Launch action created and started the
	// container (Component, Gen).
	Launched
	// LaunchFailed reports that a Launch action failed (Component, Gen,
	// Permanent, Detail).
	LaunchFailed
	// Ready reports that the component passed its readiness probe (Component, Gen).
	Ready
	// Unhealthy reports that a ready component stopped answering or
	// reported unhealthy (Component, Gen, Detail).
	Unhealthy
	// Exited reports that the container exited (Component, Gen, ExitCode).
	Exited
	// Removed reports that a Teardown action finished and the container is
	// gone (Component, Gen).
	Removed
	// TimerFired reports an expired SetTimer (Timer, Key, Gen).
	TimerFired
)

// TimerKind identifies a timer.
type TimerKind int

// Timer kinds.
const (
	// TimerReadyTimeout limits how long a launched component may take to be
	// ready. Key is the component ID, Gen the component's generation.
	TimerReadyTimeout TimerKind = iota + 1
	// TimerBackoff delays the relaunch after a recovery teardown. Key is the group key.
	TimerBackoff
	// TimerStable marks a group as stable. Key is the group key.
	TimerStable
	// TimerRecheck repeats the preflight while unavailable. Key is empty.
	TimerRecheck
)

// PreflightReport is the result of a preflight check.
type PreflightReport struct {
	// Problem is one of the Problem constants, or empty when all is well.
	Problem string
	// MissingImages lists the images that are not built.
	MissingImages []string
	// BusyPorts lists the ports in use, for example "8765/udp".
	BusyPorts []string
	// Detail is a technical explanation for logs and the failure panel.
	Detail string
}

// Event is an input to Step. Which fields matter depends on Kind; see the
// EventKind constants.
type Event struct {
	// Kind says what happened.
	Kind EventKind
	// Component is the component ID for component events.
	Component string
	// Gen is the generation the event belongs to: the component's launch
	// generation, the preflight generation, or the timer generation.
	Gen uint64
	// Report is the PreflightDone result.
	Report PreflightReport
	// Permanent marks a LaunchFailed that retrying cannot fix.
	Permanent bool
	// ExitCode is the container exit status for Exited.
	ExitCode int
	// Detail is a technical explanation for LaunchFailed and Unhealthy.
	Detail string
	// Timer and Key identify a TimerFired.
	Timer TimerKind
	// Key is the timer key, see TimerKind.
	Key string
}

// ActionKind identifies an Action.
type ActionKind int

// Action kinds.
const (
	// ActionPreflight asks the driver to check the runtime, images and
	// ports, and answer with PreflightDone carrying Gen.
	ActionPreflight ActionKind = iota + 1
	// ActionLaunch asks the driver to create and start Component's
	// container, answering Launched or LaunchFailed with Gen, and to begin
	// watching it (Ready, Unhealthy, Exited).
	ActionLaunch
	// ActionTeardown asks the driver to stop and remove Component's
	// container of generation Gen, answering Removed. If a Launch of that
	// generation is still in flight the driver must finish it first. With
	// CaptureLogs the driver saves the container's log tail beforehand.
	ActionTeardown
	// ActionSetTimer asks the driver to deliver TimerFired with the same
	// Timer, Key and Gen after After. A timer is never cancelled; stale
	// firings are ignored by the supervisor.
	ActionSetTimer
	// ActionExit tells the driver the process may exit with Code.
	ActionExit
)

// Action is an output of Step for the driver to execute.
type Action struct {
	// Kind says what to do.
	Kind ActionKind
	// Component is the target component for Launch and Teardown.
	Component string
	// Gen is the generation to echo back in the resulting event.
	Gen uint64
	// CaptureLogs asks Teardown to save the log tail first.
	CaptureLogs bool
	// Reason says why a Teardown happens: "restart", "stop", "shutdown" or "failed".
	Reason string
	// Timer, Key and After describe a SetTimer.
	Timer TimerKind
	// Key is the timer key.
	Key string
	// After is the timer delay.
	After time.Duration
	// Code is the exit status for ActionExit.
	Code int
}

// Reason explains why the stack is in its current phase.
type Reason struct {
	// Code is a Reason or Problem constant; empty when nothing needs explaining.
	Code string
	// Detail is a technical explanation.
	Detail string
	// Component is the component concerned, if any.
	Component string
}

// ComponentStatus is the snapshot of one component.
type ComponentStatus struct {
	// ID is the component ID.
	ID string
	// State is the component's state.
	State ComponentState
	// Gen is the generation of the current (or last) launch; 0 if never launched.
	Gen uint64
	// LastExitCode is the last observed exit status, nil if none.
	LastExitCode *int
}

// State is a snapshot of the supervisor for the status API.
type State struct {
	// Phase is the overall phase.
	Phase Phase
	// Since is when the phase was entered (the time passed to Step).
	Since time.Time
	// Reason explains the phase after a failure or preflight problem.
	Reason Reason
	// Preflight is the latest preflight report.
	Preflight PreflightReport
	// Attempt is the highest failure count of any group, capped at MaxAttempts.
	Attempt int
	// MaxAttempts is the restart budget.
	MaxAttempts int
	// CanStart reports whether StartRequested would be honoured.
	CanStart bool
	// CanStop reports whether StopRequested would be honoured.
	CanStop bool
	// Components lists every component in launch order.
	Components []ComponentStatus
}
