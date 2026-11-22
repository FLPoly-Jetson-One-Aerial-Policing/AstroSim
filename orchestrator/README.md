# AstroSim orchestrator

The operational dashboard and orchestrator: one program, `astrosim`, that starts, supervises and stops the simulation
containers (physics engine and flight computer) and serves a small web page with a Start/Stop button and the status of
each part. It checks for a container runtime and the images, launches the containers in the right order, watches that
they are healthy, restarts them when they fail, and removes them when you quit.

It runs **natively on the host** (Windows, macOS or Linux), not in a container. [build.Dockerfile](build.Dockerfile) is
used only to build and test it. It is plain Go with no dependencies outside the standard library. The trainer
interface is a separate piece and is not part of this program.

## Quick start

1. Build the orchestrator and the two images once (needs a Linux machine with Podman or Docker; see
   [Building and testing](#building-and-testing)):

   ```sh
   orchestrator/build.sh
   podman build -t astrosim-engine -f gazebo-container/Dockerfile .
   podman build -t astrosim-fc -f fc-container/Dockerfile .
   ```

2. Run `./astrosim` from the repository root (or `astrosim.exe`). The dashboard opens in your browser at
   `http://127.0.0.1:8770`. Press **Start**.
3. Press `Ctrl+C` (or close the window) to stop the containers and quit. See [Shutting down](#shutting-down) for
   every way of ending it and what happens after a crash.

Closing the browser tab does not stop the simulation. Use `--autostart` to skip the button and `--no-browser` to not
open a page.

## Configuration

Settings come from four layers; a later one overrides an earlier one: built-in defaults, the optional JSON file
(`--config`), `ASTROSIM_ORCH_*` environment variables, command-line flags. Flags accept `-` or `--`.

| Flag | Environment variable | JSON key | Default | Meaning |
|-|-|-|-|-|
| `--listen` | `ASTROSIM_ORCH_LISTEN` | `listen` | `127.0.0.1:8770` | Dashboard address, `host:port` |
| `--allow-remote` | `ASTROSIM_ORCH_ALLOW_REMOTE` | `allowRemote` | false | Allow a non-loopback `--listen` (no authentication) |
| `--runtime` | `ASTROSIM_ORCH_RUNTIME` | `runtime` | `auto` | `auto` (tries podman, then docker), `podman` or `docker` |
| `--runtime-path` | `ASTROSIM_ORCH_RUNTIME_PATH` | `runtimePath` | none | Path of the podman or docker executable |
| `--network` | `ASTROSIM_ORCH_NETWORK` | `network` | `bridge` | `bridge`, or `host` (Linux only) |
| `--publish-addr` | `ASTROSIM_ORCH_PUBLISH_ADDR` | `publishAddr` | `127.0.0.1` | Host address the container ports are published on (bridge mode) |
| `--no-browser` | `ASTROSIM_ORCH_NO_BROWSER` | `noBrowser` | false | Do not open a browser |
| `--autostart` | `ASTROSIM_ORCH_AUTOSTART` | `autostart` | false | Start as soon as the first check passes |
| `--restart-max` | `ASTROSIM_ORCH_RESTART_MAX` | `restartMax` | 3 | Automatic restarts before giving up (at least 1) |
| `--no-fail` | `ASTROSIM_ORCH_NO_FAIL` | `noFail` | false | Never give up restarting (see [Restart policy](#restart-policy)) |
| `--log-level` | `ASTROSIM_ORCH_LOG_LEVEL` | `logLevel` | `info` | `debug`, `info`, `warn` or `error` |
| `--config` | `ASTROSIM_ORCH_CONFIG` | n/a | none | JSON configuration file |
| `--version` | n/a | n/a | n/a | Print the version and exit |
| `--cleanup` | n/a | n/a | false | Remove leftover AstroSim containers and exit (see [Shutting down](#shutting-down)) |
| `--prune` | n/a | n/a | false | Like `--cleanup`, and also remove the AstroSim network |

Booleans in environment variables use Go's syntax (`1`, `true`, `0`, `false`). Unknown JSON keys are an error.

`--cleanup`, `--prune` and `--version` are one-shot commands and exist only as flags, with no environment variable or
JSON key: a setting left in the environment or a file would turn every launch into a cleanup. `--cleanup` and `--prune`
cannot be combined with `--autostart`, whether that comes from the flag, the environment or the file.

Per-component overrides can only be set in the JSON file, under `components`, keyed by component ID (`engine`, `fc`).
They adjust a built-in component; they cannot define a new one. Zero or missing values keep the built-in setting:

```json
{
  "listen": "127.0.0.1:9000",
  "restartMax": 5,
  "components": {
    "engine": {
      "image": "localhost/astrosim-engine:dev",
      "env": { "ASTROSIM_LOG_LEVEL": "debug" },
      "readyTimeout": "90s"
    },
    "fc": {
      "livenessTimeout": "8s",
      "stopTimeout": "5s"
    }
  }
}
```

| Component key | Meaning |
|-|-|
| `image` | Container image reference |
| `env` | Environment variables merged over the component's own; a key set here wins |
| `readyTimeout` | How long the component may take to become ready (Go duration, such as `"90s"`) |
| `livenessTimeout` | How long a ready component may be silent before it counts as unhealthy |
| `stopTimeout` | Grace period given to the container on stop |
| `disabled` | Remove the component from the stack (not allowed while another enabled component depends on it) |

## Shutting down

The orchestrator stops and removes both containers on every normal way of quitting. Each container's entrypoint
forwards the stop signal to its processes and waits for them, so nothing is left running inside either.

| How it ends | What happens |
|-|-|
| **Stop** button | Stops and removes the containers; the orchestrator keeps running |
| `Ctrl+C` (SIGINT) | Stops and removes the containers, then exits |
| `kill` (SIGTERM) | Same as `Ctrl+C` |
| Terminal closed (SIGHUP, Linux and macOS) | Same as `Ctrl+C`. Logging to the closed terminal is ignored, so it cannot get in the way |
| Windows console closed, logoff or shutdown | Go reports these as SIGTERM, so it is handled like `kill`. Not yet verified on Windows hardware |
| Second `Ctrl+C` while stopping | Exits at once (status 130). Containers may be left running |
| Crash, `kill -9`, power loss | **Abnormal.** Nothing can run a teardown, so the containers keep running until you run `astrosim --cleanup` or start `astrosim` again, which removes them first |

Leftovers can therefore never conflict with the next start: every launch removes all containers labelled
`astrosim.managed=true` while holding the [instance lock](#single-instance-and-lock-file).

### `--cleanup` and `--prune`

`astrosim --cleanup` removes leftover managed containers and exits, without serving the dashboard or opening a browser.
It prints one line per removed container, or `nothing to clean up`, and exits 0 (1 on failure). Use it after a crash
if you do not want to start AstroSim again.

The `astrosim` network is deliberately kept after a run and reused by the next one. `astrosim --prune` does what
`--cleanup` does and then removes the networks labelled `astrosim.managed=true`; the next start creates it again.

Both take the instance lock first. If an AstroSim is running they print `AstroSim is running at <url>; stop it
before cleaning up` and exit 1, leaving the session untouched. They only touch resources carrying the label. These are
never removed, and need removing by hand if you want them gone: containers you started yourself (`engine`, `fc`), the
top-level README's `astrosim` pod, and an `astrosim` network created before networks were labelled
(`podman network rm astrosim`).

## Network modes

**`bridge` (default, all operating systems).** The orchestrator creates a user network named `astrosim`, labelled
`astrosim.managed=true`, and keeps it between runs (remove it with `--prune`). Each
container gets a network alias equal to its component ID, so the flight computer reaches the engine as `engine`. The
engine's UDP port 8765 and the flight computer's 8766 are published on `--publish-addr` (`127.0.0.1` by default), and
the orchestrator checks health through those published ports from the host.

**`host` (Linux only).** The containers share the host's network, with no network alias or published ports; the flight
computer reaches the engine on `127.0.0.1`. Use it if UDP through the published ports is slow or unreliable, as can
happen with rootless Podman. Caveats:

- The ports of everything in the containers become host ports. Besides 8765 and 8766 this includes PX4's 4560 (TCP,
  simulator link), 14540 and 14580 (MAVLink) and others. Anything else on the host using those ports clashes, and the
  startup check only looks at ports 8765 and 8766.
- Gazebo's transport would otherwise find other Gazebo programs on the machine. In host mode the engine is given
  `GZ_PARTITION=astrosim` and `GZ_IP=127.0.0.1` to keep it to itself. See
  [Known limitations](#known-limitations--unverified).

Containers are named `astrosim-<id>` and labelled `astrosim.managed=true`, plus `astrosim.session`,
`astrosim.component` and `astrosim.group`. Containers you start by hand (named `engine` and `fc`, as in the top-level
README) are never touched, but if they hold the ports, the check reports `port_busy`.

## Restart policy

If a container exits or stops responding, the engine and flight computer are restarted together (the PX4 autopilot
cannot survive the engine's clock restarting, and the flight computer's script runs on the engine's clock). The status
shows `recovering`. There are up to `--restart-max` (default 3) consecutive attempts, with a 2 s, 5 s and then 10 s
wait. The count resets after 2 minutes of stable running. After the last attempt the phase becomes `failed` with the
reason `restart_limit` and the last log lines, and you can press Start again.

With **`--no-fail`** the orchestrator never gives up: once the waits are used up it keeps retrying every 10 s until you
press Stop or quit, and the status shows `attempt N` without a limit (`maxAttempts` is 0). The exception is a
permanent failure, which retrying cannot fix: a container exiting with status 2 (invalid configuration) or a missing
image still goes to `failed`.

A component that does not become ready in its ready timeout (engine 60 s, flight computer 90 s) counts as a failure.
The first start took about 5 s to the engine and about 7 s to the whole stack on the test machine.

## Single instance and lock file

Only one orchestrator runs per user. It holds an exclusive lock on `orchestrator.lock` in `astrosim/` under the
user's cache directory (`~/.cache` on Linux, `~/Library/Caches` on macOS, `%LocalAppData%` on Windows) and writes the
dashboard URL into it. A second `astrosim` finds that URL, opens the browser there and exits.

While holding the lock, a new orchestrator removes every container labelled `astrosim.managed=true`: they can only be
leftovers from a crash or a forced quit. `--cleanup` and `--prune` do the same on demand, see
[Shutting down](#shutting-down).

## HTTP API

The dashboard is a plain-JavaScript page embedded in the program. It uses this API, which you can also script:

| Request | Result |
|-|-|
| `GET /api/v1/status` | The current snapshot (JSON, below) |
| `POST /api/v1/start` | `202` with the snapshot, or `409 {"error":"not_allowed","phase":...}` |
| `POST /api/v1/stop` | The same |
| `GET /api/v1/events` | Server-sent events: a `status` event with a snapshot on connect and on each change (at most every 250 ms), and a ping every 15 s |

```sh
curl -s http://127.0.0.1:8770/api/v1/status
curl -s -X POST -H 'Content-Type: application/json' http://127.0.0.1:8770/api/v1/start
```

The snapshot (all keys camelCase; lists are `[]`, never `null`):

```json
{
  "version": "df69a59",
  "phase": "running",
  "since": "2026-11-22T13:26:58-05:00",
  "reason": { "code": "", "detail": "", "component": "" },
  "attempt": 0,
  "maxAttempts": 3,
  "canStart": false,
  "canStop": true,
  "runtime": { "name": "podman", "version": "5.8.9", "network": "bridge" },
  "components": [
    {
      "id": "engine",
      "name": "Physics engine",
      "description": "Simulates the world and the aircraft's physics.",
      "image": "astrosim-engine",
      "installed": true,
      "buildCommand": "podman build -t astrosim-engine -f gazebo-container/Dockerfile .",
      "state": "ready",
      "facts": [{ "key": "sim_time", "label": "Simulation time", "value": "0:02.8" }],
      "exitCode": null,
      "lastLog": [],
      "endpoints": [{ "name": "state", "proto": "udp", "address": "127.0.0.1:8765" }]
    }
  ]
}
```

- `phase` is one of `checking`, `unavailable`, `stopped`, `starting`, `running`, `recovering`, `stopping`, `failed`.
- `reason.code` explains `unavailable` (`runtime_missing`, `runtime_not_running`, `runtime_too_old`, `images_missing`,
  `port_busy`) and `failed` or `recovering` (`component_exited`, `component_unhealthy`, `ready_timeout`,
  `launch_failed`, `invalid_config`, `restart_limit`). The page turns these into plain sentences.
- Component `state` is one of `pending`, `launching`, `starting`, `ready`, `stale`, `stopping`, `stopped`, `exited`,
  `failed`. `facts` are live details from the component's health check; `lastLog` is the container's log tail,
  captured when it fails. `installed` is false when the image is missing, and `buildCommand` says how to build it.
- `maxAttempts` is 0 with `--no-fail` (unlimited).

## Security notes

- The dashboard has **no authentication**. Anyone who can reach it can start and stop the simulation.
- It listens on loopback (`127.0.0.1`) by default. A non-loopback `--listen` is refused unless `--allow-remote` is
  given; use that only on a network you trust.
- Every request must have a `Host` header that matches the listen address (or `localhost` / `127.0.0.1` on the same
  port), which blocks DNS-rebinding attacks. POST requests must be `application/json`, and an `Origin` header, if
  sent, must also be an allowed host, which blocks cross-site requests from other web pages.
- With a wildcard listen address (`0.0.0.0:8770`), the machine's hostname and its interface IP addresses are added to
  the allowed hosts, so other machines can use the dashboard by name or address. A DNS alias that is neither is
  rejected.
- The container runtime itself (Docker or Podman) gives whoever controls it control of the machine. The orchestrator
  only runs the two configured images.

## Building and testing

Everything is built and tested in a container, from the repository root (the tests read `shared/proto/testdata`):

```sh
orchestrator/build.sh          # binaries in build/orchestrator/
orchestrator/build.sh --test   # gofmt, go vet and go test -race only
```

`build.sh` produces `astrosim-{linux,darwin}-{amd64,arm64}` and `astrosim-windows-amd64.exe`; building every target
catches platform-specific code outside build-tagged files. It also copies this machine's binary to
`build/orchestrator/astrosim`, which the top-level `astrosim` symlink points at. Set `CONTAINER_RUNTIME=docker` to use
Docker. For quick iteration with a local Go toolchain (1.26), `cd orchestrator && go test -race ./...` also works.

`scripts/smoke.sh` is an end-to-end test against the real images (they must be built, and nothing else may be using
the managed containers or port 8770). It starts the orchestrator, waits for `running`, kills the engine and waits for
recovery, stops via the API, restarts, and checks after SIGINT, SIGTERM, SIGHUP and a closed pseudo-terminal that no
managed containers remain. It then checks that `--cleanup` is refused while running, that `kill -9` leaves the
containers behind, that `--cleanup` removes them, and that `--prune` removes the network, which the next start
recreates. It honours `CONTAINER_RUNTIME` and `ASTROSIM_ORCH_LISTEN`, uses only bash and curl (the pseudo-terminal
check also needs util-linux `script` and is skipped without it), and exits non-zero on any failure. It takes about
45 s.

## Package layout

```
cmd/astrosim/            main: wiring and signals; version.go holds the -ldflags version
internal/component/      generic component model: Definition, ports, dependencies, StackContext
internal/components/     registry.go lists every component; engine/ and fc/ define the two we have
internal/health/         health probes and the Monitor (ready / unhealthy edges)
internal/wire/           byte-exact decoders for the engine and FC protocols
internal/runtime/        podman and docker behind one interface, detection, a fake for tests
internal/supervisor/     pure state machine: start order, restarts, teardown
internal/session/        driver that runs the supervisor's actions against the runtime
internal/status/         the snapshot JSON and its broadcaster
internal/api/            HTTP API, SSE and the Host/Origin guards
internal/web/            the embedded dashboard (index.html, app.js, style.css)
internal/config/         defaults, JSON file, environment and flags
internal/instance/       single-instance lock
internal/browser/        opening the dashboard in a browser
internal/logging/        log format shared with the containers
scripts/smoke.sh         end-to-end test
```

## Adding a component

The supervisor, session, API and dashboard are generic: the dashboard draws whatever components the status lists. To
add one, touch only these files:

1. `internal/components/<name>/<name>.go`: a `Definition()` function returning a `component.Definition` (ID, display
   name, description, image, build hint, ports, environment, dependencies, restart group, health probe, timeouts).
   The probe is usually a `health.UDPSubscribe` with your own classify function that turns a packet into facts and a
   healthy flag. Copy `engine/` or `fc/`.
2. Optional, if it speaks a new binary protocol: `internal/wire/<name>.go` with the decoder, and golden-vector tests
   against files from `shared/proto/testdata`.
3. One line in `internal/components/registry.go`: add `<name>.Definition()` to `All()`.
4. Tests: a `<name>_test.go` next to the definition (probe classification, environment per network mode). The existing
   `registry_test.go` already checks the whole set: unique IDs, dependencies exist, no cycles, no port clashes.

## Known limitations / unverified

- **Host-network Gazebo isolation is not confirmed.** In `--network host` mode the engine gets `GZ_PARTITION=astrosim`
  and `GZ_IP=127.0.0.1`. This is deduced from gz-transport's documented defaults and has NOT been confirmed on a real
  host-mode run (for example by checking that another Gazebo on the same machine cannot see the engine, or with a
  packet capture). A host-mode run on Linux with Podman did start and reach `running`, but that does not prove
  isolation.
- **Docker is untested against a live daemon.** The docker error-string and `inspect` fixtures in
  `internal/runtime` are hand-written, not captured from Docker. The Docker 20.10 minimum (Podman 4.0) is a judgment
  call about when the needed networking behaviour appeared, not something tested.
- **Windows and macOS have not been run.** The Windows lock file, console-window hiding for the runtime commands,
  the `rundll32` browser open and shutdown when the console window is closed are cross-compiled but have never run on
  Windows. Nothing has run on macOS.
- **UDP publishing on Windows and macOS is untested.** Docker Desktop and `podman machine` run containers in a virtual
  machine; whether the published UDP ports reach the host with acceptable latency is unknown.
- **`lock_other.go` is not crash-safe.** On platforms that are neither Unix nor Windows the lock is a file created
  exclusively; after a crash it must be deleted by hand.
- **Probes use subscriber slots.** The engine and the flight computer each accept 8 subscribers. The orchestrator's
  health check holds one slot on each while running, and so does each `astrosim-probe` or `astrosim-fc-probe` run for
  a few seconds (a slot is dropped after 5 s of silence). Leave room if you run many displays.
- **Stale details after a recovery.** While a component is being stopped, its last exit code and log tail from the
  previous failure can still be shown, and the `attempt` counter stays at its last value after a Stop until the next
  Start.
- **Real-image runs were tried on one machine only** (Linux, rootless Podman 5.8, bridge mode and a short host-mode
  run). UDP through the published ports worked there, the flight computer resolved `engine` through the network
  alias, the ready timeouts were far longer than needed, and the stops caused by Stop, SIGINT or a recovery were
  handled correctly.
