# AGENTS.md

Guidance for AI coding agents (and humans) working in this repository.

## Commits

Make scoped commits: each commit is one logical change, not a dump of the whole branch.

Message format:

```
<branch-name>: <Imperative summary ending with a period.>

* <Optional bullet describing one part of the change.>
* <Another bullet.>
```

- The prefix is the branch name, e.g. `59-aircraft-config-file: Add the Gazebo model generator.`
- Summary in the imperative mood ("Add", "Remove", "Run"), sentence case, ending with a period.
- Body is optional: `*` bullets, one per notable part of the change, each a short sentence.
- A commit that does not build gets `WIP` after the prefix, e.g.
  `36-containerize-physics-engine: WIP Add gz-free gateway core.` Prefer splitting so every commit builds.

## Issues

Every issue has an issue type, a parent epic and a place on the project board. Pick the title prefix and type by what the
issue is:

| Kind | Title | Type | Body |
|-|-|-|-|
| User story (a new capability) | `[STORY] As a <role>, I want <thing> so that <benefit>.` | Feature | The same sentence |
| Improvement to existing behaviour, such as a follow-up found while working | `[IMPV] <Plain imperative summary>`, e.g. `[IMPV] Remove old simulator image versions with --prune` | Improvement | A plain problem statement, then `## Notes` |

- Don't force an improvement into the user-story format when it doesn't fit.
- Other issue types: Task, Bug, Epic, Research Spike.
- Link the issue to its parent epic as a sub-issue (orchestrator work: #8 "[EPIC] System Orchestrator").
- Add the issue to the "AstroSim Agile Board" project.
- `gh issue create` can't set the type or the parent. Set them on the issue page, or with the GraphQL mutations
  `updateIssueIssueType` and `addSubIssue`. Add the issue to the board with
  `gh project item-add 1 --owner FLPoly-Jetson-One-Aerial-Policing --url <issue URL>`.
