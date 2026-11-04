# Jetson One simulator: feasibility

Oct 28, 2026 · @Eli

Feasible, and simpler than a generic VTOL: the Jetson One is an open-frame, eight-motor multirotor with no wings or tilting parts, so headless Gazebo can supply most of the physics.

## The vehicle

The Jetson One has eight electric motors and eight propellers, described as stacked, which I read as four stacked pairs on an H-shaped aluminium frame ([evtol.news](https://evtol.news/aircraft/jetsonaerospeeder/)). The page does not say outright that the pairs are coaxial. It is a multirotor, not a transitioning VTOL: there is no wing, no tilting rotor and no forward-flight mode to model separately.

| Parameter | Value | Why it matters for the sim |
| --- | --- | --- |
| Propulsion | 8 brushless motors, 8 fixed propellers | One thrust and torque model, repeated eight times |
| Control | Joystick, throttle lever, yaw pedals; triple-redundant flight computer | Maps cleanly onto a normalized input schema |
| Mass | 55 kg empty, 115 kg with batteries, 210 kg maximum takeoff | Mass changes with pilot weight; expose it as a parameter |
| Top speed | 102 km/h (software-limited, level flight) | Low speed keeps aerodynamics simple |
| Endurance | About 20 minutes | Battery model is optional for a pilot-training sim |
| Failure behavior | Flies safely after losing one of eight motors | Worth simulating: motor-out is a likely training scenario |

Sources for the table: [evtol.news production model](https://www.evtol.news/jetson-one-production-model), [Flyer launch coverage](https://flyer.co.uk/jetsons-return-with-evtol-anyone-can-fly/), [AvGeekery overview (Oct 2021)](https://avgeekery.com/the-jetson-one-a-high-tech-personal-evtol-for-the-masses/).

That the physics reduces to eight thrust vectors plus body drag is what makes this feasible without a custom backend.

## Fit with Gazebo

Gazebo Sim already ships a multicopter motor model, so the custom physics work is mostly tuning, plus a few effects that matter for a believable feel. Plugin names below are from my knowledge of the gz-sim plugin set, not from a fresh docs check; confirm them against your Gazebo version (Harmonic or newer).

| Component | Status | Notes |
| --- | --- | --- |
| Rigid-body dynamics and collisions | Off the shelf | Default DART physics, 1 ms step |
| Per-rotor thrust and reaction torque | Mostly off the shelf | `MulticopterMotorModel`, configured eight times with alternating spin directions |
| Coaxial interaction (lower prop in the upper prop's wash) | Build | If the stacked propellers are coaxial, the lower one loses efficiency; apply an empirical thrust factor, to be calibrated against Jetson data |
| Body drag on the open frame | Small build | `LiftDrag` or a short custom force plugin; speeds stay under 102 km/h |
| Ground effect near the pad | Build, optional | Matters for landing feel; scale thrust by height above ground |
| Motor-out and failure injection | Small build | Zero one motor's command from a topic |
| Stabilization and control mixing | Build or reuse | The real aircraft stabilizes attitude and hovers hands-free; reimplement as a controller node, or use PX4 SITL if its tuning is acceptable |
| Wind and turbulence | Partly off the shelf | Basic wind system exists; gusts and turbulence are custom |
| Battery and sag | Off the shelf, optional | `LinearBatteryPlugin`; skip for a first version |
| Frontend interface | Build | Gateway described below |

The heavy items are stabilization and the thrust calibration, not the physics engine.

## Architecture

&#91;embedded content: architecture · 2 inputs, 1 gateway, 1 headless Gazebo, 2 frontends\]

Control inputs and frontends talk only to the gateway. Gazebo runs headless with `gz sim -s`, and the gateway republishes poses in a schema that does not change when Gazebo versions do.

## Risks and open questions

None of these block feasibility; the first two decide how faithful the result will be.

| Risk | Severity | Mitigation |
| --- | --- | --- |
| Handling feel depends on Jetson's proprietary flight controller, which stabilizes attitude and holds hover hands-free | High for fidelity | Get the control law and gains from Jetson, or tune an open controller against recorded flight data |
| No public rotor thrust curves, motor constants or inertia tensor | High for fidelity | Request from Jetson, or measure on a bench; estimate inertia from CAD in the meantime |
| Published specs disagree: all-up weight with battery is 90 kg in 2021 launch coverage and 115 kg in the current production listing, and peak power is 84 kW in one 2021 source and 88 kW in another | Medium | Treat public figures as placeholders; confirm against official documentation |
| Latency and jitter between gateway, sim and a VR headset | Medium | Fixed 1 ms physics step, interpolation on the render side, measure the control-to-visual delay early |
| Landing and ground contact feel with DART contact | Medium | Budget tuning time; test with simple gear geometry first |
| Coaxial and ground-effect losses are unmodeled by default | Low to medium | Empirical correction factors, refined as data arrives |

Open questions:

- Does "actual airframe" mean only stick, throttle and pedals read over USB or serial, or the real flight computer in the loop? The second needs an interface that I could not find documented publicly.
- Is the goal training-grade fidelity or a convincing demonstration? That sets how much calibration data you need.

## Effort and first steps

My rough estimate, not a measured figure: one engineer can have a controllable, hovering model with a desktop frontend in a few weeks. A calibrated, training-grade model depends on getting data from Jetson, not on engineering time.

1. Define the control message (stick, throttle, yaw) and the state message (pose, velocity, rotor speeds), since every other piece depends on them.
2. Build the vehicle in SDF with eight motor joints in alternating spin directions and one motor model per rotor; get it hovering with a simple controller.
3. Add the stabilization controller and mixing; check attitude response and a single motor-out case.
4. Add body drag, wind and ground contact; tune landing.
5. Write the gateway and connect a desktop frontend with mock inputs.
6. Add the VR frontend and the real-airframe input adapter; measure control-to-visual latency.
7. Calibrate thrust, drag and control response against Jetson data as it becomes available.

## Sources

Pages opened on 2026-10-28; figures not marked as from memory come from these.

- [Jetson One production model, evtol.news](https://www.evtol.news/jetson-one-production-model): weights, payload, endurance, motor-out wording
- [Jetson One prototype, evtol.news](https://evtol.news/aircraft/jetsonaerospeeder/): stacked propellers, H-shaped frame
- [Jetson's return with eVTOL anyone can fly, Flyer](https://flyer.co.uk/jetsons-return-with-evtol-anyone-can-fly/): controls, 84 kW, 90 kg all-up, dimensions
- [The Jetson ONE, AvGeekery](https://avgeekery.com/the-jetson-one-a-high-tech-personal-evtol-for-the-masses/): 88 kW, triple-redundant flight computer, speed limit
