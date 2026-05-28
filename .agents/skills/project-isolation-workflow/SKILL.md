---
name: project-isolation-workflow
description: Maintain InlongSlicer repository-local Codex memory, isolation rules, and controller plus employee-agent workflows. Use when auditing project isolation, migrating global Codex knowledge into this repo, or coordinating sub-agents.
---

# Project Isolation Workflow

## Overview

Use this skill to keep InlongSlicer Codex work self-contained. Prefer repository files over global Codex Memory, and keep long explanations in `.agents/docs/` instead of bloating `AGENTS.md`.

## Workflow

1. Inspect `git status -sb --untracked-files=all`.
2. Read `AGENTS.md` first.
3. Read `.agents/README.md` when local agent assets are in scope.
4. Read `.agents/docs/project-structure.md` when file layout or knowledge layers are in scope.
5. Read `.agents/docs/project-memory.md` for the memory system overview when memory structure is in scope.
6. Search `.agents/docs/memory/index.md` when prior project experience could affect the task.
7. Read detailed files in `.agents/docs/memory/entries/` only when the index points to a relevant entry.
8. Read `.agents/docs/codex-memory.md` when deciding whether a lesson belongs in project memory, repo docs, a decision, a runbook, a project-local skill, or nowhere.
9. Read `.agents/docs/runbooks/multi-agent-workflow.md` when creating or coordinating employee agents.
10. Read `.agents/docs/runbooks/skill-authoring.md` before creating or updating project-local skills.
11. Read `.agents/docs/runbooks/isolation-audit.md` when skill-source classification, project-external access, or closeout reporting is in scope.
12. Read `.agents/docs/runbooks/session-handoff.md` when work may continue across multiple sessions, windows, or sub-agents.
13. Read and update or explicitly reconcile `.agents/docs/agent-status.md` when assigning agents, receiving agent results, or handing off unfinished work.
14. Read `.agents/docs/runbooks/global-knowledge-migration.md` before importing any global Codex Memory or global skill content.
15. Keep all new project knowledge inside this repo unless the user explicitly asks to re-enable global Memory.
16. Do not intentionally use global/system skills for normal project work.
17. If system/global Codex resources are used because of explicit user request or higher-priority runtime instructions, report what was used and keep project-specific outputs in repo-local files.
18. Do not read or write filesystem paths outside this repository unless the user explicitly authorizes the exact external path and action.

## File Placement Rules

- Put every-session rules in `AGENTS.md`.
- Put agent asset index updates in `.agents/README.md`.
- Put project memory overview in `.agents/docs/project-memory.md`.
- Put searchable memory rows in `.agents/docs/memory/index.md`.
- Put detailed memory entries in `.agents/docs/memory/entries/`.
- Put durable operating decisions in `.agents/docs/decisions/`.
- Put repeatable procedures in `.agents/docs/runbooks/`.
- Put current multi-agent status in `.agents/docs/agent-status.md`.
- Put project-local skills in `.agents/skills/<skill-name>/SKILL.md`.
- Put Codex App project settings in `.codex/`.
- Put product, migration, release, and functional change docs in `InlongSlicer_doc/`.
- Do not write to `C:\Users\v_jar\.codex\memories\` for this project.
- Do not put InlongSlicer-specific rules, lessons, or workflow definitions in global Codex folders.

## System Boundary

- This skill defines repository behavior, not a runtime sandbox.
- Codex system tools, plugins, built-in skills, and global instructions may still exist in the session.
- Project-local skills under this repository's `.agents/skills/` are allowed.
- Do not use global Memory as project context or project storage unless the user explicitly asks.
- Report any system/global Codex resource read or modified during isolation-related work.
- Report global Memory usage, global Skill usage, project-external reads, and project-external writes at the end of every non-trivial Codex reply in this repository.

## Multi-Agent Rules

When the user says `招聘一個員工`:

1. Create a Codex sub-agent only for a bounded task.
2. Assign one role: `explorer` for read-only investigation, or `worker` for bounded implementation.
3. State allowed scope, forbidden scope, verification, and expected final report.
4. Avoid overlapping worker write scopes.
5. Review and integrate results in the controller session.
6. Tell the employee to keep project knowledge repo-local and not to use global/system tools or skills unless explicitly authorized by the controller assignment, the user request, or higher-priority runtime instructions.
7. Require the employee to report any allowed system/global resource use.

## Closeout Checklist

After meaningful work, report:

- files changed,
- verification run,
- remaining risk,
- system/global Codex resources used, if any,
- global Memory usage,
- global Skill usage,
- project-external reads,
- project-external writes,
- whether a new `AGENTS.md` rule, memory entry, decision, runbook, or project-local skill should be added.
