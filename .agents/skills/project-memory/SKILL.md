---
name: project-memory
description: Recall, review and maintain immutable Agents v5 knowledge and task checkpoints without granting action authority.
---

# Project memory

Use `scripts/agents.ps1 memory recall` before relying on a past finding. Queries
accept multiple terms and may filter by module, tag, file or scope. The index is
an advisory cache; every returned record is rechecked against its source, hash,
freshness and retirement graph. Expand only when evidence is needed.

Promote only reviewed, reusable, nonsensitive facts. Record the source kind,
locator, snapshot hash, verification method and review deadline. Official claims
need a live source comparison before automatic promotion. User decisions require
an explicit recorded decision; memory never grants permission. Keep hypotheses,
temporary receipts and unresolved conclusions in runtime.

Records are immutable. Resolve drift, expiry, conflict, duplicate keys and
retraction with a new reviewed record. A retired fact cannot revive because a
replacement became unusable. Native Codex memory is an optional local recall
layer; required rules and project knowledge remain explicit files.

Use `scripts/agents.ps1 task` for checkpoints. Save with an expected revision,
record the latest objective, adjustment, acceptance criteria, boundaries,
knowledge IDs, receipts and completed external actions. Resume checks Git, files
and evidence. Multiple active tasks require an explicit ID. Plan and read-only
operations never promote knowledge, clean runtime or replay actions.
