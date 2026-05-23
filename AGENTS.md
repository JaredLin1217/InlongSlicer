# AGENTS.md

InlongSlicer is forked from OrcaSlicer 2.4.x. Treat `inlong/orca-2.4-base` as the clean upstream base for comparison, rebase analysis, and separating Inlong-specific changes from upstream OrcaSlicer behavior.

## Use Existing Documentation First

Do not duplicate OrcaSlicer documentation here. Before modifying code, inspect the relevant existing files:

- Project overview and install notes: `README.md`
- Current agent/developer guidance: `CLAUDE.md`
- Build system and packaging rules: `CMakeLists.txt`, `src/CMakeLists.txt`
- Windows build scripts: `build_release_vs.bat`, `build_release_vs2022.bat`, `build_release.bat`
- Linux/macOS build scripts: `build_linux.sh`, `build_release_macos.sh`
- Flatpak/package scripts: `build_flatpak.sh`, `scripts/flatpak/`
- CI build/package workflows: `.github/workflows/build_*.yml`
- Profile validation: `.github/workflows/check_profiles.yml`, `scripts/orca_extra_profile_check.py`, `scripts/orca_filament_lib.py`
- Translation/i18n: `scripts/run_gettext.sh`, `scripts/run_gettext.bat`, `localization/i18n/`, `resources/i18n/`
- Tests: `tests/CLAUDE.md`, `tests/`, `scripts/run_unit_tests.sh`

## Safe Working Rules For Codex

- Read the relevant docs, scripts, CMake files, and nearby source before editing.
- Keep changes small, focused, and reviewable.
- Prefer existing OrcaSlicer/InlongSlicer patterns over new abstractions.
- Preserve cross-platform behavior on Windows, macOS, and Linux.
- Preserve backward compatibility for printer profiles, project files, presets, and user configuration.
- Treat profile changes as high risk; validate against the existing profile checks whenever possible.
- Treat branding, installer IDs, bundle IDs, resource paths, and executable names as high risk.
- Do not assume a README statement is current when scripts or CI disagree. Prefer live scripts/CMake/CI as the implementation source of truth.
- When changing behavior, include verification notes: what was built, what tests ran, what profile/resource checks ran, or why verification was not possible.

## Forbidden Without Explicit Approval

Do not perform these actions unless the user explicitly asks for them:

- Broad renaming of OrcaSlicer to InlongSlicer across the repository.
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
- Profile changes: run `scripts/orca_extra_profile_check.py` and, when available, `OrcaSlicer_profile_validator`.
- Translation changes: run the relevant gettext script.
- Test changes: run the affected Catch2 suite or `scripts/run_unit_tests.sh`.
- Packaging changes: inspect the matching CMake/CI/package script path and note the expected artifact impact.

If verification cannot be run, state the blocker clearly.

## Recommended First-Task Workflow

1. Identify the task area: build, packaging, branding, profiles, translation, GUI, slicing logic, or tests.
2. Read the relevant documentation and implementation files listed above.
3. Compare against `inlong/orca-2.4-base` when the task involves fork-specific behavior or upstream divergence.
4. Inspect nearby source and existing patterns before proposing edits.
5. Make the smallest change that solves the request.
6. Run targeted verification, or record why it was not run.
7. Summarize the changed files, behavior impact, and verification result.
