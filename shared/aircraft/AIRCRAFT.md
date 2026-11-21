# Aircraft data file

`jetson_one.toml` is the single source of truth for the aircraft's physics. The Gazebo model generator and the
gateway read it through the `astrosim::aircraft` library (`parseAircraft` / `loadAircraft`). New Jetson data means
editing this file; no code changes are needed.

## Rules

- **No defaults.** Every key listed below is required. A missing key, an unknown key, a wrong type or an
  out-of-range value is an error. Nothing is built into the code.
- **Errors name the key.** Messages start with the full key path, e.g. `motor[3].xyz_m: required key missing`
  or `airframe.inertia_kg_m2: inertia is not positive-definite ...`. `loadAircraft` prefixes the file path.
- Integers are accepted where a number is expected. All numbers must be finite.
- Names (`collision`, `rotor_type`) match `[A-Za-z0-9_]+` and are unique within their array.
- The parser checks the file, not the protocol: it does not require exactly 8 motors. The gateway enforces that.

## Frame, units and conventions

- FLU body frame: x forward, y left, z up. SI units; the unit is the key-name suffix (`_kg`, `_m`, `_rad`, ...).
- The origin is the airframe reference point (Jetson CAD reference, see B2.4). In the shipped file it is 0.45 m
  above the skid bottoms, roughly mid-tub.
- Inertia tensors are about the centre of mass, in body axes. `ixy`, `ixz`, `iyz` are the off-diagonal matrix
  elements (SDF/URDF convention), so a point mass m at (x, y, z) contributes `ixy = -m*x*y`.
- `rpy_rad` is SDF fixed-axis roll-pitch-yaw. Spin direction is seen from above.

## Schema

### Top level

| Key | Type | Meaning |
|-|-|-|
| `schema_version` | integer | Must equal the parser's version (currently 1). |

### `[aircraft]`

| Key | Type | Meaning |
|-|-|-|
| `name` | string, non-empty | Label for logs and display. |

### `[airframe]` and `[pilot]`

| Key | Unit | Constraint | Meaning |
|-|-|-|-|
| `mass_kg` | kg | > 0 | Mass. Airframe = empty aircraft including its rotors. Pilot = seated pilot. |
| `center_of_mass_m` | m | 3 numbers | Centre of mass in the body frame. |
| `inertia_kg_m2` | kg m^2 | inline table `ixx, iyy, izz, ixy, ixz, iyz`, all required | Inertia about the CoM. Must be physical: finite, positive-definite, principal moments satisfy the triangle inequality. |

### `[[collision]]` (at least one)

| Key | Constraint | Meaning |
|-|-|-|
| `name` | unique name | Identifier. |
| `shape` | `"box"`, `"cylinder"`, `"sphere"` | Primitive type. |
| `size_m` | 3 numbers > 0 | Box only: full extents along local x, y, z. |
| `radius_m` | > 0 | Cylinder and sphere. |
| `length_m` | > 0 | Cylinder only, along local z. |
| `xyz_m` | 3 numbers | Primitive origin in the body frame. |
| `rpy_rad` | 3 numbers | Primitive orientation. |

Keys of another shape are rejected (for example `size_m` on a cylinder).

### `[[rotor_type]]` (at least one)

A motor and propeller definition that several motors share. Referenced by name; there is no inheritance.

| Key | Unit | Constraint | Meaning |
|-|-|-|-|
| `name` | | unique name | Referenced by `motor.rotor_type`. |
| `prop_radius_m` | m | > 0 | Propeller radius (collision disc). |
| `collision_thickness_m` | m | > 0 | Thickness of the disc collision. |
| `rotating_mass_kg` | kg | > 0 | Rotating parts (prop and motor rotor). |
| `rotating_inertia_kg_m2` | kg m^2 | table with exactly `ixx, iyy, izz`, all > 0 | Spin inertia at the hub; products are 0. |
| `max_rot_velocity_rad_s` | rad/s | > 0 | Rotor speed at power 1.0. |
| `motor_constant_n_s2` | N s^2 | > 0 | Thrust = constant * w^2. |
| `moment_constant_m` | m | > 0 | Drag torque / thrust. |
| `time_constant_up_s`, `time_constant_down_s` | s | > 0 | First-order spin-up and spin-down time constants. |
| `rotor_drag_coefficient` | | >= 0 | Rotor drag coefficient. |
| `rolling_moment_coefficient` | | >= 0 | Rolling moment coefficient. |

### `[[motor]]` (at least one)

Array order is the protocol motor index and Jetson's motor numbering.

| Key | Constraint | Meaning |
|-|-|-|
| `rotor_type` | name of an existing `rotor_type` | Which definition this motor uses. |
| `xyz_m` | 3 numbers | Hub position; the spin axis is body z. |
| `direction` | `"ccw"` or `"cw"` | Spin direction seen from above. |

### `[drag]`

Positive magnitudes along body x, y, z. The generator applies the sign.

| Key | Unit | Constraint |
|-|-|-|
| `linear_n_s_m` | N/(m/s) | 3 numbers >= 0 |
| `quadratic_n_s2_m2` | N/(m/s)^2 | 3 numbers >= 0 |

### `[simulation]`

Simulation knobs, not aircraft data.

| Key | Constraint | Meaning |
|-|-|-|
| `rotor_velocity_slowdown` | > 0 | Gazebo motor model `rotorVelocitySlowdownSim`. Gateway motor speed scale. |
| `odometry_rate_hz` | > 0 | Odometry publish rate. |

## Mass model

`[airframe]` is the empty aircraft including its rotors, as Jetson would measure it. `[pilot]` is separate, so
trainee weight can change without touching the airframe. Loaded mass properties are
`combine(airframe, pilot)` (`loadedMass`).

The generator gives each rotor its own link (needed to spin it). It therefore removes each rotor's rotating mass
and inertia from the body: `base_link = airframe + pilot - sum(rotors)`, using `rotorMass` and the parallel-axis
theorem in `mass.hpp`. The total mass, centre of mass and inertia then equal the measured values exactly. If the
remainder is not physical, generation fails.

## Replacing guesses with Jetson data

Every value in `jetson_one.toml` is tagged `# GUESS` or `# SOURCE`. IDs refer to
`docs/jetson-data-requirements.ai.md`.

| Keys | Data request |
|-|-|
| `airframe.mass_kg`, `pilot.mass_kg` | B1.1 (empty, loaded and maximum mass; pilot range) |
| `airframe.center_of_mass_m`, `pilot.center_of_mass_m` | B1.2, B1.5 (CG and its variation; seat and pilot distribution) |
| `airframe.inertia_kg_m2`, `pilot.inertia_kg_m2` | B1.3, B1.5 |
| frame origin, all `xyz_m` | B2.4, B2.15 (origin and reference point; CAD aligned to CG and motor positions) |
| `collision` | B1.4, B2.1-B2.3, B2.12 (gear geometry; CAD or drawings; level of detail) |
| `motor.xyz_m` | B3.1 (motor positions), B2.6 (hubs and axes) |
| `motor.direction`, motor order | B3.2 (spin direction and FC numbering) |
| `rotor_type.prop_radius_m` | B3.3 (prop diameter) |
| `rotor_type.rotating_mass_kg`, `rotating_inertia_kg_m2` | B3.5 (prop inertia) |
| lower versus upper `motor_constant_n_s2` | B3.4 (coaxial interference) |
| `rotor_type.motor_constant_n_s2`, `max_rot_velocity_rad_s` | B4.1 (thrust vs command), B4.3 (RPM limits) |
| `rotor_type.moment_constant_m` | B4.2 (torque vs command) |
| `rotor_type.time_constant_up_s`, `time_constant_down_s` | B4.4 (step response) |
| `rotor_type.rotor_drag_coefficient`, `rolling_moment_coefficient` | B6.6 (prop inflow effects), B3.3 |
| `drag` | B6.1 (drag coefficients, CdA) |

Sanity check after editing: hover needs `loaded weight / 8` per motor on average; the required fraction of
`max_rot_velocity_rad_s` should be well below 1.

## Keeping the 3D model in step

The display's 3D model and these physics shapes describe the same aircraft and must not drift apart (issue #58).
Ideas, not built yet:

1. The display reads this same file through `astrosim::aircraft` and places rotor animation and the CoM marker
   from `motor.xyz_m` and `airframe.center_of_mass_m`, rather than from positions baked into the mesh.
2. The 3D asset carries named anchors: `rotor_0_hub` ... `rotor_7_hub`, `bbox_min`/`bbox_max`, `ref_origin`.
3. A CI parity check reads the glTF anchors and mesh bounds and asserts that hubs match `motor.xyz_m` and the
   mesh bounding box matches the union of the collision primitives, within a tolerance.
4. A debug overlay in the display draws the collision primitives from this file over the mesh.
5. Once CAD arrives (B2.1), a script fits the collision primitives from it and writes them back into this file,
   so hand-tuned boxes stay under review instead of being derived at runtime.
