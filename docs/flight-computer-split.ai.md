# Splitting the flight computer out of the engine

Status: notes for the production architecture. The PoC will not be refactored; it is throwaway.

## 1. Where flight behaviour lives in the PoC

| Piece | Location | Role |
| --- | --- | --- |
| Stick -> velocity command (`fly()`, tilt/governor/climb/yaw constants) | `container/gateway/mixer.hpp` | Angle mode with momentum |
| Velocity/attitude/rate loops + mixer | gz `MulticopterVelocityControl` in `container/worlds/poc.sdf` (gains 2.7 / 2/3/0.15 / 0.4/0.52/0.18) | Inner loop, runs *inside the engine* |
| Airframe drag | Hydrodynamics plugin in `poc.sdf` (constants mirrored in `mixer.hpp`) | Gives the momentum feel |
| Auto hover | `HoverAssist` in `host/app/input_map.hpp` | Rewrites sticks on the client |
| Motor thrust/torque | `MulticopterMotorModel` x4 in `poc.sdf` | Engine (correctly placed) |

Flow today: viewer sticks -> UDP -> gateway `fly()` -> world-to-body Twist -> gz velocity controller -> motor speeds -> motor models.
Flight control is split across the client, the gateway and the engine config, and the "flight computer" is really the gz plugin.

## 2. Target boundary

```
pilot input (sticks + mode) ─┐
                             ├─> FlightComputer ──> motor power [0..1] x N ──> engine (motors, physics)
sensors (IMU, state est.) ───┘
```

- Engine = physics, motors, sensors. Nothing in it knows about stick, mode or velocity commands.
- Modes (angle, simple, aero, auto-hover, ...) live inside the FC and are selected by a mode field from the pilot; the client sends sticks plus mode, never rewritten sticks.
- The FC runs at its own fixed rate, decoupled from the network tick (the PoC gateway only acts when a UDP packet arrives).
- The real Jetson flight computer is dropped in behind the same interface via a small adapter (SITL or hardware-in-the-loop). Our own simple FC is the reference implementation.

Interface sketch (no engine dependencies, unit-testable):

```cpp
struct SensorFrame { double t; float gyro[3], accel[3]; /* + estimate: quat, pos, vel */ };
struct PilotInput  { ControlMsg sticks; uint8_t mode; };
struct MotorOutput { float power[4]; };  // normalized; engine scales to rotor speed
class FlightComputer {
  virtual MotorOutput update(const SensorFrame&, const PilotInput&, double dt) = 0;
  virtual uint8_t activeMode() const = 0;
  virtual void reset() = 0;
};
```

## 3. What the split entails

- **Engine:** publish `gz::msgs::Actuators` to `gazebo/command/motor_speed` (what the controller publishes today); add an IMU sensor; remove `MulticopterVelocityControl` and its `enable` topic. Keep the motor models and drag.
- **Gateway:** latch the latest pilot input on UDP receive; run the FC loop on its own timer; deadman becomes neutral sticks in a hold mode rather than a zero-velocity command.
- **Protocol:** add `mode` to the control message and `active_mode` to the state message (regenerate golden files, mirror in the orchestrator's wire package).
- **Client:** drop `HoverAssist`; hover becomes a mode toggle. The HUD shows the active mode.
- **Shared constants:** drag coefficients are used by both the engine and the angle-mode governor; keep them in one place.

## 4. Main risk: flight feel

The tuned feel depends on the gz controller's gains and limits as well as on `fly()`. Producing motor power means reimplementing that inner loop (attitude P, rate PID, mixer). Mitigation: port gz's algorithm faithfully with the same gains, then A/B scripted stick sequences (climb, full forward to governor, yaw, hover, stop) against the old Twist path and compare velocity/attitude traces before removing it. A transitional option is to keep the Twist path selectable until the traces match.

## 5. Open questions for the real design

- The real FC's I/O: protocol, sensor set, motor command scale and rate.
- State estimate vs sim-truth: what the FC is allowed to see (the PoC feeds it odometry truth).
- Process boundary for SITL, and the latency budget between engine and FC.
- Where mode switching and failsafes (deadman, motor-out) belong: pilot input path, FC, or both.
- How fault injection (motor-out, see `FaultMsg` and `container/tools/motor_spike.cpp`) interacts with an FC that owns motor outputs.
