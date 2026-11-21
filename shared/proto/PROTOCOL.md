# AstroSim Engine Protocol v1

Normative wire specification between the AstroSim physics engine (gateway) and its clients (flight computer, displays such as Unity). Reference implementation: `include/astrosim/proto/`, `src/wire.cpp`.

Key words MUST / SHOULD are used as in RFC 2119.

The flight computer's avionics telemetry is a separate stream on its own port with its own magic; see [FC_TELEMETRY.md](FC_TELEMETRY.md).

## 1. Transport

| Item | Value |
|-|-|
| Protocol | UDP |
| Framing | exactly one message per datagram |
| Default port | 8765 (engine listens) |
| Byte order | little-endian for every multi-byte field, on every host |
| Floats | IEEE 754 binary32 (`f32`) / binary64 (`f64`), transmitted as their little-endian bit pattern |
| Max datagram | 117 bytes |

A receiver MUST silently drop (and SHOULD count) any datagram that fails validation in the order below:

1. length < 8 : too short
2. magic != 0xA57A : bad magic
3. version != 1 : bad version
4. type not in {1,2,3} : unknown type
5. length != exact size for that type : bad size

## 2. Header (8 bytes, all messages)

| Offset | Size | Type | Field | Value |
|-|-|-|-|-|
| 0 | 2 | u16 | magic | 0xA57A (bytes `7A A5`) |
| 2 | 1 | u8 | version | 1 |
| 3 | 1 | u8 | type | 1 Control, 2 State, 3 Subscribe |
| 4 | 4 | u32 | seq | sender's counter, incremented by 1 per message of its own stream, wraps at 2^32 |

Example: Subscribe, seq 1 = `7A A5 01 03 01 00 00 00`.

`seq` is per sender and per direction. State seq is the engine's own counter.

## 3. Messages

### 3.1 Control (type 1, 40 bytes) : flight computer -> engine

| Offset | Size | Type | Field | Unit |
|-|-|-|-|-|
| 0 | 8 | | header | |
| 8 | 4 | f32 | power[0] | normalized 0..1 |
| 12 | 4 | f32 | power[1] | " |
| 16 | 4 | f32 | power[2] | " |
| 20 | 4 | f32 | power[3] | " |
| 24 | 4 | f32 | power[4] | " |
| 28 | 4 | f32 | power[5] | " |
| 32 | 4 | f32 | power[6] | " |
| 36 | 4 | f32 | power[7] | " |

### 3.2 State (type 2, 117 bytes) : engine -> subscribers

| Offset | Size | Type | Field | Unit / frame |
|-|-|-|-|-|
| 0 | 8 | | header | |
| 8 | 8 | f64 | sim_time | s |
| 16 | 8 | f64 | pos.x (East) | m, world ENU |
| 24 | 8 | f64 | pos.y (North) | m |
| 32 | 8 | f64 | pos.z (Up) | m |
| 40 | 4 | f32 | quat.x | body -> world |
| 44 | 4 | f32 | quat.y | |
| 48 | 4 | f32 | quat.z | |
| 52 | 4 | f32 | quat.w | |
| 56 | 4 | f32 | vel.x | m/s, world ENU |
| 60 | 4 | f32 | vel.y | |
| 64 | 4 | f32 | vel.z | |
| 68 | 4 | f32 | ang_vel.x | rad/s, body FLU |
| 72 | 4 | f32 | ang_vel.y | |
| 76 | 4 | f32 | ang_vel.z | |
| 80 | 4 | f32 | motor_speed[0] | rad/s, protocol motor order |
| 84 | 4 | f32 | motor_speed[1] | |
| 88 | 4 | f32 | motor_speed[2] | |
| 92 | 4 | f32 | motor_speed[3] | |
| 96 | 4 | f32 | motor_speed[4] | |
| 100 | 4 | f32 | motor_speed[5] | |
| 104 | 4 | f32 | motor_speed[6] | |
| 108 | 4 | f32 | motor_speed[7] | |
| 112 | 1 | u8 | flags | see 6 |
| 113 | 4 | u32 | echo_seq | seq of the last applied Control |

### 3.3 Subscribe (type 3, 8 bytes) : any client -> engine

Header only, no payload. Registers the sender's endpoint (IP, port) as a State subscriber, or keeps the registration alive.

## 4. Units and frames

- World frame: ENU, right-handed. x = East, y = North, z = Up. Origin is the simulation world origin.
- Body frame: FLU, right-handed. x = Forward, y = Left, z = Up.
- `quat` = (x, y, z, w), unit quaternion (Hamilton), rotating body-frame vectors into the world frame. Identity = body forward points East, left points North.
- `vel` is world-frame; `ang_vel` is body-frame.
- SI units: metres, seconds, radians.

## 5. Converting to Unity (left-handed, Y-up)

Unity axes: X = right, Y = up, Z = forward. With East -> X, Up -> Y, North -> Z.

World vectors (`pos`, `vel`):

```
unity.x = enu.x   // East
unity.y = enu.z   // Up
unity.z = enu.y   // North
```

This swap is a reflection (determinant -1), so rotation axes pick up an extra sign. For a quaternion `q = (x, y, z, w)` that rotates a body frame into ENU, the Unity-space rotation of a GameObject whose local axes are X = right, Y = up, Z = forward (so local right = -FLU y, up = FLU z, forward = FLU x) is:

```
h = sqrt(0.5)
unity.q.x = h * ( y - x)
unity.q.y = h * ( w - z)
unity.q.z = h * (-x - y)
unity.q.w = h * ( w + z)
```

Derivation: the world-side map M_w = (x,y,z)->(x,z,y) and the body-side map M_b = (x,y,z)->(-y,z,x) differ by the proper rotation A = M_b^-1 M_w, which is a -90 degree turn about Up (quaternion (0,0,-h,h)). So `unity_q = reflect(A * q)`, where reflecting a quaternion through an improper axis map M_b negates and permutes its vector part: (vx,vy,vz) -> (vy, -vz, -vx), w unchanged. Expanding A * q gives the formulas above.

Sanity checks:

| ENU orientation | q (x,y,z,w) | Unity q |
|-|-|-|
| body forward = East (identity) | (0,0,0,1) | (0, h, 0, h) = +90 deg about Y, forward -> +X (East) |
| body forward = North | (0,0,h,h) | (0,0,0,1) identity, forward -> +Z (North) |

Body angular velocity (FLU) in the Unity object's local frame, as an axis-angle rate vector: `(ωy, -ωz, -ωx)` (Unity's rotation sense is left-handed, hence the sign changes). Motor speeds are scalars and need no conversion. Conversion is the display's responsibility; the engine always sends ENU/FLU.

## 6. Semantics

### Power
- `power[i]` is normalized in [0, 1], following PX4's `actuator_motors` convention.
- The engine sanitizes on receipt: NaN or +/-Inf -> 0 (PX4 uses NaN for disarmed); then clamped to [0, 1]. Wire decoding itself does not alter values.
- 0 means stopped, 1 means the configured maximum motor speed.

### Motor numbering
Index 0-7 is the protocol motor number; the number of motors is fixed at 8. The mapping to a physical motor on the vehicle is defined by the engine configuration/model and will be finalized with the production model. State `motor_speed[i]` uses the same index.

### Flags (State.flags, bit mask)

| Bit | Value | Name | Meaning |
|-|-|-|-|
| 0 | 1 | ENGINE_OK | engine state (odometry) is fresh |
| 1 | 2 | CONTROL_ACTIVE | a controller holds the lease and is within the deadman window |
| 2 | 4 | DEADMAN_TRIPPED | motors were zeroed because Control stopped arriving |
| 3-7 | | reserved | senders write 0; receivers ignore |

### echo_seq
`seq` of the last Control that was applied. 0 before any Control. A controller can compare it with its own sent seq to measure round-trip latency.

## 7. Session rules

Defaults (configurable in the engine):

| Parameter | Default |
|-|-|
| Subscriber timeout | 5 s |
| Max subscribers | configured by engine (excess Subscribes dropped) |
| Controller lease timeout | 1 s |
| Deadman timeout | 0.5 s |

- **Subscribers.** Any endpoint that sends Subscribe (or Control) receives State at the engine's configured rate. Clients MUST send Subscribe at >= 1 Hz as a keepalive; an endpoint silent for the subscriber timeout is removed.
- **Controller lease.** The first endpoint to send a valid Control holds the lease. Control from other endpoints is dropped until the holder has been silent for longer than the lease timeout, after which the next sender takes over.
- **Sequence check.** A Control whose seq is not newer than the last accepted one is dropped (stale or duplicate). "Newer" uses wraparound compare: `(int32)(a - b) > 0`. A newly taking-over controller starts fresh.
- **Deadman.** If no Control is accepted for the deadman timeout, all motors are set to 0 and DEADMAN_TRIPPED is set until Control resumes. A controller SHOULD send Control at a steady rate (e.g. >= 50 Hz).

## 8. Versioning

Any change to a message layout, size, field meaning, or motor count bumps `version`. Receivers MUST drop datagrams with an unknown version. Adding a message type also requires a bump unless old receivers' drop-on-unknown-type behavior is acceptable to all deployments.

## 9. Conformance vectors

`testdata/` holds golden datagrams. A client implementation SHOULD decode each to the values below and re-encode to identical bytes.

| File | Size | Seq | Content |
|-|-|-|-|
| `control.bin` | 40 | 7 | power = {0, 0.125, 0.25, 0.5, 0.75, 1, 0.5, 0.0625} |
| `state.bin` | 117 | 0xFFFFFFFE | sim_time 123.456; pos (1234.5, -678.25, 90.125); quat (0.1, -0.2, 0.3, 0.9273618); vel (1.5, -2.5, 3.5); ang_vel (0.01, -0.02, 0.03); motor_speed 100..800 step 100; flags 3 (ENGINE_OK, CONTROL_ACTIVE); echo_seq 4242 |
| `subscribe.bin` | 8 | 1 | none |

Regenerate with `UPDATE_GOLDEN=1` running the C++ tests; any change to them implies a protocol change.
