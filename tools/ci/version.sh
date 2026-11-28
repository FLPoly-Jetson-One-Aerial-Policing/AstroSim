#!/usr/bin/env bash
# Works out what a CI run is building, from the GitHub event it was started by.
#
#   tools/ci/version.sh
#
# Reads these environment variables (GitHub Actions sets the GITHUB_* ones):
#
#   GITHUB_EVENT_NAME        pull_request, push or workflow_dispatch
#   GITHUB_REF               refs/pull/<n>/merge, refs/heads/main or refs/tags/v<semver>
#   GITHUB_SHA               the commit being built
#   GITHUB_REPOSITORY_OWNER  used for the image repository (lowercased here)
#   PR_NUMBER                optional; the pull request number (default: taken from GITHUB_REF)
#   PR_HEAD_SHA              optional; a pull request's own head commit, which is more
#                            useful in a version than GitHub's temporary merge commit
#   COMMIT_DATE              optional; YYYYMMDD for a nightly (default: the commit's UTC date)
#
# and prints "key=value" lines, to $GITHUB_OUTPUT when that is set, else to stdout:
#
#   kind              pr, nightly, release or dev
#   version           what "astrosim --version" reports
#   image_tag         tag embedded in release-flavoured binaries ("" = dev build, no pulling)
#   archive_label     <label> in astrosim-<label>-linux-amd64.tar.gz
#   prerelease        true for a SemVer prerelease tag such as v0.1.0-rc.1, else false
#   sha7              the first 7 characters of the commit
#   sha_tag           sha-<sha7>, the image tag CI pushes first
#   promote_tags      space-separated tags to add once the gates pass ("" = none)
#   image_repository  ghcr.io/<owner>, lowercase ("" when there is no owner)
#
# The rules are documented in docs/releasing.md. A tag that starts with "v" but is
# not SemVer (vbad, v1.2, v01.2.3, v1.2.3+meta) is rejected, so nothing odd is released.
set -euo pipefail

die() {
    echo "version.sh: $*" >&2
    exit 1
}

event="${GITHUB_EVENT_NAME:-}"
ref="${GITHUB_REF:-}"
sha="${GITHUB_SHA:-}"
[ -n "$event" ] || die "GITHUB_EVENT_NAME is not set"
[ -n "$sha" ] || die "GITHUB_SHA is not set"

# A pull request is better identified by its own head than by the merge commit.
[ "$event" = pull_request ] && sha="${PR_HEAD_SHA:-$sha}"
case "$sha" in
    *[!0-9a-f]* | "") die "the commit id must be lowercase hex, got '$sha'" ;;
esac
[ "${#sha}" -ge 7 ] || die "commit id '$sha' is too short"
sha7="${sha:0:7}"

kind="" version="" image_tag="" archive_label="" prerelease=false promote_tags=""

# SemVer 2.0.0 without build metadata ("+" is not allowed in a Docker tag).
num='0|[1-9][0-9]*'
pre_id='0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*'
semver_re="^v(${num})\\.(${num})\\.(${num})(-((${pre_id})(\\.(${pre_id}))*))?\$"

case "$event" in
    pull_request)
        pr="${PR_NUMBER:-}"
        if [ -z "$pr" ]; then
            case "$ref" in
                refs/pull/*/*)
                    pr="${ref#refs/pull/}"
                    pr="${pr%%/*}"
                    ;;
            esac
        fi
        case "$pr" in
            "" | *[!0-9]*) die "cannot work out the pull request number (PR_NUMBER='${PR_NUMBER:-}', GITHUB_REF='$ref')" ;;
        esac
        kind="pr"
        version="pr${pr}-${sha7}"
        archive_label="dev-${sha7}"
        ;;
    push)
        case "$ref" in
            refs/heads/main)
                date="${COMMIT_DATE:-}"
                if [ -z "$date" ]; then
                    date="$(TZ=UTC git show -s --format=%cd --date=format-local:%Y%m%d "$sha" 2>/dev/null)" ||
                        die "cannot read the date of commit $sha; set COMMIT_DATE=YYYYMMDD"
                fi
                case "$date" in
                    [0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]) ;;
                    *) die "COMMIT_DATE must be YYYYMMDD, got '$date'" ;;
                esac
                kind=nightly
                version="nightly-${date}-${sha7}"
                image_tag="sha-${sha7}"
                archive_label=nightly
                promote_tags=main
                ;;
            refs/tags/v*)
                tag="${ref#refs/tags/}"
                [[ $tag =~ $semver_re ]] ||
                    die "tag '$tag' is not SemVer (expected vMAJOR.MINOR.PATCH or vMAJOR.MINOR.PATCH-prerelease, e.g. v0.1.0 or v0.1.0-rc.1)"
                core="${tag#v}"
                kind=release
                version="$core"
                image_tag="$core"
                archive_label="$core"
                promote_tags="$core"
                if [ -n "${BASH_REMATCH[4]}" ]; then
                    prerelease=true
                else
                    # Only a stable release moves the floating X.Y and latest tags.
                    promote_tags="$core ${BASH_REMATCH[1]}.${BASH_REMATCH[2]} latest"
                fi
                ;;
            *) die "a push to '$ref' is not built; expected refs/heads/main or refs/tags/v*" ;;
        esac
        ;;
    workflow_dispatch)
        # A manual run builds and tests like a pull request, and never publishes.
        kind=dev
        version="dev-${sha7}"
        archive_label="dev-${sha7}"
        ;;
    *) die "unsupported event '$event' (expected pull_request, push or workflow_dispatch)" ;;
esac

owner="${GITHUB_REPOSITORY_OWNER:-}"
image_repository=""
[ -n "$owner" ] && image_repository="ghcr.io/${owner,,}"

emit() {
    printf '%s=%s\n' \
        kind "$kind" \
        version "$version" \
        image_tag "$image_tag" \
        archive_label "$archive_label" \
        prerelease "$prerelease" \
        sha7 "$sha7" \
        sha_tag "sha-${sha7}" \
        promote_tags "$promote_tags" \
        image_repository "$image_repository"
}

if [ -n "${GITHUB_OUTPUT:-}" ]; then
    emit >>"$GITHUB_OUTPUT"
else
    emit
fi
