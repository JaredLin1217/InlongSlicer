# Project Memory Index

This is the searchable index for InlongSlicer's project-local Codex memory. It lists verified, reusable lessons and links to detailed entries.

Global Codex Memory is not used for normal project work.

## Lookup Rules

- Search this file first for relevant triggers, keywords, files, commands, or workflow names.
- Read a detailed entry only when the index row is relevant.
- Verify details against current repo files, scripts, logs, tests, or observed behavior before acting.
- If no row matches, continue from current repository state.

## Index

| Date | Title | Trigger | Keywords | Summary | Detail |
|---|---|---|---|---|---|
| 2026-05-27 | Inlong branding migration guardrails | Before upstream syncs or branding edits that may reintroduce Orca naming | branding, upstream sync, Orca allowlist, InlongSlicer_doc, inlong/orca-2.4-base, orca_state | Use the repo migration record and branding skill before touching identity, assets, profiles, packaging, colors, translations, web resources, or generated outputs. Preserve allowlisted Orca service/base/dependency references. | `entries/2026-05-27-inlong-branding-migration-guardrails.md` |
| 2026-05-26 | GitHub origin/upstream branch clarification | When GitHub branch UI mentions discarding commits or the user asks whether commits were pushed upstream | git, GitHub, origin, upstream, discard commits, feature/inlong-slicer-initial, release/v2.4 | Explain fork-vs-upstream first. `Discard commits` would throw away fork branch commits; it is not an upstream merge. Use the safe upstream merge sequence for `release/v2.4` when requested. | `entries/2026-05-26-github-origin-upstream-merge.md` |
| 2026-05-24 | Nozzle heading localization | When renaming visible left/right nozzle labels across locales | localization, Plater.cpp, Left Nozzle, Right Nozzle, Main Nozzle, Sub Nozzle, msgfmt, i18n | Change only the visible title entries backed by `_L("Left Nozzle")` and `_L("Right Nozzle")`; do not rewrite lower-case warning/help strings. Regenerate `.mo` files. | `entries/2026-05-24-nozzle-heading-localization.md` |
| 2026-05-24 | Runtime log triage and noise reduction | When inspecting latest InlongSlicer runtime logs or reducing startup log noise | logs, calc_exclude_triangles, bed_exclude_area, profile_update_url, WebGuideDialog, hints.cereal | Triage latest logs, separate info noise from real errors, trace warnings to source/profile data, then verify with fresh build and fresh app log. | `entries/2026-05-24-runtime-log-triage-noise-reduction.md` |
| 2026-05-23 | Profile validation and documentation map | When importing or validating INLONG / _Infinity3DP profiles, or when mapping build/branding docs | profiles, INLONG, _Infinity3DP, profile_validator, orca_extra_profile_check, version.inc, docs | Profile work needs validator-driven checks across machine/material/process relationships; repo documentation is split across AGENTS, scripts, CMake, CI, and InlongSlicer_doc. | `entries/2026-05-23-profile-validation-doc-map.md` |
