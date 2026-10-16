# OrcaSlicer upstream integration, 2026-10-16

## Source and scope

The local branch is `feature/inlong-slicer-initial`. Its original tip was
`f9c8b09d2ebffd4de1da23215c00cbc51d5f11e2`. The official upstream repository is
`https://github.com/OrcaSlicer/OrcaSlicer.git`; it has `main`, not `master`.
The fetched and integrated tip is
`109d09e0b8a25490ee0394f55769b1d735c36bba`, titled
"Fix multi-tool standby heaters by turning off unused extruders (#15849)".
The previous common ancestor was
`b920116fbd68781652a868fccc1e84718409c1f8`. The incoming range contains 111
commits and changes 533 paths. This task creates a local merge; it does not push
or publish a release.

## Integration decisions

- Resolved all 70 conflicted files. Preserved the managed `AGENTS.md` byte for
  byte, the existing Consumer deployment, and the fork's canonical migration
  instructions.
- Preserved Inlong application identity, cloud classes, filament IDs, build and
  installer target names, and the visible Inlong Palette. Retained external Orca
  service URLs, historical preset inheritance, plugin ABI names and provider
  aliases where they are compatibility contracts.
- Preserved the fork's sequential preset loading and support/cache behavior
  while incorporating independent upstream fixes. Integrated the new cached
  shrinkage helpers and consistent G-code viewer path-boundary fields.
- Retained every original curated INLONG, Infinity3DP and InlongArena profile
  source value. Kept all 512 original filament-library entries and added the 44
  genuinely new entries. Updated 22 setting-ID and snapshot records with a
  zero-parameter-change check. A destructive formatting/normalization preview
  was confined to disposable data and was not applied.
- Added explicit `--fork-compatible` profile checks for the four curated Inlong
  bundles. Reference integrity, coverage and ID checks remain enabled. Added
  negative-reference and legacy-library regressions. The strict default remains
  available for other authoring workflows.
- Merged 24 translation catalogs by context/message ID, repaired Turkish plural
  entries, and adapted the incoming profile/translation tools, local build
  commands and CI actions to existing Inlong conventions.
- Repaired native integration mismatches in `InstanceLock` inclusion, the AMS
  filament-ID helper and device-access test constants. Device-access tests also
  retain explicit coverage of the legacy `orca` provider alias.

## Verification

Evidence is retained under the registered run
`.agents/runtime/runs/run-68466b09078d4b8cb9eb41cd1cefa97d`.

| Area | Result and evidence |
| --- | --- |
| Windows Release targets | All five pass: application GUI, profile validator, FFF tests, core tests and GUI utility tests. `validation/native-build-retry-3.json` and its build log; the final test-only changes also pass in `validation/native-build-retry-4.json`. |
| Native effective profiles | Passed for 74 reported vendors; 13,788 JSON files and four info files were mirrored with matching source hashes. `validation/native-tests/early-profile-report.json`. |
| Native Unicode profile fixtures | Both passed, with no skips, using the current validator. Same report as above. |
| FFF affected regressions | 292 cases, 591,546 assertions passed. `validation/native-tests/early-suite-fff_print.json`. |
| Core affected regressions | 497 cases, 18,248 assertions passed. `validation/native-tests/early-suite-libslic3r-short-temp.json`. |
| GUI logic and CLI help | 107 GUI cases pass, one symlink case is skipped because Windows does not grant the required privilege, and zero cases fail. CLI help and test-runtime NumPy 2.5.3 checks pass. `validation/native-tests/gui-isolated-report.json` and `validation/native-tests/report.json`. |
| Python and profile tooling | Initial sweep ran 372 cases, exposing seven integration failures and one optional Bambu local-clone skip. An affected rerun passed 112 of 113 cases; the remaining real-tree policy failure subsequently passed in `validation/profile-real-tree-unit-current.json` and `validation/profile-policy-current.json`. The two added reference/legacy-library cases pass in the otherwise superseded `validation/profile-compatibility-final.log`. ID policy and both Windows installer dry-run cases pass. Earlier aggregate reports retain their original failed status. |
| Profile diagnostics | Supported policy: 73 vendor directories, zero errors, 28 files with existing obsolete-key warnings. Extra profile checker: 72 vendors, zero errors or warnings. |
| Localization and web | All 23 locale checks, validation-only template-header check, 14 real Edge color-picker cases and five web contracts pass after the final palette changes. `validation/palette/report.json`. |
| Static and source review | YAML/local action paths, shell and Python syntax, guidance, brand colors, source identity, custom-profile preservation and whitespace checks pass. `validation/final-source-audit.json` and related reports. |
| Consumer Changed | Passed. Receipt: `.agents/runtime/runs/run-d610de4a2f524702bdfbcc3c00eb7448/validation/report.json`. Input digest: `fbf7f2373131c684ead94cd9bc108b56a09846ebd058eb8fd9588189ab38fae6`. |
| Consumer Checkpoint | All six checks pass. Receipt: `.agents/runtime/runs/run-52028782704649c8ab971c66257d2d5c/validation/report.json`. Input digest: `febf8dac8165f549b9cba706eccf61168f3c2fe22ea0b49cf63ce2700d3988e8`. |
| Original portable settings | All 791 original files restored with matching SHA-256 hashes; all three directory file sets exactly match the task-start snapshot. Newer changed/extra files were archived before restoration. `validation/portable-settings-restored.json`. |

The first core regression run encountered one fixture copy failure because the
registered temporary path produced a 268-character Windows filename. Repeating
the same complete selected suite with a separately registered shorter temporary
directory passed all 497 cases. This required no production-code or slicing
parameter change. Earlier failure logs remain available.

The first GUI batch exposed stale application/provider expectations and attempted
to reinitialize embedded Python between cases in one process. The expectations
were adapted to the existing Inlong contracts. Final GUI execution follows the
CTest-discovered commands with a separate process for every case, as required by
the plugin fixtures. All embedded-Python cases execute successfully; only the
actual symlink privilege case is skipped. The earlier batch report and logs
remain available.

Successful early native checks are reused only if the profile-input and relevant
executable/library hashes still match. Changed targets are rebuilt and checked.
Consumer validation is separate from product validation; its passing result does
not assert Provider release readiness.

## Settings preservation

Before the native build, all 791 original portable files were backed up and
hash-verified across the three local build-output data directories. A later
runtime initialization produced 832 files; these were independently backed up
and verified before another resetting build. The user confirmed that they had
not opened the application during that interval and explicitly requested the
task-start settings. The original snapshot is authoritative for restoration.
Both snapshots remain protected rollback artifacts; no retention cleanup was
requested. After the final tests, 51 newer changed or extra files were archived
under `backup/post-validation`, and all 791 original files were restored with
matching hashes and an exact active-file-set comparison. The other two portable
data directories are empty, as they were at task start. No further resetting
build was run after restoration.

## Limits

The native build uses the existing Windows dependency prefix, with at most three
parallel compiler jobs. It does not establish a clean third-party dependency
rebuild, macOS or Linux runtime compatibility, actual NSIS packaging, execution
of `install` or `install_only_inlong`, full manual GUI behavior, live cloud
authentication, or unattended release publication. Native suites are selected
for affected slicing, profiles, core and GUI logic; they are not the entire
upstream test inventory. Build warnings and earlier repaired failures are
retained in the logs. The built executable records the pre-commit dirty source
version because compilation precedes the local merge commit.

One root agent performed this task; no worker agents were spawned.
