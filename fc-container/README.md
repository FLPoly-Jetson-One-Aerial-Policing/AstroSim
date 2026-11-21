# AstroSim flight computer container

PX4 SITL (software-in-the-loop) plus the **FC bridge**, in one image. The bridge connects to the physics engine over the
engine protocol ([../shared/proto/PROTOCOL.md](../shared/proto/PROTOCOL.md)): it reads the aircraft state, feeds
the flight computer (FC) and sends the engine the power for each of the 8 motors. It also publishes avionics telemetry
(mode, armed, GPS, attitude, altitude) on UDP port 8766 for displays
([../shared/proto/FC_TELEMETRY.md](../shared/proto/FC_TELEMETRY.md)). The engine's protocol is unchanged.

The bridge talks to the FC through a generic interface ([../shared/fc/FC_INTERFACE.md](../shared/fc/FC_INTERFACE.md)),
and PX4 is one adapter behind it ([px4/README.md](px4/README.md)). PX4 and its ports stay inside the container.

The FC has exactly two flight modes, **GPS hold** and **no-GPS hold**. Both command the aircraft's tilt angle from the
roll and pitch sticks; there is no rate (acro) mode.

> ## Temporary: synthesized sensors and spoofed pilot input
>
> **Sensors are synthesized.** There is no sensor model yet, so the sensors (IMU, GPS, barometer, magnetometer) are
> computed from the engine's true state. They are perfectly aligned and never fail. A tiny deterministic dither is added
> so PX4 does not flag them as stale.
>
> **Pilot input is a script.** There is no real pilot yet, so a keyframe script in the config stands in for one (arm,
> climb to about 10 m, hold, switch mode, descend, land).
>
> Both are flagged at startup, in the stats log line (`sensors=SYNTHESIZED`, `pilot=SPOOFED`) and in the telemetry
> provenance flags (`SENSORS_SYNTHESIZED`, `PILOT_SPOOFED`). Both are to be replaced by the sensor/GPS and pilot input
> tickets.

## Build

From the repository root (the build context must contain `shared/` and `fc-container/`):

```sh
docker build -t astrosim-fc -f fc-container/Dockerfile .
```

`podman build` works the same. The first build is slow because it compiles PX4 (pinned release tag, cached as its own
stage). The unit tests run inside the build under ASan/UBSan, and a failing test fails the build.

## Run

The FC needs a running engine ([../gazebo-container/README.md](../gazebo-container/README.md)). It reaches the engine
at `[engine] host`/`port` from the config (default `127.0.0.1:8765`), or `ASTROSIM_ENGINE_HOST` /
`ASTROSIM_ENGINE_PORT`.

**In a pod (shared network).** The containers share `localhost`, so the default engine host works:

```sh
podman pod create --name astrosim -p 8765:8765/udp -p 8766:8766/udp
podman run -d --pod astrosim --name engine astrosim-engine
podman run -d --pod astrosim --name fc astrosim-fc
```

**On a shared network.** Give the engine a name the FC can resolve:

```sh
docker network create astrosim
docker run -d --network astrosim --name engine -p 8765:8765/udp astrosim-engine
docker run -d --network astrosim --name fc -e ASTROSIM_ENGINE_HOST=engine -p 8766:8766/udp astrosim-fc
```

Start both containers together (see [Known issues](#known-issues)). The container runs as a non-root user and stops
cleanly on `stop`. If either the bridge or PX4 exits, the other is stopped and the container exits. Invalid
configuration or aircraft data exits with status 2.

Check it from outside the containers:

```sh
docker exec engine astrosim-probe --subscribe          # aircraft state: should climb to about 10 m, then land
docker exec fc astrosim-fc-probe --telemetry           # avionics: mode, armed, GPS, attitude, altitude
```

`astrosim-fc-probe [--host H] [--port P] --telemetry [--count N]` defaults to `127.0.0.1:8766` and prints one line per
telemetry message; `--count N` stops after N messages. The lines show the active mode switching from GPS hold to
no-GPS hold, armed, GPS fix and satellites, attitude, altitude and vertical speed, and the provenance flags.

## Ports

| Port | Where | Purpose |
|-|-|-|
| 8766/udp | published | FC avionics telemetry for displays |
| 4560/tcp | inside the container only | PX4 simulator link (HIL, lockstep); the bridge listens, PX4 connects |
| 14540/udp, 14580/udp | inside the container only | MAVLink link between the bridge (14540) and PX4's onboard instance (14580) |

The ports on 4560, 14540 and 14580 are PX4's SITL defaults and can be changed under `[px4]`. PX4's GCS port (14550) is
not exposed.

## Configuration

`config/fc.toml` (installed at `/opt/astrosim/config/fc.toml`) holds the defaults; its comments describe every key. The
path can be changed with `ASTROSIM_CONFIG` and the aircraft file (default `/opt/astrosim/config/jetson_one.toml`) with
`ASTROSIM_AIRCRAFT`. The PX4 airframe is generated from the aircraft file at every start.

| Section | Contents |
|-|-|
| `[engine]` | engine host and port |
| `[timing]` | control, pilot and telemetry rates, subscribe keepalive, FC timeout (zero power after this much silence), stats period |
| `[telemetry]` | bind address, port (8766), max subscribers, subscriber timeout |
| `[fc]` | `adapter` (`px4`, or `null` for tests only) and `sensor_source` (`synthesized_temporary` is the only accepted value) |
| `[home]`, `[mag]` | geodetic position of the world origin and the magnetic field, used by the synthesized sensors |
| `[px4]` | the ports above, `no_gps_mode` (`altctl` or `stabilized`), `max_tilt_deg`, `max_yaw_rate_dps`, `sensor_dither` |
| `[log]` | `level` (trace, debug, info, warn, error) |
| `[pilot]`, `[[pilot.script]]` | the spoofed pilot script |

Every scalar key can be overridden by an environment variable named `ASTROSIM_<SECTION>_<KEY>` (upper case), for example
`ASTROSIM_ENGINE_HOST`, `ASTROSIM_TELEMETRY_PORT` or `ASTROSIM_PX4_NO_GPS_MODE`. The pilot script is set only in the file.

**The spoofed pilot script.** Each `[[pilot.script]]` entry is a keyframe with `time_s` (engine simulation time),
`roll`, `pitch`, `yaw` (angle demands, -1 to 1), `throttle` (0 to 1; for PX4, 0.5 holds altitude, above climbs, below
descends), `mode` (`gps_hold` or `no_gps_hold`) and `arm`. With `interpolation = "hold"` values hold until the next
keyframe; `"linear"` ramps between them. The default script arms at 20 s, climbs to about 10 m, holds, switches to
no-GPS hold at 50 s, pitches forward and back, then descends and lands.

To use a different script, copy `config/fc.toml`, edit it and mount it:

```sh
docker run -d ... -v ./my-fc.toml:/etc/astrosim/fc.toml:ro -e ASTROSIM_CONFIG=/etc/astrosim/fc.toml astrosim-fc
```

## Known issues

- The script follows absolute engine simulation time, so start both containers together. A late FC starts partway
  through the script.
- The engine and PX4 must be restarted together. PX4 does not cope with the engine's clock going back to zero.
- The PX4 gains are fitted to the placeholder aircraft data, not a real Jetson ONE.
- The EKF heading is about 3 degrees off, probably because of the magnetic declination (the synthesized field has none).
- The GCS port is not exposed, so a ground station cannot connect.

## Writing a new adapter

To put a different flight computer (ArduPilot, the real Jetson computer) behind the bridge, write one class derived
from `FlightComputer`. See [../shared/fc/FC_INTERFACE.md](../shared/fc/FC_INTERFACE.md), and
[px4/README.md](px4/README.md) for a worked example.
