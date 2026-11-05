#!/usr/bin/env bash
# Build everything: host C++ (viewer, tools, tests), Go orchestrator, engine container image.
#
#   ./build.sh                 check tools, then build
#   ./build.sh --deps          install missing host dependencies first (dnf, apt or pacman; uses sudo)
#   ./build.sh --deps --dry-run   print the install command instead of running it
#   ./build.sh --pm apt        override package-manager detection (dnf | apt | pacman)
#   ./build.sh --no-image      skip the engine container image build
#   ./build.sh --raylib MODE   AUTO (default: installed raylib, else build 5.5 from source) | SYSTEM | FETCH | OFF
#
# Outputs: build/host/{viewer,climb,latency,...}, build/astrosim, image astrosim-engine:dev
set -euo pipefail
cd "$(dirname "$0")"

DEPS=0 DRY=0 IMAGE=1 PM="" RAYLIB=AUTO
while (($#)); do
  case "$1" in
    --deps) DEPS=1 ;;
    --dry-run) DRY=1 ;;
    --no-image) IMAGE=0 ;;
    --pm) PM="${2:-}"; shift ;;
    --raylib) RAYLIB="${2:-}"; shift ;;
    -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
  shift
done

if [[ -z "$PM" ]]; then
  if command -v dnf >/dev/null; then PM=dnf
  elif command -v apt-get >/dev/null; then PM=apt
  elif command -v pacman >/dev/null; then PM=pacman
  fi
fi

# Core packages: the build cannot work without these.
packages_core() {
  case "$1" in
    dnf)    echo podman gcc-c++ cmake ninja-build git golang pkgconf-pkg-config ;;
    apt)    echo podman g++ cmake ninja-build git golang-go pkg-config ;;
    pacman) echo podman gcc cmake ninja git go pkgconf ;;
    *) return 1 ;;
  esac
}
# Build dependencies for raylib from source, X11 backend. Taken from the raylib wiki
# (https://github.com/raysan5/raylib/wiki/Working-on-GNU-Linux); Wayland packages omitted (experimental, off by default).
# Not needed if you only use an installed raylib (--raylib SYSTEM), but the default AUTO mode may fall back to building it.
packages_gui() {
  case "$1" in
    dnf)    echo alsa-lib-devel mesa-libGL-devel libX11-devel libXrandr-devel libXi-devel libXcursor-devel libXinerama-devel libatomic ;;
    apt)    echo libasound2-dev libx11-dev libxrandr-dev libxi-dev libgl1-mesa-dev libglu1-mesa-dev libxcursor-dev libxinerama-dev ;;
    pacman) echo alsa-lib mesa libx11 libxrandr libxi libxcursor libxinerama ;;
  esac
}

install_deps() {
  [[ -n "$PM" ]] || { echo "no supported package manager found (dnf, apt, pacman); install manually, see SETUP.md" >&2; exit 1; }
  local core gui
  core=$(packages_core "$PM") || { echo "unsupported package manager: $PM (want dnf, apt or pacman)" >&2; exit 2; }
  gui=$(packages_gui "$PM")

  local sudo=""
  if ((EUID != 0)); then
    command -v sudo >/dev/null || { echo "need root or sudo to install packages" >&2; exit 1; }
    sudo="sudo"
  fi
  local -a install
  case "$PM" in
    dnf)    install=($sudo dnf install -y) ;;
    apt)    install=($sudo apt-get install -y) ;;
    pacman) install=($sudo pacman -S --needed --noconfirm) ;;
  esac

  run() { if ((DRY)); then echo "+ $*"; else echo "+ $*"; "$@"; fi; }
  [[ "$PM" == apt ]] && run $sudo apt-get update
  # shellcheck disable=SC2086
  run "${install[@]}" $core
  # shellcheck disable=SC2086
  run "${install[@]}" $gui
}

((DEPS)) && install_deps
((DRY)) && { echo "dry run: nothing built"; exit 0; }

missing=()
for t in g++ cmake go podman; do command -v "$t" >/dev/null || missing+=("$t"); done
if ((${#missing[@]})); then
  echo "missing required tools: ${missing[*]}" >&2
  echo "run ./build.sh --deps to install them (or see SETUP.md)" >&2
  exit 1
fi

gen=()
if command -v ninja >/dev/null; then gen=(-G Ninja); else echo "note: ninja not found, using the default generator"; fi

echo "== C++ (host)"
cmake -S . -B build/host "${gen[@]}" -DCMAKE_BUILD_TYPE=Release -DASTROSIM_RAYLIB="$RAYLIB"
cmake --build build/host
[[ -x build/host/viewer ]] || echo "note: viewer not built (--raylib $RAYLIB found no raylib); engine, tools and tests are unaffected"

echo "== Go orchestrator"
(cd orchestrator && go build -o ../build/astrosim ./cmd/astrosim)

if ((IMAGE)); then
  echo "== engine image (first build takes several minutes)"
  podman build -t astrosim-engine:dev -f Containerfile .
fi

echo
echo "built:"
for f in build/host/viewer build/host/climb build/host/latency build/astrosim; do
  [[ -x "$f" ]] && echo "  $f"
done
((IMAGE)) && echo "  image astrosim-engine:dev"
echo "next: ./test.sh   |   build/astrosim up"
