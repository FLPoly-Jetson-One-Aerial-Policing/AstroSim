# Gazebo vetting results (in progress)

Environment: Fedora 44 on WSL2, podman 5.8 rootless, container Ubuntu 24.04, **gz Jetty** (gz-sim 10.6, gz-transport 15, gz-msgs 12), stock Fuel `X3 UAV` v4, 1 ms step, headless `gz sim -s -r`.

## Criteria status

| Criterion | Result | Status |
| --- | --- | --- |
| Installs and runs headless | Containerfile builds; `gz sim -s -r worlds/poc.sdf` runs; X3 loads | pass |
| RTF, 1 ms step, 60 s | overall 0.9998, mean 1.0005, p5 0.987, min 0.34 (brief dip) | pass |
| Determinism | final-pose delta 0.0038 m between two scripted runs (commands timed by sim clock, delivered with wall-clock jitter) | pass |
| C++ access via gz-transport | subscribe odometry, publish Twist/Boolean/Actuators, no ROS | pass |
| Host client -> container gateway -> gz -> host | scripted climb to 2.000 m, error 0.000, over published UDP port | pass |
| Control-to-state latency (< 50 ms p95) | see Latency section | echo pass; literal "first pose change" fails at 60 Hz, borderline at 500 Hz (plant response) |
| Per-motor command injection | see below | works, with a caveat |
| Orchestrator, viewer, latency | not done yet | pending |

## Motor-out spike

- The motor model subscribes to `/X3/gazebo/command/motor_speed` (`gz.msgs.Actuators`, `velocity[i]` indexed by `actuator_number`). With the `MulticopterVelocityControl` plugin **removed** from the world, publishing to it works: 700 rad/s on all four lifts the X3 (about 1.52 kg, hover ~660 rad/s), and zeroing motor 0 gives an immediate response (roll flipped 162 deg within 1.5 s, crash). `tools/motor_spike.cpp direct`, `worlds/spike_nocontrol.sdf`.
- With the velocity controller plugin **loaded** (as in `worlds/poc.sdf`), the topic is **ignored**: no response with the controller enabled, and no response with it disabled (the vehicle simply drops at zero thrust). The plugin owns the motor command channel inside the sim even when idle; nothing is published on the topic, so it cannot be intercepted from outside. `tools/motor_spike.cpp override`.
- Consequence: motor-out and velocity-command flight are mutually exclusive with the stock plugin. A fault-injection mode needs either a world without the controller plus our own controller/mixer publishing `Actuators` (which the 8-rotor Jetson needs anyway since the stock controller is quad-oriented), or a switchable world. Not yet decided.
- Note `gz topic -p` from the CLI is unreliable for this (fresh publisher not discovered before the process exits); use a persistent publisher.

## Install notes (surprises)

- Jetty OSRF packages: `libgz-sim10-dev` etc. do not pull the CLI; needs `gz-sim10-cli`, `gz-transport15-cli`, `gz-msgs12-cli`. CMake package names are unversioned (`gz-transport`, `gz-msgs`), unlike Harmonic (`gz-transport13`).
- Fuel model name is lowercased in topics: `/model/x3/odometry`, while command topics keep `/X3/...`.
- The velocity controller is not enabled by default; publish `true` to `/X3/enable` (the gateway re-sends it at 1 Hz).
- Stock world rendering plugins (sensors, ogre2) removed for headless; no GPU needed in the container.

## Latency (`tools/latency.cpp`, host client -> podman port-forward -> gateway -> gz -> back; 20 trials, throttle step from hover at 2 m)

| Measure | 60 Hz state tick | 500 Hz state tick (`TICK_HZ=500`) |
| --- | --- | --- |
| command -> gateway applied (State echoes seq) | p50 16.6, p95 16.7 ms | p50 2.0, p95 2.0 ms |
| command -> vz > 0.02 m/s | p50 50.0, p95 50.0 ms | p50 36.0, p95 46.0 ms |
| command -> dz > 0.5 mm | p50 50.0, p95 50.0 ms | p50 46.0, p95 54.0 ms |
| command -> dz > 2 mm | p50 66.6, p95 83.4 ms | p50 70.0, p95 76.0 ms |

- At 60 Hz every number is quantized to the 16.7 ms state tick. Network + gateway cost is ~1 tick (<= 16.7 ms) worst case, ~2 ms with a fast tick. Port-forward overhead is not measurable at this resolution.
- The remaining ~35-70 ms is the airframe's own response to a step (motor time constants 12.5/25 ms, controller `maximumLinearAcceleration` 2 m/s^2, detection threshold), not plumbing. The literal "first pose change < 50 ms" criterion mostly measures the plant and the chosen threshold; it should be reframed as "command applied by the engine < 50 ms" (pass) plus a separately reported plant response.
- Implication for the real product: state rate is a tunable (a 60 Hz stream adds up to 16.7 ms of avoidable latency); consider sending State immediately on Control receipt or at a higher rate for the VR/airframe-in-loop case.
