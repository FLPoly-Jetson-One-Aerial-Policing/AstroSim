# Unity flight-sim plugins and their usability for the Jetson One airframe

Nov 4, 2026 · Companion to [jetson-one-simulator-feasibility-unity.ai.md](jetson-one-simulator-feasibility-unity.ai.md).

**Short answer:** no off-the-shelf asset gives an eight-motor, calibrated multirotor model for Unity, so keep the plan to write your own `Rotor` component. The useful finds are different: (1) ArduPilot's JSON SITL interface lets Unity act as the physics backend for a real open-source flight controller, which could replace a hand-built stabilizer; and (2) a few small assets and repos are worth reading as references or starting points. Fixed-wing and helicopter toolkits are poor fits.

How this was checked: web searches plus fetches of each page on 2026-11-04. Marketing text and search snippets are labeled as such. I did not install or run any asset, so "usability" below is judged from descriptions, not tests. Prices and Unity-version support were not available for most Asset Store listings.

## 1. ArduPilot SITL over the JSON interface

The most valuable finding. ArduPilot's SITL can run against an external physics backend over UDP ([ArduPilot JSON SITL docs](https://ardupilot.org/dev/docs/sitl-with-JSON.html), [protocol readme](https://raw.githubusercontent.com/ArduPilot/ardupilot/master/libraries/SITL/examples/JSON/readme.md)).

- **Flow:** SITL sends a binary packet with a frame counter and PWM outputs (16 channels by default, 32 with `SERVO_32_ENABLE = 1`). The backend listens on UDP port 9002 and replies with newline-delimited JSON.
- **Required reply fields:** `timestamp`, `imu` (`gyro` in rad/s and `accel_body` in m/s², body frame), `position` (NED, m), `velocity` (NED, m/s) and attitude (Euler or quaternion). Optional fields include `rc`, `battery`, `velocity_wind` and rangefinders.
- **Rate:** `SIM_RATE_HZ` sets the step SITL requests and should stay above the vehicle loop rate (default 400 Hz for copter). The backend may apply its own maximum step.
- **Example in the wild:** [unity-arduboat-sim](https://github.com/chrisdalke/unity-arduboat-sim) drives a Unity boat with ArduPilot JSON. Its README warns that startup order matters, `127.0.0.1` may fail where a LAN IP works, and acceleration is not reported. No license stated; 6 commits.
- **Research:** a 2026 paper reports a Unity to ArduPilot SITL link over MAVLink for gamified drone training, with no main-thread bottleneck observed ([abstract](https://csitjournal.khmnu.edu.ua/index.php/csit/article/view/533)). I read only the search summary.
- **PX4:** searches found no Unity-PX4 SITL project.

**Why it matters for the Jetson One:** the original risk was that stabilization and control mixing are a heavy build. With this interface, Unity only has to turn eight PWM values into eight rotor commands and return sensor data. ArduPilot supplies attitude control, mixing and motor-out handling. Caveats:

- It is the open ArduPilot controller, not Jetson's proprietary one, so handling will not match the real aircraft until tuned.
- Unity's IMU data must be synthesized (angular velocity and specific force) from the rigidbody. The boat example shows this is doable but also shows acceleration reporting being skipped.
- Whether ArduPilot ships a frame definition matching an eight-motor stacked layout, and how it maps motor outputs, was not checked. From memory it has coaxial octa frames; verify before relying on it.
- SITL runs as a separate process (Linux or WSL typically), which cuts against the single-process Unity design. Plan for it as an optional dev mode, with a C# controller for the shipping build.
- Input latency over UDP adds to the control-to-visual delay; measure it.

## 2. Unity multirotor assets and repos

| Item | What it claims | Usability for an eight-motor frame | Evidence |
| --- | --- | --- | --- |
| [Drone Flight Controller Pro](https://discussions.unity.com/t/released-drone-flight-controller-pro-physics-based-drone-flight-ai-combat-drone-creator/1739594) (RageRun Games, Asset Store) | Per-motor thrust and torque, wind, turbulence, ground effect, crash behavior, a Drone Creator tool; Cinematic, Sport, Acro, Arcade modes | Closest match on paper. Unknown: motor count, custom layouts, source included, price, Unity versions. Game-oriented (AI drones, combat), not calibrated | Forum post only. The post states none of the unknowns above |
| [Yue Ultimate FPV Drone Physics](https://assetstore.unity.com/packages/tools/physics/yue-ultimate-fpv-drone-physics-231651) | Tagged quadrocopter, PID, FPV | Likely four-motor only; no description on the page. Free, v1.0 from Sept 2022, tested on Unity 2021.3.2 | Asset Store page; description empty, so features are inferred from tags |
| [thundercarrot/drone-simualator](https://github.com/thundercarrot/drone-simualator) | Applies forces and torques at motor positions relative to the rigidbody center | Same idea as the planned `Rotor` component; useful as a minimal reference | Search snippet; not opened |
| [quadrotor-simulation-unity](https://github.com/Oneiben/quadrotor-simulation-unity) | RL training with ML-Agents; applies an engine force to counter gravity | Too simplified (a single lift force) | Search snippet; not opened |
| [UnityFPVDroneSimulator](https://github.com/Venkatesan-M/UnityFPVDroneSimulator) | Real RC controller support, multiple flight modes | Useful reference for reading stick input; quadcopter-focused | Search snippet; not opened |
| [Flightmare](https://github.com/uzh-rpg/flightmare) | Quadrotor simulator, Unity rendering plus separate dynamics, RL API | Quadrotor-only as described, and its README lists a September 2020 release and does not state Unity version or maintenance status. Reference only | README fetched; physics engine not named |

No source I opened states that any of these supports eight motors or coaxial pairs. Assume you will edit or replace any of them.

## 3. Fixed-wing and helicopter toolkits

These came up for "flight sim" searches. They model lift from wings or rotor disks, not independent thrust vectors, so they do not map to this airframe.

- [Silantro Flight System](https://marketplace.unity.com/packages/tools/physics/silantro-flight-simulator-toolkit-128025): fixed-wing aircraft setup, per its description. Reviewers say drones and helicopters share its controller class, which I did not verify.
- [NWH Aerodynamics](https://marketplace.unity.com/packages/tools/physics/nwh-aerodynamics-193240), [Versatile Aerodynamics Package](https://assetstore-fallback.unity.com/packages/3d/vehicles/air/versatile-aerodynamics-package-210366) (drag, stalling, wind), [USim](https://assetstore.unity.com/packages/templates/systems/usim-vehicle-simulation-framework-15674/reviews), [Advanced Helicopter Controller](https://assetstore.unity.com/packages/tools/physics/advanced-helicopter-controller-215906).
- Possible reuse: the drag and wind pieces of the aerodynamics packages, if their code is readable and licensed for your use. Not checked.
- Several listings are stale: Simple Airplane Physics Toolkit's latest release is 2022, Easy Aircraft Controls 2019, and Airplane Physics Controller is deprecated.

## 4. Simulators with a Unity mode

- **AirSim:** Microsoft archived the original, and its Unity support was experimental ([The Register, 2022](https://www.theregister.com/2022/07/19/microsofts_metaverse_is_for_training/)). [Colosseum](https://github.com/CodexLabsLLC/Colosseum) is a successor, built on Unreal Engine 5; its README mentions an experimental Unity release, whose status I could not confirm. One community fork is archived and points to [Project AirSim](https://github.com/iamaisim/ProjectAirSim). Not recommended as a base.

## Recommendation

1. Write the `Rotor` and drag code yourself, as planned. Use the small GitHub repos for reference only.
2. Evaluate Drone Flight Controller Pro only if its listing confirms source access and arbitrary motor layouts; otherwise skip it. Ask the publisher or read the Asset Store page, which I could not fetch.
3. Prototype the ArduPilot JSON link early. It is the one option that could remove the stabilization build, and it can be tested with a hovering octa before any calibration.
4. Do not use fixed-wing or helicopter toolkits for the core model.

## Not researched

Unity licensing for a commercial product, Asset Store licenses for each item, whether Unity Physics (DOTS) has any multirotor packages, and JSBSim or other flight dynamics libraries for Unity (one search returned nothing).
