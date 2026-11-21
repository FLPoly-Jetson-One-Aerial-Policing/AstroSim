# AstroSim physics engine image: headless gz-sim server plus the UDP gateway.
#
# Build from the REPOSITORY ROOT (the build context must contain shared/ and gazebo-container/):
#   docker build -t astrosim-engine -f gazebo-container/Dockerfile .
# Run (any host OS with Docker; only the UDP port is published):
#   docker run --rm -p 8765:8765/udp astrosim-engine
#
# Stages:
#   build   dev packages, unit tests under ASan/UBSan (the build fails if a test fails), then a Release build
#   runtime gz-sim server + runtime libraries only, the gateway binaries, config, world and models
ARG UBUNTU_TAG=26.04

# ---------------------------------------------------------------------------------------------------------------
FROM docker.io/library/ubuntu:${UBUNTU_TAG} AS build

ARG GZ_SIM_VER=10
ARG GZ_TRANSPORT_VER=15
ARG GZ_MSGS_VER=12

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl gnupg lsb-release \
    && curl -fsSL https://packages.osrfoundation.org/gazebo.gpg --output /usr/share/keyrings/pkgs-osrf-archive-keyring.gpg \
    && echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/pkgs-osrf-archive-keyring.gpg] https://packages.osrfoundation.org/gazebo/ubuntu-stable $(lsb_release -cs) main" > /etc/apt/sources.list.d/gazebo-stable.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config git \
    libgtest-dev libtomlplusplus-dev \
    libgz-transport${GZ_TRANSPORT_VER}-dev \
    libgz-msgs${GZ_MSGS_VER}-dev \
    libgz-math9-dev \
    libsdformat16-dev \
    && rm -rf /var/lib/apt/lists/*

# Mirror the repo layout so the relative path from gazebo-container/ to shared/ holds.
WORKDIR /src
COPY shared/ /src/shared/
COPY gazebo-container/CMakeLists.txt /src/gazebo-container/CMakeLists.txt
COPY gazebo-container/gateway/ /src/gazebo-container/gateway/
COPY gazebo-container/modelgen/ /src/gazebo-container/modelgen/
COPY gazebo-container/config/ /src/gazebo-container/config/
COPY gazebo-container/tests/ /src/gazebo-container/tests/

# Unit tests under ASan/UBSan (no gz needed), including loading the generated SDF with sdformat. A failing test
# fails the image build.
RUN cmake -S gazebo-container -B /tmp/build-test -G Ninja -DCMAKE_BUILD_TYPE=Debug \
        -DASTROSIM_SANITIZE=ON -DASTROSIM_BUILD_TESTS=ON -DASTROSIM_BUILD_SDF_TEST=ON -DASTROSIM_BUILD_GZ=OFF \
    && cmake --build /tmp/build-test \
    && ctest --test-dir /tmp/build-test --output-on-failure

# Release build of the gz-linked binaries, installed to a staging prefix.
RUN cmake -S gazebo-container -B /tmp/build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DASTROSIM_BUILD_GZ=ON -DASTROSIM_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=/opt/stage \
    && cmake --build /tmp/build-release \
    && cmake --install /tmp/build-release

# ---------------------------------------------------------------------------------------------------------------
FROM docker.io/library/ubuntu:${UBUNTU_TAG} AS runtime

ARG GZ_SIM_VER=10
ARG GZ_TRANSPORT_VER=15
ARG GZ_MSGS_VER=12

ENV DEBIAN_FRONTEND=noninteractive \
    GZ_SIM_VER=${GZ_SIM_VER} \
    GZ_SIM_RESOURCE_PATH=/opt/astrosim/models:/opt/astrosim/worlds \
    HOME=/home/astrosim

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl gnupg lsb-release \
    && curl -fsSL https://packages.osrfoundation.org/gazebo.gpg --output /usr/share/keyrings/pkgs-osrf-archive-keyring.gpg \
    && echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/pkgs-osrf-archive-keyring.gpg] https://packages.osrfoundation.org/gazebo/ubuntu-stable $(lsb_release -cs) main" > /etc/apt/sources.list.d/gazebo-stable.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
    gz-sim${GZ_SIM_VER}-server \
    libgz-transport${GZ_TRANSPORT_VER} \
    libgz-msgs${GZ_MSGS_VER} \
    libgz-math9 \
    libtomlplusplus3t64 \
    && apt-get purge -y --auto-remove curl gnupg lsb-release \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --home-dir /home/astrosim --shell /usr/sbin/nologin --uid 10001 astrosim

COPY --from=build /opt/stage/bin/gateway /opt/stage/bin/astrosim-probe /usr/local/bin/

WORKDIR /opt/astrosim
COPY gazebo-container/config/ /opt/astrosim/config/
COPY gazebo-container/worlds/ /opt/astrosim/worlds/
COPY gazebo-container/models/ /opt/astrosim/models/
COPY --chmod=755 gazebo-container/entrypoint.sh /opt/astrosim/entrypoint.sh

USER astrosim

EXPOSE 8765/udp

HEALTHCHECK --interval=10s --timeout=5s --start-period=30s CMD ["astrosim-probe", "--health"]

CMD ["/bin/sh", "-c", "exec /opt/astrosim/entrypoint.sh \"$GZ_SIM_VER\""]
