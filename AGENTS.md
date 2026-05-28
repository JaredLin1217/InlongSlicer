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
- `.agents/skills/*/SKILL.md`: Codex skill entrypoints for migrated source commands.
- `.claude/commands/*.md`: legacy Claude command prompts. Keep matching command behavior aligned with the corresponding Codex skill when both exist.
- `tests/CLAUDE.md`: test-specific guidance for the `tests/` tree.
- `.github/pull_request_template.md`: PR summary and verification expectations.

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
