# AGENTS.md

InlongSlicer is forked from the upstream 2.4.x slicer codebase. Treat `inlong/orca-2.4-base` as the clean upstream base for comparison, rebase analysis, and separating Inlong-specific changes from upstream behavior.

This file is the repository-level agent entrypoint. Keep broad workflow policy here; keep tool-specific prompts inside their own command or skill files.

## Markdown File Map

- `README.md`: user-facing InlongSlicer overview and install notes. Keep upstream references only where they point to retained upstream resources such as wiki/cloud documentation.
- `AGENTS.md`: repository-wide instructions for Codex and compatible coding agents.
- `CLAUDE.md`: compatibility shim that points to `AGENTS.md`; keep it short unless a Claude-only exception is required.
- `InlongSlicer_doc/orcaslicer_to_inlongslicer_migration.md`: source-of-truth checklist for preserving the OrcaSlicer-to-InlongSlicer migration across upstream updates, including naming, colors, assets, profiles, package identity, and allowlisted Orca references.
- `InlongSlicer_doc/functional_change_log.md`: source-of-truth record for Inlong-specific bug fixes, functional changes, profile behavior changes, build workflow changes, packaging behavior changes, and verification notes outside the pure branding migration checklist.
- `.agents/README.md`: local index for Codex agent assets.
- `.agents/docs/`: repository-local Codex memory, runbooks, decisions, and agent handoff status.
- `.agents/docs/project-memory.md`: entry point for project-local memory.
- `.agents/docs/memory/index.md`: searchable index for verified reusable project lessons.
- `.agents/docs/agent-status.template.md`: tracked template for local controller and employee-agent status.
- `.agents/docs/agent-status.md`: local runtime status board; do not deploy it to other projects.
- `.agents/docs/runbooks/`: repeatable Codex workflows that are too long for this file.
- `.agents/docs/runbooks/task-closeout.md`: closeout checklist for non-trivial single-session tasks.
- `.agents/skills/*/SKILL.md`: Codex skill entrypoints for migrated source commands.
- `.claude/commands/*.md`: legacy Claude command prompts. Keep matching command behavior aligned with the corresponding Codex skill when both exist.
- `tests/CLAUDE.md`: test-specific guidance for the `tests/` tree.
- `.github/pull_request_template.md`: PR summary and verification expectations.

## Codex Project Isolation

- Keep InlongSlicer-specific Codex knowledge inside this repository.
- Do not use global Codex Memory as project context or project storage unless the user explicitly authorizes that use.
- Do not place project-specific rules, memory, skills, runbooks, or decisions in `C:\Users\v_jar\.codex\` global locations.
- Do not read, list, create, edit, delete, move, stage, commit, or configure project-external filesystem paths unless the user explicitly authorizes the exact path and action.
- `AGENTS.md` and `.agents/docs/` define behavior rules, not a runtime sandbox. Codex system tools, plugins, built-in skills, and higher-priority instructions may still exist.
- This repo-level/runtime boundary is an accepted limitation, not a defect. Make isolation auditable by avoiding global Memory and Global Skills for normal work, requiring exact authorization for project-external filesystem access, and reporting every exception.
- Project-local skills under this repository's `.agents/skills/` are allowed for normal project work. A Global Skill is any `SKILL.md` outside this repository's `.agents/skills/`.
- Do not intentionally use Global Skills for normal InlongSlicer work unless the user explicitly requests that capability or a higher-priority runtime instruction requires it.
- If a global/system tool, Global Skill, global Memory path, or project-external path is used under an allowed exception, report what was used and why.
- Use `.agents/docs/runbooks/isolation-audit.md` for source classification, project-external access checks, and closeout reporting.

## Project-Local Knowledge Layers

- `AGENTS.md`: rules every session must know before working in this repo.
- `.agents/README.md`: index of local agent assets.
- `.agents/docs/project-memory.md`: overview of project-local memory.
- `.agents/docs/memory/index.md`: searchable memory index.
- `.agents/docs/memory/entries/`: detailed verified memory entries.
- `.agents/docs/agent-status.template.md`: tracked template for local multi-session and employee-agent status.
- `.agents/docs/agent-status.md`: local runtime status board; do not deploy it to other projects.
- `.agents/docs/decisions/`: durable decisions about Codex/project operations.
- `.agents/docs/runbooks/`: repeatable procedures.
- `.agents/skills/`: project-local skills and migrated command workflows.
- `.codex/`: Codex App project settings. Treat environment files as project/machine-specific; do not blindly copy them between repositories.

## Multi-Agent Mode

- The main session is the controller: it plans work, assigns sub-agents, reviews results, integrates changes, and reports outcome.
- When the user says `招聘一個員工`, `hire employee`, or `spawn employee`, treat it as explicit permission to create a Codex sub-agent for this repository.
- Each sub-agent must have a clear role, task, allowed scope, forbidden scope, verification expectation, and final report format.
- Use `explorer` for read-only investigation and `worker` for bounded implementation.
- Prefer disjoint write scopes for workers. Do not assign multiple agents to edit the same files unless the user accepts the conflict risk.
- Use `.agents/docs/runbooks/multi-agent-workflow.md` for the delegation protocol.
- Before assigning or closing employee-agent work, read and reconcile local `.agents/docs/agent-status.md`. If it is missing, create it from `.agents/docs/agent-status.template.md`.
- For multi-session or multi-agent work, keep local `.agents/docs/agent-status.md` current at assignment, report, and final-closeout checkpoints.

## Use Existing Documentation First

Do not duplicate InlongSlicer or retained upstream documentation here. Before modifying code, inspect the relevant existing files:

- Project overview and install notes: `README.md`
- Current agent/developer guidance: `CLAUDE.md`
- Build system and packaging rules: `CMakeLists.txt`, `src/CMakeLists.txt`
- Windows build scripts: `build_release_vs.bat`, `build_release_vs2022.bat`, `build_release.bat`
- Linux/macOS build scripts: `build_linux.sh`, `build_release_macos.sh`
- Flatpak/package scripts: `build_flatpak.sh`, `scripts/flatpak/`
- CI build/package workflows: `.github/workflows/build_*.yml`
- Profile validation: `.github/workflows/check_profiles.yml`, `scripts/inlong_extra_profile_check.py`, `scripts/inlong_filament_lib.py`
- Translation/i18n: `scripts/run_gettext.sh`, `scripts/run_gettext.bat`, `localization/i18n/`, `resources/i18n/`
- Tests: `tests/CLAUDE.md`, `tests/`, `scripts/run_unit_tests.sh`
- Agent skills and command prompts: `.agents/README.md`, `.agents/skills/`, `.claude/commands/`
- Codex memory, runbooks, decisions, and handoff status: `.agents/docs/`
- Orca-to-Inlong migration: `InlongSlicer_doc/orcaslicer_to_inlongslicer_migration.md`, `.agents/skills/inlong-branding-migration/SKILL.md`
- Functional change history: `InlongSlicer_doc/functional_change_log.md`

## Safe Working Rules For Codex

- Read the relevant docs, scripts, CMake files, and nearby source before editing.
- Keep changes small, focused, and reviewable.
- Prefer existing InlongSlicer and upstream patterns over new abstractions.
- Preserve cross-platform behavior on Windows, macOS, and Linux.
- Preserve backward compatibility for printer profiles, project files, presets, and user configuration.
- Treat profile changes as high risk; validate against the existing profile checks whenever possible.
- Treat branding, installer IDs, bundle IDs, resource paths, and executable names as high risk.
- Do not assume a README statement is current when scripts or CI disagree. Prefer live scripts/CMake/CI as the implementation source of truth.
- When changing behavior, include verification notes: what was built, what tests ran, what profile/resource checks ran, or why verification was not possible.

## Documentation Maintenance

- Update the most specific Markdown file that owns the workflow instead of copying the same guidance into several places.
- Keep `CLAUDE.md` as a pointer to `AGENTS.md` unless a tool requires separate content.
- When changing a migrated source command, update both `.agents/skills/<name>/SKILL.md` and the matching `.claude/commands/<name>.md` if both exist.
- When fixing bugs, changing user-visible behavior, changing profile behavior, changing build/package workflow, or adding features, update `InlongSlicer_doc/functional_change_log.md` in the same change unless the edit is truly documentation-only.
- Avoid editing generated, vendored, or build-output Markdown under `build/`, `deps/`, or `deps_src/` unless the task specifically requires it.
- Prefer links to existing upstream documentation over pasted copies of long build, packaging, test, or release instructions.
- For non-trivial Codex workflow or documentation tasks, use `.agents/docs/runbooks/task-closeout.md` before final response.

## Forbidden Without Explicit Approval

Do not perform these actions unless the user explicitly asks for them:

- Broad repository branding renames.
- Changing package IDs, bundle IDs, installer registry keys, executable names, or update identities.
- Dependency upgrades, dependency removals, or toolchain changes.
- Mass formatting, mass refactoring, or large mechanical rewrites.
- Reorganizing profile directories or changing profile schema conventions.
- Changing CI release/deploy destinations.
- Removing compatibility code or migration behavior.
- Reverting user changes or using destructive git commands.
- Editing generated or vendored files unless the task specifically requires it.

## Build And Test Expectations

For code changes, document at least one targeted verification step. Prefer the narrowest useful check:

- C++/GUI/build changes: run the relevant platform build target or explain why not.
- Windows build changes: inspect or run the relevant `build_release_vs*.bat` path.
- Profile changes: run `scripts/inlong_extra_profile_check.py` and, when available, `InlongSlicer_profile_validator`.
- Translation changes: run the relevant gettext script.
- Test changes: run the affected Catch2 suite or `scripts/run_unit_tests.sh`.
- Packaging changes: inspect the matching CMake/CI/package script path and note the expected artifact impact.

If verification cannot be run, state the blocker clearly.

## Required Isolation Closeout

At the end of every non-trivial Codex reply in this repository, include either these fields or the compact equivalent `Isolation: GM <used/not used> | GS <used/not used> | XR <none/paths> | XW <none/paths>`.

- Global Memory: used or not used.
- Global Skill: used or not used.
- Project-external reads: none or exact authorized paths.
- Project-external writes: none or exact authorized paths.

## Recommended First-Task Workflow

1. Identify the task area: build, packaging, branding, profiles, translation, GUI, slicing logic, or tests.
2. Read the relevant documentation and implementation files listed above.
3. For branding or upstream-sync work, read `InlongSlicer_doc/orcaslicer_to_inlongslicer_migration.md` before editing and preserve its allowlist rules.
4. Compare against `inlong/orca-2.4-base` when the task involves fork-specific behavior or upstream divergence.
5. Inspect nearby source and existing patterns before proposing edits.
6. Make the smallest change that solves the request.
7. Run targeted verification, or record why it was not run.
8. Update `InlongSlicer_doc/functional_change_log.md` for functional or bug-fix work.
9. Summarize the changed files, behavior impact, and verification result.
