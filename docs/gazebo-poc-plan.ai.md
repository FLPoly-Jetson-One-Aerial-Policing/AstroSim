# Gazebo-as-Engine Vetting PoC: 3-Day Plan (revised: C++ gateway in container, raylib viewer on host)

## Context

The PoC's purpose is to decide whether headless Gazebo is a viable engine for the Jetson One simulator described in `docs/Jetson One simulator feasibility.md`. It is not to model the Jetson. We use one of Gazebo's stock multicopter frames (the gz-sim `X3 UAV` quadcopter, which ships with `MulticopterMotorModel` and `MulticopterVelocityControl`) and test the integration path from the feasibility doc: headless `gz sim -s` -> C++ gz-transport gateway -> **stable network schema** -> client with keyboard input.

The split across a network boundary is a core part of what is being proven, not a workaround: the engine (Gazebo + gateway) lives in a container now and on dedicated compute later, and any client (here a native raylib viewer on the Fedora host, later VR or a real-airframe input rig) talks to it only through the schema. The viewer therefore links **no Gazebo libraries and needs no GL passthrough into the container**.

A host-side **orchestrator** is also part of what is being proven: one command brings up the engine container and the display (viewer), supervises both, recovers from crashes of either, and tears everything down cleanly. This is the seed of the eventual "start a sim session" service, so it talks to the engine only through the container runtime and the wire schema.

Stack: C++ with gz-transport (gateway side), plain UDP + a versioned binary schema (wire), raylib (host viewer), a small **Go** host orchestrator driving `podman` and the viewer process (Go chosen over C++ for `os/exec` process groups, `context`/signal handling, goroutine-based supervision, a single static binary, and built-in test tooling; Go 1.26 is already on the host).

## What "vetted" means (go/no-go criteria, recorded in the results doc)

| Criterion | Pass threshold |
| --- | --- |
| Installs and runs headless on this machine | Reproducible from a documented command; < 1 h of effort |
| Real-time factor, headless, at 1 ms step | RTF >= 0.95 sustained for 60 s |
| Determinism | Same inputs twice -> final pose within 1 cm |
| C++ access via gz-transport | Subscribe to pose, publish commands, without ROS |
| Control-to-state latency, **host client -> container gateway -> gz -> back to host** | < 50 ms (p95) on localhost, measured on the host side |
| Container network overhead | Host<->container UDP echo RTT recorded (rootless port-forward vs `--network=host`) |
| Per-motor command injection (motor-out path) | Documented working mechanism, or documented blocker |
| End-to-end flight, host raylib viewer <-> containerized engine, by gamepad (primary) and keyboard (fallback) | Works, 60 Hz state stream and control |
| Gamepad input path under WSL2 | A single gamepad is seen by raylib/GLFW on the host and drives control; if WSL2 cannot expose it, record the blocker and the workaround (usbipd-win passthrough, or Windows-side viewer) |
| Orchestrated bring-up | `astrosim up` on a warm image: container started, gateway ready (`gz_connected`), viewer window open, in < 30 s |
| Orchestrated recovery | `kill -9` viewer -> restarted in < 2 s; `podman kill` container -> restarted and viewer reconnects with no manual action in < 30 s; crash loops back off and give up with a clear error |
| Orchestrated teardown | Ctrl-C, SIGTERM, or closing the viewer window leaves no engine container and no viewer process (`podman ps`, `pgrep`) |
| Display preconditions | Orchestrator detects missing `DISPLAY`/`WAYLAND_DISPLAY` and fails fast, or runs `--no-display` (engine only) |
| Schema robustness | Malformed/stale/duplicate packets and client drop/restart do not disturb the sim or other clients |
| Version/API stability | Note gz release used (target Harmonic or newer) and breaking-change risk |

## Approach

**Install (Task 1):** containerized Gazebo is fine for the PoC (confirmed with user); the end target is dedicated compute, beyond MVP. Fedora 44 has no gz packages, so use `podman` + Ubuntu 24.04 image with the OSRF `gz-harmonic` apt repo (check current release names first; use the newest LTS-aligned one that installs), plus gz-transport/gz-msgs dev packages and CMake. gz-sim, the gateway and gz-transport discovery all stay inside the container; only one UDP port (8765) is published to the host, so multicast never crosses the boundary. Fallback if the container route fails after 2 h: a pixi/conda-forge install (verify `libgz-sim` availability first). Record whichever works.

**Architecture:**

```
 container                                                   host (Fedora)
 gz sim -s <-gz-transport-> GzLink <- IVehicleLink -> Gateway  <== UDP :8765 ==>  UdpLink (IVehicleLink) -> raylib viewer
                                                                                                         -> latency tool
```

- `IVehicleLink` (abstract, in `proto/`): `StateMsg latest()`, `void send(const ControlMsg&)`, `bool connected()`. Implementations: `GzLink` (container only, talks gz-transport), `UdpLink` (host, talks the wire protocol), `FakeLink` (tests). The viewer only knows `IVehicleLink`.
- **Wire protocol** (`proto/wire.{hpp,cpp}`, one UDP datagram per message, little-endian fixed layout, no external deps): header `{magic u16, version u8, type u8, seq u32}` then payload. Types: `Control{roll,pitch,yaw,throttle f32}`, `State{sim_time f64, pos f32[3], quat f32[4], vel f32[3], ang_vel f32[3], flags u8 (failed, gz_connected), echo_seq u32}`, `Fault{motor_index u8}`. `echo_seq` = seq of the last Control applied, giving the host a precise command->state latency measurement without clock sync.
- **Gateway** (`gateway/gateway.cpp`, container): binds UDP 8765, learns client address(es) from valid Control packets, streams State at 60 Hz to each active client, drops clients after 5 s silence. Sanitizes Control (clamp [-1,1], NaN -> 0), ignores bad magic/version/size and stale (older `seq`) packets, applies the 0.5 s deadman (zero Twist when no Control arrives), reports `gz_connected=false` if no pose arrived for 0.5 s.
- **Control mapping** (`gateway/mixer.cpp`, pure function, unit tested): pitch/roll -> body x/y velocity, yaw -> yaw rate, throttle -> z velocity, published as `gz::msgs::Twist` to the velocity controller's command topic.
- **UdpLink** (host): receiver thread updates a mutex-guarded latest-state; `connected()` is false if no State for 0.5 s (gateway/container down or `gz_connected` flag clear). Sends Control at 60 Hz from the viewer loop.
- **Orchestrator** (`orchestrator/`, Go module, host, binary `astrosim`, subcommands `up [--no-display] [--host-net]`, `down`, `status`): 
  1. Preflight: `podman` present, image exists (build if missing), UDP port free, display env present unless `--no-display`.
  2. Start the container (`podman run --rm --name astrosim-engine -p 8765:8765/udp ...`) via fork/exec of the `podman` CLI (the REST socket is a later option).
  3. Readiness: probe the gateway with neutral Control packets until a State with `gz_connected` arrives (30 s timeout, container log tail printed on failure).
  4. Launch the viewer as a child with `--host/--port`; pass through the display environment.
  5. Supervise with a small state machine driven by child exit/`podman` state/State-stream timeout: viewer crash -> restart with backoff (max 3 in 30 s, then give up); viewer exit code 0 (window closed) -> shut everything down; container death -> restart container, viewer keeps running and reconnects on its own.
  6. Teardown on SIGINT/SIGTERM/viewer exit: stop viewer, `podman stop -t 2`, verify nothing left. Container runs with `--rm`; the gateway also exits after a long idle period (no client for 10 min) so a SIGKILLed orchestrator cannot leak a running engine forever.
  The supervisor logic is a pure state machine over events (`ViewerExited(code)`, `ContainerExited`, `GatewayReady`, `Signal`) so it is unit tested with fake child commands, separately from the real podman integration.
  Wire-protocol duplication: the orchestrator's readiness probe needs only to encode a neutral `Control` and decode `State` (`orchestrator/wire/`, ~60 lines with `encoding/binary`). To keep the two implementations from drifting, the C++ wire tests write golden byte vectors to `proto/testdata/*.bin` (checked in) and both the C++ and Go tests decode/encode against the same files. The probe is deliberately the only Go code that speaks the protocol; this also doubles as an early check that the schema is implementable from a second language.
- Two CMake builds from one tree: `-DASTROSIM_BUILD_GATEWAY=ON` (container: needs gz-transport13/gz-msgs10) and the default host build (proto + viewer + tools; needs only raylib and POSIX sockets). On the host use the distro raylib package if present, else CMake `FetchContent`.

**Layout** (new repo, `git init`):
```
Containerfile                 # Ubuntu 24.04 + gz-harmonic dev pkgs + cmake; builds gateway
CMakeLists.txt                # ASTROSIM_BUILD_GATEWAY option; host build needs only raylib
worlds/poc.sdf                # stock X3 UAV, ground plane, 1 ms step, pose publisher
proto/messages.hpp            # ControlMsg / StateMsg / FaultMsg + sanitize helpers (the stable seam)
proto/wire.{hpp,cpp}          # encode/decode, versioning, validation
proto/vehicle_link.hpp        # IVehicleLink
proto/udp_link.{hpp,cpp}      # host-side client link
proto/fake_link.hpp           # test double
gateway/gz_link.{hpp,cpp}     # gz-transport node: pose/vel/stats in, Twist out
gateway/mixer.{hpp,cpp}       # ControlMsg -> Twist
gateway/gateway.cpp           # UDP server, 60 Hz stream, deadman, client table
app/viewer.cpp                # raylib: 3D scene, chase camera, HUD, keyboard
proto/testdata/*.bin          # golden wire vectors shared by C++ and Go tests
orchestrator/go.mod
orchestrator/cmd/astrosim/main.go   # `astrosim up|down|status`, preflight, signal handling
orchestrator/supervisor/            # pure event-driven state machine (restart/backoff/teardown policy) + tests
orchestrator/proc/                  # os/exec wrappers: process groups, wait, graceful kill
orchestrator/podman/                # CLI wrapper: build/run/stop/inspect/logs
orchestrator/wire/                  # minimal Control encode / State decode for the readiness probe + golden tests
tools/bench_rtf.cpp           # (container) RTF + determinism
tools/latency.cpp             # (host) full-path latency via echo_seq
tools/udp_echo.cpp            # (host + container) RTT probe for container network overhead
tools/motor_spike.cpp         # (container) per-motor command experiment
tests/                        # doctest/Catch2: wire, mixer, gateway-over-loopback; gz smoke (ctest label "gz")
docs/gazebo-vetting-results.md
```
Verify package names (`libgz-transport13-dev`, `libgz-msgs10-dev` for Harmonic; newer releases use transport14/msgs11) against the installed release before writing CMake, and record the version.

## Tasks

### Day 1: Get Gazebo running, reach it from the host, measure
1. **Container + headless run.** Build the image, run `gz sim -s -r worlds/poc.sdf`, confirm the X3 loads and `gz topic -l` shows its pose/command topics. Record the gz version and exact install steps. *Verify:* `gz topic -e -t <pose topic> -n 1` prints a pose.
2. **Network path check (do early).** `tools/udp_echo` in the container with `-p 8765:8765/udp`, probe from the host. Compare rootless port-forward against `--network=host`. *Verify:* RTT p50/p95 recorded; pick the default run mode. (WSL2 networking is the main new risk.)
3. **CMake skeleton + benchmark harness.** Minimal `GzLink` subscribing to pose and `/stats`. `tools/bench_rtf` runs the world 60 s, reports RTF, and runs the same scripted command sequence twice for the determinism check. *Verify:* RTF and final-pose delta printed and recorded.

### Day 2: Gateway, schema, control
4. **Wire protocol + tests.** `wire` encode/decode with version check and size validation; unit tests for round trip, truncated/oversized/bad-magic/wrong-version packets, and sanitization (clamp, NaN). *Verify:* host `ctest` green with no gz installed.
5. **Gateway + GzLink + mixer.** Implement client table, 60 Hz State stream, stale-seq rejection, deadman, `gz_connected`, mixer. Gateway tests over loopback with a fake gz side. *Verify:* `ctest` green; scripted client commands a 2 m climb and hovers within 0.2 m against real gz.
6. **Motor-out spike (time-boxed 2 h).** The velocity controller owns the motor commands, so find how to zero one motor in `tools/motor_spike.cpp`: options are the motor model's per-motor `command/motor_speed` topic with the velocity controller disabled, or a world-level failure by publishing a zero actuator message. Note a quad cannot survive a motor-out, so success = "the command reaches that motor and the vehicle visibly responds," not stable flight. Record how a `Fault` message would map onto it. The real Jetson (8 rotors) tolerance is a later question. *Verify:* documented mechanism or blocker in the results doc.

### Day 3: Viewer, latency, verdict
7. **raylib viewer (host).** `app/viewer.cpp` over `UdpLink`: `Camera3D` chase/orbit camera, ground grid, quad drawn from primitives oriented by `StateMsg.quat` (convert gz ENU/Z-up to raylib Y-up), HUD (altitude, speed, tilt, connection, gateway/gz status). Input: single gamepad as dual joystick, Mode 2 layout: left stick Y = throttle (up = climb), left stick X = yaw, right stick Y = pitch (up = forward), right stick X = roll; raylib `GetGamepadAxisMovement` (raylib reports stick-up as negative, so invert Y). Radial deadzone ~0.10 with rescale so output still reaches +/-1; no expo/rate curves yet. The stick->`ControlMsg` mapping is a pure function in `proto/input_map.hpp` with host unit tests (deadzone, inversion, clamping, NaN). Keyboard W/S/A/D/Q/E/arrows is the fallback when no gamepad is present; gamepad hot-plug and unplug are handled each frame (unplug -> zero stick, HUD shows INPUT: none); first gamepad found is used. Zero stick when the window loses focus. *Verify:* manual checklist: lift off, translate, yaw, release (centered sticks) returns to hover; unplug gamepad mid-flight -> hover; replug -> control resumes; kill viewer -> hover within 0.5 s; kill gz -> HUD shows DISCONNECTED without hanging; restart container -> viewer reconnects automatically; second viewer instance does not disturb the first.
8. **Orchestrator (time-box 3 h).** Implement `supervisor` + `proc` + `podman` + `wire` + `main` in Go (`go test ./...`). Unit test the state machine with fake children (`sleep`, `false`, `true`): restart/backoff/give-up, window-close shutdown, signal during startup, container death while viewer alive. Then integrate with real podman and the viewer. Put children in their own process group so teardown kills the whole tree. *Verify:* automated supervisor tests green; manual checklist: `astrosim up` shows the window; `kill -9` viewer -> back in < 2 s; `podman kill astrosim-engine` -> engine restarts, viewer shows DISCONNECTED then recovers; Ctrl-C, SIGTERM and window close each leave `podman ps` empty and `pgrep viewer` empty; `up --no-display` with no `DISPLAY` works; `up` without display env fails fast with a clear message; second `up` while one is running refuses cleanly (PID/lock file or container-name check).
9. **Latency + results doc.** `tools/latency` (host) sends Control with incrementing `seq`, times until a State arrives with `echo_seq >= seq` and a pose change, reports p50/p95 (steady_clock, host side only). Write `docs/gazebo-vetting-results.md`: filled criteria table, install recipe, network findings, surprises, and a go / no-go / go-with-caveats verdict, including what would have to change for the 8-rotor Jetson (custom SDF with eight `MulticopterMotorModel` instances, own mixer/controller since the stock velocity controller is quad-oriented) and what the schema needs for a real dedicated-compute deployment (auth, reliability for Fault, clock sync).

## Review Focus (inputs the spec implies but doesn't test)
- Gazebo process dies or pose data stops: `gz_connected` clears, viewer shows DISCONNECTED, nothing hangs.
- Gateway/container dies: viewer detects state timeout within 0.5 s and recovers when it returns.
- Control stream stops: deadman returns to neutral hover within 0.5 s.
- NaN / out-of-range control fields: sanitized, never forwarded to Gazebo.
- Malformed, truncated, wrong-version, duplicate or reordered datagrams: dropped; other clients and sim unaffected.
- Orchestrator killed with SIGKILL: container does not run forever (`--rm` + gateway idle exit); a later `astrosim up` detects and replaces a stale `astrosim-engine` container.
- Engine slow to start or never becomes ready: bounded wait, container log tail in the error, teardown after failure.
- Viewer or container crash loop: backoff and give-up, not a tight restart spin.
- Client drop and restart (new source port): old entry expires, new one is learned.
- Throttle at zero on the ground: vehicle stays down, no negative altitude.

## Out of scope
Terrain/building meshes and lateral collisions (flat ground plane only; the results doc should note that mesh/heightmap collision performance, collision-proxy tooling and a scene-description message in the schema are open questions for the real product), Jetson geometry/mass/inertia, eight-rotor model, coaxial effects, wind, battery, VR, real-airframe input (gamepad is a stand-in; no stick calibration UI, expo curves or button mapping), calibration, multi-host or Kubernetes-style orchestration, multiple concurrent sessions, authentication/encryption of the wire protocol, reliable delivery (UDP fire-and-forget is intentional; note the Fault-message implication in the results).

## Verification (end to end)
1. Host C++ build (`cmake` + `make`, raylib only; raylib is not installed yet, so `dnf install raylib-devel` if packaged, else `FetchContent`) produces the viewer and tools; `go build ./cmd/astrosim` in `orchestrator/` produces `astrosim`; `podman build` produces the engine image.
2. `ctest` passes on the host (wire, link) and in the container (gateway, gz smoke); `go test ./...` passes in `orchestrator/` (supervisor, golden wire vectors).
3. `astrosim up` alone brings up Gazebo, the gateway and the viewer window; `astrosim down` (or Ctrl-C) leaves nothing running.
4. `tools/bench_rtf`, `tools/udp_echo` and `tools/latency` produce numbers that fill the criteria table.
5. Manual viewer checklist (Task 7) and orchestrator checklist (Task 8) pass.
6. `docs/gazebo-vetting-results.md` contains the verdict, including an orchestrator section (what worked with the podman CLI, what a real session service would need).
