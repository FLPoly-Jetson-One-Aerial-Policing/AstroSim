#!/bin/bash
# Starts gz-sim-server and the gateway; if either exits, stops the other and exits with the first one's status.
# Usage: entrypoint.sh GZ_SIM_VERSION   (config path: $ASTROSIM_CONFIG)
set -euo pipefail

GZ_SIM_VER="${1:-${GZ_SIM_VER:?GZ_SIM_VER or first argument required}}"
WORLD_FILE="/opt/astrosim/worlds/default.sdf"
CONFIG="${ASTROSIM_CONFIG:-/opt/astrosim/config/gateway.toml}"

"/usr/libexec/gz/sim${GZ_SIM_VER}/gz-sim-server" -r "${WORLD_FILE}" &
GZ=$!
gateway --config "${CONFIG}" &
GW=$!

# Forward termination to both children; the wait below then returns.
stopping=0
trap 'stopping=1; kill -TERM "${GZ}" "${GW}" 2>/dev/null || true' TERM INT

# Wait for the first child to exit (a trapped signal also interrupts it).
status=0
wait -n "${GZ}" "${GW}" || status=$?

kill -TERM "${GZ}" "${GW}" 2>/dev/null || true
wait "${GZ}" 2>/dev/null || true
wait "${GW}" 2>/dev/null || true
# A requested stop (docker stop) is a clean exit.
[ "${stopping}" -eq 1 ] && exit 0
exit "${status}"
