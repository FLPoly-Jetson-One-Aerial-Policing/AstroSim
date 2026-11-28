# AstroSim flight computer image: PX4 SITL, the FC bridge (with the PX4 adapter) and the airframe generator.
#
# Build from the REPOSITORY ROOT (the build context must contain shared/ and fc-container/):
#   podman build -t astrosim-fc -f fc-container/Dockerfile .
# Run next to the engine (see fc-container/README.md); only UDP ports are published:
#   podman run --rm -e ASTROSIM_ENGINE_HOST=<engine host> -p 8766:8766/udp astrosim-fc
#
# Stages:
#   px4      PX4-Autopilot at a pinned release tag, built as px4_sitl_default (slow; cached on its own)
#   build    our code: unit tests under ASan/UBSan (the build fails if a test fails), then a Release build
#   runtime  the px4 binary and its ROMFS, our binaries, config and the aircraft file; non-root user
#
# PX4 supports Ubuntu 22.04 and 24.04. All stages use 24.04 so the px4 binary and our binaries share one libc.
ARG UBUNTU_TAG=24.04
# PX4-Autopilot release tag (a pinned, released version; the PX4 facts in fc-container/px4/README.md are
# verified against this tag).
ARG PX4_TAG=v1.17.0
# mavlink/c_library_v2 commit (MAVLink C headers, generated from the common dialect).
ARG MAVLINK_COMMIT=d2062a25f05fc85213830427058401b0ec345430

# ---------------------------------------------------------------------------------------------------------------
FROM docker.io/library/ubuntu:${UBUNTU_TAG} AS px4

ARG PX4_TAG

ENV DEBIAN_FRONTEND=noninteractive

# The dependencies of PX4's Tools/setup/ubuntu.sh needed to build SITL (no NuttX toolchain, no simulators).
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
    build-essential ca-certificates cmake file g++ gcc git libssl-dev libxml2-dev libxml2-utils make ninja-build \
    python3 python3-dev python3-pip python3-setuptools python3-wheel rsync unzip zip bc \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt
RUN git clone --depth 1 --branch ${PX4_TAG} --recurse-submodules --shallow-submodules --jobs 8 \
        https://github.com/PX4/PX4-Autopilot.git px4 \
    && python3 -m pip install --no-cache-dir --break-system-packages -r /opt/px4/Tools/setup/requirements.txt

# px4_sitl_default: the px4 binary and a processed ROMFS in build/px4_sitl_default/etc.
RUN make -C /opt/px4 px4_sitl_default

# ---------------------------------------------------------------------------------------------------------------
# Our code. Starts from the px4 stage so PX4's toolchain is reused; only this stage and later ones are rebuilt when
# our sources change.
FROM px4 AS build

ARG MAVLINK_COMMIT

RUN apt-get update \
    && apt-get install -y --no-install-recommends libgtest-dev libtomlplusplus-dev \
    && rm -rf /var/lib/apt/lists/*

# MAVLink C headers (common dialect) at a pinned commit; included as a SYSTEM directory by fc-container/px4.
RUN git init /opt/mavlink \
    && git -C /opt/mavlink fetch --depth 1 https://github.com/mavlink/c_library_v2.git ${MAVLINK_COMMIT} \
    && git -C /opt/mavlink checkout --detach FETCH_HEAD

# Mirror the repo layout so the relative path from fc-container/ to shared/ holds.
WORKDIR /src
COPY shared/ /src/shared/
COPY fc-container/CMakeLists.txt /src/fc-container/CMakeLists.txt
COPY fc-container/bridge/ /src/fc-container/bridge/
COPY fc-container/px4/ /src/fc-container/px4/
COPY fc-container/config/ /src/fc-container/config/
COPY fc-container/tests/ /src/fc-container/tests/

# Unit tests under ASan/UBSan (including the PX4 adapter's codec, session and airframe generator). A failing test
# fails the image build.
RUN cmake -S fc-container -B /tmp/build-test -G Ninja -DCMAKE_BUILD_TYPE=Debug \
        -DASTROSIM_SANITIZE=ON -DASTROSIM_BUILD_TESTS=ON -DASTROSIM_BUILD_PX4=ON -DASTROSIM_MAVLINK_DIR=/opt/mavlink \
    && cmake --build /tmp/build-test \
    && ctest --test-dir /tmp/build-test --output-on-failure

# Release build, installed to a staging prefix.
RUN cmake -S fc-container -B /tmp/build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DASTROSIM_BUILD_TESTS=OFF -DASTROSIM_BUILD_PX4=ON -DASTROSIM_MAVLINK_DIR=/opt/mavlink \
        -DCMAKE_INSTALL_PREFIX=/opt/stage \
    && cmake --build /tmp/build-release \
    && cmake --install /tmp/build-release --strip

# The runtime needs only the px4 binary (with its module symlinks and alias script) and the processed ROMFS.
RUN mkdir -p /opt/px4-stage \
    && cp -a /opt/px4/build/px4_sitl_default/bin /opt/px4-stage/bin \
    && cp -a /opt/px4/build/px4_sitl_default/etc /opt/px4-stage/etc \
    && strip --strip-unneeded /opt/px4-stage/bin/px4

# ---------------------------------------------------------------------------------------------------------------
FROM docker.io/library/ubuntu:${UBUNTU_TAG} AS runtime

ENV DEBIAN_FRONTEND=noninteractive \
    HOME=/home/astrosim

# bc: PX4's rcS uses it when PX4_SIM_SPEED_FACTOR is set.
RUN apt-get update \
    && apt-get install -y --no-install-recommends libtomlplusplus3t64 bc \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --home-dir /home/astrosim --shell /usr/sbin/nologin --uid 10001 astrosim

COPY --from=build /opt/stage/bin/astrosim-fc-bridge /opt/stage/bin/astrosim-fc-probe /opt/stage/bin/astrosim-px4-airframe /usr/local/bin/
COPY --from=build /opt/px4-stage/ /opt/px4/

WORKDIR /opt/astrosim
COPY fc-container/config/ /opt/astrosim/config/
COPY shared/aircraft/jetson_one.toml /opt/astrosim/config/jetson_one.toml
COPY --chmod=755 fc-container/entrypoint.sh /opt/astrosim/entrypoint.sh

USER astrosim

# FC telemetry (UDP). PX4's own ports stay inside the container.
EXPOSE 8766/udp

CMD ["/opt/astrosim/entrypoint.sh"]
