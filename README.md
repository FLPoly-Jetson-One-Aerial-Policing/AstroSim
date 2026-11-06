# AstroSim

A multicopter flight simulator proof of concept: a Gazebo physics engine in a container, plus a 3D viewer
you fly with the keyboard.

## Run it (Windows, macOS, Linux): one command

1. Install [Docker Desktop](https://www.docker.com/products/docker-desktop/) and start it
   (on Windows it uses WSL 2; the installer sets that up).
2. Download this repository (or just `compose.yaml`), open a terminal (PowerShell on Windows) in its folder, and run:

   ```
   docker compose up
   ```

   This pulls the prebuilt images from GitHub Container Registry. If the images are private you must first run
   `docker login ghcr.io` with a GitHub account that has access to the organisation. If they can't be pulled and
   you have the full repository, it builds them from source instead (several minutes; `docker compose up --build`
   forces that).

3. Wait for the download (about 1 GB the first time; later runs start in seconds),
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
| `.github/workflows/publish-images.yml` | manual workflow that publishes both images to GHCR |

### Publishing the images (maintainers)

Publishing is manual only. In GitHub: Actions, "Publish images", Run workflow, pick the branch and a tag
(default `dev`). Images land at `ghcr.io/flpoly-jetson-one-aerial-policing/astrosim-engine` and `astrosim-viewer`.
New GHCR packages are private; an org admin can make them public under the package's settings.
