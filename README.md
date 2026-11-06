# AstroSim

A multicopter flight simulator proof of concept: a Gazebo physics engine in a container, plus a 3D viewer
you fly with the keyboard.

## Run it (Windows, macOS, Linux): one command

1. Install [Docker Desktop](https://www.docker.com/products/docker-desktop/) and start it
   (on Windows it uses WSL 2; the installer sets that up).
2. Download this repository, open a terminal (PowerShell on Windows) in its folder, and run:

   ```
   docker compose up --build
   ```

3. Wait for the build (the first run takes several minutes and downloads about 1 GB; later runs start in seconds),
   then open **http://localhost:6080/vnc.html?autoconnect=1&resize=scale** in your browser.

You should see the drone and a green **CONNECTED** label. Click the page so it captures the keyboard, then fly:

| Keys | Action |
|---|---|
| W / S | throttle up / down |
| A / D | yaw left / right |
| Arrow up / down | pitch |
| Arrow left / right | roll |

Stop with `Ctrl+C` in the terminal, then `docker compose down` to clean up.

### Troubleshooting

- **Blank page or "can't connect"**: wait until the terminal prints `viewer: open http://localhost:6080/...`.
- **Choppy picture**: the viewer renders in software inside the container; close other heavy apps, or use the
  native viewer below.
- Port 6080 or 8765 already in use: stop whatever uses it, or edit the `ports:` lines in `compose.yaml`.

## Developing

Native builds (Linux; faster, uses your GPU for the viewer) are described in [SETUP.md](SETUP.md):
`./build.sh --deps`, `./test.sh`, then `./astrosim up`.

## Layout

| Directory | Contents |
|---|---|
| `container/` | engine image: Gazebo world, UDP gateway, benchmarks |
| `host/` | raylib viewer, scripted tools, viewer container image (`host/Containerfile`) |
| `orchestrator/` | Go supervisor (`./astrosim up`) that runs the engine container and viewer natively |
| `shared/proto/` | wire protocol library used by all of the above |
| `compose.yaml` | one-command setup: engine + browser-based viewer |
