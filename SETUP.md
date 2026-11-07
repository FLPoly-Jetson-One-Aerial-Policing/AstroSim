# Host setup

Host is Fedora (WSL2 or native Linux). The engine (Gazebo + gateway) runs in a podman container; the host builds only the wire library, the raylib viewer, the tools and the Go orchestrator, and needs no Gazebo packages.

Versions below are what the PoC was developed with.

| Dependency | Used for | Version | Install |
| --- | --- | --- | --- |
| podman | engine container (build, run) | 5.8 | `sudo dnf install podman` |
| g++ (C++17) | host C++ targets | GCC 16 | `sudo dnf install gcc-c++` |
| cmake | build | 4.3 (>= 3.22) | `sudo dnf install cmake` |
| ninja | build generator | 1.13 | `sudo dnf install ninja-build` |
| git | source control | 2.55 | `sudo dnf install git` |
| Go | orchestrator (`orchestrator/`) | 1.26 | `sudo dnf install golang` |
| raylib 5.5 (viewer) | window, 3D view | use an installed one, else CMake builds 5.5 from source (`-DASTROSIM_RAYLIB=AUTO\|SYSTEM\|FETCH\|OFF`) | see below |
| X11 + OpenGL + ALSA dev headers | only for the build-from-source raylib fallback | per the [raylib wiki](https://github.com/raysan5/raylib/wiki/Working-on-GNU-Linux) | `sudo dnf install alsa-lib-devel mesa-libGL-devel libX11-devel libXrandr-devel libXi-devel libXcursor-devel libXinerama-devel libatomic` |
| display (WSLg or X11/Wayland) | viewer window | n/a | WSL2 provides WSLg; native Linux needs a desktop session |

Not needed on the host: Gazebo, gz-transport, gz-msgs, Python (everything gz-related is in the container).

`./build.sh --deps` installs these for you on dnf, apt or pacman (it uses sudo; `--dry-run` shows the commands first), and `./build.sh` then builds everything. The manual one-liner for Fedora:

    sudo dnf install podman gcc-c++ cmake ninja-build git golang pkgconf-pkg-config \
      alsa-lib-devel mesa-libGL-devel libX11-devel libXrandr-devel libXi-devel libXcursor-devel libXinerama-devel libatomic

## Build and test

Everything (host C++, Go orchestrator, engine image):

    ./build.sh [--deps] [--no-image]

Or step by step:

All suites in one go (C++ ctest, Go vet/test; `--integration` also starts the container and runs the scripted climb):

    ./test.sh [--integration]

Individually:

Engine image (Ubuntu 24.04, gz Jetty; pulls ~1 GB, downloads the X3 model from Fuel at build time for the spike world; the PoC flies the in-repo `container/models/jetson_one`):

    podman build -t astrosim-engine:dev -f container/Containerfile .

Host build and tests:

    cmake -S host -B build/host -G Ninja
    cmake --build build/host
    ctest --test-dir build/host

Container-side C++ (protocol + gateway, host-buildable without gz) and its tests:

    cmake -S container -B build/container -G Ninja
    cmake --build build/container
    ctest --test-dir build/container

Orchestrator (Go):

    (cd orchestrator && go test ./... && go build -o ../build/orchestrator/astrosim ./cmd/astrosim)

Run everything (container + viewer, supervised; Ctrl-C or closing the window stops it all):

    ./astrosim up            # add --no-display for engine only, --tick 500 for a faster state stream
    ./astrosim status
    ./astrosim down

Viewer controls: WASD throttle/yaw, arrows pitch/roll (gamepad: Mode 2 sticks). `C` (gamepad Y / triangle) toggles the cockpit and chase cameras; `viewer --cockpit` starts in the cockpit.

Container-side targets (gz-dependent, e.g. `bench_rtf`), built inside the image with the source bind-mounted:

    podman run --rm -v "$PWD":/src:Z -w /src astrosim-engine:dev \
      bash -c 'cmake -S container -B build/gw -G Ninja -DASTROSIM_BUILD_GATEWAY=ON && cmake --build build/gw'

Run the sim and the benchmark:

    podman run -d --rm --name astrosim-engine -v "$PWD":/src:Z astrosim-engine:dev
    podman exec astrosim-engine /src/build/gw/bench_rtf 60
    podman stop -t 2 astrosim-engine
