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
#   4. SIGINT after a second start exits the process and leaves none,
#   5. SIGTERM does the same,
#   6. SIGHUP (the terminal closing) does the same,
#   7. closing a pseudo-terminal under the orchestrator does the same
#      (skipped without util-linux "script"),
#   8. a running orchestrator refuses --cleanup and keeps its session,
#   9. "kill -9" leaves the containers running (expected), --cleanup then
#      removes them, and a fresh start reaches running,
#  10. --prune removes the labelled network, and the next start recreates it.
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
spid=""
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
    if [ -n "$spid" ]; then kill -KILL "$spid" 2>/dev/null || true; fi
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

# networks lists the names of the networks labelled as managed by AstroSim.
networks() {
    "$runtime" network ls --filter label=astrosim.managed=true --format '{{.Name}}'
}

# wait_exit waits up to $1 seconds for the orchestrator process to end.
wait_exit() {
    local deadline=$((SECONDS + $1))
    while [ "$SECONDS" -lt "$deadline" ]; do
        kill -0 "$pid" 2>/dev/null || { wait "$pid" 2>/dev/null || true; return 0; }
        sleep 0.2
    done
    return 1
}

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

if [ -n "$(status)" ]; then
    echo "FAIL: something already answers at $base; stop that orchestrator first"
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

# signal_check STEP SIGNAL starts a run, sends the signal and expects the
# orchestrator to exit and leave no managed containers.
signal_check() {
    local name="$1" sig="$2"
    start_orchestrator --autostart
    if ! wait_running 180; then
        fail "$name: not running (phase=$(phase))"
        return
    fi
    kill "-$sig" "$pid"
    if wait_exit 20 && wait_no_managed 30; then
        pass "$name: orchestrator exited and no managed containers remain"
    else
        fail "$name: still running or containers remain: $(managed | tr '\n' ' ')"
    fi
}

# 4. Start again through the API (the orchestrator from step 1 is still up),
# then SIGINT.
if post start && wait_running 180; then
    kill -INT "$pid"
    if wait_exit 20 && wait_no_managed 30; then
        pass "SIGINT: orchestrator exited and no managed containers remain"
    else
        fail "SIGINT: containers: $(managed | tr '\n' ' ')"
    fi
else
    fail "restart after stop: not running (phase=$(phase))"
fi

# 5, 6. SIGTERM and SIGHUP.
signal_check SIGTERM TERM
signal_check SIGHUP HUP

# 7. A closed pseudo-terminal: killing "script" with SIGKILL closes the pty
# master, so the orchestrator gets the hangup and a failing stderr, exactly
# as when a terminal window is closed.
if script --version 2>&1 | grep -q util-linux; then
    ASTROSIM_ORCH_LISTEN="$listen" script -qfc "$bin --no-browser --autostart" /dev/null \
        </dev/null >>"$log" 2>&1 &
    spid=$!
    if wait_running 180; then
        kill -KILL "$spid"
        { wait "$spid"; } 2>/dev/null || true
        spid=""
        if wait_no_managed 30 && [ -z "$(status)" ]; then
            pass "closed terminal: orchestrator exited and no managed containers remain"
        else
            fail "closed terminal: containers: $(managed | tr '\n' ' ')"
        fi
    else
        fail "closed terminal: not running (phase=$(phase))"
    fi
else
    echo "SKIP: closed terminal (needs util-linux script)"
fi

# 8, 9. Cleanup is refused while running; a hard kill leaves containers.
start_orchestrator --autostart
if wait_running 180; then
    if "$bin" --cleanup >/dev/null 2>&1; then
        fail "cleanup while running: expected a refusal"
    elif [ "$(phase)" = running ] && [ "$(managed | wc -l)" -ge 2 ]; then
        pass "cleanup while running: refused and the session is intact"
    else
        fail "cleanup while running: the session was disturbed (phase=$(phase))"
    fi
    kill -KILL "$pid"
    wait "$pid" 2>/dev/null || true
    running="$("$runtime" ps --filter label=astrosim.managed=true --format '{{.Names}}' | wc -l)"
    if [ "$running" -ge 2 ]; then
        pass "kill -9: containers keep running (expected: abnormal exit)"
    else
        fail "kill -9: expected the containers to remain, found $running running"
    fi
    if "$bin" --cleanup >/dev/null && [ -z "$(managed)" ]; then
        pass "--cleanup: removed the leftover containers"
    else
        fail "--cleanup: containers remain: $(managed | tr '\n' ' ')"
    fi
    start_orchestrator --autostart
    if wait_running 180; then
        pass "restart after kill -9: running"
    else
        fail "restart after kill -9: not running (phase=$(phase))"
    fi
    kill -INT "$pid"
    wait_exit 20 || true
else
    fail "kill -9 setup: not running (phase=$(phase))"
fi

# 10. Prune: the labelled network is removed, then recreated by a new start.
if [ -z "$(networks)" ]; then
    fail "prune: no labelled network to remove (was it created before labels?)"
elif "$bin" --prune >/dev/null && [ -z "$(networks)" ]; then
    pass "--prune: removed the labelled network"
    start_orchestrator --autostart
    if wait_running 180 && [ -n "$(networks)" ]; then
        pass "after prune: the network was recreated and the run is running"
    else
        fail "after prune: phase=$(phase), networks: $(networks | tr '\n' ' ')"
    fi
    kill -INT "$pid"
    wait_exit 20 || true
else
    fail "prune: the labelled network remains: $(networks | tr '\n' ' ')"
fi

if [ "$failures" -eq 0 ]; then
    echo "ALL CHECKS PASSED"
else
    echo "$failures CHECK(S) FAILED"
    exit 1
fi
