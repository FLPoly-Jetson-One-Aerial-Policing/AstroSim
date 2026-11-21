# AstroSim

AstroSim is a flight simulator for the **Jetson ONE**, a single-seat electric aircraft with eight motors. The goal is a realistic training simulator: the aircraft should fly like the real thing and react to emergencies, such as a motor failing, the way the real one does. It is designed for virtual reality (VR) as well as ordinary screens.

## Project status

**Early development.** Only the physics engine works so far: the part that calculates how the aircraft moves. The flight computer and the visual display are still to be built, so there is nothing you can see or fly yet.

| Part | What it does | Status |
|-|-|-|
| Physics engine | Works out where the aircraft goes, given how hard each motor is pushing | **Working** (with a placeholder aircraft model) |
| Flight computer | Turns the pilot's controls into motor commands, as the real aircraft's onboard computer does | Not started (will start with the open-source PX4 autopilot) |
| Display | Shows the aircraft and the world, on a screen or in a VR headset | Not started (likely Unity) |
| Aircraft model | The simulated Jetson ONE: size, weight, motor positions | Generated from an aircraft data file (best-guess values; to be replaced with manufacturer data) |

## How it fits together

```
 Pilot controls ──▶ Flight computer ──(power for each of the 8 motors)──▶ Physics engine
                                                                               │
                         Display ◀──(position, speed, orientation)─────────────┘
```

Each part is a separate program that talks to the others over a local network connection. This means:
- Any part can be replaced without touching the others. For example, PX4 can later be swapped for the real Jetson flight computer.
- The parts can run on different computers.

## What you need

- **To run it:** [Docker](https://www.docker.com/products/docker-desktop/) on Windows, macOS or Linux. Nothing else.
- **To build it:** a Linux machine (or an automated build pipeline) with Docker. Developers, see the links below.

## Trying the physics engine

Once a developer has built it, run this in a terminal:

```
docker run --rm --name astrosim -p 8765:8765/udp astrosim-engine
```

Leave it running. Then, in a second terminal, check that it works:

```
docker exec astrosim astrosim-probe --health
```

It should print `engine healthy`. To watch the simulated aircraft's position update live:

```
docker exec astrosim astrosim-probe --subscribe
```

Press `Ctrl+C` to stop either command.

## Documentation

| If you want to... | Read |
|-|-|
| Build, configure or run the engine | [gazebo-container/README.md](gazebo-container/README.md) |
| Write a flight computer or display that talks to the engine | [shared/proto/PROTOCOL.md](shared/proto/PROTOCOL.md) |
| Understand the background research and design decisions | [docs/](docs/) |
| Edit the aircraft data file (schema, units, keys) | [shared/aircraft/AIRCRAFT.md](shared/aircraft/AIRCRAFT.md) |
