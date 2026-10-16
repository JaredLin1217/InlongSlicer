# OrcaSlicer upstream integration - 2026-10-17

Integrated upstream `main` at `6d40ec93f680ec32ca97c15c05eff79df0eb7a9d` (ten commits after the prior integration). The upstream repository has no `master`; its live default branch is `main`. Pre-merge local checkout: `d9e9b2e576c5a4bea9e09fa8c433d97807484ff7`.

Conflicts were in CMakeLists.txt and SupportCommon.cpp. The merge retains the Inlong status label and imports the Xcode compiler-launcher configuration. The new macOS comment uses InlongSlicer. Other incoming changes cover cheap PNG header probing, lazy texture thumbnails, Windows extended-length archive paths and GUI tab/publish layout. The final fetched commit fixes empty support collections and misplaced extra walls, removes forced double walls above 100 mm for organic supports, and reinforces classic-tree moving branches only where their actual areas need it. Local multi-island, normal contact-gap, roof-stack and X2 fixes remain.

## Validation

- Windows Release core and GUI libraries and all three affected test executables built successfully. A complete application DLL was linked into the registered disposable runtime, with project references and build events disabled to avoid portable-data reset targets.
- Initial candidate core tests: 123 cases, 8,775 assertions passed. Filters cover Utils, MinizExtension, PNG, TextureDisplacement and the X2 Voronoi regressions. Two additional PNG probe tests verify cheap header probing versus corrupt pixels and safe refusal of truncated headers.
- Plugin installation regressions passed: eight cases and 33 assertions, including paths beyond MAX_PATH and rejection of escaping entries.
- Eleven disposable packaging contract / installer CLI dry-run tests passed. Exact removal of the added APPLE block reproduces the old CMake file after newline normalization; Windows install rules did not change.
- Brand-color checks and Bash syntax passed. Required filename/content scans were reviewed: no new Orca product identity or old palette was introduced. Existing provenance, plugin ABI, protocol and legacy-import references remain.
- A final live ref check found the additional support commit after the first candidate had passed all three project comparisons. The pending merge was safely restarted from the new tip after backing up every owned staged file and the full patch. The affected support suite and all three slices were rerun against the final candidate. Unchanged-source hashes bind reuse of earlier checks.

Final support suite: 84 cases and 18,107 assertions passed. The earlier wrong-target attempt matched no cases and exited 1; it was retained as a failed attempt, corrected to fff_print_tests, and supplies no passing evidence.

| Project | Fresh total seconds | Complete ordered commands | Match latest fixed baseline |
| --- | ---: | ---: | --- |
| 3DBenchy.3mf | 100.796 | 811,657 | Commands, annotated motion and configuration identical |
| Vulcan600培训用档案.3mf | 31.151 | 1,211,052 | Commands, annotated motion and configuration identical |
| X2培训用档案.3mf | 62.708 | 1,348,868 | Commands, annotated motion and configuration identical |

Times include input import, fresh slicing, export and process completion. They are single validation runs, not controlled performance benchmarks. Each input SHA-256 matches the prior verified project; each log confirms use_cache=0 and one object requiring slicing. No model, material or quality override was passed. All processes exited zero and console streams were empty. Each complete native log retains five known metadata/initialization warning events. The prior Voronoi/MedialAxis and support-branch failure strings are absent.

## Preservation and scope

- The five protected support/X2 geometry and regression-test files retain their previous verified hashes. No profile source changed.
- Both package data directories retain their exact file sets and hashes. The Release data directory has no deletions: 831 original files remain byte-identical, while two preference files were rewritten and one log added by an independent old-runtime GUI session at 05:41:08 (PID 34120, build bc4d9aa5de). These current changes were preserved and separately backed up; no settings restoration was performed. GUI ownership confirmation is recorded in JSON. The test DLLs ran in separate explicitly selected data directories. All three installed DLLs retain their prior hashes. No install or filtered install target was invoked.
- One agent performed the work. GitHub access fetched the upstream source. The separately authorized prior push of 2407aade57/d9e9b2e576 completed and was remotely verified before this merge. This integration is local and is not pushed.
- Two immutable Windows install knowledge records were migrated after source drift review. Their historical installation measurements remain explicitly historical; this task does not claim a fresh installation. The normal-support contact record is separately re-reviewed against the support delta and final native regressions.

## Limits and evidence

Interactive GUI behavior and macOS/Xcode execution remain unverified. Compiler warnings remain in unchanged PresetBundle.cpp/InlongSlicer.cpp (C4805), TextureToColor.cpp (C5051) and test_vendor_cache.cpp (C4996); passing negative test fixtures intentionally log rejected input errors. Consumer validation does not establish Provider release readiness. The referenced Codex chat was unavailable, so it supplies no evidence or authority.

Machine-readable evidence: `docs/evidence/upstream-sync-verification-20261017.json`.
Raw logs and native results: `.agents/runtime/runs/run-325d0712ef36435483924c90535320a9/validation`.

Source snapshots identify the validated worktree bytes. The checkout anchor identifies the pre-merge state; the final local merge commit and Consumer receipt references are recorded in runtime task completion evidence.
