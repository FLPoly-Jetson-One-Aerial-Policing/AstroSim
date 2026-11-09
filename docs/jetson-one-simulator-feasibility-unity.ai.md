# Jetson One simulator: feasibility with Unity as the physics engine

Nov 4, 2026 · Revision of [jetson-one-simulator-feasibility.ai.md](jetson-one-simulator-feasibility.ai.md), which assumed Gazebo.

Still feasible. Swapping Gazebo for Unity does not change the physics problem (eight thrust vectors plus body drag), but it moves the work. You lose the off-the-shelf multicopter and battery plugins and have to build them. You gain a renderer, VR support and a desktop frontend in the same engine. Fidelity risks come from missing Jetson data, not from the engine choice.

Everything about the vehicle (eight motors, fixed props, motor-out tolerance, mass and speed figures, and the disagreeing public specs) is unchanged from the original doc and is not repeated here.

## What Unity gives you

Verified against Unity's manual (pages opened 2026-11-04):

- **Default 3D physics is NVIDIA PhysX** for GameObject projects. It covers rigidbodies, colliders, joints and articulations ([Physics overview](https://docs.unity.com/en-us/engine/current/manual/physics-section/physics-overview)).
- **Fixed timestep is configurable.** Physics and `FixedUpdate` run at `Time.fixedDeltaTime`, independent of render rate. `Time.maximumDeltaTime` caps catch-up work to avoid a "spiral of death" ([Time Manager](https://docs.unity.com/en-us/engine/current/manual/unity-editor/editor-settings-reference/comp-manager-group/class-time-manager)). The page does not state defaults; I recall 0.02 s (50 Hz) and 0.1 s from memory.
- **Articulation bodies** use reduced coordinates, so locked axes cannot stretch or break. They are aimed at robotic arms and kinematic chains, have a single root, no loops and a maximum depth of 64 ([Articulations](https://docs.unity.com/en-us/engine/current/manual/physics-section/physics-overview/articulations-section/physics-articulations)). That page says nothing about aircraft or applying forces. For a rigid frame with eight force points, a single `Rigidbody` is likely enough; I would not use articulations unless you model flexible gear.
- **Headless runs are supported.** Launching a Player with the batch-mode and no-graphics flags runs without a graphics device. It is not a Build Settings option and does no server optimizations; the separate Dedicated Server target adds those ([Desktop headless mode](https://docs.unity3d.com/Manual/desktop-headless-mode.html), found via search; I read only the search summary).

From memory, not checked against Unity docs this session: `Rigidbody.AddForceAtPosition` and `AddTorque` apply per-rotor thrust and reaction torque; `Rigidbody.inertiaTensor` can be set to match CAD; Unity Physics (DOTS) and Havok are alternative engines, with Unity Physics designed to be deterministic. The physics overview page I fetched does not cover them.

## Fit with Unity

| Component | Status | Notes |
| --- | --- | --- |
| Rigid-body dynamics and collisions | Off the shelf | PhysX; set mass and inertia tensor explicitly |
| Per-rotor thrust and reaction torque | **Build** | No multicopter motor model ships with Unity. Write a `Rotor` component: thrust ∝ ω², reaction torque ∝ ω², motor lag as a first-order filter, applied from `FixedUpdate`. Small, but it is the core of the sim |
| Coaxial interaction | Build | Same empirical thrust factor as before, applied inside `Rotor` |
| Body drag on the open frame | Build | A short force script; no `LiftDrag` equivalent. Speeds stay under 102 km/h |
| Ground effect near the pad | Build, optional | Raycast height, scale thrust |
| Motor-out and failure injection | Small build | Zero one `Rotor`'s command from an input |
| Stabilization and control mixing | Build | Reimplement the controller in C#. ArduPilot SITL can run against a Unity backend over its JSON interface (an existing Unity boat example uses it); I found no Unity-PX4 integration. See [unity-flightsim-plugins-multicopter.ai.md](unity-flightsim-plugins-multicopter.ai.md) |
| Wind and turbulence | Build | No built-in wind for rigidbodies; apply a wind vector and noise in the drag script (Unity's Wind Zone affects trees and particles, not rigidbodies, from memory) |
| Battery and sag | Build, optional | Simple voltage-sag model; skip for a first version |
| Rendering, desktop frontend, VR | Off the shelf | Same engine; XR is supported by Unity packages. Biggest gain over Gazebo |
| Gateway between sim and frontend | Not needed | Unity does both physics and display, so the gateway from the Gazebo design is dropped |

Net effect compared with Gazebo: roughly one extra week of engineering on rotor, drag, wind and battery code, offset by a much shorter path to a good-looking VR frontend.

## Architecture changes

The Gazebo design needed a gateway to isolate frontends from Gazebo versions. Unity handles both physics and display, so the design is one Unity project: control inputs (USB/serial stick, throttle, pedals) are read in Unity, physics runs in `FixedUpdate`, and rendering and VR run in the same process. This removes the gateway hop and its latency.

Headless Unity plus a gateway, as in the original doc, stays available if you later need remote or non-Unity frontends, but it is not part of the plan.

The control and state message definitions are still worth writing down early, because they define the boundary for the real-airframe input adapter.

## Risks and open questions

Risks from the original doc that do not depend on the engine (proprietary flight controller, missing thrust curves and inertia, conflicting public specs, ground contact tuning) carry over unchanged. Changes and additions:

| Risk | Severity | Mitigation |
| --- | --- | --- |
| Physics step vs. stability: Unity's documented default step is coarser than the 1 ms used in the Gazebo plan (default recalled from memory); stiff rotor dynamics and a high-gain controller can go unstable | Medium | Set `fixedDeltaTime` to 1–2 ms and measure CPU cost; run the controller at the physics rate; test motor-out |
| PhysX is not guaranteed bit-identical across platforms or runs (stated as background by the fetch tool, not on the manual page) | Low to medium | Not an issue for pilot training; matters if you need replay or lockstep. Unity Physics (DOTS) is the deterministic option but needs an ECS rewrite |
| Physics and display share one process: a slow frame can cap physics steps (`maximumDeltaTime`), which hurts VR comfort and control feel | Medium | Keep frame cost low; measure control-to-visual latency early. This is the main cost of the single-process design |
| Custom thrust model means no community-validated multicopter baseline as with Gazebo | Medium | Validate against a simple analytic hover case and any Jetson data |
| Unity licensing and runtime terms for a commercial training product | Unknown | Not researched; check before committing |
| Version churn: Unity's docs have moved between hosts and versions, and the headless flags are unchanged across the versions I saw | Low | Pin a Unity LTS version |

Open questions from the original doc still stand: is the real flight computer in the loop or only the stick inputs, and is the goal training-grade fidelity or a demonstration. Add: is VR the primary frontend? If yes, Unity is the stronger choice.

## Recommendation

If a VR or high-quality visual frontend is a core requirement, choose Unity: physics is adequate for this vehicle and rendering comes free. If the goal is mainly a physics backend with ROS or PX4 tooling, Gazebo is less work. Both need the same Jetson data for a calibrated result.

## Effort and first steps

My estimate, not a measured figure: one engineer can have a hovering model with desktop frontend in about four to five weeks (one more than the Gazebo plan, from the custom rotor, drag and wind code).

1. Define control and state messages.
2. Build the frame as one `Rigidbody` with eight `Rotor` components at measured positions; set mass and inertia tensor.
3. Set `fixedDeltaTime` and get a simple controller hovering.
4. Add stabilization and mixing; test attitude response and single motor-out.
5. Add drag, wind, ground effect; tune landing.
6. Add desktop input, then VR and the real-airframe adapter; measure latency.
7. Calibrate against Jetson data.

## Sources

Opened 2026-11-04:

- [Unity physics overview](https://docs.unity.com/en-us/engine/current/manual/physics-section/physics-overview): PhysX is the default 3D engine
- [Unity Time Manager](https://docs.unity.com/en-us/engine/current/manual/unity-editor/editor-settings-reference/comp-manager-group/class-time-manager): fixed and maximum timestep semantics
- [Unity articulations](https://docs.unity.com/en-us/engine/current/manual/physics-section/physics-overview/articulations-section/physics-articulations): reduced coordinates, limits
- [Unity desktop headless mode](https://docs.unity3d.com/Manual/desktop-headless-mode.html): via search summary only

Vehicle figures: see the sources list in the original doc.
