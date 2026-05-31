# Agent Assets

This directory contains local Codex agent assets for this repository.

## Skills

- `skills/source-command-dedupe/SKILL.md`: migrated source command for finding likely duplicate GitHub issues.
- `skills/source-command-oncall-triage/SKILL.md`: migrated source command for identifying high-impact GitHub issues that need oncall attention.
- `skills/inlong-branding-migration/SKILL.md`: entrypoint for preserving the OrcaSlicer-to-InlongSlicer migration during upstream updates. It points to `../InlongSlicer_doc/orcaslicer_to_inlongslicer_migration.md` for the full checklist.
- `skills/project-isolation-workflow/SKILL.md`: entrypoint for project-local Codex memory, isolation, and multi-agent coordination.

## Docs

- `docs/agent-status.template.md`: tracked template for the local controller and employee-agent status board.
- `docs/agent-status.md`: local runtime status board, intentionally gitignored and not part of Agents deployments.
- `docs/project-memory.md`: overview of repo-local memory.
- `docs/global-knowledge-imports/`: read-only import inboxes for authorized scans of global Codex memory or skill content before anything is promoted into active project knowledge.
- `docs/agents/`: canonical compact Agents policy pack imported from the local Agents project.
- `docs/memory/index.md`: searchable index for verified project lessons.
- `docs/memory/entries/`: detailed project-local memory entries.
- `docs/decisions/`: durable Codex/project operating decisions.
- `docs/runbooks/`: repeatable Codex workflows.
- `docs/templates/agents/`: source-neutral deployment templates for authorized target repos.
- `docs/runbooks/task-closeout.md`: closeout checklist for non-trivial single-session tasks.

## Maintenance

- Keep repository-wide policy in `../AGENTS.md`.
- Keep long-term Codex knowledge in `docs/` or `skills/`, not in global Codex folders.
- Keep each skill self-contained enough to run without opening the matching Claude command first.
- If a skill mirrors `.claude/commands/<name>.md`, update both files in the same change.
- Do not put build, profile, packaging, or test policy here unless it is required to operate a skill.
- Do not overwrite existing skills when importing local operating rules from another repository.
