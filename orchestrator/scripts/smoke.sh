#!/usr/bin/env bash
# End-to-end smoke test of the AstroSim orchestrator against the real images.
#
#   orchestrator/scripts/smoke.sh
#
# Needs the astrosim-engine and astrosim-fc images (see the top-level README)
# and a built orchestrator (orchestrator/build.sh). It checks that
#
#   1. a fresh start reaches the "running" phase with every component ready,
#   2. killing the engine is recovered from (both containers are relaunched),
#   3. POST /api/v1/stop leaves no managed containers,
#   4. SIGINT after a second start exits the process and leaves none.
#
# Environment:
#   CONTAINER_RUNTIME       podman (default) or docker
#   ASTROSIM_ORCH_LISTEN    dashboard address (default 127.0.0.1:8770)
#   ASTROSIM_BIN            orchestrator binary (default build/orchestrator/astrosim)
#
# The status JSON is inspected with grep and sed only, so the script needs
# nothing beyond bash and curl. That works because the snapshot is compact
# JSON, and "phase" is its first "phase" key and a component's "state" is
# unambiguous. Exit status is 0 only if every check passed.
set -euo pipefail

runtime="${CONTAINER_RUNTIME:-podman}"
listen="${ASTROSIM_ORCH_LISTEN:-127.0.0.1:8770}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
bin="${ASTROSIM_BIN:-$root/build/orchestrator/astrosim}"
base="http://$listen"
log="$(mktemp "${TMPDIR:-/tmp}/astrosim-smoke.XXXXXX")"
pid=""
failures=0

pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; failures=$((failures + 1)); }

# managed lists the names of all orchestrator-managed containers.
managed() {
    "$runtime" ps -a --filter label=astrosim.managed=true --format '{{.Names}}'
}

# cleanup always runs: it stops the orchestrator if it is still up and removes
# any managed container it left behind.
cleanup() {
    local rc=$?
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill -INT "$pid" 2>/dev/null || true
        for _ in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.2; done
        kill -KILL "$pid" 2>/dev/null || true
    fi
    local left
    left="$(managed 2>/dev/null || true)"
    if [ -n "$left" ]; then
        # shellcheck disable=SC2086
        "$runtime" rm -f $left >/dev/null 2>&1 || true
    fi
    if [ "$failures" -ne 0 ] || [ "$rc" -ne 0 ]; then
        echo "--- orchestrator log ($log) ---"
        tail -n 40 "$log" || true
    else
        rm -f "$log"
    fi
}
trap cleanup EXIT

# status prints the status JSON, or nothing if the dashboard is unreachable.
status() { curl -fsS --max-time 3 "$base/api/v1/status" 2>/dev/null || true; }

# phase prints the current phase name.
phase() { status | grep -o '"phase":"[a-z]*"' | head -n 1 | sed 's/.*:"\(.*\)"/\1/'; }

# ready_count prints how many components are in the ready state.
ready_count() { status | grep -o '"state":"ready"' | wc -l; }

# wait_running waits up to $1 seconds for the running phase with both
# components ready.
wait_running() {
    local deadline=$((SECONDS + $1))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if [ "$(phase)" = running ] && [ "$(ready_count)" -ge 2 ]; then return 0; fi
        sleep 1
    done
    return 1
}

# wait_phase waits up to $2 seconds for phase $1.
wait_phase() {
    local deadline=$((SECONDS + $2))
    while [ "$SECONDS" -lt "$deadline" ]; do
        [ "$(phase)" = "$1" ] && return 0
        sleep 0.5
    done
    return 1
}

# wait_no_managed waits up to $1 seconds for every managed container to go.
wait_no_managed() {
    local deadline=$((SECONDS + $1))
    while [ "$SECONDS" -lt "$deadline" ]; do
        [ -z "$(managed)" ] && return 0
        sleep 0.5
    done
    return 1
}

# start_orchestrator launches the binary in the background, optionally
# autostarting the simulation.
start_orchestrator() {
    ASTROSIM_ORCH_LISTEN="$listen" "$bin" --no-browser "$@" >>"$log" 2>&1 &
    pid=$!
}

post() { curl -fsS --max-time 5 -X POST -H 'Content-Type: application/json' "$base/api/v1/$1" >/dev/null; }

[ -x "$bin" ] || { echo "FAIL: $bin not found; run orchestrator/build.sh first"; exit 2; }
if [ -n "$(managed)" ]; then
    echo "FAIL: managed containers already exist ($(managed | tr '\n' ' ')); stop them first"
    exit 2
fi

# 1. Start.
start_orchestrator --autostart
if wait_running 180; then
    pass "start: running with both components ready after about ${SECONDS}s"
else
    fail "start: not running within 180 s (phase=$(phase))"
    exit 1
fi

# 2. Kill the engine; the whole sim group must come back.
old_id="$("$runtime" inspect --format '{{.Id}}' astrosim-engine)"
"$runtime" kill astrosim-engine >/dev/null
sleep 1
if wait_running 180; then
    new_id="$("$runtime" inspect --format '{{.Id}}' astrosim-engine 2>/dev/null || true)"
    if [ -n "$new_id" ] && [ "$new_id" != "$old_id" ]; then
        pass "recovery: engine relaunched and the run is running again"
    else
        fail "recovery: running again, but the engine container was not relaunched"
    fi
else
    fail "recovery: not running within 180 s after killing the engine (phase=$(phase))"
fi

# 3. Stop.
if post stop && wait_phase stopped 60 && wait_no_managed 30; then
    pass "stop: phase stopped and no managed containers remain"
else
    fail "stop: phase=$(phase), containers: $(managed | tr '\n' ' ')"
fi

# 4. Start again, then SIGINT.
if post start && wait_running 180; then
    kill -INT "$pid"
    exited=0
    for _ in $(seq 1 100); do
        kill -0 "$pid" 2>/dev/null || { exited=1; break; }
        sleep 0.2
    done
    if [ "$exited" = 1 ] && wait_no_managed 30; then
        pass "SIGINT: orchestrator exited and no managed containers remain"
    else
        fail "SIGINT: exited=$exited, containers: $(managed | tr '\n' ' ')"
    fi
else
    fail "restart after stop: not running (phase=$(phase))"
fi

if [ "$failures" -eq 0 ]; then
    echo "ALL CHECKS PASSED"
else
    echo "$failures CHECK(S) FAILED"
    exit 1
fi
