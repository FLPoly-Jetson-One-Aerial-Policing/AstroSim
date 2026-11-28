#!/usr/bin/env bash
# Packs the orchestrator binaries into the archives users download.
#
#   tools/package.sh <label> <bin-dir> <out-dir>
#
#   label     the name part of the archives, e.g. 0.1.0, nightly or dev-1a2b3c4
#   bin-dir   holds the build output of orchestrator/build.sh:
#               astrosim-linux-amd64 and astrosim-windows-amd64.exe
#   out-dir   receives (created if missing):
#               astrosim-<label>-linux-amd64.tar.gz     astrosim-<label>/{astrosim,README.md,LICENSE}
#               astrosim-<label>-windows-amd64.zip      astrosim-<label>/{astrosim.exe,README.md,LICENSE}
#               SHA256SUMS                              for the two archives, "sha256sum -c" compatible
#
# The archives are reproducible: entries are sorted, owned by root, and stamped with
# the time of the current commit (or SOURCE_DATE_EPOCH when that is set), so packing
# the same binaries twice gives byte-identical files.
#
# Needs bash, tar, gzip, zip, sha256sum and, unless SOURCE_DATE_EPOCH is set, git.
set -euo pipefail

die() {
    echo "package.sh: $*" >&2
    exit 1
}

[ "$#" -eq 3 ] || die "usage: $0 <label> <bin-dir> <out-dir>"
label="$1"
bin_dir="$2"
out_dir="$3"

# The label ends up in file names and in the archive's top directory.
case "$label" in
    "" | *[!0-9A-Za-z._-]*) die "label '$label' must be non-empty and use only letters, digits, '.', '_' and '-'" ;;
esac

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

linux_bin="$bin_dir/astrosim-linux-amd64"
windows_bin="$bin_dir/astrosim-windows-amd64.exe"
for f in "$linux_bin" "$windows_bin" "$root/README.md" "$root/LICENSE"; do
    [ -f "$f" ] || die "missing input file $f"
done
for tool in tar gzip zip sha256sum; do
    command -v "$tool" >/dev/null || die "'$tool' is required but not installed"
done

epoch="${SOURCE_DATE_EPOCH:-}"
if [ -z "$epoch" ]; then
    epoch="$(git -C "$root" log -1 --format=%ct 2>/dev/null)" ||
        die "cannot read the commit time; set SOURCE_DATE_EPOCH"
fi
case "$epoch" in
    "" | *[!0-9]*) die "commit time '$epoch' is not a number of seconds" ;;
esac

mkdir -p "$out_dir"
out_dir="$(cd "$out_dir" && pwd)"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

top="astrosim-$label"
tar_name="astrosim-$label-linux-amd64.tar.gz"
zip_name="astrosim-$label-windows-amd64.zip"

# stage_dir <os-binary> <name-in-archive> <dir>: lay out one archive's directory
# with explicit modes and times, so the umask and the checkout do not leak in.
stage_dir() {
    local src="$1" name="$2" dir="$3"
    mkdir -p "$dir"
    install -m 0755 "$src" "$dir/$name"
    install -m 0644 "$root/README.md" "$dir/README.md"
    install -m 0644 "$root/LICENSE" "$dir/LICENSE"
    chmod 0755 "$dir"
    touch -d "@$epoch" "$dir" "$dir/$name" "$dir/README.md" "$dir/LICENSE"
}

# Linux: tar.gz. "gzip -n" leaves the file name and time out of the header.
stage_dir "$linux_bin" astrosim "$stage/tar/$top"
rm -f "$out_dir/$tar_name"
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$epoch" \
    -C "$stage/tar" -cf - "$top" | gzip -n -9 >"$out_dir/$tar_name"

# Windows: zip. -X drops extra attributes; the names are given in sorted order.
stage_dir "$windows_bin" astrosim.exe "$stage/zip/$top"
rm -f "$out_dir/$zip_name"
(
    cd "$stage/zip"
    export TZ=UTC
    zip -X -q -9 "$out_dir/$zip_name" "$top" "$top/LICENSE" "$top/README.md" "$top/astrosim.exe"
)

(
    cd "$out_dir"
    sha256sum "$tar_name" "$zip_name" >SHA256SUMS
)

echo "Packed into $out_dir:"
ls -l "$out_dir/$tar_name" "$out_dir/$zip_name" "$out_dir/SHA256SUMS"
