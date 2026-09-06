---
name: project-memory
description: Recall verified project knowledge, save reusable findings, or checkpoint and resume unfinished repository work.
---

Find scripts via `.agents/managed.json`, or `scripts/` in Provider.
Run `project-memory.ps1 -Action Recall -Query <terms>` before relying on prior
facts. It validates source hashes and suppresses stale, conflicted or superseded
entries. Its generated index is advisory, never primary storage.

Prepare a JSON entry matching `knowledge.schema.json` with sources, conclusion,
verification evidence and scope. Use `-Action Promote -InputPath <file>` only
after inspecting sources and validating the conclusion. The tool checks mechanical
integrity, not semantic truth. No secrets, user data or transient guesses belong
in durable knowledge. Resolve conflicts explicitly; supersede by ID.

Save long-work checkpoints with `task-state.ps1 -Action Save -InputPath <file>`.
Record objective, latest adjustment, boundaries, completed actions, open issues
and next steps after meaningful edits, verification and handoff. Resume with
`-Action Resume -Id <id>`; compare reported Git/file state, reinspect changed
sources and never repeat deployment or push from an old summary.
