---
name: project-memory
description: Recall verified project knowledge, automatically save reviewed reusable findings, or checkpoint and resume unfinished repository work.
---

# Project memory

Resolve tools from `.agents/managed.json` in a Consumer. Start with
`project-memory.ps1 -Action Recall -Query <terms>`; Query accepts multiple terms.
Use Module, Tag, File or Scope filters and Expand only when evidence is needed.
Recall rechecks immutable records; the index never supplies authority or permission.
Native Codex memory is a supplementary recall layer, not required project storage.

After checking sources and the conclusion, automatically save reusable,
nonsensitive findings as reviewed v4 proposals with Promote -Automatic. Record
source kind, locator, snapshot hash, verification and freshness. Official-document
promotion also requires VerifyOfficial and a matching live content hash. File
commit fields identify checkout anchors; they do not prove uncommitted bytes are
in that commit. Keep official summaries or decision evidence in durable project
files; temporary receipts cannot be the only knowledge evidence. Mechanical
checks do not prove semantic truth. Unverified hypotheses stay in runtime.

Existing entries are immutable. Migrate uses a reviewed v4 proposal identifying
legacy IDs in supersedes; Retract creates a new record with retracts. Source drift,
expiry, explicit conflicts, duplicate keys and broken retirement chains suppress
recall. Resolve with a newly reviewed record; retired knowledge never revives just
because its replacement becomes unusable. Never auto-share project knowledge.

For long work, copy the task-input template into a registered runtime draft.
Save with an explicit ExpectedRevision (0 for new tasks). Record the latest goal,
acceptance criteria, boundaries, completed work, knowledge IDs, receipt references
and completed external actions. Meaningful changes create immutable revisions;
requirements changes also append their own history. Resume rechecks HEAD, index,
worktree and file hashes. Multiple tasks require explicit selection. Complete
closes the task and starts 90 days; its registered receipt dependencies remain
protected until the task expires. See the operator guide for migration or recovery.

Plan/read-only work only recalls and inspects; it never promotes or cleans. Hooks
provide advisory inspection, while explicit commands work without hook activation.
