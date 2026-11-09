# Gazebo-as-Engine Vetting PoC: 3-Day Plan (revised)

## Context

The PoC's purpose is to decide whether headless Gazebo is a viable engine for the Jetson One simulator described in `docs/Jetson One simulator feasibility.md`. It is not to model the Jetson. We use one of Gazebo's stock multicopter frames (the gz-sim `X3 UAV` quadcopter, which ships with `MulticopterMotorModel` and `MulticopterVelocityControl`) and test the integration path from the feasibility doc: headless `gz sim -s` -> Python bridge -> stable gateway schema -> browser frontend with keyboard input.

This supersedes the earlier custom-Python-physics plan (`docs/superpowers/plans/2026-11-04-jetson-poc.md`), which should be deleted or replaced on execution.

Findings so far: `gz`/`ign` are not installed; `dnf search` finds no Gazebo packages in Fedora 44 repos; `podman`, `docker`, `flatpak`, `python3` (3.14), `node` are present. Plugin/topic names below come from memory of gz-sim and must be confirmed against the installed version in Task 1.

## What "vetted" means (go/no-go criteria, recorded in the results doc)

| Criterion | Pass threshold |
| --- | --- |
| Installs and runs headless on this machine | Reproducible from a documented command; < 1 h of effort |
| Real-time factor, headless, at 1 ms step | RTF >= 0.95 sustained for 60 s |
| Determinism | Same inputs twice -> final pose within 1 cm |
| Python access via gz-transport | Subscribe to pose, publish commands, without ROS |
| Control-to-state latency (command -> first pose change at the bridge) | < 50 ms on localhost |
| Per-motor command injection (motor-out path) | Documented working mechanism, or documented blocker |
| End-to-end keyboard flight through gateway + browser | Works, 60 Hz state stream |
| Version/API stability | Note gz release used (target Harmonic or newer) and breaking-change risk |

## Approach

**Install (Task 1):** confirmed with user: containerized Gazebo is fine for the PoC; the end target is dedicated compute, which is beyond MVP (so don't optimize the container setup, but keep the gateway schema host-agnostic so the engine can move to a dedicated machine). Run Gazebo in a container, because Fedora 44 has no packages. Recommended: `podman` + Ubuntu 24.04 image with the OSRF `gz-harmonic` apt repo (check current release names first; use the newest LTS-aligned one that installs). Run gz, the bridge and the gateway all inside one container with port 8765 published, so gz-transport multicast discovery never crosses the host boundary. Fallback if the container route fails after 2 h: a pixi/conda-forge install (verify `libgz-sim` availability first). Record whichever works.

**Layout** (new repo, `git init`):
```
Containerfile                 # gz + python bindings + websockets
worlds/poc.sdf                # stock X3 UAV, ground plane, 1 ms step, pose publisher
astrosim/bridge.py            # gz-transport <-> python: pose/vel in, twist out
astrosim/messages.py          # ControlMsg / StateMsg / FaultMsg (schema = the stable seam)
astrosim/gateway.py           # websocket: 60 Hz state broadcast, control in, deadman 0.5 s
frontend/index.html           # canvas side/top view, HUD, keyboard
scripts/bench_rtf.py          # RTF + determinism
scripts/latency.py            # command -> state latency
tests/                        # pytest for messages + gateway (bridge faked); smoke tests for gz
docs/gazebo-vetting-results.md
```

## Tasks

### Day 1: Get Gazebo running and measured
1. **Container + headless run.** Build the image, run `gz sim -s -r worlds/poc.sdf`, confirm the X3 loads and `gz topic -l` shows its pose/command topics. Record the gz version and exact install steps in the results doc. *Verify:* `gz topic -e -t <pose topic> -n 1` prints a pose.
2. **Benchmark harness.** `scripts/bench_rtf.py` runs the world 60 s, reads RTF from the `/stats` topic, and runs the same scripted command sequence twice for the determinism check. *Verify:* prints RTF and pose delta; both recorded.

### Day 2: Python bridge and control
3. **Bridge.** `bridge.py` subscribes to the X3 pose/velocity via gz-transport Python bindings and publishes `Twist` to the velocity-control command topic after enabling it. Test first that the bindings import (`python -c "import gz.transport13"`; confirm module names for the installed version). If the bindings are unavailable, record that as a finding and fall back to a `gz topic` subprocess bridge, noting the cost. *Verify:* script commands a 2 m climb and hovers within 0.2 m.
4. **Messages + gateway.** Implement the schema (control: roll/pitch/yaw/throttle clamped, NaN-safe; state: t, pos, quat, vel, ang_vel, failed). Gateway maps control -> body-frame twist (pitch/roll -> x/y velocity, yaw -> yaw rate, throttle -> z velocity), broadcasts state at 60 Hz, applies 0.5 s deadman, ignores malformed JSON, survives client disconnects. pytest with a fake bridge. *Verify:* `pytest` green.
5. **Motor-out spike (time-boxed 2 h).** The velocity controller owns the motor commands, so find how to zero one motor: options are the motor model's per-motor `command/motor_speed` topic with the velocity controller disabled, or a world-level failure by publishing a zero actuator message. Note a quad cannot survive a motor-out, so success = "the command reaches that motor and the vehicle visibly responds," not stable flight. The real Jetson (8 rotors) tolerance is a later question. *Verify:* documented mechanism or blocker in the results doc.

### Day 3: Frontend, latency, verdict
6. **Frontend.** Single-file canvas page: side + top view, HUD (altitude, speed, tilt, connection), keyboard W/S/A/D/Q/E/arrows, zero stick on blur, reconnect every 1 s. *Verify:* manual checklist: lift off, translate, yaw, release returns to hover, kill gateway tab -> hover within 0.5 s, gateway restart -> auto reconnect.
7. **Latency + results doc.** `scripts/latency.py` measures command -> first state change through the full gateway. Write `docs/gazebo-vetting-results.md`: filled criteria table, install recipe, surprises, and a go / no-go / go-with-caveats verdict, including what would have to change for the 8-rotor Jetson (custom SDF with eight `MulticopterMotorModel` instances, own mixer/controller since the stock velocity controller is quad-oriented).

## Review Focus (inputs the spec implies but doesn't test)
- Gazebo process dies or the bridge loses data: gateway reports disconnected state, doesn't hang the frontend.
- Control stream stops: deadman returns to neutral hover within 0.5 s.
- NaN / out-of-range control fields: clamped, never forwarded to Gazebo.
- Malformed JSON or abrupt client disconnect: other clients and sim unaffected.
- Throttle at zero on the ground: vehicle stays down, no negative altitude.

## Out of scope
Jetson geometry/mass/inertia, eight-rotor model, coaxial effects, wind, battery, VR, real-airframe input, calibration.

## Verification (end to end)
1. `podman build` + `podman run -p 8765:8765 ...` brings up Gazebo, bridge and gateway with one command.
2. `pytest` passes.
3. `scripts/bench_rtf.py` and `scripts/latency.py` produce numbers that fill the criteria table.
4. Manual frontend checklist from Task 6 passes.
5. `docs/gazebo-vetting-results.md` contains the verdict.
