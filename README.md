# AstroSim

AstroSim is a flight simulator for the **Jetson ONE**, a single-seat electric aircraft with eight motors. The goal is a realistic training simulator: the aircraft should fly like the real thing and react to emergencies, such as a motor failing, the way the real one does. It is designed for virtual reality (VR) as well as ordinary screens.

## Project status

**Early development.** The physics engine and a first flight computer work. The flight computer flies a scripted test flight on its own; real pilot controls and the visual display are still to be built, so there is nothing you can see or fly yourself yet.

| Part | What it does | Status |
|-|-|-|
| Physics engine | Works out where the aircraft goes, given how hard each motor is pushing | **Working** (with a placeholder aircraft model) |
| Flight computer | Turns the pilot's controls into motor commands, as the real aircraft's onboard computer does | **Working, temporary inputs**: runs the open-source PX4 autopilot behind a generic flight computer interface, with two flight modes (GPS hold and no-GPS hold, both steered by tilt angle). The pilot's controls are a script and the sensors are computed from the engine's true state; both are stopgaps |
| Startup dashboard | Starts and stops the simulation from a web page and shows whether each part is running | **Working** |
| Display | Shows the aircraft and the world, on a screen or in a VR headset | Not started (likely Unity) |
| Aircraft model | The simulated Jetson ONE: size, weight, motor positions | Generated from an aircraft data file (best-guess values; to be replaced with manufacturer data) |

## How it fits together

```
 Pilot controls ──▶ Flight computer ──(power for each of the 8 motors)──▶ Physics engine
                                                                               │
                         Display ◀──(position, speed, orientation)─────────────┤
                            ▲                                                  │
                            └──(flight data: mode, GPS, altitude, attitude)── Flight computer
```

A small program called the orchestrator starts the physics engine and the flight computer, watches that they stay healthy, restarts them if they fail, and shows their status on a web page (the dashboard).

Each part is a separate program that talks to the others over a local network connection. This means:
- Any part can be replaced without touching the others. For example, PX4 can later be swapped for the real Jetson flight computer.
- The parts can run on different computers.

## Starting the simulator

You need Docker or Podman installed and running, and the simulator images built once (a developer does this; see the links below).

1. Run `astrosim` (on Windows, `astrosim.exe`). A page opens in your web browser. If it does not, open `http://127.0.0.1:8770`.
2. Press **Start**.
3. Watch the list of parts. Each shows a status:
   - **Starting**: the part is launching and not ready yet.
   - **Ready**: the part is working. When both are ready the page says the simulation is running.
   - **Recovering**: something stopped working and AstroSim is restarting it. This usually fixes itself within a few seconds.
   - **Failed**: AstroSim gave up. Open the details on the page to see what went wrong, then press **Start** to try again.
   - **Not installed yet**: an image has not been built. The page shows the command a developer needs to run.
4. To quit, press `Ctrl+C` in the window that `astrosim` is running in, or close that window. This stops the simulation. Closing the browser tab does not.

More options and how it works: [orchestrator/README.md](orchestrator/README.md).

## What you need

- **To run it:** [Docker](https://www.docker.com/products/docker-desktop/) or [Podman](https://podman.io/) on Windows, macOS or Linux, running, plus the `astrosim` program and the simulator images, which a developer builds once. Nothing else.
- **To build it:** a Linux machine (or an automated build pipeline) with Docker. Developers, see the links below.

## Running the containers by hand (developers)

The orchestrator does this for you. The commands below start the parts yourself, which is useful when working on one of them. Stop the orchestrator first: it uses the same ports. After a crash of the orchestrator, `astrosim --cleanup` removes the containers it left running (`astrosim --prune` also removes its network); see [Shutting down](orchestrator/README.md#shutting-down).

### Trying the physics engine

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

### Trying the flight computer

You need both the engine and the flight computer images built. Run them side by side in a pod (a pod lets containers share one network connection; this uses Podman, and Docker users can use a shared network instead, see [fc-container/README.md](fc-container/README.md)):

```
podman pod create --name astrosim -p 8765:8765/udp -p 8766:8766/udp
podman run -d --pod astrosim --name engine astrosim-engine
podman run -d --pod astrosim --name fc astrosim-fc
```

Start them together: the flight computer follows a script timed from the engine's clock. Then watch the aircraft climb to about 10 metres, hold, and land:

```
docker exec engine astrosim-probe --subscribe
```

and the flight computer's own data (mode, armed, GPS, attitude, altitude):

```
docker exec fc astrosim-fc-probe --telemetry
```

(Use `podman exec` if you started the containers with Podman; the commands are otherwise the same.) The flight data is marked as using synthesized sensors and a spoofed pilot. Press `Ctrl+C` to stop the probes.

## Documentation

| If you want to... | Read |
|-|-|
| Run, configure or build the startup dashboard (orchestrator) | [orchestrator/README.md](orchestrator/README.md) |
| Build, configure or run the engine | [gazebo-container/README.md](gazebo-container/README.md) |
| Write a flight computer or display that talks to the engine | [shared/proto/PROTOCOL.md](shared/proto/PROTOCOL.md) |
| Build, configure or run the flight computer | [fc-container/README.md](fc-container/README.md) |
| Plug in a different flight computer | [shared/fc/FC_INTERFACE.md](shared/fc/FC_INTERFACE.md) |
| Show flight computer data on a display | [shared/proto/FC_TELEMETRY.md](shared/proto/FC_TELEMETRY.md) |
| Understand the background research and design decisions | [docs/](docs/) |
| Edit the aircraft data file (schema, units, keys) | [shared/aircraft/AIRCRAFT.md](shared/aircraft/AIRCRAFT.md) |
