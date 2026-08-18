# Nozzle Heading Localization
ID: M002
Date: 2026-05-24
Title: Nozzle heading localization
Status: active
Confidence: high
Source Commit: 48d061e9c54a79704079e2e20488e6c686c99145
Content Hash: 1691828e4956cf7dd598bbdefcec87e3b547e3df0707f4b14e89d5eef4827ee5
Checked At: 2026-08-06T04:48:26Z
Last Verified: 2026-08-06
Next Review Due: 2026-11-04
Update Trigger: Nozzle UI strings, gettext catalog names, or resource staging paths change
Supersedes: none
Boundary: Visible main and sub nozzle headings; excludes lower-case diagnostic and help strings
Source Refs: src/slic3r/GUI/Plater.cpp; localization/i18n/en/InlongSlicer_en.po; localization/i18n/zh_CN/InlongSlicer_zh_CN.po; localization/i18n/zh_TW/InlongSlicer_zh_TW.po

## Trigger
Use this lesson when changing visible left or right nozzle headings across locales.

## Context
The GUI headings and similarly worded diagnostic or help messages are separate translation entries.

## Cause
A broad text replacement can unintentionally change warnings and instructions beyond the requested headings.

## Fix / Rule
Target only the `Left Nozzle` and `Right Nozzle` headings created in `Plater.cpp`, then update the current `InlongSlicer_*.po` catalogs. Do not change lower-case diagnostic or help strings without an explicit request.

## Verification
The current source still creates both headings in `src/slic3r/GUI/Plater.cpp`, and English, Simplified Chinese, and Traditional Chinese catalogs use the InlongSlicer filename prefix.

## Evidence
Search the source `msgid` and `msgstr` entries, rebuild the gettext catalogs, and verify the newly staged runtime `.mo` files after closing any process that holds them open.

## Reuse when
Renaming nozzle headings, repairing translated headings, or diagnosing stale staged localization resources.

## Index Row
| ID | Date | Title | Trigger | Keywords | Summary | Entry | Status | Confidence | Source Commit | Content Hash | Checked At | Last Verified | Next Review Due | Update Trigger | Supersedes | Boundary | Source Refs |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| M002 | 2026-05-24 | Nozzle heading localization | Nozzle heading or translation work | localization, nozzle, gettext | Edit only visible headings in current Inlong catalogs | `entries/2026-05-24-nozzle-heading-localization.md` | active | high | 48d061e9c54a79704079e2e20488e6c686c99145 | 1691828e4956cf7dd598bbdefcec87e3b547e3df0707f4b14e89d5eef4827ee5 | 2026-08-06T04:48:26Z | 2026-08-06 | 2026-11-04 | Nozzle UI strings, gettext catalog names, or resource staging paths change | none | Visible main and sub nozzle headings; excludes lower-case diagnostic and help strings | src/slic3r/GUI/Plater.cpp; localization/i18n/en/InlongSlicer_en.po; localization/i18n/zh_CN/InlongSlicer_zh_CN.po; localization/i18n/zh_TW/InlongSlicer_zh_TW.po |
