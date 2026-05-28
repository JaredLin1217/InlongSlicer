# Global Knowledge Migration

Use this runbook when the user wants knowledge that was stored in global Codex Memory or global Codex skill folders moved into InlongSlicer.

## Boundary

Global paths are project-external. Do not read, copy, edit, delete, or move them unless the user explicitly authorizes exact source paths and actions.

Common global Codex locations may include:

```text
C:\Users\v_jar\.codex\memories\
C:\Users\v_jar\.codex\skills\
```

These paths are examples, not authorization.

## Migration Rule

Default to copy-and-reclassify, not destructive move.

Do not delete or modify the global source after import unless the user explicitly authorizes the exact delete or edit action.

## Classification

For each authorized global item, decide where it belongs:

- Every-session InlongSlicer rule: merge into `AGENTS.md`.
- Verified reusable InlongSlicer lesson: add to `.agents/docs/memory/index.md` and `.agents/docs/memory/entries/`.
- Repeatable InlongSlicer workflow: add to `.agents/docs/runbooks/` or `.agents/skills/`.
- Durable operating decision: add to `.agents/docs/decisions/`.
- Product or migration documentation: add to `InlongSlicer_doc/`.
- Generic or unrelated content: do not import.

## Procedure

1. Confirm the exact global source path and permitted action with the user.
2. Run `git status -sb --untracked-files=all` in this repository.
3. Read only the authorized global source path.
4. Summarize the candidate content before importing it if the content is large, stale, or mixed with unrelated projects.
5. Import only InlongSlicer-specific, verified, reusable knowledge.
6. Preserve source attribution in the new local file when useful.
7. Update `.agents/README.md`, `.agents/docs/project-structure.md`, or `AGENTS.md` only if the import changes the operating layout.
8. Run the no-script checks in `.agents/docs/runbooks/isolation-audit.md`.
9. Report global reads and any global writes exactly in the closeout.

## Do Not Import

- Secrets, tokens, API keys, credentials, or private account data.
- Generic Codex preferences unrelated to InlongSlicer.
- Lessons from unrelated repositories.
- Stale instructions contradicted by current InlongSlicer files, scripts, CI, or logs.
- Generated summaries that cannot be traced to a useful rule or workflow.

## Closeout

Report:

- global source paths read,
- global source paths written or deleted, if any,
- local destination paths,
- what was skipped and why,
- verification performed,
- whether follow-up cleanup of the global source is still pending.
