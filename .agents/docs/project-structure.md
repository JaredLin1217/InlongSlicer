# InlongSlicer Project Structure

This repository is an InlongSlicer product checkout and a `full_workflow`
consumer of the repo-local Agents governance. It is not an Agents template
provider.

## Governance Layout

- `AGENTS.md` is the session entry point.
- `.agents/docs/agents/*.yaml` is the only canonical governance rule set.
- `.agents/skills/` contains project-local skills, including the Inlong
  branding migration guardrails.
- `.agents/docs/runbooks/` contains operator procedures.
- `.agents/docs/memory/index.md` and `.agents/docs/memory/entries/` contain
  target-owned, evidence-linked project memory.
- `tests/AGENTS.md` adds valid test-scope rules and is not a competing root
  layout.
- `.agents/docs/templates/agents/` is provider-only and must not contain a
  deployable mirror in this `full_workflow` consumer.

## Product Layout

| Area | Purpose | Change guidance |
|---|---|---|
| `src/libslic3r/` | Slicing engine and geometry | Run the smallest relevant unit or FFF test target. |
| `src/slic3r/` | Desktop application and GUI | Preserve Inlong identity and verify the Release application when changed. |
| `resources/profiles/` | Printer, process, and material profiles | Validate cross-profile relationships, not file presence alone. |
| `localization/i18n/` | Gettext sources using `InlongSlicer_*.po` | Rebuild catalogs and verify the staged runtime resource. |
| `tests/` | Catch2 and integration tests | Follow `tests/AGENTS.md`. |
| `cmake/`, `CMakeLists.txt`, `src/CMakeLists.txt` | Build, install, and package contracts | Verify the affected target before a full install. |
| `.github/workflows/` | CI and release automation | Keep platform and packaging behavior aligned with local build entry points. |
| `InlongSlicer_doc/` | Product-specific migration and operating records | Treat the branding migration record as the source of truth during upstream merges. |

## Build And Profile Validation

- Windows build entry points include `build_release_vs.bat` and
  `build_release_vs2022.bat`.
- Full packaging uses the `install` target; the scoped Inlong package uses
  `install_only_inlong` when configured.
- Profile changes must run `python scripts/inlong_extra_profile_check.py` and,
  when built, `build/src/Release/InlongSlicer_profile_validator.exe` for each
  affected vendor.
- Build output under `build/` is generated local state. Do not hand-edit or
  treat it as tracked source evidence.

## Upstream And Branding Boundary

`origin` is the InlongSlicer fork and `upstream` is OrcaSlicer. Before an
upstream merge or branding-sensitive edit, read
`.agents/skills/inlong-branding-migration/SKILL.md` and
`InlongSlicer_doc/orcaslicer_to_inlongslicer_migration.md`. Preserve documented
external Orca contracts and dependency names unless a separate product change
authorizes their replacement.

## Runtime And Local State

Do not deploy or check in `.agents/runtime/`, `.workflow/`, live thread IDs,
context indexes, provider session/history, secrets, `.git/`, or generated build
output. Target-owned dirty product files remain protected during Agents
maintenance.
