# InlongSlicer Functional Change Log

This file records InlongSlicer-specific functional changes, bug fixes, workflow
changes, and verification notes that are not only branding rename details.

For the OrcaSlicer-to-InlongSlicer identity checklist, use
`InlongSlicer_doc/orcaslicer_to_inlongslicer_migration.md`. This file may link to
branding work when it has functional impact, but it should not duplicate the full
branding migration checklist.

## Maintenance Rules

- Update this file whenever a bug fix, feature, profile behavior change, build
  workflow change, packaging behavior change, or license/user-facing notice
  change is made.
- Keep new entries short but complete: symptom or goal, root cause when known,
  files changed, behavior impact, and verification.
- If the change is still uncommitted, mark it as `Uncommitted`.
- If the change is mostly branding identity, update
  `orcaslicer_to_inlongslicer_migration.md` instead and add only a short
  functional-impact note here.
- Use `inlong/orca-2.4-base` as the clean upstream base when classifying fork
  changes.

## 2026-09-08 - Adopt Deterministic System Filament IDs

Status: `Uncommitted`

Type: Upstream merge and profile identity migration

- Merge Orca upstream `bcb4f17d9ae5dd9c807550c92163389dcba66f28` into the
  Inlong branch. Adopt the upstream product-based `OF` plus six base62 digit
  filament IDs and regenerated per-preset setting IDs using
  `scripts/inlong_id_tool.py`; keep both UUID namespaces unchanged.
- Preserve the names and tuning of INLONG and Infinity3DP custom profiles.
  Regenerate the fork's complete filament-ID snapshot, including InlongArena
  and InlongFilamentLibrary claims. Use upstream's corrected Elegoo product
  bases instead of the older mismatched copies.
- Retain the Bambu `GF` catalog mapping at the printer boundary and the custom
  `P` ID space. Do not rewrite saved user data; older projects or tray selections
  referring to INLONG `IF201` through `IF214` may need the material reselected.
- Keep local independent contact-layer spacing, pattern, zero-gap tree-support,
  raft, and top-surface behavior while adding upstream Spiral Inset support.
  Retain both sides' preset and support regression tests.
- Port the updated homepage account layout to Inlong names and colors; accept
  the existing local cloud callback names and both old/new login commands.
- Preserve legacy Orca Arena printer/material names in `renamed_from` metadata
  so imported user presets keep resolving their parents. Canonical Inlong names,
  new IDs and tuning remain unchanged; never rewrite live user preset files.
- Initialize the standalone profile validator's argument/filesystem encoding as
  UTF-8, matching the application. Windows legacy-fixture validation otherwise
  loses accented filenames. Cover ASCII and Unicode roots with isolated fixtures.
- Verification: ID generator/checker, profile validators, ID-tool unit tests,
  Windows build-script tests, gettext, Release build/regression tests, and
  Consumer validation. See the merge handoff for actual outcomes and gaps.

## 2026-09-08 - Distinguish Contact Layer Line Spacing

Status: `Uncommitted`

Type: UI terminology and localization change

- Rename the independent contact-layer controls to `Top contact line spacing`
  and `Bottom contact line spacing`, with matching Simplified and Traditional
  Chinese labels. The former Chinese labels duplicated the native interface
  spacing controls.
- Clarify that these values set extrusion-line gaps in the contact layer, not
  the support/object Z gap. Preserve inheritance at `-1`, solid contact at `0`,
  and the top-contact support-ironing explanation.
- Keep configuration keys, defaults, slicing behavior, native interface/Z
  labels, and contact pattern labels unchanged. Synchronize gettext catalogs
  without changing unrelated translations.
- Add configuration regression coverage for labels and existing preset values.
  Verification also covers gettext catalogs, Release resources, both local
  installation variants, and restoration of existing runtime user data.

## 2026-05-31 - INLONG And Infinity3DP Contact Pattern Defaults

Status: `Uncommitted`

Type: Profile resource change

User-visible goal:

- Set INLONG and Infinity3DP process defaults so top and bottom support contact
  layers use the concentric pattern.

Changed files:

- `resources/profiles/INLONG/process/*_common.json`
- `resources/profiles/_Infinity3DP/process/*_common.json`

Behavior after change:

- All INLONG and Infinity3DP common process profiles define
  `support_top_contact_pattern: "concentric"` and
  `support_bottom_contact_pattern: "concentric"`.
- Layer-specific process profiles inherit these defaults from their matching
  common process profile.

Verification:

```powershell
Get-ChildItem resources\profiles\INLONG\process, resources\profiles\_Infinity3DP\process -Filter *.json | ForEach-Object { Get-Content -Raw $_.FullName | ConvertFrom-Json | Out-Null }
python scripts\inlong_extra_profile_check.py --vendor INLONG --check-materials --check-obsolete-keys
python scripts\inlong_extra_profile_check.py --vendor _Infinity3DP --check-materials --check-obsolete-keys
git diff --check -- resources\profiles\INLONG\process resources\profiles\_Infinity3DP\process InlongSlicer_doc\functional_change_log.md
```

## 2026-05-31 - Tree Slim/Strong/Hybrid Top Contact Layer Order

Status: `Uncommitted`

Type: Slicing behavior bug fix

User-visible goal:

- Make Tree Slim, Tree Strong, and Tree Hybrid place the top contact layer
  nearest to the model, matching normal supports and organic tree supports.

Changed files:

- `src/libslic3r/Support/TreeSupport.cpp`

Behavior after change:

- The first printable roof layer below the top Z gap is classified as
  `Roof1stLayer` and uses `support_top_contact_*` settings.
- Remaining top roof layers below the contact layer stay as top interface
  layers before the tree support body.

Verification:

```powershell
git diff --check -- src\libslic3r\Support\TreeSupport.cpp InlongSlicer_doc\functional_change_log.md
```

## 2026-05-30 - Support Contact Layer Chinese Localization

Status: `Uncommitted`

Type: UI localization change

User-visible goal:

- Add Traditional Chinese and Simplified Chinese labels for the Support contact
  layer option group and its top/bottom contact pattern and spacing controls.

Changed files:

- `localization/i18n/zh_TW/InlongSlicer_zh_TW.po`
- `localization/i18n/zh_CN/InlongSlicer_zh_CN.po`

Behavior after change:

- The Support page can show translated labels for `Support contact layer`,
  `Top contact pattern`, `Top contact spacing`, `Bottom contact pattern`, and
  `Bottom contact spacing` in `zh_TW` and `zh_CN`.

Verification:

```powershell
.\tools\msgfmt.exe --check-format -o resources\i18n\zh_TW\InlongSlicer.mo localization\i18n\zh_TW\InlongSlicer_zh_TW.po
.\tools\msgfmt.exe --check-format -o resources\i18n\zh_CN\InlongSlicer.mo localization\i18n\zh_CN\InlongSlicer_zh_CN.po
git diff --check -- localization\i18n\zh_TW\InlongSlicer_zh_TW.po localization\i18n\zh_CN\InlongSlicer_zh_CN.po
```

## 2026-05-30 - INLONG And Infinity3DP Material/Support Profile Tuning

Status: `Uncommitted`

Type: Profile resource change

User-visible goal:

- Tune INLONG and Infinity3DP material defaults for TPU, PET-CFGF, PPA-CFGF,
  PPS-CFGF, ABS, PLA, and PATH-CFGF/APTH-CFGF.
- Set INLONG and Infinity3DP process top/bottom support Z distance from each
  preset's layer height at 85%.
- Set default process templates to 0.05 mm top/bottom contact spacing and
  0.4 mm support interface spacing.
- Keep INLONG and Infinity3DP process defaults on zigzag sparse infill,
  40 degree support threshold angle, and Tree Strong support style.

Changed files:

- `resources/profiles/INLONG/filament/*.json`
- `resources/profiles/_Infinity3DP/filament/*.json`
- `resources/profiles/INLONG/process/*.json`
- `resources/profiles/_Infinity3DP/process/*.json`
- `resources/profiles/INLONG.json`
- `resources/profiles/_Infinity3DP.json`

Behavior after change:

- Matching INLONG and Infinity3DP material profiles share the requested
  temperature, flow, shrinkage, fan, retraction, bed temperature, and wipe
  distance values.
- INLONG and Infinity3DP process profiles use `support_top_z_distance` and
  `support_bottom_z_distance` values equal to 85% of each preset's
  `layer_height`, including layer profiles that override common process
  defaults.
- Common process profiles keep `sparse_infill_pattern: "zigzag"` and now use
  `support_threshold_angle: "40"`, `support_type: "tree(auto)"`, and
  `support_style: "tree_strong"`.
- Common process profiles use `support_top_contact_spacing: "0.05"`,
  `support_bottom_contact_spacing: "0.05"`, and
  `support_interface_spacing: "0.4"`.
- INLONG and Infinity3DP vendor profile versions were bumped so installed
  profile resources can refresh.

Verification:

```powershell
Get-ChildItem resources/profiles/INLONG/filament, resources/profiles/_Infinity3DP/filament -Filter *.json | ForEach-Object { Get-Content -Raw $_.FullName | ConvertFrom-Json | Out-Null }
Get-ChildItem resources/profiles/INLONG/process, resources/profiles/_Infinity3DP/process -Filter *.json | ForEach-Object { Get-Content -Raw $_.FullName | ConvertFrom-Json | Out-Null }
python scripts\inlong_extra_profile_check.py --vendor INLONG --check-materials --check-obsolete-keys
python scripts\inlong_extra_profile_check.py --vendor _Infinity3DP --check-materials --check-obsolete-keys
build\src\Release\InlongSlicer_profile_validator.exe -p resources\profiles -l 2
```

## 2026-05-30 - Split Support Contact Layer Settings

Status: `Uncommitted`

Type: Slicing behavior and GUI change

User-visible goal:

- Let the first support interface layer touching the model use settings that are
  independent from the remaining support interface layers.
- Expose separate top and bottom contact layer controls in the Support page.

Changed files:

- `src/libslic3r/PrintConfig.hpp`
- `src/libslic3r/PrintConfig.cpp`
- `src/libslic3r/Support/SupportParameters.hpp`
- `src/libslic3r/Support/SupportCommon.cpp`
- `src/libslic3r/Support/TreeSupport.cpp`
- `src/slic3r/GUI/Tab.cpp`
- `src/slic3r/GUI/ConfigManipulation.cpp`
- `src/slic3r/GUI/Field.cpp`
- `src/slic3r/GUI/GUI.cpp`
- `src/slic3r/GUI/GUI_Factories.cpp`
- `src/slic3r/GUI/UnsavedChangesDialog.cpp`

Behavior after change:

- New `support_top_contact_pattern`, `support_top_contact_spacing`,
  `support_bottom_contact_pattern`, and `support_bottom_contact_spacing`
  process settings control the first top and bottom contact layers.
- A contact spacing value of `-1` inherits the matching interface spacing so old
  profiles keep their previous behavior unless they set the new keys.
- Normal support and tree support both use separate top contact, bottom contact,
  and remaining interface density/pattern values.
- INLONG and Infinity3DP process profiles intentionally do not write the new
  contact keys yet. This keeps existing binaries able to load the vendor bundles
  in ConfigWizard while the new source defaults preserve the same behavior by
  inheriting from existing interface settings.

Verification:

```powershell
Get-ChildItem resources/profiles/INLONG/process, resources/profiles/_Infinity3DP/process -Filter *.json | ForEach-Object { Get-Content -Raw $_.FullName | ConvertFrom-Json | Out-Null }
python scripts\inlong_extra_profile_check.py --vendor INLONG --check-materials --check-obsolete-keys
python scripts\inlong_extra_profile_check.py --vendor _Infinity3DP --check-materials --check-obsolete-keys
```

## Baseline Inventory

Snapshot date: 2026-05-28

Comparison base: `inlong/orca-2.4-base` (`42cce5399c`)

Current committed head: `feature/inlong-slicer-initial` (`932616e0b9`)

Committed fork delta from base:

- Commits since base: `12`
- Files changed: `1726`
- Diff size: `23420 insertions`, `12781 deletions`
- Largest changed areas:
  - `resources/`: `1227` files
  - `src/`: `352` files
  - `InlongSlicer_doc/`: `28` files
  - `deps/`: `27` files
  - `localization/`: `22` files
  - `scripts/`: `20` files
  - `.github/`: `14` files
  - `tests/`: `13` files

Commands used for this inventory:

```powershell
git log --oneline --reverse inlong/orca-2.4-base..HEAD
git diff --shortstat inlong/orca-2.4-base..HEAD
git diff --name-only inlong/orca-2.4-base..HEAD | ForEach-Object { ($_ -split '/')[0] } | Group-Object | Sort-Object Count -Descending
git status -sb
```

Current uncommitted delta at this snapshot:

- `resources/profiles/INLONG/**`: INLONG profile color, PET-CFGF temperature,
  ABS retraction, and default process infill pattern updates.
- `resources/profiles/_Infinity3DP/**`: Infinity3DP profile color and PET-CFGF
  temperature, ABS retraction, and default process wall/infill updates.
- `src/libslic3r/PresetBundle.cpp`: INLONG / Infinity3DP sidebar filament slot
  colors now fall back to machine `extruder_colour`.
- `src/slic3r/GUI/Plater.cpp`: mixed dual-nozzle diameter sync fix.

## Commit Inventory Since Base

| Commit | Summary | Functional area |
| --- | --- | --- |
| `6048c1053b` | Add Codex agent skills | Agent workflows |
| `0b850cf428` | Update `AGENTS.md` after adding Codex | Agent workflows |
| `0f92413666` | Add Windows build `fast` option | Build workflow |
| `1a221c61f0` | Enable non-Bambu dual-nozzle settings and add Inlong profiles | Profiles, dual-nozzle UI |
| `0e5a8c01f2` | Template fix | Profile defaults |
| `1f3d8c8eb9` | Refine Windows build modes: fast, allfast, slicer, no-arg | Build workflow |
| `d6e212cdbf` | Update UI colors to Inlong style | UI theme and profile colors |
| `87702d23d7` | Fix previously logged runtime/profile issues | Runtime log cleanup, profiles |
| `8b69eeabdb` | Organize Markdown and agent docs | Documentation workflow |
| `f2eebdcf68` | Brand visible UI as Inlong Slicer | User-visible identity |
| `012550f8ed` | Rename application identity and About notice | Packaging, executable identity, license notice |
| `932616e0b9` | Update Inlong logo assets and AGPL notices | Assets, About/README license notices |

## Functional Changes And Fixes

### Support Contact Layer Page Icon Crash

Status: `Uncommitted`

Type: GUI bug fix

User-visible goal:

- Prevent a crash when opening the Support page after adding separate support
  top/bottom contact layer controls.

Changed files:

- `src/slic3r/GUI/Tab.cpp`

Behavior after change:

- The Support contact layer option group reuses the existing support icon
  resource instead of referencing missing `param_support_contact`.

Verification:

```powershell
rg -n "param_support_contact" src resources
cmake --build build --config Release --target InlongSlicer_app_gui -- /m /nr:false
```

### INLONG SC12060 Start G-code Temperature Waits

Status: `Uncommitted`

Type: Profile resource change

User-visible goal:

- Add explicit bed and nozzle temperature setup to the SC12060 machine start
  G-code so the prime move is not emitted before the nozzle reaches first-layer
  temperature.

Changed files:

- `resources/profiles/INLONG.json`
- `resources/profiles/INLONG/machine/SC12060_common.json`

Behavior after change:

- SC12060 start G-code preheats the nozzle, waits for first-layer bed
  temperature, homes, then waits for first-layer nozzle temperature before
  priming the extruder.
- INLONG vendor profile version was bumped so installed profile resources can
  refresh.

Verification:

```powershell
Get-Content -Raw resources\profiles\INLONG\machine\SC12060_common.json | ConvertFrom-Json | Select-Object -ExpandProperty machine_start_gcode
Get-Content -Raw resources\profiles\INLONG.json | ConvertFrom-Json | Select-Object -ExpandProperty version
Get-ChildItem resources\profiles\INLONG -Recurse -Filter *.json | ForEach-Object { Get-Content -Raw $_.FullName | ConvertFrom-Json | Out-Null }
git diff --check -- resources/profiles/INLONG.json resources/profiles/INLONG/machine/SC12060_common.json InlongSlicer_doc/functional_change_log.md
python scripts\inlong_extra_profile_check.py
build\src\Release\InlongSlicer_profile_validator.exe -p resources\profiles -l 2
```

Notes:

- `scripts\inlong_extra_profile_check.py` reported no errors and one existing
  warning for missing `resources\profiles\user.json`.

### INLONG Vulcan1200 Profile

Status: `Uncommitted`

Type: Profile resource change

User-visible goal:

- Add `Vulcan1200` under the INLONG vendor by copying the existing Vulcan600
  profile set without changing the machine geometry, nozzle defaults, filament
  defaults, G-code, or color settings.

Changed files:

- `resources/profiles/INLONG.json`
- `resources/profiles/INLONG/Vulcan1200_cover.png`
- `resources/profiles/INLONG/machine/Vulcan1200.json`
- `resources/profiles/INLONG/machine/Vulcan1200_common.json`
- `resources/profiles/INLONG/machine/Vulcan1200 0.4mm nozzle.json`
- `resources/profiles/INLONG/machine/Vulcan1200 0.5mm nozzle.json`
- `resources/profiles/INLONG/machine/Vulcan1200 0.6mm nozzle.json`
- `resources/profiles/INLONG/machine/Vulcan1200 0.8mm nozzle.json`
- `resources/profiles/INLONG/process/process_common.json`
- `resources/profiles/INLONG/filament/INLONG *.json`

Behavior after change:

- INLONG machine selection includes `Vulcan1200`.
- Vulcan1200 inherits the copied Vulcan600 bed, height, extruder, nozzle,
  default material, process, and G-code behavior.
- Vulcan1200 has the same available nozzle variants as Vulcan600: `0.4`,
  `0.5`, `0.6`, and `0.8`.
- INLONG material templates and default Vulcan process profiles list the
  Vulcan1200 nozzle presets as compatible printers.
- INLONG vendor profile version was bumped so installed profile resources can
  refresh.

Verification:

```powershell
git diff --check
Get-ChildItem resources/profiles/INLONG -Recurse -Filter *.json | ForEach-Object { Get-Content -Raw $_.FullName | ConvertFrom-Json | Out-Null }
python scripts\inlong_extra_profile_check.py
build\src\Release\InlongSlicer_profile_validator.exe -p resources\profiles -l 2
```

Additional consistency check:

- Vulcan1200 machine JSON files match Vulcan600 after normalizing
  `Vulcan1200` back to `Vulcan600`.
- `Vulcan1200_cover.png` was replaced with the provided Vulcan1200 machine
  image after the initial profile copy.
- INLONG vendor `sub_path` entries exist, Vulcan1200 compatible-printer entries
  resolve to registered machine presets, and Vulcan1200 nozzle presets inherit
  `Vulcan1200_common`.

### INLONG Printer Cover Images

Status: `Uncommitted`

Type: Profile resource change

User-visible goal:

- Update the machine thumbnail images shown for INLONG `SC12060` and
  `Vulcan1200`.

Changed files:

- `resources/profiles/INLONG/SC12060_cover.png`
- `resources/profiles/INLONG/Vulcan1200_cover.png`

Behavior after change:

- The INLONG printer selection UI uses the provided SC12060 and Vulcan1200
  preview images.

Verification:

```powershell
Get-Item resources/profiles/INLONG/SC12060_cover.png, resources/profiles/INLONG/Vulcan1200_cover.png
git diff --check
```

### INLONG / Infinity3DP ABS Retraction Template

Status: `Uncommitted`

Type: Profile resource change

User-visible goal:

- Set ABS filament template retraction distance to `0.6`.

Changed files:

- `resources/profiles/INLONG.json`
- `resources/profiles/INLONG/filament/INLONG ABS.json`
- `resources/profiles/_Infinity3DP.json`
- `resources/profiles/_Infinity3DP/filament/Infinity3DP ABS.json`

Behavior after change:

- INLONG and Infinity3DP ABS filament templates use `filament_retraction_length:
  "0.6"`.
- INLONG and Infinity3DP vendor profile versions were bumped so installed
  profile resources can refresh.

Verification:

```powershell
git diff --check
Get-Content -Raw "resources/profiles/INLONG/filament/INLONG ABS.json" | ConvertFrom-Json | Out-Null
Get-Content -Raw "resources/profiles/_Infinity3DP/filament/Infinity3DP ABS.json" | ConvertFrom-Json | Out-Null
python scripts\inlong_extra_profile_check.py
build\src\Release\InlongSlicer_profile_validator.exe -p resources\profiles -l 2
```

### INLONG / Infinity3DP Default Process Wall And Infill Settings

Status: `Uncommitted`

Type: Profile resource change

User-visible goal:

- Set default process `wall_loops` to `3` across INLONG and Infinity3DP
  machines.
- Set default process `sparse_infill_pattern` to `zigzag` for the Zig Zag /
  之字形 sparse infill pattern.

Changed files:

- `resources/profiles/INLONG.json`
- `resources/profiles/INLONG/process/process_common.json`
- `resources/profiles/INLONG/process/sc_process_common.json`
- `resources/profiles/_Infinity3DP.json`
- `resources/profiles/_Infinity3DP/process/IXBOX_common.json`
- `resources/profiles/_Infinity3DP/process/IXBOX_DUO_common.json`
- `resources/profiles/_Infinity3DP/process/X1_common.json`
- `resources/profiles/_Infinity3DP/process/X2_common.json`
- `resources/profiles/_Infinity3DP/process/X2_DUO_common.json`
- `resources/profiles/_Infinity3DP/process/X3_common.json`

Behavior after change:

- INLONG Vulcan600/SC12060 and Infinity3DP IXBOX/X1/X2/X3 default print
  profiles inherit `wall_loops: "3"`.
- Their default sparse infill pattern inherits `sparse_infill_pattern:
  "zigzag"`.
- INLONG and Infinity3DP vendor profile versions were bumped so installed
  profile resources can refresh.

Verification:

```powershell
git diff --check
Get-ChildItem resources/profiles/INLONG/process, resources/profiles/_Infinity3DP/process -Filter *.json | ForEach-Object { Get-Content -Raw $_.FullName | ConvertFrom-Json | Out-Null }
python scripts\inlong_extra_profile_check.py
build\src\Release\InlongSlicer_profile_validator.exe -p resources\profiles -l 2
```

### INLONG / Infinity3DP Default G-code Filename Format

Status: `Uncommitted`

Type: Profile resource change

User-visible goal:

- Set the default G-code export filename format for INLONG and Infinity3DP
  process profiles to include model name, machine model, nozzle code, material,
  rounded weight, slice date/time, and print time.
- Keep vendor-specific expressions split: INLONG profiles use `printer_model`
  directly, while Infinity3DP profiles shorten Infinity3DP model names.

Changed files:

- `resources/profiles/INLONG.json`
- `resources/profiles/INLONG/process/process_common.json`
- `resources/profiles/INLONG/process/sc_process_common.json`
- `resources/profiles/_Infinity3DP.json`
- `resources/profiles/_Infinity3DP/process/IXBOX_common.json`
- `resources/profiles/_Infinity3DP/process/IXBOX_DUO_common.json`
- `resources/profiles/_Infinity3DP/process/X1_common.json`
- `resources/profiles/_Infinity3DP/process/X2_common.json`
- `resources/profiles/_Infinity3DP/process/X2_DUO_common.json`
- `resources/profiles/_Infinity3DP/process/X3_common.json`

Behavior after change:

- Default `filename_format` outputs names shaped like
  `InlongToleranceTest_X1_06_PLA_4g_05280539_18m5s.gcode`.
- INLONG profiles use the machine model directly, for example `Vulcan600` or
  `SC12060`, without checking Infinity3DP model names.
- Infinity3DP machine names are shortened by the profile expression, for
  example `Infinity3DP X1` becomes `X1`.
- Nozzle diameter is encoded as `04`, `05`, `06`, etc.
- INLONG and Infinity3DP vendor profile versions were bumped so installed
  profile resources can refresh.

Verification:

```powershell
git diff --check
Get-ChildItem resources/profiles/INLONG/process, resources/profiles/_Infinity3DP/process -Filter *.json | ForEach-Object { Get-Content -Raw $_.FullName | ConvertFrom-Json | Out-Null }
python scripts\inlong_extra_profile_check.py
build\src\Release\InlongSlicer_profile_validator.exe -p resources\profiles -l 2
```

### INLONG / Infinity3DP PET-CFGF Temperature Template

Status: `Uncommitted`

Type: Profile resource change

User-visible goal:

- Set PET-CFGF first-layer nozzle temperature to `290`.
- Set PET-CFGF normal nozzle temperature to `300`.
- Set PET-CFGF hot plate first-layer and normal bed temperature to `75`.

Changed files:

- `resources/profiles/INLONG.json`
- `resources/profiles/INLONG/filament/INLONG PET-CFGF.json`
- `resources/profiles/_Infinity3DP.json`
- `resources/profiles/_Infinity3DP/filament/Infinity3DP PET-CFGF.json`

Behavior after change:

- INLONG and Infinity3DP PET-CFGF filament templates use the requested nozzle
  and bed temperatures.
- INLONG and Infinity3DP vendor profile versions were bumped so installed
  profile resources can refresh.

Verification:

```powershell
git diff --check
Get-Content -Raw "resources/profiles/INLONG/filament/INLONG PET-CFGF.json" | ConvertFrom-Json | Out-Null
Get-Content -Raw "resources/profiles/_Infinity3DP/filament/Infinity3DP PET-CFGF.json" | ConvertFrom-Json | Out-Null
python scripts\inlong_extra_profile_check.py
```

### INLONG / Infinity3DP Main And Sub Nozzle Colors

Status: `Uncommitted`

Type: Profile resource change / UI default fix

User-visible goal:

- INLONG and Infinity3DP machine profiles should use the Inlong orange-red
  `#D66C47` for the main nozzle / first tool.
- Dual-nozzle profiles should use dark gray `#494949` for the sub nozzle /
  second tool.

Root cause:

- Machine profiles had mixed historical `extruder_colour` values, including
  Infinity3DP `#808080` / `#FF8000` and Vulcan600 `#E18263`.
- The sidebar filament/tool slot colors are loaded from `filament_colour` in
  project/app config. When no saved colors existed, the loader filled every slot
  with `#D66C47`; when old generated colors existed, they were reused instead of
  the machine profile's `extruder_colour`.

Changed files:

- `resources/profiles/INLONG.json`
- `resources/profiles/INLONG/machine/Vulcan600_common.json`
- `resources/profiles/_Infinity3DP.json`
- `resources/profiles/_Infinity3DP/machine/Infinity3DP IXBOX common.json`
- `resources/profiles/_Infinity3DP/machine/Infinity3DP IXBOX DUO common.json`
- `resources/profiles/_Infinity3DP/machine/Infinity3DP X1 common.json`
- `resources/profiles/_Infinity3DP/machine/Infinity3DP X2 common.json`
- `resources/profiles/_Infinity3DP/machine/Infinity3DP X2 DUO common.json`
- `resources/profiles/_Infinity3DP/machine/Infinity3DP X3 common.json`
- `src/libslic3r/PresetBundle.cpp`

Behavior after change:

- Single-nozzle INLONG / Infinity3DP profiles use `extruder_colour:
  ["#D66C47"]`.
- Dual-nozzle INLONG / Infinity3DP profiles use `extruder_colour:
  ["#D66C47", "#494949"]`.
- INLONG and Infinity3DP vendor profile versions were bumped so installed
  profile resources can refresh.
- INLONG and Infinity3DP sidebar filament/tool slot colors use the selected
  machine profile's `extruder_colour` when the stored slot colors are empty or
  match old generated defaults.
- The fallback model check includes `Vulcan600`, `Vulcan1200`, `SC12060`, and
  Infinity3DP models for cases where the selected preset is loaded without a
  vendor object.
- Custom non-generated user colors are still preserved.

Verification:

```powershell
git diff --check
Get-ChildItem resources/profiles/INLONG, resources/profiles/_Infinity3DP -Recurse -Filter *.json | ForEach-Object { Get-Content -Raw $_.FullName | ConvertFrom-Json | Out-Null }
python scripts\inlong_extra_profile_check.py
build\src\Release\InlongSlicer_profile_validator.exe -p resources\profiles -l 2
cmake --build build --config Release --target InlongSlicer -- /m:8
```

### Dual-Nozzle Mixed Diameter Sync

Status: `Uncommitted`

Type: Bug fix

User-visible problem:

- The sidebar allowed different main/sub nozzle diameters, for example main
  `0.4` and sub `0.6`.
- `Printer Settings -> Extruder 1 / Extruder 2 -> Nozzle diameter` still showed
  both extruders as `0.4`.

Root cause:

- `Sidebar::priv::switch_diameter(false)` treated a mixed left/right diameter as
  a single-print diameter choice and selected a same-diameter printer preset.
- Mixed hardware such as `0.4/0.6` has no matching single-variant preset, so the
  per-extruder `nozzle_diameter` vector was not updated.
- `sync_extruder_list()` also looked up the right-nozzle diameter in the left
  combo box, which could fail or select the wrong value for custom choices.

Changed files:

- `src/slic3r/GUI/Plater.cpp`

Behavior after change:

- Mixed dual-nozzle values now update the active printer config as a
  per-extruder `nozzle_diameter` vector, for example `[0.4, 0.6]`.
- The current printer preset is kept for mixed diameters instead of switching to
  a same-diameter preset.
- Same left/right diameter still uses the original similar-preset switching
  path.
- Device sync now selects each extruder's own combo box and adds a missing
  diameter option before selecting it.

Verification:

```powershell
git diff --check
cmake --build build --config Release --target InlongSlicer_app_gui -- /m:4 /nr:false /v:m
cmake --build build --config Release --target INSTALL -- /m:4 /nr:false /v:m
```

Notes:

- The first build attempt failed because `build\src\Release\inlong-slicer.exe`
  was still running and locking `InlongSlicer.dll`; closing the process allowed
  the same build to pass.

### Non-Bambu Dual-Nozzle Settings

Status: `Committed`

Representative commit: `1a221c61f0`

Type: Feature

Goal:

- Let Inlong/Infinity3DP dual-nozzle machines expose left/right nozzle controls
  instead of relying on Bambu-only UI gates.

Changed files:

- `src/slic3r/GUI/Plater.cpp`
- `src/libslic3r/PresetBundle.cpp`
- `src/slic3r/GUI/ConfigWizard.cpp`
- `src/slic3r/GUI/WebGuideDialog.cpp`
- `src/slic3r/GUI/PresetComboBoxes.cpp`
- `resources/profiles/INLONG/**`
- `resources/profiles/_Infinity3DP/**`

Behavior after change:

- Inlong dual-extruder presets can show dual-nozzle controls.
- Sidebar nozzle combos use the selected printer preset's actual
  `nozzle_diameter` values.
- Filament and printer preset compatibility is refreshed after nozzle/profile
  changes so the visible filament list stays aligned with the selected machine.

Verification recorded from the migration work:

- Targeted source patching was used instead of overwriting newer upstream
  functions wholesale.
- Profile JSON files were checked with JSON parsing during the migration work.
- `git diff --check` passed after the targeted source/profile edits.

### Inlong Vendor Profiles

Status: `Committed`

Representative commit: `1a221c61f0`

Type: Feature

Goal:

- Add native INLONG printer and filament profiles.

Changed files:

- `resources/profiles/INLONG.json`
- `resources/profiles/INLONG/machine/**`
- `resources/profiles/INLONG/filament/**`
- `resources/profiles/INLONG/process/**`
- `resources/profiles/INLONG/SC12060_cover.png`
- `resources/profiles/INLONG/Vulcan600_cover.png`

Behavior after change:

- Adds INLONG machine families:
  - `Vulcan600`
  - `SC12060`
- Adds nozzle variants:
  - `Vulcan600`: `0.4`, `0.5`, `0.6`, `0.8`
  - `SC12060`: `0.6`, `0.8`
- Adds INLONG filament presets:
  - `ABS`, `HIPS`, `PA6`, `PAEK`, `PATH-CFGF`, `PC`, `PEEK`, `PET-CFGF`,
    `PETG-CFGF`, `PLA`, `PPA-CFGF`, `PPS-CFGF`, `TPU`, `VXL`
- Adds INLONG process/layer presets for the above machine families.

Follow-up fixes:

- `0e5a8c01f2` adjusted `SC12060_common.json`.
- `d6e212cdbf` adjusted INLONG profile colors to the Inlong visual style.
- `87702d23d7` removed or corrected profile metadata that caused runtime
  warnings.

### Infinity3DP Vendor Profiles

Status: `Committed`

Representative commit: `1a221c61f0`

Type: Feature

Goal:

- Add `_Infinity3DP` machine, filament, and process profiles.

Changed files:

- `resources/profiles/_Infinity3DP.json`
- `resources/profiles/_Infinity3DP/machine/**`
- `resources/profiles/_Infinity3DP/filament/**`
- `resources/profiles/_Infinity3DP/process/**`
- `resources/profiles/_Infinity3DP/*_cover.png`

Behavior after change:

- Adds machine families:
  - `Infinity3DP IXBOX`
  - `Infinity3DP IXBOX DUO`
  - `Infinity3DP X1`
  - `Infinity3DP X2`
  - `Infinity3DP X2 DUO`
  - `Infinity3DP X3`
- Adds nozzle variants from `0.2` through `1.2` where supported.
- Adds Infinity3DP materials including common engineering materials and
  high-temperature composites.
- Adds per-machine process presets for IXBOX, X1, X2, and X3 families.

Follow-up fixes:

- Profile color and default-material cleanup was applied after the initial add.
- Wizard/default selection behavior was adjusted so first-run profile application
  uses the intended machine default filament instead of stale or broad installed
  material lists.

### First-Run Wizard Profile Loading And Defaults

Status: `Committed`

Representative commits: `1a221c61f0`, `87702d23d7`

Type: Bug fix

Problem:

- Newly added INLONG / Infinity3DP profiles could show stale or wrong defaults
  during first-run wizard selection.
- Installed profile data under the runtime data directory could override bundled
  resources.
- First-run filament selection could drift away from the printer preset's
  `default_filament_profile`.

Changed files:

- `src/slic3r/GUI/WebGuideDialog.cpp`
- `src/slic3r/GUI/ConfigWizard.cpp`
- `src/libslic3r/PresetBundle.cpp`
- `src/slic3r/GUI/PresetComboBoxes.cpp`

Behavior after change:

- Wizard profile loading prefers the intended bundled/default data path instead
  of silently trusting stale installed vendor JSON.
- Applying a printer through the guide/wizard re-applies the printer preset's
  default filament profile when needed.
- Dynamic filament list updates after wizard/profile changes are more consistent.

Verification:

- Runtime log triage confirmed the relevant profile/wizard warnings were reduced
  or removed after rebuild and relaunch in the prior cleanup pass.

### Runtime Log Noise And Startup Warning Cleanup

Status: `Committed`

Representative commit: `87702d23d7`

Type: Bug fix / diagnostics cleanup

Problems:

- Startup logs included noisy full JSON dumps from guide/profile loading.
- Some warnings were caused by profile metadata mismatches rather than active
  runtime failures.
- Unsupported vendor update checks and optional network/plugin calls produced
  confusing warnings in an Inlong-focused build.

Changed files:

- `src/slic3r/GUI/WebGuideDialog.cpp`
- `src/slic3r/GUI/HintNotification.cpp`
- `src/slic3r/GUI/PartPlate.cpp`
- `resources/profiles/INLONG.json`
- `resources/profiles/INLONG/machine/**`
- `resources/profiles/_Infinity3DP.json`
- `resources/profiles/_Infinity3DP/machine/**`

Behavior after change:

- Guide/profile logs are summarized instead of dumping large JSON payloads.
- Part plate/profile data warnings were cleaned up.
- Unsupported vendor/profile update paths were quieted or guarded.

Verification:

- The previous cleanup pass rebuilt the app and inspected a fresh runtime log.
- The key warning/error strings from the initial log review no longer appeared
  in the fresh log.

### Windows Build Workflow Shortcuts

Status: `Committed`

Representative commits: `0f92413666`, `1f3d8c8eb9`

Type: Build workflow feature

Goal:

- Make Windows builds faster and easier to run during iterative development.

Changed files:

- `build_release_vs.bat`

Behavior after change:

- Adds and refines command-line modes:
  - `fast`
  - `allfast`
  - `slicer`
  - no-argument default mode
- Keeps a faster path for app-only rebuilds rather than always rebuilding or
  installing everything.

Verification:

- Later GUI and install builds used the generated CMake/Visual Studio tree
  successfully:

```powershell
cmake --build build --config Release --target InlongSlicer_app_gui -- /m:4 /nr:false /v:m
cmake --build build --config Release --target INSTALL -- /m:4 /nr:false /v:m
```

### Profile Validation Script Rename And Inlong Checks

Status: `Committed`

Representative commit: `012550f8ed`

Type: Build/test workflow change

Goal:

- Align profile validation tooling with Inlong naming while keeping validation
  behavior available.

Changed files:

- `scripts/orca_extra_profile_check.py` -> `scripts/inlong_extra_profile_check.py`
- `scripts/orca_filament_lib.py` -> `scripts/inlong_filament_lib.py`
- `.github/workflows/check_profiles.yml`
- `src/dev-utils/OrcaSlicer_profile_validator.cpp` ->
  `src/dev-utils/InlongSlicer_profile_validator.cpp`

Behavior after change:

- Profile validation is now run through Inlong-named scripts and validator
  target.
- Agent/build guidance points to `scripts/inlong_extra_profile_check.py` and
  `InlongSlicer_profile_validator`.

Preferred verification for future profile edits:

```powershell
python scripts\inlong_extra_profile_check.py
cmake --build build --config Release --target InlongSlicer_profile_validator -- /m:4 /nr:false /v:m
build\src\Release\InlongSlicer_profile_validator.exe
```

### User-Visible Inlong UI Identity

Status: `Committed`

Representative commits: `d6e212cdbf`, `f2eebdcf68`, `012550f8ed`,
`932616e0b9`

Type: Branding with user-facing functional impact

Goal:

- Make visible UI, executable identity, icons, About text, and installed package
  metadata consistently show InlongSlicer / Inlong Slicer.

Changed files:

- `version.inc`
- `CMakeLists.txt`
- `src/CMakeLists.txt`
- `src/OrcaSlicer.cpp` -> `src/InlongSlicer.cpp`
- `src/OrcaSlicer.hpp` -> `src/InlongSlicer.hpp`
- `src/OrcaSlicer_app_msvc.cpp` -> `src/InlongSlicer_app_msvc.cpp`
- `src/slic3r/GUI/AboutDialog.cpp`
- `resources/images/InlongSlicer*`
- `resources/web/image/logo*.png`
- `resources/Icon.icns`
- `scripts/flatpak/io.github.JaredLin1217.InlongSlicer.*`
- `src/dev-utils/platform/**`

Behavior after change:

- Windows build outputs use `inlong-slicer.exe` and `InlongSlicer.dll`.
- The About dialog and README explicitly mention AGPL-3.0 licensing.
- App icons, splash/logo resources, web logo assets, and macOS/Windows icon
  bundles use the Inlong logo assets.
- Package and desktop metadata use `io.github.JaredLin1217.InlongSlicer`.

Notes:

- Allowed Orca cloud/wiki/protocol references are intentionally retained and are
  documented in `orcaslicer_to_inlongslicer_migration.md`.

### Inlong Visual Theme And Accent Color

Status: `Committed`

Representative commit: `d6e212cdbf`

Type: UI theme change

Goal:

- Move visible UI accent styling toward the Inlong style.

Changed files:

- `resources/images/**`
- `src/libslic3r/PresetBundle.cpp`
- `src/slic3r/GUI/Plater.cpp`
- `localization/i18n/en/OrcaSlicer_en.po`
- `localization/i18n/zh_CN/OrcaSlicer_zh_CN.po`
- `localization/i18n/zh_TW/OrcaSlicer_zh_TW.po`
- profile color metadata under `resources/profiles/INLONG/**` and
  `resources/profiles/_Infinity3DP/**`

Behavior after change:

- UI accent color and many SVG/icon assets were recolored for the Inlong palette.
- INLONG and Infinity3DP profile metadata uses the Inlong visual style.
- Main/sub nozzle labels were localized in English, Simplified Chinese, and
  Traditional Chinese for the visible panel headings.

### InlongArena And Filament Library Rename

Status: `Committed`

Representative commit: `012550f8ed`

Type: Profile/resource migration

Goal:

- Rename Orca-branded bundled profile libraries to Inlong-branded equivalents
  while preserving the underlying profile content.

Changed files:

- `resources/profiles/OrcaArena.json` -> `resources/profiles/InlongArena.json`
- `resources/profiles/OrcaArena/**` -> `resources/profiles/InlongArena/**`
- `resources/profiles/OrcaFilamentLibrary/**` ->
  `resources/profiles/InlongFilamentLibrary/**`
- `resources/profiles/Custom/orcaslicer_bed_texture.svg` ->
  `resources/profiles/Custom/inlongslicer_bed_texture.svg`

Behavior after change:

- The bundled Arena and filament-library profile paths align with Inlong naming.
- Compatible profile references and inherited profile names were updated to keep
  profile loading functional after the rename.

### Flatpak, Linux, macOS, And CI Package Identity

Status: `Committed`

Representative commit: `012550f8ed`

Type: Packaging workflow change

Goal:

- Align package metadata and build scripts with InlongSlicer.

Changed files:

- `build_flatpak.sh`
- `build_linux.sh`
- `build_release_macos.sh`
- `scripts/flatpak/**`
- `.github/workflows/build_*.yml`
- `src/dev-utils/platform/osx/Info.plist.in`
- `src/dev-utils/platform/unix/io.github.JaredLin1217.InlongSlicer.desktop`

Behavior after change:

- Package IDs, desktop files, and Flatpak metadata use the Inlong application id.
- CI/build workflow names and generated artifacts use Inlong naming where this
  fork owns the output identity.

### Agent And Documentation Workflow

Status: `Committed` plus this file

Representative commits: `6048c1053b`, `0b850cf428`, `8b69eeabdb`

Type: Developer workflow feature

Goal:

- Make future local coding-agent work reproducible and safer.

Changed files:

- `AGENTS.md`
- `.agents/README.md`
- `.agents/skills/source-command-dedupe/SKILL.md`
- `.agents/skills/source-command-oncall-triage/SKILL.md`
- `.agents/skills/inlong-branding-migration/SKILL.md`
- `.claude/commands/dedupe.md`
- `.claude/commands/oncall-triage.md`

Behavior after change:

- `AGENTS.md` defines repo-wide safe working rules, documentation ownership,
  build/test expectations, and the `inlong/orca-2.4-base` comparison base.
- Source-command skills were ported into `.agents/skills`.
- The branding migration workflow has a repo-local skill and canonical Markdown
  record.
- This `functional_change_log.md` is now the record for functional changes and
  bug fixes outside the pure branding checklist.

## Future Entry Template

Use this template for new entries:

````markdown
### Short Change Title

Status: `Uncommitted` or `Committed`

Representative commit: `hash` or `pending`

Type: Bug fix / Feature / Build workflow / Packaging / Profile / Documentation

Problem or goal:

- ...

Root cause:

- ...

Changed files:

- `path/to/file`

Behavior after change:

- ...

Verification:

```powershell
command
```

Notes:

- ...
````
