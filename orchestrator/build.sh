#!/usr/bin/env bash
# Builds (or just tests) the AstroSim orchestrator inside a container.
#
#   orchestrator/build.sh          build all binaries into build/orchestrator/
#   orchestrator/build.sh --test   only run gofmt, vet and the unit tests
#
# Set CONTAINER_RUNTIME=docker to use docker instead of podman. The binary for
# this machine is also copied to build/orchestrator/astrosim, which the
# repository's top-level "astrosim" symlink points at.
set -euo pipefail

runtime="${CONTAINER_RUNTIME:-podman}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

case "${1:-}" in
    --test)
        exec "$runtime" build -f orchestrator/build.Dockerfile --target test "$root"
        ;;
    "") ;;
    -h|--help)
        sed -n '2,9p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "usage: $0 [--test]" >&2
        exit 2
        ;;
esac

version="$(git describe --always --dirty 2>/dev/null || echo dev)"
out="$root/build/orchestrator"
mkdir -p "$out"
"$runtime" build -f orchestrator/build.Dockerfile --target dist \
    --build-arg "VERSION=$version" -o "$out" "$root"

case "$(uname -s)" in
    Linux)  os=linux ;;
    Darwin) os=darwin ;;
    MINGW*|MSYS*|CYGWIN*) os=windows ;;
    *) echo "unknown host OS $(uname -s); built binaries are in $out" >&2; exit 0 ;;
esac
case "$(uname -m)" in
    x86_64|amd64)  arch=amd64 ;;
    aarch64|arm64) arch=arm64 ;;
    *) echo "unknown host architecture $(uname -m); built binaries are in $out" >&2; exit 0 ;;
esac
ext=""
[ "$os" = windows ] && ext=".exe"
cp -f "$out/astrosim-$os-$arch$ext" "$out/astrosim$ext"
echo "Built $version; this machine's binary is $out/astrosim$ext"
