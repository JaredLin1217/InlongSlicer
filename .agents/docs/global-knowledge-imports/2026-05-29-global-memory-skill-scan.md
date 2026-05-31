# 2026-05-29 Global Memory And Skill Scan

This file records global Codex `MEMORY.md` / `SKILL.md` content that looked related to the current InlongSlicer work area.

It is an import inbox, not authoritative project memory. Before using any item below, verify it against current InlongSlicer files, scripts, logs, or tests.

## Scan Scope

- Current repo: `D:\inlong\Slicer\GitHub\InlongSlicer`
- Global files listed or searched:
  - `C:\Users\v_jar\.codex\memories\MEMORY.md`
  - `C:\Users\v_jar\.codex\memories\skills\orcaslicer-external-build-debug\SKILL.md`
  - `C:\Users\v_jar\.codex\skills\inlong3d-license-maintenance\SKILL.md`
  - other `C:\Users\v_jar\.codex\**\SKILL.md` files by filename/content search only
- Global files were read-only. No global source was edited, moved, or deleted.

## Direct Current-Repo Matches

No remaining direct `D:\inlong\Slicer\GitHub\InlongSlicer` path match was found in the scanned global `MEMORY.md` or global `SKILL.md` files.

Project-local memory already contains earlier imported InlongSlicer entries:

- `.agents/docs/memory/entries/2026-05-23-profile-validation-doc-map.md`
- `.agents/docs/memory/entries/2026-05-24-runtime-log-triage-noise-reduction.md`
- `.agents/docs/memory/entries/2026-05-27-inlong-branding-migration-guardrails.md`

## Global Memory Candidates

### OrcaSlicer 2.3.2 External Build

- Source: `C:\Users\v_jar\.codex\memories\MEMORY.md`
- Applies to source memory cwd: `D:\inlong\Slicer\GitHub\OrcaSlicer-2.3.2`
- Status: stored here as adjacent historical build knowledge only.
- Do not promote directly to a project-local skill unless current InlongSlicer build scripts are verified first.

Candidate lessons to verify before reuse:

- Verify build output paths from batch scripts and filesystem instead of assuming standard locations.
- External Windows build roots should stay outside the source checkout.
- In-tree `build` and `deps\build` can remain after switching to external scripts; cleanup is separate from fixing scripts.
- Stale `CMakeCache.txt` values can keep old checkout paths such as `OpenCASCADE_DIR`.
- OCCT `Permission denied`, `error C1083`, or `error D8040` on object files can be file contention from leftover `MSBuild`, `cmake`, `cl`, or `link` processes.

### OrcaSlicer 2.3.2 To 3.2.2 Migration

- Source: `C:\Users\v_jar\.codex\memories\MEMORY.md`
- Applies to source memory cwd: `D:\inlong\Slicer\GitHub\OrcaSlicer3.2.2`
- Status: stored here as adjacent migration history only.
- Current repo is an InlongSlicer fork from upstream 2.4.x, so exact file counts and donor paths are stale.

Candidate lessons to verify before reuse:

- For folder migrations, enumerate with `git diff --no-index --name-status` before claiming completion.
- Source files from an older donor tree should be hand-ported into newer call sites rather than copied wholesale.
- Ignored files such as `.mo` translations may require direct filesystem checks or `git status --ignored`.
- Behavior-sensitive paths mentioned by the source memory include `PresetBundle.cpp`, `ConfigWizard.cpp`, `WebGuideDialog.cpp`, `Plater.cpp`, and `PresetComboBoxes.cpp`.

### OrcaSlicer Baseline Diff Interpretation

- Source: `C:\Users\v_jar\.codex\memories\MEMORY.md`
- Applies to source memory cwd: `D:\inlong\Slicer\GitHub\OrcaSlicer`
- Status: not imported as project memory.

Candidate lesson:

- When a UI reports a huge diff count, verify `git status -sb` and `git diff --shortstat` before treating it as working-tree dirtiness. The count may be a branch comparison against the wrong base.

### OrcaSlicer Windows Build And Vendor Profile Wizard Debugging

- Source: `C:\Users\v_jar\.codex\memories\MEMORY.md`
- Applies to source memory cwd: `D:\inlong\Slicer\ORCA 2.3.2\OrcaSlicer-2.3.2-sourecode\OrcaSlicer-2.3.2`
- Status: partly overlapping with existing project-local memories. Keep this section as candidate detail only.

Candidate lessons to verify before reuse:

- `WebGuideDialog.cpp` was the decisive path for first-run / wizard-selected vendor profiles.
- The source memory says installed vendor JSON under `data_dir/system` could override bundled `resources/profiles` data until load order was fixed.
- The reliable first-run filament source was the printer preset `default_filament_profile`; `default_materials` could bias initial selection.
- `PresetBundle::load_selections()` and printer `printer_variant` ordering affected selected nozzle/default behavior in that old checkout.
- After INLONG resource edits, bumping the vendor/resource version helped the app reload updated resources instead of cached vendor data.
- Validate profile JSON with focused vendor checks and `ConvertFrom-Json`; parse each JSON file individually.

### OrcaSlicer INLONG Vulcan600 Preset And UI Investigation

- Source: `C:\Users\v_jar\.codex\memories\MEMORY.md`
- Applies to source memory cwd: `D:\inlong\Slicer\ORCA 2.3.2\OrcaSlicer_Inlong_v1.0.6`
- Status: candidate detail only. Verify against current InlongSlicer source and profiles before use.

Candidate lessons to verify before reuse:

- H2D-style left/right nozzle selector behavior was source-gated by BBL vendor logic in the old checkout, not enabled only by two-nozzle preset data.
- Avoid reclassifying Vulcan600 as BBL/H2D just to unlock UI behavior unless the user explicitly accepts vendor-side effects.
- Relevant old-code search terms were `ExtruderGroup`, `layout_printer`, `is_bbl_vendor`, `use_bbl_network`, `Plater.cpp`, and `PresetBundle.cpp`.
- Old resource cleanup touched `resources/printers/Vulcan600model.json`, `resources/profiles/INLONG/machine/Vulcan600_common.json`, `resources/printers/version.txt`, and `resources/profiles/INLONG.json`.

## Global Skill Candidates

### `orcaslicer-external-build-debug`

- Source: `C:\Users\v_jar\.codex\memories\skills\orcaslicer-external-build-debug\SKILL.md`
- Status: not copied to `.agents/skills/`.
- Reason: it targets older `OrcaSlicer-2.3.2` scripts and includes reset/clean guidance that is unsafe to make active in this repo without rewriting for InlongSlicer.

Reusable procedure shape to consider later:

- Read the active build scripts first.
- Verify output roots from script variables and actual directories.
- Separate source-tree bloat cleanup from build-script correction.
- Search exact build error strings before rereading broad logs.
- Treat stale CMake cache paths and compiler process contention as separate failure modes.

### `inlong3d-license-maintenance`

- Source: `C:\Users\v_jar\.codex\skills\inlong3d-license-maintenance\SKILL.md`
- Status: skipped.
- Reason: it is for a separate INLONG3D QNAP license API project, not this InlongSlicer repository.

## Skipped Generic Global Skills

Generic or unrelated global/plugin skills were skipped, including OpenAI docs, Figma, GitHub, Linear, Notion, Slack, browser, documents, presentations, and spreadsheets skills. They are not project-specific InlongSlicer knowledge.

## Follow-Up Classification

Only promote a candidate above if it is verified against current InlongSlicer. Likely destinations:

- Verified reusable profile or build lesson: `.agents/docs/memory/index.md` plus `.agents/docs/memory/entries/`
- Repeatable current InlongSlicer workflow: `.agents/docs/runbooks/` or `.agents/skills/`
- Every-session isolation/build rule: `AGENTS.md`
- Functional product/build behavior: `InlongSlicer_doc/functional_change_log.md`
