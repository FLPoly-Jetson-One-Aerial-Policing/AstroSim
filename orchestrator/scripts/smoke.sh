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
#  10. --prune removes the labelled network, and the next start recreates it,
#  11. the session logs: a download during a run is a zip, the SIGINT in step 4
#      leaves exactly one new zip and no session folder, its combined.log has
#      engine, fc and orchestrator lines and fc.log a launch marker, and after
#      "kill -9" the folder remains and the next start archives it.
#
# With ASTROSIM_SMOKE_PULL=1 it first exercises the download of the images,
# which needs a release build (one that prints an "images" line for --version):
# it removes the build's two images, then checks that the startup check
# downloads them, that Stop cancels the download, and that Start downloads
# them again and reaches "running". Everything else then runs as above. Run it
# where the registry is reachable without credentials, as an end user's machine
# is, and expect it to download the images in full (the engine is about 1.6 GB).
#
# Environment:
#   CONTAINER_RUNTIME       podman (default) or docker
#   ASTROSIM_ORCH_LISTEN    dashboard address (default 127.0.0.1:8770)
#   ASTROSIM_BIN            orchestrator binary (default build/orchestrator/astrosim)
#   ASTROSIM_SMOKE_PULL     set to 1 to test downloading the images first
#
# The test keeps its logs in a temporary ASTROSIM_ORCH_LOG_DIR of its own.
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
logroot="$(mktemp -d "${TMPDIR:-/tmp}/astrosim-smoke-logs.XXXXXX")"
export ASTROSIM_ORCH_LOG_DIR="$logroot"
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
    rm -rf "$logroot"
    if [ "$failures" -ne 0 ] || [ "$rc" -ne 0 ]; then
        echo "--- orchestrator log ($log) ---"
        tail -n 40 "$log" || true
    else
        rm -f "$log"
    fi
}
trap cleanup EXIT

# zips lists the session log archives; folders lists the session folders.
zips() { find "$logroot" -maxdepth 1 -name '*.zip' | sort; }
folders() { find "$logroot" -maxdepth 1 -mindepth 1 -type d | sort; }

# zip_member ZIP NAME prints one file of an archive; it fails when no unzip
# tool is available (status 3), so the caller can skip the check.
zip_member() {
    if command -v unzip >/dev/null 2>&1; then
        unzip -p "$1" "*/$2"
    elif command -v python3 >/dev/null 2>&1; then
        python3 -I - "$1" "$2" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
for n in z.namelist():
    if n.endswith("/" + sys.argv[2]):
        sys.stdout.buffer.write(z.read(n))
PY
    else
        return 3
    fi
}

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

# 0. Optional: downloading the images (ASTROSIM_SMOKE_PULL=1).
if [ "${ASTROSIM_SMOKE_PULL:-}" = 1 ]; then
    refs="$("$bin" --version | sed -n 's/^images //p')"
    [ -n "$refs" ] || { echo "FAIL: ASTROSIM_SMOKE_PULL needs a release build ($bin --version has no images line)"; exit 2; }
    read -r -a ref_list <<<"$refs"
    "$runtime" rmi -f "${ref_list[@]}" >/dev/null 2>&1 || true
    # No --autostart: the download begins with the startup check, and Start is pressed by hand.
    start_orchestrator
    if wait_phase downloading 60; then
        pass "pull: the startup check is downloading"
        if status | grep -q '"download":{"state":"downloading"'; then
            pass "pull: the status shows the download"
        else
            fail "pull: the status has no download while downloading"
        fi
    else
        fail "pull: not downloading within 60 s (phase=$(phase))"
        exit 1
    fi
    if post stop && wait_phase stopped 30 && status | grep -q '"code":"pull_cancelled"'; then
        pass "pull: Stop cancels the download"
    else
        fail "pull: Stop did not cancel the download (phase=$(phase))"
    fi
    pull_started=$SECONDS
    if post start && wait_running 1800; then
        pass "pull: downloaded and running after $((SECONDS - pull_started))s"
    else
        fail "pull: not running within 1800 s of Start (phase=$(phase))"
        exit 1
    fi
    for r in "${ref_list[@]}"; do
        if "$runtime" image inspect "$r" >/dev/null 2>&1; then
            pass "pull: $r is present"
        else
            fail "pull: $r is missing after the download"
        fi
    done
    kill -INT "$pid"
    if wait_exit 30 && wait_no_managed 30; then
        pass "pull: orchestrator exited and no managed containers remain"
    else
        fail "pull: orchestrator or containers remain: $(managed | tr '\n' ' ')"
    fi
    newest="$(zips | tail -n 1)"
    if engine_log="$(zip_member "$newest" engine.log)"; then
        if grep -q '| ==== pull .* ====$' <<<"$engine_log" && grep -q '| ==== pull cancelled ====$' <<<"$engine_log" &&
            grep -q '| ==== pull done in ' <<<"$engine_log"; then
            pass "pull: engine.log has the pull markers, including the cancelled download"
        else
            fail "pull: engine.log lacks the pull markers"
        fi
    else
        echo "SKIP: reading the log archive (needs unzip or python3)"
    fi
fi

# 1. Start.
start_orchestrator --autostart
if wait_running 180; then
    pass "start: running with both components ready after about ${SECONDS}s"
else
    fail "start: not running within 180 s (phase=$(phase))"
    exit 1
fi

# 1b. A download during the run is a zip.
if [ "$(curl -fsS --max-time 20 "$base/api/v1/logs/download" 2>/dev/null | head -c 2)" = PK ]; then
    pass "logs: a download during the run is a zip"
else
    fail "logs: the download during the run is not a zip"
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
    zips_before="$(zips | wc -l)"
    kill -INT "$pid"
    if wait_exit 20 && wait_no_managed 30; then
        pass "SIGINT: orchestrator exited and no managed containers remain"
    else
        fail "SIGINT: containers: $(managed | tr '\n' ' ')"
    fi
    # 11a. The session's logs were archived: one new zip, no folder.
    zips_after="$(zips | wc -l)"
    if [ "$((zips_after - zips_before))" -eq 1 ] && [ -z "$(folders)" ]; then
        pass "logs: SIGINT left one new zip and no session folder"
    else
        fail "logs: expected one new zip and no folder; zips $zips_before -> $zips_after, folders: $(folders | tr '\n' ' ')"
    fi
    newest="$(zips | tail -n 1)"
    if combined="$(zip_member "$newest" combined.log)"; then
        missing=""
        for src in engine fc orchestrator; do
            grep -Eq "^[0-9-]+ [0-9:.]+ $src +\| " <<<"$combined" || missing="$missing $src"
        done
        if [ -z "$missing" ]; then
            pass "logs: combined.log has engine, fc and orchestrator lines"
        else
            fail "logs: combined.log has no lines from:$missing"
        fi
        fclog="$(zip_member "$newest" fc.log)"
        if grep -q '| ==== launch gen=' <<<"$fclog"; then
            pass "logs: fc.log has a launch marker"
        else
            fail "logs: fc.log has no launch marker"
        fi
    else
        echo "SKIP: reading the log archive (needs unzip or python3)"
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
    killed_folders="$(folders | wc -l)"
    killed_zips="$(zips | wc -l)"
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
    if [ "$killed_folders" -eq 1 ]; then
        pass "logs: kill -9 left the session folder"
    else
        fail "logs: expected one folder after kill -9, found $killed_folders"
    fi
    start_orchestrator --autostart
    if wait_running 180; then
        pass "restart after kill -9: running"
    else
        fail "restart after kill -9: not running (phase=$(phase))"
    fi
    # Recovery runs in the background, so give it a moment. The folder left by
    # the hard kill must now be a zip; the current session's folder remains.
    recovered=""
    for _ in $(seq 1 25); do
        if [ "$(zips | wc -l)" -gt "$killed_zips" ] && [ "$(folders | wc -l)" -eq 1 ]; then
            recovered=1
            break
        fi
        sleep 0.2
    done
    if [ -n "$recovered" ]; then
        pass "logs: the next start archived the folder left by kill -9"
    else
        fail "logs: the leftover folder was not archived (zips $killed_zips -> $(zips | wc -l), folders: $(folders | tr '\n' ' '))"
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
