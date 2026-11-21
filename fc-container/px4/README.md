# PX4 SITL adapter and airframe generator

`Px4Adapter` is the `fc::FlightComputer` implementation for PX4 SITL ([../../shared/fc/FC_INTERFACE.md](../../shared/fc/FC_INTERFACE.md)), built into the bridge with `-DASTROSIM_BUILD_PX4=ON`. `astrosim-px4-airframe` generates the PX4 airframe script from the aircraft file.

| File | Role |
|-|-|
| `mavlink_codec.{hpp,cpp}` | pure: interface types to and from MAVLink messages, mode mapping, a framing/parsing `MavlinkLink` without global state |
| `px4_telemetry.{hpp,cpp}` | pure: `FcTelemetry` from PX4's MAVLink stream |
| `px4_session.{hpp,cpp}` | pure: the protocol (lockstep time, rates, mode/arm commands and retries, refusal detection, sensor dither) with injected time |
| `px4_adapter.{hpp,cpp}` | POSIX sockets only (container-only): TCP server for the HIL link, UDP for the MAVLink link |
| `airframe.{hpp,cpp}`, `airframe_main.cpp` | pure airframe script generation, and the `astrosim-px4-airframe` tool |

## Links to PX4 (SITL instance 0, all inside the FC container)

- **HIL link, TCP 4560.** PX4's `simulator_mavlink` is the TCP *client*; the adapter listens. The bridge sends `HIL_SENSOR` per engine State, `HIL_GPS` at 10 Hz and `HIL_STATE_QUATERNION` (truth, logged by PX4) at 25 Hz, all stamped with the **simulation time**; PX4 sets its clock from `HIL_SENSOR` (sensor id 0) and answers every step with `HIL_ACTUATOR_CONTROLS` (lockstep). If the engine restarts (sim time goes backwards) PX4 must be restarted too; the entrypoint's supervision does that when the container restarts.
- **MAVLink link, UDP.** PX4's "onboard" instance sends to `127.0.0.1:14540` (the adapter binds it) and listens on `14580`. The adapter sends `MANUAL_CONTROL` at 50 Hz, `COMMAND_LONG` `DO_SET_MODE` and `ARM_DISARM`, and receives the telemetry streams.

## Behaviour

- Modes: GPS hold is PX4 POSCTL; no-GPS hold is ALTCTL (default, angle sticks plus barometric altitude hold) or STABILIZED (angle sticks plus direct throttle), `[px4] no_gps_mode`. **Never acro.** The adapter requests the mode (retrying every second), arms only once PX4 is in it, and reports `mode_refused` if PX4 is not in it 2.5 s after the request or denies it. It never substitutes another mode.
- Arming is an event on the rising edge of `PilotInput::arm`, retried until PX4 reports armed (15 s). If PX4 disarms itself (auto-disarm after landing) it is not re-armed until the request goes false and true again.
- Throttle: ALTCTL and POSCTL treat the stick as a climb-rate demand with 0.5 holding altitude (`MANUAL_CONTROL` z = 500); STABILIZED maps 0.5 to hover thrust.
- Telemetry sources: HEARTBEAT, ATTITUDE, GLOBAL_POSITION_INT, VFR_HUD, GPS_RAW_INT, SYS_STATUS, ESTIMATOR_STATUS (all streamed by the onboard instance). `alt_agl_m` is the altitude relative to PX4's home (the take-off point), not terrain. Battery is PX4's simulated battery (a placeholder). The motor-failure warning is not available from PX4 and is never raised.
- **Sensor dither.** PX4's data validators drop a sensor whose value is identical for 100 samples (`DataValidator.cpp`, "state: STALE"; then `ekf2 missing data` and no arming). The synthesized sensors are noise free and constant at rest, so the adapter adds a tiny deterministic dither to `HIL_SENSOR` (about the quantization step of PX4's simulated IMU). Turn it off with `[px4] sensor_dither = false` when the sensors carry real noise. The sensors stay flagged `SYNTHESIZED`.

## Airframe generator

`astrosim-px4-airframe --aircraft jetson_one.toml --config fc.toml --out <PX4 etc>/init.d-posix/airframes` writes `99001_astrosim_aircraft` (`SYS_AUTOSTART` 99001). It sets `CA_AIRFRAME 0`, `CA_ROTOR_COUNT`, per rotor `CA_ROTORn_PX/PY/PZ` (FRD, relative to the loaded centre of mass), `CA_ROTORn_KM` (signed moment constant: positive for a counter-clockwise rotor seen from above) and `CA_ROTORn_CT` (relative thrust), `PWM_MAIN_FUNCn` (output i is protocol motor i), `THR_MDL_FAC 1`, `MPC_THR_HOVER` (loaded weight over the sum of maximum thrust), `MPC_MAN_TILT_MAX` and `MPC_MAN_Y_MAX` from `[px4]`, the SITL no-RC/no-GCS parameters, and control gains.

**Control gains.** PX4's defaults assume a small fast quadrotor; with them the sample aircraft (control authority per inertia about 8x lower, rotor lag 0.1 s) oscillates in pitch. The generator derives rate, attitude and velocity gains from the aircraft file: the angular acceleration per unit PX4 command (from the allocator's normalization and the loaded inertia) and the rotor lag set a rate-loop crossover of 0.6 / lag, with the outer loops nested inside it (attitude 0.25 / lag, velocity 0.4 of that, position 0.4 of that). The constants come from SITL runs of the sample aircraft and are placeholders like the aircraft data. Not verified: other aircraft.

## PX4 facts, verified against PX4-Autopilot v1.17.0 (source and SITL runs)

| Fact | Source |
|-|-|
| `simulator_mavlink` connects to TCP 4560 as a client (the simulator listens); `PX4_SIM_HOSTNAME` selects the host | `px4-rc.mavlinksim`, `SimulatorMavlink.cpp` (`run()`) |
| Lockstep: PX4's clock is set from `HIL_SENSOR.time_usec` of sensor id 0; each `HIL_SENSOR` id 0 advances lockstep; `HIL_ACTUATOR_CONTROLS` has `flags` bit 0 set in lockstep | `handle_message_hil_sensor`, `actuator_controls_from_outputs` |
| `HIL_SENSOR`: accel m/s^2 (specific force), gyro rad/s, mag gauss, `abs_pressure` hPa, `pressure_alt` m, temperature C, FRD; `fields_updated` tests accel `0b111`, gyro `0b111000`, mag `0b111000000`, baro `0b1101000000000` (bits 9, 11, 12), differential pressure bit 10 | `update_sensors`, `enum class SensorSource` |
| `HIL_GPS`: lat/lon 1e-7 deg, alt mm, eph/epv cm, velocities cm/s, cog cdeg, `satellites_visible` | `handle_message_hil_gps` |
| `HIL_ACTUATOR_CONTROLS.mode` bit 128 is armed; controls are 0 when disarmed | `actuator_controls_from_outputs` |
| `CA_ROTORn_KM`: torque = KM * thrust, positive for a counter-clockwise rotor; rotor axis defaults to (0, 0, -1) (up in FRD); `CA_ROTORn_CT` is relative | `control_allocator/module.yaml`, `ActuatorEffectivenessRotors.cpp` |
| Roll and pitch share one allocator normalization scale, yaw has its own | `ControlAllocationPseudoInverse.cpp` |
| `THR_MDL_FAC`: rel_thrust = f * s^2 + (1 - f) * s | `lib/mixer_module/motor_params.c` |
| `MANUAL_CONTROL`: x pitch (forward positive is nose down), y roll, r yaw, in [-1000, 1000]; z throttle in [0, 1000] mapped to [-1, 1] | `mavlink_receiver.cpp` |
| Stick z: ALTCTL/POSCTL climb rate with 0.5 neutral; STABILIZED `throttle_curve`: -1/0/1 to minimum/hover/maximum thrust | `mc_att_control_main.cpp` |
| `custom_mode`: main mode in bits 16-23, sub mode in bits 24-31; ALTCTL 2, POSCTL 3, STABILIZED 7, ACRO 5 | `commander/px4_custom_mode.h` |
| `DO_SET_MODE`: param1 base mode (custom enabled = 1), param2 main mode, param3 sub mode | `Commander.cpp` |
| SITL instance 0 MAVLink: GCS 18570 to 14550, onboard 14580 to 14540, payload 14280 to 14030, gimbal 13030 to 13280 | `px4-rc.mavlink` |
| The onboard instance streams HEARTBEAT, ATTITUDE (100 Hz), GLOBAL_POSITION_INT (50 Hz), VFR_HUD (10 Hz), GPS_RAW_INT, SYS_STATUS (5 Hz), ESTIMATOR_STATUS (1 Hz) | `mavlink_main.cpp` (`MAVLINK_MODE_ONBOARD`) |
| `SYS_AUTOSTART 0` selects SIH, not the HIL simulator; the generated id is 99001 | `px4-rc.simulator` |
| No-RC parameters: `COM_RC_IN_MODE 1` (MAVLink only), `NAV_RCL_ACT 0`, `NAV_DLL_ACT 0`, `COM_ARM_WO_GPS 1` | `commander_params.c` |
| Data validators flag identical values (100 in a row) as stale | `sensors/data_validator/DataValidator.cpp` |

Observed in a SITL run with the sample aircraft (see the container README for the commands): `ekf2` aligns, arms in GPS hold, climbs to about 10 m, holds within a few centimetres, switches to no-GPS hold, follows small pitch demands, descends, lands and PX4 auto-disarms; yaw does not drift (the `KM` signs are right for the sample aircraft's motor order).

Not verified: other PX4 versions; STABILIZED beyond a single flight (the sample script's descent at throttle 0.1 is a crash landing in that mode); `mode_refused` against a real PX4 refusal (unit-tested with canned messages); `kWarnPilotLost` (derived from the RC sensor health bit of SYS_STATUS, never raised in the runs); QGroundControl access to the GCS port (not published); the gain derivation for other aircraft.
