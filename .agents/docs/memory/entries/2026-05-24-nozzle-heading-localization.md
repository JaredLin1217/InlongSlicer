# 2026-05-24 - Nozzle Heading Localization

- Trigger: When the user asks to rename visible left/right nozzle labels across locales.
- Context: Imported from authorized global Codex Memory for `D:\inlong\Slicer\GitHub\InlongSlicer`. The task changed visible UI headings in Chinese and English while preserving unrelated warning/help text.
- Cause: `Left Nozzle` / `Right Nozzle` visible headings and lower-case `left nozzle` / `right nozzle` diagnostic/help strings are separate localization entries. A broad translation edit can rewrite more text than intended.
- Fix / Rule: Target only the visible headings backed by `src/slic3r/GUI/Plater.cpp` `_L("Left Nozzle")` and `_L("Right Nozzle")`. Do not change lower-case warning/help strings unless the user explicitly requests those messages too.
- Verification: Source memory recorded regenerated `.mo` files with `tools/msgfmt.exe`; runtime copies under `build/OrcaSlicer/resources/i18n/...` were synced successfully, while `build/src/Release/resources/i18n/...` could be locked by the running app.
- Reuse when: Renaming nozzle labels, editing `localization/i18n/*/OrcaSlicer_*.po`, regenerating `resources/i18n/*/OrcaSlicer.mo`, or verifying that a running app is not holding runtime `.mo` files open.

## Working Notes

- Source title entries: `src/slic3r/GUI/Plater.cpp` creates the panel titles from `_L("Left Nozzle")` and `_L("Right Nozzle")`.
- English target wording used in memory: `Main Nozzle` and `Sub Nozzle`.
- Chinese locale edits were applied to `localization/i18n/zh_CN/OrcaSlicer_zh_CN.po` and `localization/i18n/zh_TW/OrcaSlicer_zh_TW.po`.
- English locale edits were applied to `localization/i18n/en/OrcaSlicer_en.po`.
- If `.mo` copy into `build/src/Release/resources/i18n/...` fails, check whether InlongSlicer is running. Sync `build/OrcaSlicer/resources/i18n/...` or restart/rebuild before expecting the locked tree to refresh.

## Source

- Imported from authorized global Codex Memory on 2026-05-29.
- Source category: summarized global memory plus an InlongSlicer nozzle-heading localization rollout summary.
