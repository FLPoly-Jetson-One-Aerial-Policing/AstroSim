#!/bin/bash
# Generates the PX4 airframe from the aircraft file, then starts the FC bridge and PX4 SITL; if either exits, stops the
# other and exits with the first one's status.
# Config path: $ASTROSIM_CONFIG, aircraft file: $ASTROSIM_AIRCRAFT. The engine to talk to is set in the config
# ([engine] host/port) or with ASTROSIM_ENGINE_HOST / ASTROSIM_ENGINE_PORT.
set -euo pipefail

CONFIG="${ASTROSIM_CONFIG:-/opt/astrosim/config/fc.toml}"
AIRCRAFT="${ASTROSIM_AIRCRAFT:-/opt/astrosim/config/jetson_one.toml}"
PX4_DIR="/opt/px4"
# PX4's working directory ("rootfs"): writable, recreated at every start so parameters never persist between runs.
WORK="${ASTROSIM_PX4_WORKDIR:-/tmp/astrosim/px4}"

rm -rf "${WORK}"
mkdir -p "${WORK}"
cp -r "${PX4_DIR}/etc" "${WORK}/etc"

# Generate the airframe before anything starts; an invalid aircraft or config stops the container here (status 2).
astrosim-px4-airframe --aircraft "${AIRCRAFT}" --config "${CONFIG}" --out "${WORK}/etc/init.d-posix/airframes" || exit $?
AUTOSTART_ID="$(astrosim-px4-airframe --print-autostart-id)"

# The bridge first: it listens on the HIL port that PX4's simulator_mavlink connects to (PX4 retries until it can).
astrosim-fc-bridge --config "${CONFIG}" &
BRIDGE=$!

# PX4 SITL in daemon mode (no interactive shell). PX4_SYS_AUTOSTART selects the generated airframe;
# PX4_SIM_HOSTNAME is where simulator_mavlink connects (the bridge, in this container).
(
  cd "${WORK}"
  export PATH="${PX4_DIR}/bin:${PATH}"
  export PX4_SYS_AUTOSTART="${AUTOSTART_ID}"
  export PX4_SIM_HOSTNAME=localhost
  exec "${PX4_DIR}/bin/px4" -d -s etc/init.d-posix/rcS -w "${WORK}"
) &
PX4=$!

# Forward termination to both children; the wait below then returns.
stopping=0
trap 'stopping=1; kill -TERM "${BRIDGE}" "${PX4}" 2>/dev/null || true' TERM INT

# Wait for the first child to exit (a trapped signal also interrupts it).
status=0
wait -n "${BRIDGE}" "${PX4}" || status=$?

kill -TERM "${BRIDGE}" "${PX4}" 2>/dev/null || true
wait "${BRIDGE}" 2>/dev/null || true
wait "${PX4}" 2>/dev/null || true
# A requested stop (podman stop) is a clean exit.
[ "${stopping}" -eq 1 ] && exit 0
exit "${status}"
