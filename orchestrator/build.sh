#!/usr/bin/env bash
# Builds (or just tests) the AstroSim orchestrator inside a container.
#
#   orchestrator/build.sh          build all binaries into build/orchestrator/
#   orchestrator/build.sh --test   only run gofmt, vet, staticcheck and the unit tests
#
# Set CONTAINER_RUNTIME=docker to use docker instead of podman. The binary for
# this machine is also copied to build/orchestrator/astrosim, which the
# repository's top-level "astrosim" symlink points at.
#
# Optional environment, for release builds (CI sets these):
#   ASTROSIM_VERSION           the version to embed instead of `git describe`
#   ASTROSIM_IMAGE_REPOSITORY  registry path of the component images, e.g.
#                              ghcr.io/flpoly-jetson-one-aerial-policing
#   ASTROSIM_IMAGE_TAG         their tag, e.g. 0.1.0
# The last two go together: set both for a release build, which downloads its
# images, or neither for a development build, which uses local images.
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
        sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "usage: $0 [--test]" >&2
        exit 2
        ;;
esac

version="${ASTROSIM_VERSION:-$(git describe --always --dirty 2>/dev/null || echo dev)}"
image_repository="${ASTROSIM_IMAGE_REPOSITORY:-}"
image_tag="${ASTROSIM_IMAGE_TAG:-}"
if [ -n "$image_repository" ] && [ -z "$image_tag" ] || { [ -z "$image_repository" ] && [ -n "$image_tag" ]; }; then
    echo "set ASTROSIM_IMAGE_REPOSITORY and ASTROSIM_IMAGE_TAG together or not at all" >&2
    exit 2
fi
out="$root/build/orchestrator"
mkdir -p "$out"
"$runtime" build -f orchestrator/build.Dockerfile --target dist \
    --build-arg "VERSION=$version" \
    --build-arg "IMAGE_REPOSITORY=$image_repository" --build-arg "IMAGE_TAG=$image_tag" -o "$out" "$root"

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
