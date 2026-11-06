#!/usr/bin/env bash
# Run all test suites. Usage: ./test.sh [--integration]
#   default        : C++ host suites (host: input; container: wire, gateway; via ctest) + Go suites (go vet, go test)
#   --integration  : also start the engine container and run the scripted climb against it
#                    (needs podman and the astrosim-engine:dev image)
set -u
cd "$(dirname "$0")"

INTEGRATION=0
[[ "${1:-}" == "--integration" ]] && INTEGRATION=1

declare -a NAMES=() RESULTS=()
run() {  # run <name> <command...>
  local name=$1; shift
  echo "=== $name"
  if "$@"; then RESULTS+=(PASS); else RESULTS+=(FAIL); fi
  NAMES+=("$name")
}

gen=()
command -v ninja >/dev/null && gen=(-G Ninja)

for part in host container; do
  run "cmake configure ($part)" cmake -S "$part" -B "build/$part" "${gen[@]}" -DCMAKE_BUILD_TYPE=Release
  run "cmake build ($part)"     cmake --build "build/$part"
  run "C++ ctest ($part)"       ctest --test-dir "build/$part" --output-on-failure
done
run "Go vet"          bash -c 'cd orchestrator && go vet ./...'
run "Go test"         bash -c 'cd orchestrator && go test -count=1 ./...'

integration() {
  local rc=1
  podman image exists astrosim-engine:dev || { echo "image astrosim-engine:dev missing (see SETUP.md)"; return 1; }
  podman rm -f astrosim-engine >/dev/null 2>&1
  podman run -d --rm --name astrosim-engine -p 8765:8765/udp astrosim-engine:dev >/dev/null || return 1
  for _ in $(seq 1 60); do  # wait until gz is stepping
    podman exec astrosim-engine timeout 2 gz topic -e -t /stats -n 1 >/dev/null 2>&1 && break
    sleep 0.5
  done
  sleep 1
  build/host/climb 127.0.0.1 8765 2.0 12 && rc=0
  podman stop -t 2 astrosim-engine >/dev/null 2>&1
  return $rc
}
if (( INTEGRATION )); then
  run "integration (container + climb to 2 m)" integration
fi

echo
echo "=== summary"
fail=0
for i in "${!NAMES[@]}"; do
  printf '  %-45s %s\n' "${NAMES[$i]}" "${RESULTS[$i]}"
  [[ "${RESULTS[$i]}" == FAIL ]] && fail=1
done
exit $fail
