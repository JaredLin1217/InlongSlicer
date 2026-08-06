# Profile Validation And Documentation Map
ID: M001
Date: 2026-05-23
Title: Profile validation and documentation map
Status: active
Confidence: high
Source Commit: 48d061e9c54a79704079e2e20488e6c686c99145
Content Hash: fe19cf4e3cd2eaaa33e98cbc504a0cdc6ef6360160caff1254e4bd992e6e2204
Checked At: 2026-08-06T04:48:26Z
Last Verified: 2026-08-06
Next Review Due: 2026-11-04
Update Trigger: Profile schema, vendor layout, validator name, or packaging entry point changes
Supersedes: none
Boundary: INLONG and _Infinity3DP profile maintenance in this checkout; not a guarantee for unrelated vendors
Source Refs: scripts/inlong_extra_profile_check.py; build/src/Release/InlongSlicer_profile_validator.exe; resources/profiles/INLONG; resources/profiles/_Infinity3DP; CMakeLists.txt

## Trigger
Use this lesson when adding, migrating, or validating INLONG or Infinity3DP profiles.

## Context
Profile truth is split across vendor indexes, machine, process, and filament JSON plus build and packaging contracts.

## Cause
File-presence checks miss broken inheritance, compatibility, default-material, and vendor-index relationships.

## Fix / Rule
Run the current Inlong checker and validator for every affected vendor. Verify cross-profile relationships and strict UTF-8 encoding; do not treat machine-model files as machine presets.

## Verification
The current checkout contains `scripts/inlong_extra_profile_check.py`, the built `InlongSlicer_profile_validator.exe`, and both vendor roots. The validator help accepts `-p`, `-v`, and `-l`.

## Evidence
Use `python scripts/inlong_extra_profile_check.py --vendor <vendor> --check-materials --check-obsolete-keys` and `build/src/Release/InlongSlicer_profile_validator.exe -p resources/profiles -v <vendor> -l 2`.

## Reuse when
Changing vendor indexes, compatibility rules, default materials, profile inheritance, or profile packaging.

## Index Row
| ID | Date | Title | Trigger | Keywords | Summary | Entry | Status | Confidence | Source Commit | Content Hash | Checked At | Last Verified | Next Review Due | Update Trigger | Supersedes | Boundary | Source Refs |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| M001 | 2026-05-23 | Profile validation and documentation map | Profile or vendor resource work | profiles, INLONG, Infinity3DP, validator | Validate relationships with current Inlong tools | `entries/2026-05-23-profile-validation-doc-map.md` | active | high | 48d061e9c54a79704079e2e20488e6c686c99145 | fe19cf4e3cd2eaaa33e98cbc504a0cdc6ef6360160caff1254e4bd992e6e2204 | 2026-08-06T04:48:26Z | 2026-08-06 | 2026-11-04 | Profile schema, vendor layout, validator name, or packaging entry point changes | none | INLONG and _Infinity3DP profile maintenance in this checkout; not a guarantee for unrelated vendors | scripts/inlong_extra_profile_check.py; build/src/Release/InlongSlicer_profile_validator.exe; resources/profiles/INLONG; resources/profiles/_Infinity3DP; CMakeLists.txt |
