#!/usr/bin/env bash
# Build the engine and viewer images locally and push them to GHCR.
# Use this for branches where the "Publish images" workflow can't be dispatched
# (GitHub only dispatches workflow_dispatch workflows that exist on the default branch).
#
# Usage: ./push_containers.sh [--tag TAG] [--registry REG] [--no-push]
#   --tag TAG        image tag to publish (default: dev, which compose pulls); sha-<commit> is always pushed too
#   --registry REG   default: ghcr.io/flpoly-jetson-one-aerial-policing
#   --no-push        build only
#
# Needs a registry login with write:packages, e.g.
#   gh auth refresh -s write:packages
#   gh auth token | podman login ghcr.io -u "$(gh api user -q .login)" --password-stdin
set -euo pipefail
cd "$(dirname "$0")"

TAG=dev
REGISTRY=ghcr.io/flpoly-jetson-one-aerial-policing
PUSH=1
while (($#)); do
  case $1 in
    --tag) TAG=${2:?--tag needs a value}; shift 2 ;;
    --registry) REGISTRY=${2:?--registry needs a value}; shift 2 ;;
    --no-push) PUSH=0; shift ;;
    -h|--help) sed -n '2,/^set -/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

if command -v podman >/dev/null; then CE=podman
elif command -v docker >/dev/null; then CE=docker
else echo "need podman or docker" >&2; exit 1; fi

SHA=$(git rev-parse HEAD)
if [[ -n $(git status --porcelain --untracked-files=no) ]]; then
  echo "warning: uncommitted changes; the sha-$SHA tag will not match the pushed image exactly" >&2
fi
SOURCE=$(git remote get-url origin 2>/dev/null | sed -E 's#^git@github.com:#https://github.com/#; s#\.git$##' || true)

# name:Containerfile (build context is the repo root for both)
images=("astrosim-engine:container/Containerfile" "astrosim-viewer:host/Containerfile")

for entry in "${images[@]}"; do
  name=${entry%%:*} file=${entry#*:}
  echo "== build $name ($file)"
  "$CE" build -f "$file" \
    -t "$REGISTRY/$name:$TAG" -t "$REGISTRY/$name:sha-$SHA" \
    ${SOURCE:+--label "org.opencontainers.image.source=$SOURCE"} .
done

if ((PUSH)); then
  for entry in "${images[@]}"; do
    name=${entry%%:*}
    echo "== push $name"
    "$CE" push "$REGISTRY/$name:$TAG"
    "$CE" push "$REGISTRY/$name:sha-$SHA"
  done
fi
echo "done: $REGISTRY/{astrosim-engine,astrosim-viewer}:$TAG (sha-$SHA)"
