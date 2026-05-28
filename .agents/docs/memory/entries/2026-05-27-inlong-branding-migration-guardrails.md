# 2026-05-27 - Inlong Branding Migration Guardrails

- Trigger: Before upstream syncs or branding edits that may reintroduce Orca naming.
- Context: Imported from authorized global Codex Memory for `D:\inlong\Slicer\GitHub\InlongSlicer`. The original memory came from an ad-hoc request to remember the full OrcaSlicer-to-InlongSlicer migration process.
- Cause: Branding cleanup can accidentally break external service contracts, package identity, profile inheritance, generated runtime resources, or upstream comparison references if every `Orca` string is treated as cosmetic.
- Fix / Rule: Start from `InlongSlicer_doc/orcaslicer_to_inlongslicer_migration.md` and `.agents/skills/inlong-branding-migration/SKILL.md` before editing code, profiles, packaging, assets, colors, translations, web resources, or generated output. Preserve explicitly allowlisted Orca references such as Orca cloud/wiki URLs, `api.orcaslicer.com`, `auth.orcaslicer.com`, `cloud.orcaslicer.com`, `/orcaslicer-login`, `orca_state`, `inlong/orca-2.4-base`, and SoftFever/Orca dependency-source names unless there is an explicit product decision and replacement plan.
- Verification: Source memory pointed to the repo migration record and local branding skill. The migration record is already referenced from `AGENTS.md` as the source of truth for Orca-to-Inlong work.
- Reuse when: Handling upstream merges, branding scans, package identity edits, profile migration, generated runtime resources, icon/color changes, or any task where old Orca naming appears.

## Working Notes

- Canonical identity values recorded in memory: `InlongSlicer`, `Inlong Slicer`, `inlong-slicer`, `io.github.JaredLin1217.InlongSlicer`, `InlongSlicer_dep`, `inlong-slicer.exe`, `InlongSlicer.dll`, `InlongSlicer_profile_validator`, runtime config folder `InlongSlicer`, and accent `#D66C47`.
- High-risk replacement surfaces: `version.inc`, top-level `CMakeLists.txt`, `src/CMakeLists.txt`, source filenames/symbols like `OrcaCloudServiceAgent` / `OrcaPrinterAgent`, hard-coded accent colors, icons/images, `resources/web/**`, profile JSON names and `inherits` chains, gettext names, platform packaging metadata, CI/release artifact names, and staged runtime resources under `build/src/<config>/resources`.
- Scan with exclusions before editing: exclude `.git`, `build`, `deps/build`, and `resources/plugins`; classify remaining Orca hits as replacement, allowlisted, stale/generated, or false positive.
- Profile validation should use `python scripts\inlong_extra_profile_check.py` and `build\src\Release\InlongSlicer_profile_validator.exe` when available. If the validator is missing, build the validator instead of trusting renamed JSON by inspection alone.

## Source

- Imported from authorized global Codex Memory on 2026-05-29.
- Source category: summarized global memory plus an ad-hoc InlongSlicer branding migration note.
