# Flight computer interface (`shared/fc`)

An FC-agnostic C++20 interface between the AstroSim bridge and a black-boxed flight computer (FC). Swapping the FC (PX4 today, ArduPilot or the proprietary Jetson FC later) means writing one new adapter class. The engine protocol ([../proto/PROTOCOL.md](../proto/PROTOCOL.md)) does not change.

Pure portable C++20: no OS headers, no MAVLink. Time is injected; nothing here reads a clock.

> ## Temporary: synthesized sensors and spoofed pilot
>
> **The sensors are SYNTHESIZED from engine truth state** (`TemporarySynthSensors`, `synth_sensors_TEMPORARY.{hpp,cpp}`). They are noise-free, perfectly aligned and unfailing. They are not a sensor model and are a TEMPORARY stopgap until engine-side sensors arrive with the sensor/GPS ticket. The config key is `sensor_source = "synthesized_temporary"`, the only accepted value for now.
>
> **The pilot input is SPOOFED** (`ScriptedPilot`): a scripted keyframe timeline, not a real pilot. Real input is a separate ticket.
>
> Both are flagged everywhere: Doxygen `@warning`, a startup WARN log, the stats line (`sensors=SYNTHESIZED`, `pilot=SPOOFED`), and the provenance bits `kSensorsSynthesized` / `kPilotSpoofed` in the FC telemetry stream ([../proto/FC_TELEMETRY.md](../proto/FC_TELEMETRY.md)).

## Flight modes

There are exactly two, and **both command attitude ANGLE** from the roll and pitch sticks, never rate. There is no acro mode.

| `FlightMode` | Meaning |
|-|-|
| `kGpsHold` | attitude-angle sticks with GPS position hold |
| `kNoGpsHold` | attitude-angle sticks without GPS (no position hold) |

The interface exposes nothing FC-specific. Each adapter maps the two modes to its FC's native modes. If the FC refuses a mode (for example GPS hold without a fix), the adapter keeps the mode actually active in `telemetry().mode` and sets `telemetry().mode_refused`. It must never silently switch to a different mode.

## Types (`types.hpp`)

- `SensorFrame`: `time_s`; `gyro` (rad/s); `accel` (specific force, m/s^2); `mag` (gauss); `baro_pressure_pa`, `baro_altitude_m`, `baro_temperature_c`; `Gps` (lat, lon, alt MSL, NED velocity, fix, satellites); optional `Truth` (quaternion, position, velocity) for adapters that want ground truth (for example HIL state logging).
- `PilotInput`: `roll`, `pitch` in [-1, 1] as **attitude angle demands** scaled by the FC's maximum tilt; `yaw` in [-1, 1]; `throttle` in [0, 1]; `mode`; `arm`.
- `MotorCommand`: `power[8]` in [0, 1] in protocol motor order, and `armed`. When not armed the bridge sends zero power.
- `FcTelemetry` (= `proto::FcTelemetryMsg`): the FC's own view for avionics displays. It is the wire message of FC_TELEMETRY.md, shared so there is a single definition.

## Frame and unit contract

SI units. All frames are right-handed.

| Where | Frame |
|-|-|
| world / `Truth` | ENU: x East, y North, z Up |
| body (`gyro`, `accel`, `mag`) | FLU: x Forward, y Left, z Up |
| `Gps.vel_ned` | NED: north, east, down |
| quaternions | (x, y, z, w), Hamilton, body to world |

- `accel` is **specific force** (what an accelerometer reads), not kinematic acceleration: hover reads about (0, 0, +9.81), free fall reads about 0.
- `frames.hpp` converts ENU/FLU to NED/FRD (vectors and attitude quaternions); adapters whose FC uses NED/FRD use it rather than re-deriving signs.

## Writing an adapter

Derive from `FlightComputer` (`flight_computer.hpp`):

| Method | Purpose |
|-|-|
| `onSensors(frame, now)` | a new sensor sample arrived; forward it to the FC in its own format |
| `onPilot(input, now)` | the pilot command (sampled by the bridge at its own rate); map mode and sticks to the FC |
| `poll(now)` | service the FC link (sockets, serial, timers); must not block |
| `takeMotors()` | newest motor command since the last call, or `nullopt` |
| `telemetry()` | the FC's current view; the bridge overlays provenance, so adapters need not set it |
| `reset()` | return to the power-on state |

Guarantees by the bridge:

- All methods are called from a single thread (the bridge event loop); adapters need no locking.
- `now` is a monotonic clock owned by the bridge. Adapters must use it rather than reading a clock, so tests are deterministic.
- `onSensors` is called once per accepted engine State (the simulation clock drives the FC); sensor frames are in increasing `time_s`.
- If the FC is disconnected, disarmed or silent beyond the bridge's timeout, the bridge sends zero power on its own. An adapter does not need to implement that, but must report `armed=false` and `connected=false` truthfully.
- `power` values are sanitized by the engine (NaN becomes 0, clamped to [0, 1]); adapters should still emit finite values in [0, 1], using 0 for disarmed.

## Sensor and pilot sources

- `SensorSource::onState(StateMsg)` returns an optional `SensorFrame`. `TemporarySynthSensors` returns nothing for the first sample, a duplicate timestamp or a non-finite state, and treats a backwards timestamp as an engine restart.
- `PilotSource::sample(time_s)`; `ScriptedPilot` replays keyframes (sample-and-hold, or linear ramp for the continuous axes), holds the last keyframe forever, and is disarmed with zero sticks before the first keyframe.

## Build and test

`shared/fc` builds standalone. From the repo root, in a container with a C++20 compiler, CMake, Ninja and GoogleTest:

```
podman run --rm -v "$PWD":/src:ro,z docker.io/library/ubuntu:26.04 sh -c '
  apt-get update -qq && apt-get install -y -qq build-essential cmake ninja-build libgtest-dev &&
  cmake -S /src/shared/fc -B /tmp/b -G Ninja -DCMAKE_BUILD_TYPE=Debug -DASTROSIM_SANITIZE=ON &&
  cmake --build /tmp/b && ctest --test-dir /tmp/b --output-on-failure'
```

Or from the engine project: `cmake -S gazebo-container -B build -DASTROSIM_BUILD_FC_TESTS=ON`.
