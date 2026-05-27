# Agent Assets

This directory contains local Codex agent assets for this repository.

## Skills

- `skills/source-command-dedupe/SKILL.md`: migrated source command for finding likely duplicate GitHub issues.
- `skills/source-command-oncall-triage/SKILL.md`: migrated source command for identifying high-impact GitHub issues that need oncall attention.
- `skills/inlong-branding-migration/SKILL.md`: entrypoint for preserving the OrcaSlicer-to-InlongSlicer migration during upstream updates. It points to `../InlongSlicer_doc/orcaslicer_to_inlongslicer_migration.md` for the full checklist.

## Maintenance

- Keep repository-wide policy in `../AGENTS.md`.
- Keep each skill self-contained enough to run without opening the matching Claude command first.
- If a skill mirrors `.claude/commands/<name>.md`, update both files in the same change.
- Do not put build, profile, packaging, or test policy here unless it is required to operate a skill.
