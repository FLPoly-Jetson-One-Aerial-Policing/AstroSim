#!/usr/bin/env bash
# Runs every static check on the repository inside a container with pinned tools (tools/lint.Dockerfile).
#
#   tools/lint.sh          run all checks; the repository is mounted read-only
#   tools/lint.sh --fix    first reformat the C/C++ sources with clang-format, then run all checks
#
# Checks: clang-format (all tracked C/C++ files), shellcheck (tracked *.sh), hadolint (tracked Dockerfiles),
# actionlint (tracked .github/workflows files, when there are any) and doxygen (the repository Doxyfile, which
# turns every warning into an error). Every check runs even when an earlier one fails. The exit status is
# non-zero if any check failed. Only clang-format findings can be fixed automatically.
#
# Set CONTAINER_RUNTIME=docker to use docker instead of podman.
set -euo pipefail

# Runs the checks. Executed inside the container, where the repository is mounted at the current directory and the
# list of tracked files is mounted at $LINT_FILE_LIST.
run_checks() {
    local fix="$1"
    local -a cxx=() shell=() dockerfiles=() workflows=()
    local file
    while IFS= read -r file; do
        [ -f "$file" ] || continue
        case "$file" in
            *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.hxx) cxx+=("$file") ;;
        esac
        case "$file" in
            *.sh) shell+=("$file") ;;
        esac
        case "$(basename "$file")" in
            Dockerfile|*.Dockerfile) dockerfiles+=("$file") ;;
        esac
        case "$file" in
            .github/workflows/*.yml|.github/workflows/*.yaml) workflows+=("$file") ;;
        esac
    done <"$LINT_FILE_LIST"

    # Global (not local) so that the EXIT trap can still see it after this function returns.
    scratch="$(mktemp -d)"
    trap 'rm -rf "$scratch"' EXIT

    local -a names=() results=()
    # check <name> <command...>: runs the command, keeps its output, and prints the output only if it fails.
    check() {
        local name="$1"
        shift
        local log="$scratch/check.log"
        echo "==> $name"
        if "$@" >"$log" 2>&1; then
            results+=("PASS")
        else
            results+=("FAIL")
            cat "$log"
        fi
        names+=("$name")
    }

    # Doxygen writes its output and warning log to the scratch directory, never to the repository. Later settings
    # override earlier ones, so the extra lines after the Doxyfile redirect it.
    # shellcheck disable=SC2329  # Called through check().
    doxygen_check() {
        local rc=0
        {
            cat Doxyfile
            echo "OUTPUT_DIRECTORY = $scratch/doxygen"
            echo "WARN_LOGFILE = $scratch/doxygen-warnings.log"
        } | doxygen - || rc=$?
        [ -f "$scratch/doxygen-warnings.log" ] && cat "$scratch/doxygen-warnings.log"
        return "$rc"
    }

    if [ "$fix" = 1 ] && [ "${#cxx[@]}" -gt 0 ]; then
        echo "==> clang-format --fix"
        clang-format -i "${cxx[@]}"
    fi

    if [ "${#cxx[@]}" -gt 0 ]; then
        check "clang-format (${#cxx[@]} files)" clang-format --dry-run -Werror "${cxx[@]}"
    fi
    if [ "${#shell[@]}" -gt 0 ]; then
        check "shellcheck (${#shell[@]} files)" shellcheck -x "${shell[@]}"
    fi
    if [ "${#dockerfiles[@]}" -gt 0 ]; then
        check "hadolint (${#dockerfiles[@]} files)" hadolint --config .hadolint.yaml "${dockerfiles[@]}"
    fi
    if [ "${#workflows[@]}" -gt 0 ]; then
        check "actionlint (${#workflows[@]} files)" actionlint "${workflows[@]}"
    fi
    check "doxygen" doxygen_check

    local i failed=0
    echo
    echo "Summary"
    for i in "${!names[@]}"; do
        printf '  %-4s  %s\n' "${results[$i]}" "${names[$i]}"
        [ "${results[$i]}" = PASS ] || failed=1
    done
    return "$failed"
}

if [ "${1:-}" = --in-container ]; then
    run_checks "${2:-0}"
    exit $?
fi

fix=0
case "${1:-}" in
    --fix) fix=1 ;;
    "") ;;
    -h|--help)
        sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "usage: $0 [--fix]" >&2
        exit 2
        ;;
esac

runtime="${CONTAINER_RUNTIME:-podman}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

# The container has no git, so the list of tracked files is computed here and mounted in.
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
git ls-files >"$scratch/files"
# The container user must be able to read the list.
chmod 0644 "$scratch/files"

# The image build is quiet unless it fails. Layers are cached, so this is fast after the first run.
image="astrosim-lint"
if ! "$runtime" build -t "$image" -f tools/lint.Dockerfile tools >"$scratch/build.log" 2>&1; then
    cat "$scratch/build.log" >&2
    echo "lint: building the $image image failed" >&2
    exit 1
fi

# SELinux labels are disabled for the container rather than relabelling the repository. The container runs as the
# calling user so that files rewritten by --fix stay owned by that user.
args=(run --rm --security-opt label=disable)
if [ "$runtime" = podman ]; then
    args+=(--userns=keep-id)
else
    args+=(--user "$(id -u):$(id -g)")
fi
mount=ro
[ "$fix" = 1 ] && mount=rw
args+=(
    -v "$root:/repo:$mount"
    -v "$scratch/files:/lint-files:ro"
    -e LINT_FILE_LIST=/lint-files
    -e HOME=/tmp
    -w /repo
    "$image"
    tools/lint.sh --in-container "$fix"
)
"$runtime" "${args[@]}"
