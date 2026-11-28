# Releases, versions and the build pipeline

This page is for maintainers and contributors. Users only need to download an archive from the
[Releases page](https://github.com/FLPoly-Jetson-One-Aerial-Policing/AstroSim/releases) and run it; see
[Starting the simulator](../README.md#starting-the-simulator).

Everything is built by one GitHub Actions workflow, [`.github/workflows/ci.yml`](../.github/workflows/ci.yml). It lints, builds
and tests the orchestrator (on Linux and Windows) and both images, runs the end-to-end smoke test, and packages the archives.
A release is gated by the same checks as every pull request.

## What a build produces

| Trigger | Version (`astrosim --version`) | Image tags pushed | Archive label | Published to |
|-|-|-|-|-|
| Pull request | `pr<N>-<sha7>` | none (built only) | `dev-<sha7>` | workflow artifact (7 days) |
| Push to `main` | `nightly-<YYYYMMDD>-<sha7>` | `sha-<sha7>`, then `main` once the checks pass | `nightly` | workflow artifact, and the `nightly` prerelease |
| Tag `vX.Y.Z` | `X.Y.Z` | `sha-<sha7>`, then `X.Y.Z`, `X.Y` and `latest` | `X.Y.Z` | a GitHub Release, with generated notes |
| Tag `vX.Y.Z-pre` | `X.Y.Z-pre` | `sha-<sha7>`, then `X.Y.Z-pre` (no `X.Y`, no `latest`) | `X.Y.Z-pre` | a GitHub prerelease |
| Manual run | `dev-<sha7>` | none | `dev-<sha7>` | workflow artifact |

The rules live in [`tools/ci/version.sh`](../tools/ci/version.sh), which you can run by hand with the same environment variables
to see what a tag or push would produce. A tag that starts with `v` but is not SemVer (`vbad`, `v1.2`, `v1.2.3+meta`) is
rejected, so a typo cannot publish anything.

An archive is named `astrosim-<label>-linux-amd64.tar.gz` or `astrosim-<label>-windows-amd64.zip`, and a `SHA256SUMS` file is
published with them (`sha256sum -c SHA256SUMS`). Each archive holds one folder, `astrosim-<label>/`, with the program,
`README.md` and `LICENSE`. They are packed by [`tools/package.sh`](../tools/package.sh), reproducibly: the same binaries give
the same bytes.

### The images

The two images are published to the GitHub Container Registry as
`ghcr.io/flpoly-jetson-one-aerial-policing/astrosim-engine` and `.../astrosim-fc`.

| Tag | Meaning |
|-|-|
| `sha-<sha7>` | the exact commit. Pushed first, before any check beyond the image's own unit tests. Kept forever |
| `main` | the latest commit on `main` that passed every check |
| `X.Y.Z` | that release (or prerelease, for `X.Y.Z-rc.1` and similar) |
| `X.Y` | the newest stable release of that minor version |
| `latest` | the newest stable release |
| `buildcache` | BuildKit's layer cache, used to make builds faster. Not an image to run |

Images are built once. The extra tags are added by retagging the image that was tested (`docker buildx imagetools create`), never
by rebuilding it.

A release or nightly `astrosim` has its own image tag built in (`X.Y.Z` for a release, `sha-<sha7>` for a nightly), so it always
downloads exactly the images it was tested with, the first time it runs. A binary built on a pull request or on your own machine
uses the local images `astrosim-engine` and `astrosim-fc` and never downloads anything.

## Versioning

**There are no tagged releases until the MVP is reached.** Until then, the nightly prerelease is the only published build,
so don't push a `v*` tag before the maintainers agree the MVP is done.

AstroSim uses [Semantic Versioning](https://semver.org/), starting at **v0.1.0** for the MVP. Before 1.0, a
new minor version (`0.2.0`) brings features and a patch version (`0.1.1`) brings fixes only. Use a prerelease suffix such as
`-rc.1` for a dry run: it gets a prerelease with its own assets and image tag, and does not touch `latest` or `X.Y`.

## Cutting a release

1. Make sure `main` has everything the release needs, and that its last CI run is green.
2. Create an annotated tag on the commit, and push it:

   ```
   git switch main && git pull
   git tag -a v0.1.0 -m "AstroSim 0.1.0"
   git push origin v0.1.0
   ```

   For a dry run use a prerelease tag, `v0.1.0-rc.1`.
3. Watch the **CI** run for the tag in the Actions tab. When every check, the image download test and the retagging have
   passed, the **release** job creates the GitHub Release with the archives and `SHA256SUMS`, and release notes generated from
   the merged pull requests.
4. Download an archive on a clean machine and run it, to check the user's view.

If a run fails after the tag is pushed (for example a flaky check), fix `main` and re-run the failed jobs, or make the next
version. Tags that start with `v` cannot be moved or deleted once pushed (see the rulesets below), so do not reuse one.

If somebody creates the release by hand in the GitHub web page first, the workflow notices it and only uploads the archives to it.

## The nightly prerelease

Every push to `main` that passes all checks refreshes one **prerelease** called `nightly`. The `nightly` tag moves to that
commit and the archives are replaced, under stable names (`astrosim-nightly-linux-amd64.tar.gz`, and so on). Its notes say
which commit it was built from. It is for trying the latest work, is not a supported release, and is never marked as the
latest release. A nightly `astrosim` downloads the `sha-<sha7>` images of its own commit.

## One-time GitHub settings

These are done once by a repository admin, after the pipeline has run for the first time. Nothing here is applied by merging a
pull request.

1. **Make the two packages public.** In the organisation, open *Packages*, choose `astrosim-engine`, then *Package settings*,
   *Change visibility*, *Public*. Do the same for `astrosim-fc`. Until this is done, users cannot download the images without
   logging in, and the **smoke-pull** job fails, which blocks promotion and the nightly. That failure is the check working. Make the
   packages public and re-run the failed jobs. If the organisation does not allow public packages, an organisation owner has to
   enable that first.
2. **Create the `maintainers` team** (see [CODEOWNERS](../.github/CODEOWNERS)) and give it *maintain* access to the repository.
   Pull requests need one approval from a code owner.
3. **Apply the rulesets** in [`.github/rulesets/`](../.github/rulesets/README.md). It lists which are updated and which are new, and the
   `gh api` commands. Apply `main-ci-green` once the workflow has run, so the required check names exist.
4. **Allow only merge commits** in the repository settings, and turn on the *Update branch* button. The rulesets' README has the
   one `gh api` command for this.
5. Leave the default workflow token permissions on *read*. Each job asks for the extra access it needs.

## Contributing: keeping your branch up to date

- **Merge `main` into your branch, never rebase.** When `main` has moved and the pull request says the branch is out of date,
  press **Update branch** on the pull request, or run:

  ```
  git fetch origin
  git merge origin/main
  ```

  Resolve any conflicts in your branch and push. Do not rebase or force-push a branch that others have reviewed: it throws away
  their approvals and their comments.
- **Pull requests are merged with a merge commit.** Squash and rebase merging are turned off.
- **A pull request needs 2 approvals, at least one from a maintainer** (a code owner). A new push after an approval needs a fresh
  approval, and the last person to push cannot be the one who approves. Every review conversation must be resolved.
- **CI must be green and the branch must be up to date with `main`** before it can merge. After you update the branch, CI runs
  again on the combined code, so `main` only ever receives a commit that was tested against the current `main`.
- Commits follow [AGENTS.md](../AGENTS.md): `<branch-name>: <Imperative summary ending with a period.>`.

## Checking the workflow locally

You cannot run the whole workflow outside GitHub, but its parts run on a developer machine:

- `tools/lint.sh` runs the same lint checks, in the same container.
- `orchestrator/build.sh --test` and `orchestrator/build.sh` run the orchestrator tests and builds.
- `tools/ci/version.sh` and `tools/package.sh` are plain bash scripts. Try them with `GITHUB_EVENT_NAME=push
  GITHUB_REF=refs/tags/v0.1.0 GITHUB_SHA=$(git rev-parse HEAD) tools/ci/version.sh`.
- `orchestrator/scripts/smoke.sh` is the end-to-end test; see [orchestrator/README.md](../orchestrator/README.md).
