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
| raylib + headers | viewer (`viewer`, built when raylib is found) | 5.5 | `sudo dnf install raylib-devel` |
| display (WSLg or X11/Wayland) | viewer window | n/a | WSL2 provides WSLg; native Linux needs a desktop session |

Not needed on the host: Gazebo, gz-transport, gz-msgs, Python (everything gz-related is in the container).

One-liner:

    sudo dnf install podman gcc-c++ cmake ninja-build git golang raylib-devel

## Build and test

Engine image (Ubuntu 24.04, gz Jetty; pulls ~1 GB, downloads the X3 model from Fuel at build time):

    podman build -t astrosim-engine:dev -f Containerfile .

Host build and tests:

    cmake -S . -B build/host -G Ninja
    cmake --build build/host
    ctest --test-dir build/host

Container-side targets (gz-dependent, e.g. `bench_rtf`), built inside the image with the source bind-mounted:

    podman run --rm -v "$PWD":/src:Z -w /src astrosim-engine:dev \
      bash -c 'cmake -S . -B build/gw -G Ninja -DASTROSIM_BUILD_GATEWAY=ON && cmake --build build/gw'

Run the sim and the benchmark:

    podman run -d --rm --name astrosim-engine -v "$PWD":/src:Z astrosim-engine:dev
    podman exec astrosim-engine /src/build/gw/bench_rtf 60
    podman stop -t 2 astrosim-engine
