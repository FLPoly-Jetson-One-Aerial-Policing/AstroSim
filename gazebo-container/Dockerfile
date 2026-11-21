FROM docker.io/library/ubuntu:26.04

ARG GZ_SIM_VER=10
ARG GZ_TRANSPORT_VER=15
ARG GZ_MSGS_VER=12

ENV DEBIAN_FRONTEND=noninteractive \
    GZ_SIM_VER=${GZ_SIM_VER} \
    GZ_SIM_RESOURCE_PATH=/opt/astrosim/models:/opt/astrosim/worlds

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl gnupg lsb-release \
    && curl -fsSL https://packages.osrfoundation.org/gazebo.gpg --output /usr/share/keyrings/pkgs-osrf-archive-keyring.gpg \
    && echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/pkgs-osrf-archive-keyring.gpg] https://packages.osrfoundation.org/gazebo/ubuntu-stable $(lsb_release -cs) main" > /etc/apt/sources.list.d/gazebo-stable.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config git \
    gz-sim${GZ_SIM_VER}-server \
    libgz-sim${GZ_SIM_VER}-dev \
    libgz-transport${GZ_TRANSPORT_VER}-dev \
    libgz-msgs${GZ_MSGS_VER}-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/astrosim
# Paths are relative to the build context, which is this directory:
#   docker build gazebo-container
COPY worlds/ /opt/astrosim/worlds/
COPY models/ /opt/astrosim/models/
COPY --chmod=755 entrypoint.sh /opt/astrosim/entrypoint.sh

EXPOSE 8765/udp

CMD ["/bin/sh", "-c", "exec /opt/astrosim/entrypoint.sh \"$GZ_SIM_VER\""]
