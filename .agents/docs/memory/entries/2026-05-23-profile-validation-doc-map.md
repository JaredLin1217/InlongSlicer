# 2026-05-23 - Profile Validation And Documentation Map

- Trigger: When importing or validating `INLONG` / `_Infinity3DP` profile resources, or when mapping InlongSlicer docs/build/branding guidance.
- Context: Imported from authorized global Codex Memory for `D:\inlong\Slicer\GitHub\InlongSlicer`. The original work inventoried important docs, then validated and corrected `INLONG` and `_Infinity3DP` profile resources.
- Cause: InlongSlicer build/profile/branding guidance is split across docs, scripts, CMake, and CI; profile correctness also depends on relationships across machine, material, process, vendor index, and file names.
- Fix / Rule: For doc inventory, start from repo-owned docs and then verify against scripts/CMake/CI. For profile work, use validator-driven checks and include machine/material/process relationships instead of checking file presence only.
- Verification: Source memory recorded successful vendor-scoped profile validation and extra checks with zero errors/warnings after fixes.
- Reuse when: Adding vendor profiles, migrating `INLONG` / `_Infinity3DP`, fixing `compatible_printers`, checking default materials, correcting profile JSON encoding, or answering where build/branding/profile truth lives.

## Documentation Map Notes

- Practical build and packaging truth is split across `AGENTS.md`, top-level scripts, `CMakeLists.txt`, `src/CMakeLists.txt`, and `.github/workflows/`.
- Windows build paths evidenced by repo files include `build_release_vs.bat`, `build_release_vs2022.bat`, and older `build_release.bat` guidance.
- Branding/resource configuration is encoded in `version.inc`, CMake files, platform resource templates, and packaging metadata.

## Profile Validation Notes

- Useful validator calls recorded in memory:

```powershell
build\src\Release\OrcaSlicer_profile_validator.exe -p resources\profiles -v INLONG -l 2
build\src\Release\OrcaSlicer_profile_validator.exe -p resources\profiles -v _Infinity3DP -l 2
python scripts\orca_extra_profile_check.py --vendor INLONG --check-materials --check-obsolete-keys
python scripts\orca_extra_profile_check.py --vendor _Infinity3DP --check-materials --check-obsolete-keys
```

- The extra checker reads strict UTF-8; imported JSON with UTF-8 BOM can fail and should be rewritten without BOM.
- Vendor top-level JSON files are authoritative for `machine_model_list`, `process_list`, `machine_list`, and `filament_list`.
- Recorded fixes included `_Infinity3DP.json` process name alignment and `resources/profiles/INLONG/machine/SC12060_common.json` `printer_variant` correction from `0.4` to `0.6`.
- Do not treat machine-model files as machine presets when writing custom cross-file checks.

## Source

- Imported from authorized global Codex Memory on 2026-05-29.
- Source category: InlongSlicer profile validation and documentation-map rollout summary.
