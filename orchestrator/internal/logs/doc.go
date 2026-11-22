// Package logs collects the technical debugging logs of the orchestrator and
// of every component container into one folder per session.
//
// These are diagnostic logs for a developer who needs to find out why
// something failed. They are not flight-recorder or telemetry data.
//
// A session folder is named <UTC start time>-<session ID> and holds
// combined.log (every line from every source, tagged with its source),
// orchestrator.log and one <component>.log per component. All files are
// written by a single goroutine so lines never interleave, and each file is
// size-capped: it keeps its first part and a rolling tail of its last parts.
// At shutdown the folder is zipped and old archives are pruned (see Archive,
// Recover and Retain).
package logs
