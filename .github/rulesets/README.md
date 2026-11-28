# Repository rulesets

These files are the rules for this repository, kept here so they can be reviewed and
re-applied. Each one is a REST API request body: send it with `gh api --input <file>`.
Nothing here applies itself. A repository admin applies them once (see
[docs/releasing.md](../../docs/releasing.md#one-time-github-settings)).

| File | Ruleset | Status | What it does |
|-|-|-|-|
| `main-must-PR.json` | `main-must-PR` | **Updated**, id `26548048` | Changes to `main` go through a pull request: 2 approvals (one from a code owner), stale approvals dismissed on a new push, the last pusher cannot be the approver, conversations resolved, merge commits only |
| `main-no-force.json` | `main-no-force` | **Unchanged**, id `26548047` | `main` cannot be deleted or force-pushed |
| `main-ci-green.json` | `main-ci-green` | **New** | `lint`, `orchestrator`, `orchestrator-windows`, `images (engine)`, `images (fc)`, `smoke` and `package` must pass, and the branch must be up to date with `main` |
| `release-tags.json` | `release-tags` | **New** | `v*` tags cannot be deleted or moved. The `nightly` tag is not covered, so CI can move it |

The repository admin role can bypass `main-must-PR` and `main-ci-green` (for emergencies, or
for merging your own pull request while working alone). Nobody can bypass the other two.

## Applying

Replace `OWNER/REPO` with `FLPoly-Jetson-One-Aerial-Policing/AstroSim`.

Update the two existing rulesets in place:

```
gh api -X PUT repos/OWNER/REPO/rulesets/26548048 --input .github/rulesets/main-must-PR.json
gh api -X PUT repos/OWNER/REPO/rulesets/26548047 --input .github/rulesets/main-no-force.json
```

Create the new ones:

```
gh api -X POST repos/OWNER/REPO/rulesets --input .github/rulesets/release-tags.json
gh api -X POST repos/OWNER/REPO/rulesets --input .github/rulesets/main-ci-green.json
```

Apply `main-ci-green.json` only **after the workflow has run once** (for example on the pull
request that adds it), because a required check has to exist under its exact name or nothing
can merge. Check the names in the pull request's checks list first.

The code owner rule needs the `maintainers` organisation team to exist, with write access (or
more) to this repository. [`CODEOWNERS`](../CODEOWNERS) names it, and takes effect once it is
on `main`. Create the team before applying `main-must-PR.json`, or no pull request can get the
required code owner approval, except through the admin bypass.

## Merge settings

`main-must-PR` allows only merge commits. Make the repository settings agree, and turn on the
"Update branch" button that contributors use to bring their branch up to date:

```
gh api -X PATCH repos/OWNER/REPO \
  -F allow_squash_merge=false -F allow_rebase_merge=false \
  -F allow_merge_commit=true -F allow_update_branch=true
```
