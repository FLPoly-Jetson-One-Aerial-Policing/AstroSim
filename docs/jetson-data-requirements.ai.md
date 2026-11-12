# Jetson ONE: data required from Jetson

Scope: everything needed to build a production simulator that hosts Jetson's own flight computer (FC) and avionics. Values are not being requested here; this lists the items to ask for.

Each table has a plain-English column explaining why the item matters.

VR is a first-class target. The rendering framework and graphics/XR stack are not yet decided; this list assumes none of them.

Priority: **P0** blocks implementation. **P1** needed for fidelity. **P2** nice to have.
Each item has an ID.

## Minimum first request

The absolute minimum to ask for first. The detailed items that each one expands into are listed in the last column.

|Request|Why we need it (plain English)|Detail|
|-|-|-|
|Flight computer: whether a Linux/simulation build or an interface specification (message/bus definitions, motor command format) exists or could exist, and what terms would apply.|Decides whether we can run Jetson's real flight software in the simulator or must build a substitute. Everything else is planned around the answer.|A1, A2, A4, A5, A7, C2|
|Geometry: CAD (STEP preferred), or a simplified exterior model or dimensioned drawings if the full files cannot be shared, with the permitted uses.|A correctly scaled model of the aircraft, and clarity on what we're allowed to do with it.|B2.1-B2.3, B2.13|
|Mass properties: empty and loaded mass, center of gravity, and moments of inertia.|Weight and balance drive how the aircraft accelerates and rotates.|B1.1-B1.3|
|Propulsion: thrust and torque versus motor command, motor numbering, positions and spin directions, and motor response time.|How much push the eight motors make, where they are, and how quickly they respond.|B3.1, B3.2, B4.1, B4.2, B4.4|
|Flight modes and stick-to-command mapping, including limits and rates.|How the aircraft responds to the pilot, which is what makes it feel like the real one.|A9.1, A9.2|
|A summary of motor-out and parachute behavior.|The main emergency scenarios a trainee would see.|B9.1, B9.2, B9.4|
|A few sample flight logs (hover, step inputs, motor-out) so we can check the simulator against the real aircraft.|Real recordings are our proof the simulated aircraft flies like the real one.|B11.1|
|Cockpit display screenshots or a page and alert list.|Lets us show the pilot what the real screen shows.|A6.4, A6.5|
|Practical terms: an NDA, what we may use or show, the software version to target, and a technical contact for questions.|The legal and working arrangements needed before any data can be shared.|C1, C2, C6, C8|

## Part A. Flight computer and avionics integration

### A1. What we need from the FC (target outcome)

Preferred, in order:

1. **SITL build**: a Linux library or executable of the production FC logic that can be stepped by the sim with injected time.
2. **Interface spec only** (ICD): enough to write a bus-level emulation or an adapter. The binary is not delivered.
3. **HIL**: a physical FC unit loaned to us, with the bench harness and bus/ESC interface details.

Plain English: best case, Jetson gives us their real flight software in a form we can run on a normal computer (1). If not, a detailed written description of how it communicates (2). If not, a physical unit we connect to the simulator (3).

### A2. Artifact and packaging

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|A2.1|Form of delivery for FC: shared library, static library, executable with socket/IPC protocol, firmware image for an emulator|We need to know what Jetson would actually hand us (a software library, a program, or a firmware file). Everything else depends on this.|P0|
|A2.2|Form of delivery for avionics/display software: shared library, static library, executable with socket/IPC protocol, firmware image for an emulator|The cockpit screen software may come in a different form from the flight computer, so we ask separately.|P0|
|A2.3|CPU architecture, OS, libc/ABI, compiler and toolchain versions, build type (debug/release), `dlopen` loadability|Software only runs on a matching computer type and operating system. If theirs doesn't match ours, we need to know early.|P0|
|A2.4|Headers and exported API, or message interface documentation if there is no API|This is the instruction sheet for talking to their software. Without it we can't connect to it.|P0|
|A2.5|Third-party dependencies, RTOS/HAL shims, hardware assumptions that must be stubbed|Their software may expect real aircraft hardware to be present. We need to know what to fake.|P0|
|A2.6|Debug symbols, logging hooks, internal-state inspection|Lets us see inside the software when something goes wrong, instead of guessing.|P1|
|A2.7|Thread-safety, reentrancy, whether multiple instances can run in one process or host|Tells us whether we can run several simulated aircraft, or several tests, at the same time.|P1|
|A2.8|Release/versioning policy, update channel, which firmware version is the target|The software will change over time. We need to know which version we're building against and how updates reach us.|P1|

### A3. Execution model

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|A3.1|Clock ownership: step function vs. own threads/timers; whether time can be injected; behaviour when sim time differs from wall time|The simulator can run faster or slower than real life. We need to know whether their software can follow our clock or insists on its own.|P0|
|A3.2|Loop rates (attitude, rate, position, mixer, estimator), per-sensor update rates, allowed jitter|The flight computer does its work in tight repeating cycles. We have to feed it data on the same schedule.|P0|
|A3.3|Initialisation, arming conditions, self-tests, boot duration, whether a clean in-process reset is possible|The real aircraft goes through a start-up routine before it will fly. We need to reproduce that, and be able to restart cleanly between tests.|P0|
|A3.4|Determinism guarantee (same inputs give same outputs)|If the same inputs always give the same result, we can run automated tests and trust them.|P1|
|A3.5|Persistent state: calibration, parameters, logs. Location, format, whether it can be redirected or preloaded|The software may save settings and logs to disk. We need to control where, so tests start from a known state.|P1|
|A3.6|Heap/static memory, stack, thread count, CPU load|Tells us what size of computer is needed to run it.|P2|
|A3.7|Triple-redundant architecture: whether lanes are three instances of one library, what cross-lane messages exist, what voting/arbitration does, what must be emulated vs. is hardware-only|The real aircraft has three flight computers cross-checking each other. We need to know what that looks like in software and how much of it we must copy.|P0|
|A3.8|Latency budget from sensor input to actuator output, and watchdog/timeout behaviour on late or missing data|The flight computer reacts when data arrives late or not at all. We need to know how strict it is so the simulator doesn't trigger false failures.|P1|

### A4. FC inputs (everything the sim must generate)

For each signal: name, type, units, frame, rate, range, and behaviour on stale/invalid data.

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|A4.1|Complete input signal list (IMUs, magnetometer, barometer, GNSS, radar/laser altimeter, airspeed, current/voltage, others)|The complete shopping list of data the flight computer expects (position, motion, pressure and so on). The simulator has to produce every item.|P0|
|A4.2|Pilot input signals in raw form (ADC counts or calibrated), stick, throttle lever, pedals, switches, discretes|How the pilot's stick, lever and pedals reach the computer (raw electrical readings or processed values). We have to send it in the same form.|P0|
|A4.3|Battery/BMS and ESC/motor telemetry fields consumed (voltage, current, SoC, temperature, RPM, fault flags)|The computer watches battery and motor health. The simulator must supply those readings too.|P0|
|A4.4|Discrete signals: arm, parachute state, ground contact, latches/interlocks|Simple on/off signals such as armed, parachute state, and whether the aircraft is on the ground.|P1|
|A4.5|Sensor calibration it expects (scale, offset, mounting rotation) and how to supply it|Sensors have small errors that the computer corrects with stored settings. We need those settings, or the simulated sensors will look broken.|P1|
|A4.6|Sensor health model: criteria for declaring a sensor failed or disagreeing|Tells us when the computer decides a sensor has failed, so we can test those situations.|P1|
|A4.7|Whether an externally supplied state estimate can replace raw sensors (for early bring-up)|If the computer can accept a ready-made position/attitude, we can get flying sooner and add realistic sensors later.|P1|
|A4.8|Time sources including GNSS time and date|Time and date information some systems need to start up.|P2|

### A5. FC outputs

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|A5.1|Motor command: channel count, mapping to physical motors 1..8, value type (normalised/RPM/PWM/duty), scale, rate, heartbeat|What the flight computer tells each of the eight motors, and in what units. Our physics has to understand it.|P0|
|A5.2|Motor-command-to-thrust relationship (see B4); the output is not interpretable without it|A motor command like '60%' means nothing until we know how much push it produces. This links the two.|P0|
|A5.3|Parachute deploy output, arm/disarm state, active mode, alert outputs|Other outputs: parachute firing, armed status, flight mode and warnings.|P0|
|A5.4|Telemetry sent to the avionics/display and to loggers|What the flight computer sends to the cockpit screen and to the flight recorder.|P1|
|A5.5|Outputs that expect feedback (command echo, acknowledgements)|Some outputs expect a confirming reply. If we don't send it, the computer may think something is broken.|P1|

### A6. Avionics / display

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|A6.1|Display hardware: size, resolution, pixel format, refresh rate|The physical size and sharpness of the real screen, so ours looks right.|P0|
|A6.2|Rendering path: framebuffer, GL/Vulkan, widget toolkit; whether the sim can supply the surface or texture, or the software can output an image/frame buffer we display ourselves|How the screen software draws its picture. We need to place that picture inside our simulated cockpit, including the VR view.|P0|
|A6.3|Inputs to the display: bus messages from FC, touch/button input|What feeds the screen: data from the flight computer, and any buttons or touch.|P0|
|A6.4|Page/screen inventory, instrument layout, units|A list of every screen page and where each instrument sits, so we reproduce what the pilot sees.|P0|
|A6.5|Complete alert/caution/warning list: priority, text, colour, audio cue, trigger condition|Every warning, its wording, colour, sound and the condition that triggers it. Pilots train to react to these exactly.|P0|
|A6.6|Audio output (tones, voice) and who plays it|Alert sounds and voice. We need to know who plays them, the screen or the computer.|P1|
|A6.7|Fonts, icons, assets, licences; GPU and memory requirements|Fonts, icons and artwork (and their usage rights), plus the graphics power needed to run it.|P1|
|A6.8|Mode annunciation; how range/endurance/battery estimates are computed and shown|How flight mode and remaining range or battery are worked out and shown. A wrong estimate on screen misleads trainees.|P1|

### A7. Bus between FC, avionics and peripherals

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|A7.1|Protocol (CAN, CAN-FD, UART, Ethernet) and message specification (DBC or ICD)|The wiring language between the flight computer, screen and other parts, plus the message dictionary. Without it we can't connect them.|P0|
|A7.2|Whether the library abstracts the transport or expects us to supply it|Tells us whether their software handles the communication itself, or whether we must build that link.|P0|
|A7.3|Timing/arbitration rules, behaviour on lost or late messages|The rules for message timing and what happens when messages are lost. Needed for realistic failures.|P1|
|A7.4|Recorded bus traffic from a real aircraft for replay|A recording of real messages lets us check our simulator speaks the same way.|P1|

### A8. Test and fault-injection hooks

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|A8.1|Built-in test/fault-injection API, or confirmation that faults must be injected through inputs only|Whether the software has a built-in way to simulate failures, or whether we can only cause them by feeding it bad data.|P0|
|A8.2|Ability to force mode, load parameter sets, freeze or override state|Lets automated tests put the software into a chosen state without flying there first.|P1|
|A8.3|Readable internal mode, alert and health flags for automated assertions|Lets tests read the aircraft's mode and warnings directly and check them automatically.|P1|
|A8.4|Golden vectors: recorded input sequences with expected outputs, to verify our hosting reproduces their behaviour|Recorded 'when this goes in, that comes out' examples from their software. This is our proof we are running it correctly.|P0|

### A9. Reference for the FC behaviour (needed if hosting a binary is refused; useful as validation otherwise)

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|A9.1|Flight modes available to a customer pilot and the transitions between them|The list of ways a pilot can fly the aircraft (for example manual or hover-assist) and how they switch between them.|P0|
|A9.2|Stick-to-command mapping per mode: attitude or rate command, limits, expo, deadband, max climb/descent/yaw rate|How far and how fast the aircraft responds when the pilot moves the stick. This determines how it feels to fly.|P0|
|A9.3|Control structure and gains: loop architecture, gains, filters, limits, anti-windup|The tuning of the flight computer's control loops. Needed only if we have to rebuild their controller ourselves.|P0|
|A9.4|Control allocation/mixer: matrix, saturation handling, priorities|How the computer splits a command among the eight motors, including when some motors are at their limit.|P0|
|A9.5|Speed governor (102 km/h): enforcement mechanism|How the 102 km/h speed limit is enforced.|P1|
|A9.6|Altitude/geofence limits|Maximum and minimum height and area restrictions the computer enforces.|P1|
|A9.7|Auto-hover behaviour (brake profile, tolerances)|How the hands-off hover feature stops the aircraft and holds position.|P1|
|A9.8|Behaviour on loss of pilot input and on stick centred|What the aircraft does if the pilot lets go of the controls or loses control input.|P1|
|A9.9|Takeoff/landing logic: ground detection, thrust ramp, disarm|How takeoff and landing are detected and handled, including motor ramp-up and shutdown.|P1|
|A9.10|Yaw authority, mixing and saturation behaviour|How turning (yaw) works and what happens when the motors can't deliver the requested turn.|P1|
|A9.11|State estimator type, fused sensors, rate, failure handling|How the aircraft works out its own position and attitude from the sensors, and what it does when sensors disagree.|P1|

## Part B. Aircraft data for the physics engine

Needed in either path: sensors and motors are driven by the engine, and the FC (real or substitute) depends on them being right.

### B1. Airframe and mass properties

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|B1.1|Empty, with-battery and maximum takeoff mass; supported pilot weight range|Weight drives every flight characteristic, so we need it empty, with batteries and at maximum.|P0|
|B1.2|CG location in body frame and its variation with pilot weight and battery position|The balance point. If it's wrong the simulated aircraft will tilt or handle incorrectly.|P0|
|B1.3|Inertia tensor (including products) for empty and loaded configurations, or a mass-properties export|How hard the aircraft is to rotate in each direction. Determines how quickly it responds to commands.|P0|
|B1.4|Skid/gear geometry (from B2), ground clearance, gear stiffness and damping; which surfaces should collide with the ground and obstacles|Landing gear shape and stiffness, so touchdowns and ground contact look and behave correctly.|P1|
|B1.5|Seat position, pilot mass distribution, eye point|Where the pilot sits and how their weight is distributed. Both affect balance and what the pilot sees.|P1|
|B1.6|Frame flexibility or folding (to confirm it can be ignored)|Confirms we can treat the frame as rigid, which keeps the simulation simpler.|P2|

### B2. CAD and geometry for scaled models

Purpose: a correctly scaled visual and collision model of the aircraft. Fallbacks, in order: full CAD, simplified exterior CAD, dimensioned 2D drawings, photographs with reference dimensions.

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|B2.1|3D CAD of the complete aircraft as flown (frame, arms, motors, props, cockpit tub, seat, roll cage/safety cell, skids, canopy or fairings), in a neutral exchange format (STEP preferred; IGES, Parasolid, or a mesh such as FBX/OBJ/glTF are acceptable)|The 3D design files are the best way to build a correctly scaled model.|P1|
|B2.2|Simplified/exterior-only version with internal parts, fasteners and wiring removed (a "shrinkwrap" or release-for-visualisation model), if the full CAD cannot be shared|A cleaned-up version without internal parts, if the full design can't be shared.|P1|
|B2.3|Dimensioned 2D drawings (front, side, top, plus sections through the cockpit) if 3D is not shareable|Flat dimensioned drawings, if 3D files can't be shared.|P1|
|B2.4|Units, origin and axis convention of the files, and the reference point on the aircraft that the origin corresponds to|Tells us what size unit and which direction is 'forward' in the files so the model isn't scaled or rotated wrongly.|P1|
|B2.5|Whether the CAD is separable by component (frame, each arm, each motor, each prop, seat, controls, display, battery bays), with assembly hierarchy and part names, so parts can be animated or assigned masses|Separate parts let us make propellers spin and assign weights to individual components.|P1|
|B2.6|Moving-part definitions: propeller hubs and rotation axes, control lever and stick pivot points and travel, any hinged or folding parts (folded vs. flight configuration)|Which parts move (propellers, stick, lever, fold points) and around what pivots.|P1|
|B2.7|Cockpit interior detail: seat, harness, stick, throttle lever, pedals, display mount, switch panel, instrument position, at a quality suitable for a VR viewpoint|The pilot sits inside the cockpit in VR, so the interior has to be accurate, not just the outside.|P1|
|B2.8|Materials and finish: colours (paint codes or RAL), carbon-fibre weave areas, decals/livery files, logo artwork, and any texture or reference photography|Colours, finishes, logos and livery, so the model looks like the real aircraft.|P2|
|B2.9|Prop and blade geometry as a mesh and/or airfoil section definition (also used for B3.3)|Propeller shape affects both how it looks and how much thrust it makes.|P1|
|B2.10|Variant list: production vs. earlier prototype, colour/trim options, optional equipment, and which variant the CAD represents|Several versions of the aircraft exist. We need to know which one the files show.|P1|
|B2.11|Reference photographs and 360 deg walkaround/cockpit imagery to check model accuracy|Photos to compare our model against, so we can spot mistakes.|P2|
|B2.12|Level of detail guidance: which parts are structurally or visually significant, and which may be omitted|Tells us which details matter, so we don't spend effort modelling the unimportant ones.|P2|
|B2.13|Licence for the geometry: permitted uses (internal modelling, simulator visuals, redistribution to end users, marketing), and whether a derived/simplified model may be shipped|What we're legally allowed to do with the design files, such as show them to end users.|P0|
|B2.14|Confidentiality or watermarking requirements; whether geometry may be reduced to a lower-resolution, non-reversible mesh before distribution|Whether Jetson wants the files protected or reduced in detail before we distribute anything.|P1|
|B2.15|Alignment of CAD to the other data: CAD coordinates relative to the CG reference (B1.2), motor positions (B3.1) and sensor mounting poses (B7.1), so they share one frame|The model, the weight balance, the motors and the sensors must all use the same reference point, or they won't line up.|P0|

### B3. Rotor geometry

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|B3.1|Position and orientation of each of the 8 motors/props (lever arms); cant angles; confirmation of coaxial layout|Where each of the eight motors sits and which way it points. This sets how the aircraft turns and tilts.|P0|
|B3.2|Spin direction per motor and the motor numbering used by the FC|Which way each propeller spins, and how the flight computer numbers the motors. Mixing these up would flip the aircraft.|P0|
|B3.3|Prop diameter, pitch, blade count, airfoil; thrust and power map (Ct, Cp vs. advance ratio)|Propeller dimensions and performance data to compute how much thrust and drag each one gives.|P1|
|B3.4|Coaxial interference: lower-rotor efficiency loss, upper/lower thrust split, spacing|Two propellers stacked on top of each other interfere. We need to know how much thrust that costs.|P1|
|B3.5|Prop inertia|How much the propellers resist speeding up or slowing down.|P1|
|B3.6|ESC/motor grouping on buses and battery packs|Which motors share controllers and battery packs. Needed to simulate failures that take out several motors at once.|P1|
|B3.7|Downwash impact on pilot and frame|The airflow pushed down by the propellers can hit the pilot and parts of the frame.|P2|

### B4. Motor and ESC model

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|B4.1|Thrust vs. command (table or curve), static and with airspeed|How much push each motor gives at each command level. The most important number for realistic flight.|P0|
|B4.2|Torque (reaction and drag) vs. command, or moment constant|How much twisting force each motor produces, which determines turning (yaw) behaviour.|P0|
|B4.3|Max, idle and minimum RPM; whether motors may stop in flight; spin-up floor|The slowest and fastest the motors can spin, and whether they can stop mid-flight.|P0|
|B4.4|Step response: time constants or lag, slew limits, spin-up vs. spin-down asymmetry|How quickly the motors respond to a command. Slow response changes how the aircraft handles.|P0|
|B4.5|Motor KV, torque constant, resistance, inductance, current and thermal limits|Electrical details of the motors, for limits and heating calculations.|P1|
|B4.6|ESC command protocol and rate (PWM, DShot, CAN), resolution, command semantics (throttle, RPM, torque)|The signal format between flight computer and motor controllers, and what the numbers mean.|P0|
|B4.7|Whether the ESC closes an RPM loop; telemetry it returns|Whether the motor controllers regulate speed themselves, and what status they report.|P1|
|B4.8|Thermal derating curves and overtemperature response|How motors reduce power when hot, which affects long or hard flights.|P1|
|B4.9|Thrust ripple, vibration and noise spectra|Vibration and noise from the motors, which the sensors pick up.|P2|

### B5. Power system

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|B5.1|Pack architecture: chemistry, series/parallel counts, number of packs, mapping to motors|How the batteries are arranged and which motors each one feeds.|P1|
|B5.2|Capacity, nominal and min/max voltage, internal resistance|The battery's size, voltage and internal resistance.|P1|
|B5.3|Discharge curve (OCV vs. SoC) and sag under load|How battery voltage drops as it empties and under heavy load.|P1|
|B5.4|Current limits; SoC, SoH, temperature, power telemetry|Limits on power draw and the battery readings shown to the pilot.|P1|
|B5.5|Low-battery thresholds and the aircraft's response at each|Warning levels and what the aircraft does as the battery runs low.|P1|
|B5.6|Pack thermal model and charge behaviour|Battery heating and charging behaviour (only if we simulate ground operations).|P2|
|B5.7|Resolution of conflicting public figures (power 84 vs. 88 kW, endurance 15 vs. 20 min)|Public sources disagree on power and flight time. We want the real figures.|P2|

### B6. Aerodynamics

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|B6.1|Drag coefficients or CdA per body axis with reference areas; drag polar or wind-tunnel/CFD data|How much air resistance the aircraft has in each direction. Determines top speed and how it slows down.|P0|
|B6.2|Vertical-descent behaviour and vortex-ring-state onset|Descending straight down can cause the aircraft to lose lift in its own airflow. We need to know when.|P1|
|B6.3|Ground effect (thrust gain vs. height)|Near the ground, the aircraft gets extra lift. Matters for takeoff and landing.|P1|
|B6.4|Aerodynamic moments at speed (pitch-up, weathervane)|Forces that try to tilt or turn the aircraft at speed.|P1|
|B6.5|Gust and wind response; certified wind limits|How wind and gusts push the aircraft, and the limits it is certified for.|P1|
|B6.6|Prop inflow effects on thrust in forward flight|How the propellers perform when the aircraft is moving forward.|P1|

### B7. Sensors (physical models)

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|B7.1|Sensor suite: models, counts, mounting pose|Which sensors the aircraft has and where each is mounted.|P0|
|B7.2|Per sensor: rate, noise density, bias and drift, scale factor, latency, saturation|Accuracy, noise and delays of each sensor. Perfect virtual sensors would hide problems.|P0|
|B7.3|Vibration environment at the IMU and filtering applied|Vibration affects the motion sensors, and the computer filters it. We need to match both.|P1|
|B7.4|Realistic sensor-fault modes|How real sensors fail, so we can recreate it.|P1|

### B8. Pilot controls (physical)

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|B8.1|Device list: joystick axes and travel, throttle lever (spring-return or detent), yaw pedals, every button and switch with function|A list of every control the pilot touches, how far it moves and what it does.|P0|
|B8.2|Raw sensor characteristics: resolution, deadband, noise, calibration, centring|Electrical behaviour of the controls, such as dead zones and noise.|P1|
|B8.3|Force/spring rates|How heavy the controls feel, if we build a physical cockpit.|P2|
|B8.4|Switch guards and sequences (arm, mode, parachute)|Switch guards and sequences, such as how the parachute is deployed.|P1|
|B8.5|Cockpit geometry for the VR rig: stick, lever, seat and eye-to-instrument distances|Distances in the cockpit, so the VR view puts everything where the pilot expects.|P1|

### B9. Failures and safety systems

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|B9.1|Motor-out cases the aircraft is designed for (single and multiple) and the response to each|Which motor failures the aircraft is designed to survive and what it does. A key training scenario.|P0|
|B9.2|Failure taxonomy (motor, ESC, battery pack, sensor, FC lane) with the response to each: degrade, warn, land, parachute|Every kind of failure and the aircraft's reaction to each.|P0|
|B9.3|Test data for the one-motor-out flight: attitude excursion, altitude loss, recovery time|Measured results of the one-motor-out test. Gives a target to check the simulation against.|P1|
|B9.4|ParaZero parachute: deploy trigger and logic, minimum altitude, deploy time, descent rate, automatic vs. manual|How and when the parachute deploys, and how fast the aircraft descends under it.|P1|
|B9.5|Behaviour on loss of power, comms, or one FC lane|What happens if power, communication or one flight computer is lost.|P1|
|B9.6|Emergency procedures trained for; official failure scenarios to include|The emergencies pilots are trained for, so the simulator includes the right scenarios.|P1|

### B10. Operating envelope

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|B10.1|Certified limits: speed (software and structural), altitude, climb/descent rate, bank/tilt, wind/gust|The limits the aircraft is certified to operate within.|P1|
|B10.2|Pilot weight limits, operating temperature range, density-altitude effects|Limits on pilot weight, temperature and altitude effects on performance.|P1|
|B10.3|Hover thrust margin and throttle-to-thrust across the weight range|How much spare power there is in a hover, at different weights.|P1|
|B10.4|Performance validation targets: climb rate vs. mass, hover power, range, endurance, top speed vs. pilot weight|Published performance figures, which we use to check the simulator is realistic.|P1|

### B11. Validation data

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|B11.1|Flight telemetry (sticks, attitude, rates, motor commands, battery) covering hover, step inputs, yaw, climb/descent, motor-out|Recordings from real flights. The best way to prove the simulated aircraft flies like the real one.|P0|
|B11.2|Flight video with matching logs|Video matched to the recordings, which helps interpret them.|P2|
|B11.3|Bench data: motor+prop thrust stand, tethered hover|Measurements from tests on the ground, which give motor and propeller data directly.|P1|
|B11.4|Test-pilot description of handling and quirks|Pilots' own description of how the aircraft feels and any quirks.|P2|

## Part C. Legal and process

|ID|Item|Why we need it (plain English)|Pri|
|-|-|-|-|
|C1|NDA/IP terms: what may be used, shipped to customers, kept internal|The legal agreement covering what we may use and share.|P0|
|C2|Licence terms for redistributing the FC/avionics binary, including inside a container image shipped to a customer|Whether we're allowed to include their software in a product we deliver to customers.|P0|
|C3|Export-control or security restrictions on the binary and data|Whether export laws or security rules limit who can see or hold the data.|P1|
|C4|Name, logo and trademark use of "Jetson ONE"|Permission to use the 'Jetson ONE' name and logo.|P1|
|C5|Liability/support terms when the binary runs outside Jetson-qualified hardware|Who is responsible if their software misbehaves in a setting they didn't test.|P1|
|C6|Firmware and parameter versions targeted; how updates are delivered|Which software version we're building against and how updates are delivered.|P1|
|C7|Existing Jetson simulator or digital twin that could be used or inspected|If Jetson already has a simulator, we may be able to reuse or compare against it.|P1|
|C8|Named engineering contact; review cycle for validating the sim against the real aircraft|A named person to ask questions of, and a way to have Jetson check our results.|P1|
|C9|Intended use of the sim and regulatory expectations (training credit, certification, demo), which set the required fidelity and validation evidence|What the simulator will be used for (training, certification or demos). The purpose sets how accurate it must be.|P1|

## Part D. Contingency if hosting their FC is refused

Paths are ordered from closest to Jetson's behaviour to least. Each lists what we need in addition to Part B (always required).

|Path|Description|Plain English|Needed beyond Part B|
|-|-|-|-|
|D1|**Reduced binary**: no FC library, but a compiled estimator, mixer or control-law module, or a library with a restricted API|Jetson gives us a smaller piece of their software instead of the whole thing. We build the rest.|A2.1-A2.5, A3.1-A3.3, the I/O of the module|
|D2|**Interface spec + black-box tests**: no code. We write an FC against their ICD and verify against recorded data|No software at all. We write our own flight computer and check it against their specification and recordings.|A4, A5, A7 (full signal and bus specs), A8.4 (golden vectors), B11.1 (flight logs), A9.1-A9.2|
|D3|**HIL**: Jetson FC unit on the bench, our sim feeds it over the real bus and reads real motor commands|Jetson lends us a real flight computer, and the simulator plugs into it like an aircraft. Most realistic, but needs hardware.|Loan or purchase of an FC unit (and avionics unit), harness/connector pinout, bus spec (A7.1), power requirements, sensor injection method (bus-level injection vs. analog), ESC output capture method, bench safety interlocks, A3.3 arming behaviour with no real sensors present|
|D4|**Parameters and spec only**: we reimplement control from their documented structure|We rebuild their flight computer from their documentation. Accuracy depends on how complete it is.|A9.1-A9.11 in full, B4 (motor lag), B9.1-B9.2, B11.1|
|D5|**System identification from logs only**: no spec; fit our own FC and mode behaviour to flight data|We work out how it behaves by studying flight recordings, with no documentation.|B11.1 with enough variety (step responses, doublets, mode changes, motor-out), sensor and motor logs sampled at the FC rate, mode flags in the logs|
|D6|**Independent FC on public data**: our reference FC, tuned to published performance. Flagged in the product as non-representative of Jetson's software|We build our own flight computer from public information. It won't match Jetson's software and will be labelled as such.|B1, B3, B4, B6, B10 (best available values)|

Avionics contingency:

|Path|Description|Plain English|Needed|
|-|-|-|-|
|D7|Display binary or web/embedded UI delivered as a self-contained program|Jetson gives us the cockpit screen software as a ready-to-run program.|A6.1-A6.3, bus spec A7|
|D8|Recreate the UI|We redraw the screen ourselves from screenshots and a description of every page.|A6.4-A6.5 (page inventory, alert list), screenshots or screen recordings of every page and alert state, fonts and colour palette (A6.7), unit conventions, the signal list the display shows (A5.4)|
|D9|Generic PFD, labelled as such|A generic flight display. Useful, but not the real Jetson screen.|Only the instrument list and units|

Requests to make in any contingency, even if the binary is refused:

* Golden vectors (A8.4) or recorded FC input/output traces. These can be shared without exposing source and let us measure how close our substitute is.
* Failure-response summary (B9) and the mode list (A9.1).
* Review access: a Jetson engineer who can fly the sim and report deviations (C8).

## Part E. Decisions the answers determine

|Question|Decides|Plain English|
|-|-|-|
|Can a SITL/Linux build exist?|SITL (A1.1) vs. HIL (D3)|Whether their software can run on an ordinary computer, or only on aircraft hardware. If not, we need the real unit.|
|Step function with injectable time, or threaded black box?|Whether faster-than-real-time and deterministic tests are possible; adapter complexity|Whether we can control their software's clock. If we can, tests run faster and are repeatable.|
|Avionics as render-to-texture component, or full-screen only?|Whether it can be embedded in a VR cockpit|Whether the cockpit screen can be placed inside our virtual cockpit, or only runs as a full-screen app.|
|Raw sensors in, or fused state in?|How much sensor modelling (B7) the engine must carry; whether a sim-truth state can be fed in early|Whether the computer wants raw sensor data (more work for us, more realistic) or a finished position and attitude.|
|Motor command semantics (normalised, RPM, torque)?|The motor model interface and the number of actuator channels (8)|What the motor numbers mean, which sets how we build the motor model.|
|How are the 8 motors and 3 lanes exposed?|Whether the engine models 4 stacked-pair rotors or 8 independent rotors|How closely we need to model the paired motors and the three-way computer redundancy.|
|Licence permits redistribution in a customer image?|Packaging and distribution of the binary|Whether we can ship their software to customers at all.|



