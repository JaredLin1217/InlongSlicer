# Codex Memory Layout

InlongSlicer uses repository-local Codex memory instead of global Codex Memory.

## Isolation Boundary

- Do not use global Codex Memory as a context source, routing hint, or storage target during normal InlongSlicer work.
- Do not read from or write to `C:\Users\v_jar\.codex\memories\` unless the user explicitly authorizes the exact path and action.
- Do not place InlongSlicer-specific memory, runbooks, decisions, or skills in global Codex folders.
- If a higher-priority runtime instruction or explicit user request requires a global/system skill, tool, plugin, or Codex config, report that usage and keep InlongSlicer-specific results in this repository.
- If a task requires reading or modifying a global Codex path, stop and ask first unless the exact source path and action are already authorized.
- Project-external filesystem access is forbidden by default. Do not read, list, create, edit, delete, move, stage, commit, or configure files outside this repository unless the user explicitly authorizes the exact path and action.

## What Goes Where

- `AGENTS.md`: rules every session must know before working in this repo.
- `.agents/docs/project-memory.md`: overview of the project-local memory system.
- `.agents/docs/memory/index.md`: searchable list of verified, reusable project lessons.
- `.agents/docs/memory/entries/`: detailed project-local memory entries.
- `.agents/docs/decisions/`: durable Codex/project operating decisions.
- `.agents/docs/runbooks/`: repeatable procedures.
- `.agents/docs/runbooks/task-closeout.md`: closeout checklist for non-trivial single-session tasks.
- `.agents/skills/`: project-local skills.
- `InlongSlicer_doc/`: product, migration, release, and functional change documentation.

## When To Propose Project Memory

Propose a project memory draft only when all are true:

- The lesson was verified by files, commands, logs, tests, or observed behavior.
- It is specific to InlongSlicer.
- It is likely to save time in a future task.
- It is not already covered clearly in `AGENTS.md`, `.agents/docs/`, or `InlongSlicer_doc/`.

Do not propose Memory for:

- One-off opinions.
- Unverified assumptions.
- Generic coding preferences.
- Temporary workarounds that may change soon.
- Lessons from unrelated projects.

## Project Memory Draft Template

```text
Index:
- Title:
- Trigger:
- Keywords:
- Summary:
- Detail:

Entry:
## YYYY-MM-DD - Short title

- Trigger:
- Context:
- Cause:
- Fix / Rule:
- Verification:
- Reuse when:
```

## Required Closeout Fields

```text
Global Memory: used / not used
Global Skill: used / not used
Project-external reads: none / authorized paths
Project-external writes: none / authorized paths
```

Compact equivalent:

```text
Isolation: GM used/not used | GS used/not used | XR none/paths | XW none/paths
```
