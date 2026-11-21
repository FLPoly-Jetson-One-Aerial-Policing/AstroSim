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
