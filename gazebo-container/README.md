# AstroSim physics engine container

Headless Gazebo (`gz-sim-server`) plus a gateway that bridges it to a fixed binary UDP protocol. The flight
computer sends **Control** (power for 8 motors); displays receive **State** (pose, velocity, rates, motor speeds,
sim time). Gazebo and its transport never leave the container: the only interface is one UDP port. The protocol is
specified in [../shared/proto/PROTOCOL.md](../shared/proto/PROTOCOL.md).

The world (`worlds/default.sdf`) and model (`models/jetson_one`) are placeholders (four rotors; protocol motors
4-7 are ignored by the model).

## Build

From the repository root, on a Linux builder (all compilation and tests run inside the image build; the unit tests
run under ASan/UBSan and a failing test fails the build):

```sh
docker build -t astrosim-engine -f gazebo-container/Dockerfile .
```

## Run

Works on any host OS with Docker:

```sh
docker run --rm -p 8765:8765/udp astrosim-engine
```

The container runs as a non-root user and stops cleanly on `docker stop` (motors are zeroed first).

## Configuration

`config/gateway.toml` (installed at `/opt/astrosim/config/gateway.toml`) holds the defaults. Every scalar key can
be overridden by an environment variable `ASTROSIM_<SECTION>_<KEY>` (upper case). The `[[motor]]` array is file
only. Invalid configuration prints an error naming the key and the container exits with status 2.

| Key | Default | Environment variable | Meaning |
|-|-|-|-|
| `network.port` | 8765 | `ASTROSIM_NETWORK_PORT` | UDP port |
| `network.bind` | "0.0.0.0" | `ASTROSIM_NETWORK_BIND` | IPv4 bind address |
| `network.max_subscribers` | 8 | `ASTROSIM_NETWORK_MAX_SUBSCRIBERS` | Concurrent State subscribers |
| `timing.state_rate_hz` | 60 | `ASTROSIM_TIMING_STATE_RATE_HZ` | State broadcast rate |
| `timing.deadman_timeout_s` | 0.5 | `ASTROSIM_TIMING_DEADMAN_TIMEOUT_S` | No Control for this long: motors zeroed |
| `timing.lease_timeout_s` | 1.0 | `ASTROSIM_TIMING_LEASE_TIMEOUT_S` | Controller silent this long: another may take over |
| `timing.subscriber_timeout_s` | 5 | `ASTROSIM_TIMING_SUBSCRIBER_TIMEOUT_S` | Subscriber silent this long: dropped |
| `timing.engine_stale_s` | 0.5 | `ASTROSIM_TIMING_ENGINE_STALE_S` | Odometry older than this clears ENGINE_OK |
| `timing.stats_interval_s` | 10 | `ASTROSIM_TIMING_STATS_INTERVAL_S` | Period of the stats log line |
| `engine.model` | "jetson_one" | `ASTROSIM_ENGINE_MODEL` | Model name |
| `engine.odometry_topic` | "/model/jetson_one/odometry" | `ASTROSIM_ENGINE_ODOMETRY_TOPIC` | gz Odometry topic |
| `engine.motor_command_topic` | "/jetson_one/gazebo/command/motor_speed" | `ASTROSIM_ENGINE_MOTOR_COMMAND_TOPIC` | gz Actuators command topic |
| `engine.motor_speed_topic` | "/jetson_one/joint_state" | `ASTROSIM_ENGINE_MOTOR_SPEED_TOPIC` | gz Model topic with the rotor joint velocities; empty disables motor speed feedback |
| `engine.motor_joint_name` | "jetson_one/rotor_{actuator}_joint" | `ASTROSIM_ENGINE_MOTOR_JOINT_NAME` | Joint name per motor; `{actuator}` is replaced by the actuator index |
| `engine.motor_speed_scale` | 10 | `ASTROSIM_ENGINE_MOTOR_SPEED_SCALE` | Joint velocity times this = rotor rad/s (the motor model's `rotorVelocitySlowdownSim`) |
| `log.level` | "info" | `ASTROSIM_LOG_LEVEL` | trace, debug, info, warn, error |
| `[[motor]]` x8 | identity, 800 | (file only) | Protocol motor i (array order) to `actuator_index`, and `max_rad_s` at power 1.0 |

To use a custom file, mount it and point `ASTROSIM_CONFIG` at it:

```sh
docker run --rm -p 8765:8765/udp -v $PWD/my.toml:/etc/astrosim.toml:ro \
  -e ASTROSIM_CONFIG=/etc/astrosim.toml astrosim-engine
```

Changing `network.port` also requires publishing that port (`-p 9000:9000/udp`).

## astrosim-probe

A small client installed in the image (no Gazebo dependency), usable with `docker exec`:

```sh
docker exec <container> astrosim-probe --subscribe --count 5     # print State lines
docker exec <container> astrosim-probe --control 0.9 0.9 0.9 0.9 0 0 0 0 --duration 4 [--rate 100]
docker exec <container> astrosim-probe --health [--timeout 3]    # prints "engine healthy", exit 0 on State with ENGINE_OK
```

Common options: `--host H` (default 127.0.0.1), `--port P` (default 8765; use the configured port if changed).

## Health check

The image's `HEALTHCHECK` runs `astrosim-probe --health` (10 s interval, 30 s start period). It passes when the
gateway answers with a State whose ENGINE_OK flag is set, meaning gz-sim is running and odometry is fresh.
If the port was changed, adjust the check (`--health-cmd`) accordingly.
