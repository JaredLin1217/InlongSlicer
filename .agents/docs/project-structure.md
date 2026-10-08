# InlongSlicer Project Structure

This repository is an InlongSlicer product checkout and an AI Agents v4
consumer. It is not an Agents distribution provider.

## Governance Layout

- `AGENTS.md` is the session entry point.
- `.agents/managed.json` records the deployed version and owned file hashes.
- `.agents/scripts/validate.ps1 -Scope Consumer -Profile Checkpoint` checks
  the installed rules without running Provider-only checks.
- `pwsh -NoProfile -File scripts/check-agents-guidance.ps1` checks project-owned
  guidance entry points, resource references and retired operational instructions.
- `.agents/skills/` contains project-local skills, including the Inlong
  branding migration guardrails.
- `.agents/docs/runbooks/` contains operator procedures.
- `docs/memory/entries/*.json` contains immutable, reviewed project knowledge.
  Recall checks sources through the project-memory tool; generated indexes are
  advisory. Older Markdown entries are historical references, not active rules
  or proof of current verification.
- `.agents/runtime/runs/` holds registered temporary payloads, `state/` holds
  recovery pointers and locks, and `ledger/` retains provenance and deletion
  records. Inspect and preview cleanup; never clear the runtime root.
- `.agents/docs/templates/codex-hooks.json` is an optional template. Deployment
  does not activate native hooks or change local/global Codex settings.
- `tests/AGENTS.md` adds valid test-scope rules and is not a competing root
  layout.
- Deployment does not replace product files, local configuration, project
  knowledge, or project-owned skills.

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

Agents-created test copies, diagnostics and backups are disposable only after
checking their provenance, current hashes, active users and recovery dependencies.
Installed applications, user data, source fixtures and uncommitted product changes
are not cleanup targets. Explicit user-directed legacy cleanup must record its
reviewed scope and deletions separately from the normal runtime retention policy.
