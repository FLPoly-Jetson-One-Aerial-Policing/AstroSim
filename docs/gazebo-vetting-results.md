# Gazebo vetting results (in progress)

Environment: Fedora 44 on WSL2, podman 5.8 rootless, container Ubuntu 24.04, **gz Jetty** (gz-sim 10.6, gz-transport 15, gz-msgs 12), stock Fuel `X3 UAV` v4, 1 ms step, headless `gz sim -s -r`.

## Criteria status

| Criterion | Result | Status |
| --- | --- | --- |
| Installs and runs headless | Containerfile builds; `gz sim -s -r worlds/poc.sdf` runs; X3 loads | pass |
| RTF, 1 ms step, 60 s | overall 0.9998, mean 1.0005, p5 0.987, min 0.34 (brief dip) | pass |
| Determinism | final-pose delta 0.0038 m between two scripted runs (commands timed by sim clock, delivered with wall-clock jitter) | pass |
| C++ access via gz-transport | subscribe odometry, publish Twist/Boolean/Actuators, no ROS | pass |
| Host client -> container gateway -> gz -> host | scripted climb to 2.000 m, error 0.000, over published UDP port | pass (latency numbers pending) |
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
