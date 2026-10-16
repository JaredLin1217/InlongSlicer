# Three-project slicing and native log audit

All three supplied projects were freshly sliced twice on the latest local candidate. Every native process exited 0 and emitted one complete plate G-code. The zero-warning/error acceptance condition failed. X2 has reproducible geometry errors even though the CLI exits successfully.

Candidate DLL SHA-256: `858ff538cc66521c133148473cd3b4f265fa1d83df833bf33d99e3783a95f779`. Checkout anchor: `a53b59db336d3e87f0cd55206bf49ffe3e81a9f0`. The candidate includes the uncommitted contact-sampling performance change; the installed runtime was not updated.

| Project | Primary total (s) | Repeat (s) | Peak RSS (GiB) | Errors | Warnings |
| --- | ---: | ---: | ---: | ---: | ---: |
| 3DBenchy.3mf | 23.378 | 16.914 | 1.575 | 0 | 5 |
| Vulcan600培训用档案.3mf | 31.613 | 29.600 | 3.169 | 0 | 5 |
| X2培训用档案.3mf | 52.452 | 53.034 | 2.627 | 7 | 52 |

Times include project import, fresh slicing, G-code export and native process completion. These single pairs are validation timings, not a controlled speedup benchmark.

## Findings

- Each project emits five warning-level events: one CLI version banner, two missing legacy BBL full-preset compatibility lookups, one missing filament-color metadata message and one backward-compatible nozzle metadata reconstruction message. Source review shows these are initialization/metadata/compatibility paths, not branch propagation failures. They remain counted as warnings.
- X2 additionally emits 47 missing-Voronoi-vertex retry warnings, six errors after rotation repair, and one MedialAxis error after morphological closing. These counts repeat exactly. Rotation repair is in `src/libslic3r/Geometry/Voronoi.cpp:39-54`; the final closing failure is in `src/libslic3r/Geometry/MedialAxis.cpp:461-474`.
- The X2 messages occur after organic branch geometry assembly, during subsequent path generation. The logs do not include a call stack, layer or XY location, so the exact originating feature remains unproven. No geometry repair or logging-level code change was made.
- None of the six runs contains the earlier failed branch expansion, invalid gracious landing, or uninitialized support-element messages. This does not prove all resulting geometry is valid.

## Verification and boundaries

- Each input has one plate. `--slice 0` covers all plates. Full logs show `use_cache=0` and one object requiring slicing, rather than reusing stored G-code.
- The first pass captures info/warning/error logs with `--debug 3`; the second pass captures every warning/error event with `--debug 2`. Every nonempty warning-pass message was reviewed and mapped to its native source severity; its count also matches the first-pass log. Both stdout/stderr streams are empty for all six runs.
- Input file SHA-256 values are unchanged. Primary and repeat G-code have identical complete ordered commands, annotated motion and emitted configuration for each project.
- Normal CLI system-preset refreshes/missing-key migration occurred. The JSON records every emitted refresh. No manual model, quality or material override was passed. Original files and live user configuration were preserved.
- Windows does not emit the CLI `result.json` warning array: `record_exit_reson` is guarded for Linux. This audit therefore uses native logs and reviewed severity-producing source sites.
- One agent executed six native slices sequentially. No installation, commit or push was performed.

## Artifacts

Machine-readable evidence: `docs/evidence/three-project-slicing-log-audit-20261016.json`.

### 3DBenchy.3mf

Input SHA-256: `c65b1fcafd39c2c64ed9b906459d4c6ef2b0821b6b938e91fdfb7943bc181ea6`.
G-code: `D:\Work\Nowing\GitHub\InlongSlicer\.agents\runtime\runs\run-961ca6884f174742b74f4d590c84c9ea\work\benchy\plate_1.gcode`.
Full log: `.agents/runtime/runs/run-961ca6884f174742b74f4d590c84c9ea/validation/benchy-native.log`.
Warning/error-only log: `.agents/runtime/runs/run-961ca6884f174742b74f4d590c84c9ea/validation/benchy-severity-native.log`.
Ordered command count: 811,657.

### Vulcan600培训用档案.3mf

Input SHA-256: `3152e607f007309fbe5d0e840b35ee45ff712a3a0e074d55225c23861d0a9644`.
G-code: `D:\Work\Nowing\GitHub\InlongSlicer\.agents\runtime\runs\run-961ca6884f174742b74f4d590c84c9ea\work\vulcan600\plate_1.gcode`.
Full log: `.agents/runtime/runs/run-961ca6884f174742b74f4d590c84c9ea/validation/vulcan600-native.log`.
Warning/error-only log: `.agents/runtime/runs/run-961ca6884f174742b74f4d590c84c9ea/validation/vulcan600-severity-native.log`.
Ordered command count: 1,211,052.

### X2培训用档案.3mf

Input SHA-256: `ea552203efa070cd53aa4f29dd8572ef5a7a97a8978a857154e41baba3c4dff9`.
G-code: `D:\Work\Nowing\GitHub\InlongSlicer\.agents\runtime\runs\run-961ca6884f174742b74f4d590c84c9ea\work\x2\plate_1.gcode`.
Full log: `.agents/runtime/runs/run-961ca6884f174742b74f4d590c84c9ea/validation/x2-native.log`.
Warning/error-only log: `.agents/runtime/runs/run-961ca6884f174742b74f4d590c84c9ea/validation/x2-severity-native.log`.
Ordered command count: 1,348,868.
