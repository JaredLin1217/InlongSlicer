# Task Closeout

Use this runbook before ending any non-trivial InlongSlicer task, including file edits, debugging, audits, documentation updates, multi-step investigations, or work that may need a handoff. Tiny direct answers can use the normal reply format, but must still include the required isolation closeout fields.

## Checklist

1. Review changed files.
   - Run `git status -sb --untracked-files=all`.
   - Inspect the relevant diff and confirm only intended files changed.
   - Note user or parallel-agent changes separately. Do not revert them.

2. Report verification.
   - Run the closest practical test, lint, build, audit, or focused check for the task.
   - For docs-only changes, use `git diff --check -- <changed-files>` when available.
   - State each command run and its result.
   - If verification was skipped or blocked, state why and what risk remains.

3. State remaining risk.
   - Call out unverified behavior, stale generated or runtime copies, unresolved conflicts, missing review, or assumptions.
   - Do not treat the known repo-level/runtime boundary as a new defect when no global Memory, global/system skill, or project-external access exception occurred.
   - For isolation work, mention the boundary only when it affects the current task's trust model or next decision.
   - If no known risk remains, say so.

4. Decide whether durable project knowledge is needed.
   - Every-session rule: `AGENTS.md`.
   - Verified reusable lesson: `.agents/docs/memory/`.
   - Repeatable workflow: `.agents/docs/runbooks/` or `.agents/skills/`.
   - Durable decision: `.agents/docs/decisions/`.
   - Product or behavior documentation: `InlongSlicer_doc/`.
   - Do not create these unless the need is verified and the current task scope allows it. Otherwise, recommend the next action.

5. Use the right handoff workflow.
   - For multi-agent tasks, also use `.agents/docs/runbooks/multi-agent-workflow.md`.
   - For work that may continue across sessions, windows, or sub-agents, also use `.agents/docs/runbooks/session-handoff.md`.
   - If the task used another session or employee agent, update or explicitly reconcile local `.agents/docs/agent-status.md` before the final reply. If it is missing, create it from `.agents/docs/agent-status.template.md`.

## Required Isolation Closeout

Every non-trivial final reply in this repository must include either these expanded fields:

```text
Global Memory: used / not used
Global Skill: used / not used
Project-external reads: none / authorized paths
Project-external writes: none / authorized paths
```

Or the compact equivalent:

```text
Isolation: GM used/not used | GS used/not used | XR none/paths | XW none/paths
```

If any field is not `none` or `not used`, include the exact source, path, action, and reason.
