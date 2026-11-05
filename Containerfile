# Engine image: headless Gazebo + gateway build toolchain.
# Release is selectable; versions below are the OSRF noble package suffixes.
#   jetty    : --build-arg GZ_SIM=10 --build-arg GZ_TRANSPORT=15 --build-arg GZ_MSGS=12  (default)
#   harmonic : --build-arg GZ_SIM=8  --build-arg GZ_TRANSPORT=13 --build-arg GZ_MSGS=10
FROM docker.io/library/ubuntu:24.04

ARG GZ_SIM=10
ARG GZ_TRANSPORT=15
ARG GZ_MSGS=12

ENV DEBIAN_FRONTEND=noninteractive \
    GZ_SIM_RESOURCE_PATH=/opt/astrosim/models:/opt/astrosim/worlds

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        ca-certificates curl gnupg lsb-release \
 && curl -fsSL https://packages.osrfoundation.org/gazebo.gpg \
        -o /usr/share/keyrings/pkgs-osrf-archive-keyring.gpg \
 && echo "deb [arch=amd64 signed-by=/usr/share/keyrings/pkgs-osrf-archive-keyring.gpg] http://packages.osrfoundation.org/gazebo/ubuntu-stable $(lsb_release -cs) main" \
        > /etc/apt/sources.list.d/gazebo-stable.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git \
        libgz-sim${GZ_SIM}-dev \
        libgz-transport${GZ_TRANSPORT}-dev \
        libgz-msgs${GZ_MSGS}-dev \
 && rm -rf /var/lib/apt/lists/*

RUN apt-get update \
 && apt-get install -y --no-install-recommends gz-sim${GZ_SIM}-cli gz-transport${GZ_TRANSPORT}-cli gz-msgs${GZ_MSGS}-cli iputils-ping \
 && rm -rf /var/lib/apt/lists/*

# Pre-fetch the stock X3 UAV so the sim does not need the network at run time.
RUN gz fuel download -u "https://fuel.gazebosim.org/1.0/OpenRobotics/models/X3 UAV/4" >/dev/null 2>&1 \
 && test -f "/root/.gz/fuel/fuel.gazebosim.org/openrobotics/models/x3 uav/4/model.sdf"

# gz-transport discovery stays inside the container; only the gateway UDP port is published.
WORKDIR /opt/astrosim
COPY worlds/ /opt/astrosim/worlds/

EXPOSE 8765/udp

CMD ["gz", "sim", "-s", "-r", "-v", "3", "/opt/astrosim/worlds/poc.sdf"]
