# AstroSim Flight Computer Telemetry Protocol v1

Normative wire specification for the avionics stream published by the AstroSim flight computer (FC) to visual frontends (displays such as Unity). It is a **separate stream** from the engine protocol ([PROTOCOL.md](PROTOCOL.md)); the engine protocol is untouched. Displays subscribe to the engine (port 8765) for truth and visuals and to the FC (port 8766) for avionics. Reference implementation: `include/astrosim/proto/fc_telemetry.hpp`, `src/fc_telemetry.cpp`.

Key words MUST / SHOULD are used as in RFC 2119.

> **TEMPORARY data warning.** Until the sensor/GPS work lands, the FC's sensors are **synthesized from engine truth state**, and the pilot input is a **scripted spoof**. The `provenance` byte (section 4) says so, and a display SHOULD show it.

## 1. Transport

| Item | Value |
|-|-|
| Protocol | UDP |
| Framing | exactly one message per datagram |
| Default port | 8766 (the FC telemetry server listens) |
| Byte order | little-endian for every multi-byte field |
| Floats | IEEE 754 binary32 (`f32`) / binary64 (`f64`), little-endian bit pattern |
| Max datagram | 95 bytes |

A receiver MUST silently drop (and SHOULD count) any datagram that fails validation in the order below:

1. length < 8 : too short
2. magic != 0xA5FC : bad magic (the engine protocol's magic 0xA57A is rejected here)
3. version != 1 : bad version
4. type not in {1,2} : unknown type
5. length != exact size for that type : bad size
6. telemetry `mode` not in {0,1} : bad value

## 2. Header (8 bytes, all messages)

| Offset | Size | Type | Field | Value |
|-|-|-|-|-|
| 0 | 2 | u16 | magic | 0xA5FC (bytes `FC A5`) |
| 2 | 1 | u8 | version | 1 |
| 3 | 1 | u8 | type | 1 Telemetry, 2 Subscribe |
| 4 | 4 | u32 | seq | sender's counter, +1 per message of its own stream, wraps at 2^32 |

Example: Subscribe, seq 1 = `FC A5 01 02 01 00 00 00`.

## 3. Messages

### 3.1 Telemetry (type 1, 95 bytes) : FC -> subscribers

| Offset | Size | Type | Field | Unit / meaning |
|-|-|-|-|-|
| 0 | 8 | | header | |
| 8 | 8 | f64 | time_s | s, simulation time of the sensor frame the data derives from |
| 16 | 1 | u8 | status | bit 0 connected, bit 1 armed, bit 2 mode_refused, bits 3-7 reserved (senders write 0, receivers ignore) |
| 17 | 1 | u8 | mode | active flight mode: 0 GPS hold, 1 no-GPS hold |
| 18 | 1 | u8 | gps_fix | 0 no fix, 2 2D, 3 3D |
| 19 | 1 | u8 | gps_sats | satellites used |
| 20 | 1 | u8 | provenance | see 4 |
| 21 | 2 | u16 | warnings | see 5 |
| 23 | 4 | f32 | roll | rad, right wing down positive |
| 27 | 4 | f32 | pitch | rad, nose up positive |
| 31 | 4 | f32 | heading | rad, clockwise from true north, [0, 2 pi) |
| 35 | 4 | f32 | alt_msl | m above mean sea level |
| 39 | 4 | f32 | alt_agl | m above ground level |
| 43 | 4 | f32 | vspeed | m/s, positive up |
| 47 | 4 | f32 | ground_speed | m/s |
| 51 | 4 | f32 | course | rad, clockwise from true north, [0, 2 pi) |
| 55 | 4 | f32 | battery_pct | percent (placeholder until the battery model exists) |
| 59 | 4 | f32 | battery_v | V (placeholder until the battery model exists) |
| 63 | 4 | f32 | motors[0] | commanded output, [0,1], protocol motor order |
| 67..91 | 4 each | f32 | motors[1..7] | " |

Every value is the **FC's own view** (its estimate or state), never engine truth. `mode_refused` is set when the requested mode was not entered (for example GPS hold without a fix); `mode` then still shows the mode actually active.

### 3.2 Subscribe (type 2, 8 bytes) : display -> FC

Header only. Registers the sender's endpoint as a telemetry subscriber or keeps the registration alive.

## 4. Provenance (`provenance` byte)

| Bit | Name | Meaning |
|-|-|-|
| 0 | SENSORS_SYNTHESIZED | TEMPORARY: the FC's sensors are synthesized from engine truth state, not a sensor model |
| 1 | PILOT_SPOOFED | TEMPORARY: pilot input is a scripted spoof, not a real pilot |
| 2-7 | | reserved; senders write 0, receivers ignore |

The bridge sets these itself, so no flight computer adapter can forget them.

## 5. Warnings (`warnings` bit mask)

| Bit | Value | Name |
|-|-|-|
| 0 | 1 | GPS_LOST |
| 1 | 2 | EKF_UNHEALTHY |
| 2 | 4 | PILOT_LOST |
| 3 | 8 | LOW_BATTERY |
| 4 | 16 | MOTOR_FAILURE |
| 5-15 | | reserved; senders write 0, receivers ignore |

## 6. Session rules

Same as the engine protocol (PROTOCOL.md section 7): any endpoint that sends Subscribe receives Telemetry at the FC's configured rate (default 30 Hz); clients MUST send Subscribe at >= 1 Hz as a keepalive; an endpoint silent for the subscriber timeout (default 5 s) is removed; the server caps the subscriber count and drops excess Subscribes. There is no controller lease: the stream is read-only.

## 7. Versioning

Any change to a layout, size, field meaning or the set of defined enum values bumps `version`. Receivers MUST drop datagrams with an unknown version.

## 8. Conformance vectors

`testdata/` holds golden datagrams. A client implementation SHOULD decode each to the values below and re-encode to identical bytes.

| File | Size | Seq | Content |
|-|-|-|-|
| `fc_telemetry.bin` | 95 | 9 | time 123.456; connected + armed; mode 1 (no-GPS hold); fix 3, 12 sats; provenance 3; warnings 10 (EKF_UNHEALTHY, LOW_BATTERY); roll 0.05, pitch -0.1, heading 1.5; alt_msl 160.5, alt_agl 10.25, vspeed 0.75; ground_speed 2.5, course 3.0; battery 87.5 %, 48.25 V; motors 0.1 .. 0.8 step 0.1 |
| `fc_subscribe.bin` | 8 | 1 | none |

Regenerate with `UPDATE_GOLDEN=1` running the C++ tests; any change to them implies a protocol change.
