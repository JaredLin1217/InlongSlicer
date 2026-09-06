# OrcaSlicer To InlongSlicer Migration Record

This file is the source of truth for keeping the fork fully Inlong branded after
upstream OrcaSlicer changes are merged. InlongSlicer follows `upstream/main`
for version and architecture. Use this record before and after every
upstream sync, rebase, cherry-pick, profile refresh, packaging change, or asset
update.

The goal is not a blind text replacement. Some Orca references are retained
because they identify upstream comparison branches, retained external services,
or third-party dependency sources. Everything else should converge to the
Inlong naming, color, asset, profile, package, and runtime identity listed here.

## Canonical Inlong Identity

| Area | Canonical value |
| --- | --- |
| Product name, compact | `InlongSlicer` |
| Product name, display | `Inlong Slicer` |
| Command name | `inlong-slicer` |
| CMake project | `InlongSlicer` |
| GUI wrapper target | `InlongSlicer_app_gui` |
| Core library or executable target | `InlongSlicer` |
| Profile validator target | `InlongSlicer_profile_validator` |
| Windows GUI exe | `inlong-slicer.exe` |
| Windows core DLL | `InlongSlicer.dll` |
| Main exported entry | `inlongslicer_main` |
| Application key | `InlongSlicer` |
| Application id | `io.github.JaredLin1217.InlongSlicer` |
| GitHub repository | `JaredLin1217/InlongSlicer` |
| Dependency install prefix | `InlongSlicer_dep` |
| Primary accent color | `#D66C47` |
| Primary accent RGB | `214, 108, 71` |
| Primary accent normalized RGB | `214.0f / 255.0f, 108.0f / 255.0f, 71.0f / 255.0f` |
| Runtime config folder | `InlongSlicer` |
| Log folder | `data_dir/log` in portable builds, otherwise platform config under `InlongSlicer/log` |

Keep these values aligned in `version.inc`, `CMakeLists.txt`,
`src/CMakeLists.txt`, `src/libslic3r/libslic3r_version.h.in`, platform resource
templates, packaging scripts, CI, translations, and runtime messages.

## Allowed Orca References

Do not remove these without an explicit product decision:

- Git comparison/base names such as `inlong/orca-2.4-base`.
- `.git` refs, tags, logs, and remote branch names containing `orca`.
- Retained external Orca cloud and wiki URLs:
  - `https://www.orcaslicer.com/wiki`
  - `https://cloud.orcaslicer.com`
  - `https://auth.orcaslicer.com`
  - `api.orcaslicer.com`
  - `/orcaslicer-login`
  - `orca_state`
- Upstream Python plugin ABI and discovery contracts introduced on the 2.5
  development line, including `ORCA_PY_*`, `orca_version`,
  `[tool.orcaslicer.plugin]`, `orca_plugins`, `orca_stubgen`, upstream example
  scripts, and sandbox filenames. These remain compatible even though visible
  UI labels use Inlong branding.
- OrcaSlicer G-code producer detection, legacy Flatpak data-directory names,
  the `orcaslicer://` deep-link protocol, and imported `.orca_printer`,
  `.orca_bundle`, and `.orca_filament` package formats.
- Third-party dependency download origins that still live under SoftFever or
  Orca dependency repositories, for example `Orca-deps-wxWidgets`,
  `OrcaSlicer_deps`, and `orca_deps`.
- The `OrcaSlicer/OrcaSlicer-profile-validator` fixture archive is retained only
  to test compatibility with custom presets saved by older OrcaSlicer versions.
  Run those fixtures with the locally built InlongSlicer profile validator;
  validator binary downloads use the InlongSlicer nightly release.
- Historical notes that explicitly describe upstream comparison context, when
  they are not user-visible product identity.

If one of these needs to change, document the new service, protocol, branch, or
mirror first. Cloud login paths and callback parameters can be service contracts,
not just strings.

## Required Replacement Surface

Every upstream merge must check the following areas.

### Build And Target Names

- `version.inc`
  - `SLIC3R_APP_NAME`
  - `SLIC3R_APP_DISPLAY_NAME`
  - `SLIC3R_APP_KEY`
  - `SLIC3R_APP_CMD`
  - `SLIC3R_APP_ID`
  - `INLONGSLICER_VERSION`
- Top-level `CMakeLists.txt`
  - `project(InlongSlicer)`
  - dependency prefix `InlongSlicer_dep`
  - startup project `InlongSlicer_app_gui`
  - CPack package name, vendor, icon, registry key, shortcut, and installer file
    naming
  - gettext output names based on `SLIC3R_APP_KEY`
- `src/CMakeLists.txt`
  - target names
  - output names
  - Windows wrapper exe
  - macOS bundle resources
  - install target names
- Generated CMake cache/build output is not source truth. Clean stale
  `build`, `deps/build`, or old `OrcaSlicer_dep` output before judging whether
  the source tree is fully renamed.

### Source Filenames And Symbols

- Root launcher/source files:
  - `src/InlongSlicer.cpp`
  - `src/InlongSlicer.hpp`
  - `src/InlongSlicer_app_msvc.cpp`
- Dev utilities:
  - `src/dev-utils/InlongSlicer_profile_validator.cpp`
  - platform templates under `src/dev-utils/platform`
- Network and printer agents:
  - `InlongCloudServiceAgent`
  - `InlongPrinterAgent`
  - `ICloudServiceAgent` and `IPrinterAgent` call sites
  - factory registration and include paths in `NetworkAgentFactory`
- Logging, crash, exception, CLI, upload, and validation messages must say
  `InlongSlicer` or `Inlong Slicer` unless they identify an allowed external
  service.
- Search for stale symbols such as `OrcaCloudServiceAgent`,
  `OrcaPrinterAgent`, `OrcaSlicer_profile_validator`, `OrcaSlicer.cpp`,
  `OrcaSlicer.hpp`, `orca_slicer`, and `orcaslicer` outside the allowlist.

### UI Brand Color

Use Inlong accent `#D66C47` consistently for product-colored UI accents.

Required source anchors:

- `src/libslic3r/Color.hpp`
  - `ColorRGB::INLONG()`
  - `ColorRGBA::INLONG()`
- GUI code that uses hard-coded accent colors, including wx color values such as
  `wxColour("#D66C47")` or `wxColour(0xD6, 0x6C, 0x47)`.
- Web resources:
  - `resources/web/dialog/css/common.css`
  - `resources/web/dialog/css/theme.css`
  - `resources/web/guide/**`
  - `resources/web/model/**`
  - `resources/web/homepage/**`
- SVG icons and image assets that previously used the Orca accent.
- Flatpak branding colors in
  `scripts/flatpak/io.github.JaredLin1217.InlongSlicer.metainfo.xml`.

When changing color in the future, update all code, CSS, SVG, web, and bitmap
asset sources together. Do not leave mixed brand accents.

Run the automated color guard after every upstream merge:

```powershell
python scripts\check_inlong_brand_colors.py
```

### Logos, Icons, And Images

Required resource names should use `InlongSlicer` or `inlong`:

- `resources/images/InlongSlicer.png`
- `resources/images/InlongSlicer.ico`
- `resources/images/InlongSlicer.icns`
- `resources/images/InlongSlicer.svg`
- `resources/images/InlongSlicerTitle.*`
- `resources/images/InlongSlicer_about*.svg`
- `resources/images/InlongSlicer_gradient*.svg`
- `resources/images/inlong_bed_pct_left.svg`
- `resources/profiles/Custom/inlongslicer_bed_texture.svg`

Also check:

- README logo references.
- About dialog image references.
- Windows `.rc.in` icon paths.
- macOS `Info.plist.in` icon paths.
- Linux desktop/AppImage/Flatpak icon paths.
- Web homepage icons and account icons.
- Staged runtime resources copied into `build/src/<config>/resources`.

### Web Resources

Required Inlong paths and commands include:

- `resources/web/inlong`
- `resources/web/guide/4inlong`
- homepage commands such as `homepage_inlong_login_or_register`,
  `homepage_inlong_logout`, and `get_inlong_login_info`
- visible text such as `Inlong Cloud Account`
- translation keys may use an `inlong` prefix when the key was introduced for
  this fork

Keep Bambu-specific text where it is actually about Bambu printers or Bambu
cloud. Keep Orca cloud/wiki URLs only under the allowlist.

### Profiles And Presets

Profile migration is high risk. Change names, references, and validation
together.

Required top-level profile identities:

- `resources/profiles/INLONG`
- `resources/profiles/InlongArena.json`
- `resources/profiles/InlongArena/`
- `resources/profiles/InlongFilamentLibrary.json`
- `resources/profiles/InlongFilamentLibrary/`

Check all JSON keys and values:

- `name`
- `vendor`
- `inherits`
- `compatible_printers`
- `compatible_prints`
- `default_filament_profile`
- `default_materials`
- `printer_model`
- `printer_variant`
- any embedded profile names in process, machine, and filament presets

After profile edits, run:

```powershell
python scripts\inlong_extra_profile_check.py
build\src\Release\InlongSlicer_profile_validator.exe
```

If the validator is unavailable, build it first:

```powershell
cmake --build build --config Release --target InlongSlicer_profile_validator -- /m:4 /nr:false /v:m
```

### Calibration And Handy Models

Files bundled with the product should use Inlong names:

- `resources/calib/filament_flow/Inlong-LinearFlow.3mf`
- `resources/calib/filament_flow/Inlong-LinearFlow_fine.3mf`
- `resources/handy_models/InlongCube_v2.3mf`
- `resources/handy_models/InlongToleranceTest.drc`
- `resources/handy_models/Inlong_stringhell.drc`

If model metadata inside these files contains product names, inspect and update
the archive contents as well. Do not rely only on the outer filename.

### Localization

Required gettext names:

- `localization/i18n/InlongSlicer.pot`
- `localization/i18n/<locale>/InlongSlicer_<locale>.po`
- runtime resources `resources/i18n/<locale>/InlongSlicer.mo`

Update visible product strings in all supported locales. Avoid changing
unrelated translations while doing brand migration.

Regenerate or update translations with the repo scripts:

```powershell
scripts\run_gettext.bat
```

If only compiling `.po` to `.mo` on Windows, use the repo's `tools/msgfmt.exe`
when available.

### Packaging And Platform Integration

Check every platform integration point:

- Windows:
  - `src/dev-utils/platform/msw/InlongSlicer.rc.in`
  - `src/dev-utils/platform/msw/InlongSlicer-gcodeviewer.rc.in`
  - `src/dev-utils/platform/msw/InlongSlicer.manifest.in`
  - CPack/NSIS package name, registry key, icon, shortcut, and installed exe
- macOS:
  - `src/dev-utils/platform/osx/Info.plist.in`
  - bundle display name
  - bundle id
  - app icon
  - file association names
- Linux:
  - `src/dev-utils/platform/unix/io.github.JaredLin1217.InlongSlicer.desktop`
  - `src/dev-utils/platform/unix/build_appimage.sh.in`
  - `src/dev-utils/platform/unix/build_linux_image.sh.in`
  - Flatpak manifest and metainfo under `scripts/flatpak`
  - Docker scripts and AppImage output names
- CI:
  - `.github/workflows/build_*.yml`
  - `.github/workflows/check_profiles.yml`
  - `.github/workflows/check_locale.yml`
  - release artifact names
  - issue and PR templates

Changing package ids, bundle ids, installer registry keys, update identities, or
protocol handlers can break installed-user behavior. Do it intentionally and
verify upgrade and fresh-install behavior.

### Documentation And Agent Assets

Keep documentation consistent:

- `README.md`
- `AGENTS.md`
- `CLAUDE.md`
- `.agents/README.md`
- `.agents/skills/inlong-branding-migration/SKILL.md`
- `.claude/commands/*` if a matching legacy command exists
- `.github/pull_request_template.md`
- `tests/CLAUDE.md` if tests need workflow changes

Do not duplicate the full checklist in many places. Link back to this document.

## Upstream Update Workflow

1. Start from a clean understanding of the current worktree:

```powershell
git status -sb
git diff --stat
```

2. Compare incoming upstream changes against the base branch:

```powershell
git diff --name-status inlong/orca-2.4-base...HEAD
```

3. Re-apply Inlong identities conservatively:

- Prefer existing Inlong names and helper APIs.
- Port upstream logic without restoring Orca file names.
- Re-read JSON and CMake before patching.
- Do not rely on old build output or generated resources as source truth.

4. Run filename scan:

```powershell
rg --files -uu -g '!.git/**' -g '!build/**' -g '!deps/build/**' -g '!resources/plugins/**' | rg -i 'orca|orcaslicer'
```

Expected result: no source/resource filename hits outside explicitly allowed
areas.

5. Run content scan:

```powershell
rg -n -i 'orcaslicer|orca_slicer|orca-slicer|orcacloud|orcaprinter|orcaarena|orca' --glob '!.git/**' --glob '!build/**' --glob '!deps/build/**' --glob '!resources/plugins/**' .
```

Classify every hit as one of:

- required Inlong replacement
- allowed cloud/wiki/protocol reference
- allowed upstream branch/base reference
- allowed SoftFever dependency source
- false positive in a non-brand word
- generated or stale build output to delete or regenerate

6. Confirm key binaries and resources:

```powershell
Test-Path build\src\Release\inlong-slicer.exe
Test-Path build\src\Release\InlongSlicer.dll
Test-Path build\src\Release\InlongSlicer_profile_validator.exe
```

7. Run targeted verification:

```powershell
cmake --build build --config Release --target InlongSlicer_app_gui InlongSlicer_profile_validator -- /m:4 /nr:false /v:m
python scripts\inlong_extra_profile_check.py
build\src\Release\InlongSlicer_profile_validator.exe
```

8. After running the app, inspect the newest runtime log:

```powershell
Get-ChildItem build\src\Release\data_dir\log -File | Sort-Object LastWriteTime -Descending | Select-Object -First 1
```

The log should use Inlong process names, Inlong executable names, and no stale
Orca source-file or runtime-resource names except the allowed external
cloud/wiki/protocol URLs.

## Common Pitfalls

- Windows search includes `.git`, `build`, and `deps/build`; these can show old
  Orca names even when source files are clean.
- `git status` may show old rename source names. Use `git add -A` before final
  commit review so deletions, additions, and renames are aligned.
- Runtime resources under `build/src/<config>/resources` can be locked while
  the app is running.
- `.mo` files and binary model archives can hide stale product text.
- CMake cache can retain old `OrcaSlicer_dep` paths until the dependency build
  directory is cleaned or reconfigured.
- Cloud login paths and callback parameters may be external service contracts.
- Profile JSON names, file names, and `inherits` values must be changed
  together.
- Do not touch `.git` refs or logs just to make text search clean.

## Current Known Allowlist At The Time Of This Record

These were intentionally left as Orca references:

- `orcaslicer.com` wiki and cloud URLs.
- `api.orcaslicer.com`, `auth.orcaslicer.com`, `cloud.orcaslicer.com`.
- `/orcaslicer-login` and `orca_state`.
- `inlong/orca-2.4-base` and related `.git` refs/logs.
- SoftFever dependency URLs containing `Orca-deps`, `OrcaSlicer_deps`, or
  `orca_deps`.

These should not be accepted as final source leftovers:

- `OrcaSlicer.pot` output paths in active tools.
- `OrcaSlicer_profile_validator`.
- `OrcaCloudServiceAgent` or `OrcaPrinterAgent`.
- `OrcaArena` or `Orca Filament Library` profile names.
- `OrcaSlicer` image/icon/desktop/package/metainfo names.
- stale `deps/build/OrcaSlicer_dep` output when evaluating a clean dependency
  build.
