# Runtime Log Triage And Noise Reduction
ID: M003
Date: 2026-05-24
Title: Runtime log triage and noise reduction
Status: active
Confidence: medium
Source Commit: 48d061e9c54a79704079e2e20488e6c686c99145
Content Hash: a8c82f1b79884586b168b1fcd68c1cccb9cb3a3ac238c44d2847e9aab90df4de
Checked At: 2026-08-06T04:48:26Z
Last Verified: 2026-08-06
Next Review Due: 2026-11-04
Update Trigger: Runtime data directory, logging format, or startup profile-loading path changes
Supersedes: none
Boundary: Windows Release logs in this checkout; historical warning fixes must be reverified before reuse
Source Refs: build/src/Release/data_dir/log; src/slic3r/GUI/PartPlate.cpp; src/slic3r/GUI/WebGuideDialog.cpp; resources/profiles/INLONG

## Trigger
Use this lesson when inspecting the newest InlongSlicer runtime log or reducing startup noise.

## Context
The local Release runtime log directory exists, but older logs preserve failures from older binaries and resources.

## Cause
Informational text can contain failure-like words, and a rebuild alone does not prove a fixed path was executed.

## Fix / Rule
Select the newest log, classify fatal, error, and warning records separately from informational text, trace high-signal records to source or profile loading, rebuild, relaunch, and inspect a newly created log.

## Verification
`build/src/Release/data_dir/log` exists in the current checkout. Historical fixes named in this entry remain context only until a new runtime log demonstrates their current behavior.

## Evidence
Use `Get-ChildItem build/src/Release/data_dir/log | Sort-Object LastWriteTime -Descending | Select-Object -First 1`, then search structured severity and the specific subsystem markers.

## Reuse when
Triaging startup logs, profile update warnings, geometry warnings, or excessively large informational messages.

## Index Row
| ID | Date | Title | Trigger | Keywords | Summary | Entry | Status | Confidence | Source Commit | Content Hash | Checked At | Last Verified | Next Review Due | Update Trigger | Supersedes | Boundary | Source Refs |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| M003 | 2026-05-24 | Runtime log triage and noise reduction | Runtime log or startup warning investigation | logs, warnings, startup, Release | Verify fixes in a newly generated structured log | `entries/2026-05-24-runtime-log-triage-noise-reduction.md` | active | medium | 48d061e9c54a79704079e2e20488e6c686c99145 | a8c82f1b79884586b168b1fcd68c1cccb9cb3a3ac238c44d2847e9aab90df4de | 2026-08-06T04:48:26Z | 2026-08-06 | 2026-11-04 | Runtime data directory, logging format, or startup profile-loading path changes | none | Windows Release logs in this checkout; historical warning fixes must be reverified before reuse | build/src/Release/data_dir/log; src/slic3r/GUI/PartPlate.cpp; src/slic3r/GUI/WebGuideDialog.cpp; resources/profiles/INLONG |
