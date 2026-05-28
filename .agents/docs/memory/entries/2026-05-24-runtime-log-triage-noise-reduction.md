# 2026-05-24 - Runtime Log Triage And Noise Reduction

- Trigger: When the user asks to inspect the latest InlongSlicer runtime log, trace warnings to source/profile files, or reduce startup log noise.
- Context: Imported from authorized global Codex Memory for `D:\inlong\Slicer\GitHub\InlongSlicer`. The original work repeatedly inspected fresh logs under `build\src\Release\data_dir\log`, patched source/profile causes, rebuilt, relaunched, and checked newer logs.
- Cause: Runtime logs can contain both real warnings and initialization noise. Old logs also keep old errors after fixes unless the app is relaunched with the new binary/resources.
- Fix / Rule: Always pick the newest log, classify `[fatal]`, `[error]`, and `[warning]` separately from `info` text that merely contains words like `failed` or `invalid`, then trace high-signal warnings to source/profile load paths. Verify with both a successful rebuild and a fresh app launch that creates a newer log.
- Verification: Source memory recorded successful Release builds and final newer logs with zero counts for `calc_exclude_triangles`, vendor update HTTP errors, and large JSON dump strings.
- Reuse when: Reviewing `build\src\Release\data_dir\log`, debugging `calc_exclude_triangles`, `hints.cereal`, vendor profile update warnings, `get_version not supported`, or excessive `WebGuideDialog` JSON logging.

## Working Notes

- Latest log selection pattern:

```powershell
Get-ChildItem "build\src\Release\data_dir\log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
```

- High-signal search terms: `calc_exclude_triangles`, `vendor check HTTP error`, `hints.cereal`, `get_version not supported`, `select_preset_by_name_strict`.
- `PartPlate.cpp` and INLONG profile data both mattered: `calc_exclude_triangles()` should return early for empty polygons, and `bed_exclude_area` should be empty when there is no excluded area instead of four `"0x0"` entries.
- `AppConfig::profile_update_url()` returning empty disables the unsupported vendor profile-update path that produced 404s for `INLONG` / `_Infinity3DP`.
- `WebGuideDialog.cpp` was the largest startup-log spam source; full JSON dumps should be summaries or debug-level details, not large info logs.
- If validation still shows old warnings, launch the fresh build and inspect the next newest log instead of rereading an old file.
- When validating multiple JSON files with `ConvertFrom-Json`, parse each file individually rather than piping multiple JSON documents into one parse.

## Source

- Imported from authorized global Codex Memory on 2026-05-29.
- Source category: summarized global memory plus an InlongSlicer runtime-log triage rollout summary.
