# X2 Voronoi and MedialAxis repair

The reproduced X2 geometry failures are repaired. Six fresh native verification slices complete with zero geometry warnings/errors; five existing CLI initialization and metadata compatibility warnings remain per run.

## Root cause and repair

- Captured 42 initially invalid medial-axis contours from the supplied X2 project. Every captured case contains nanometre clipping edges. Raw-diagram validation fails for all 42; the diagnostic probe validates all after near-collinear cleanup within the existing geometry tolerance.
- MedialAxis now preserves valid inputs and only conditions diagnosed invalid inputs containing sub-tolerance edges. Cleanup is bounded by `min(SCALED_EPSILON, 1% of minimum printable width)` and accepted only after the replacement diagram validates. Normal repair/error reporting remains in place when normalization cannot establish validity.
- Voronoi rotation repair moves storage to local containers. A later construction previously continued exposing that old storage. All three construction overloads now clear old state first.

## Native regression evidence

- Two new regression cases fail on the previous implementation with six failed assertions. They cover printable contact skeletons across nanometre clipping edges and reuse after a rotation repair for segment, point and mixed inputs.
- The final geometry suite exits 0: 44 cases pass, including both new cases. One existing hidden NaN-coordinate case marked `[!mayfail]` fails as expected; 20,027 assertions pass and one fails as expected. This expected upstream failure is explicitly retained.
- The affected support suite passes 84 cases and 18,107 assertions. Temporary diagnostic instrumentation and the probe-only test were removed.

## Supplied-project verification

| Project | Geometry errors before / after | Geometry warnings before / after | Remaining CLI warnings | Same baseline commands | Same configuration |
| --- | ---: | ---: | ---: | --- | --- |
| x2 | 7 / 0 | 47 / 0 | 5 | False | True |
| benchy | 0 / 0 | 0 / 0 | 5 | True | True |
| vulcan600 | 0 / 0 | 0 / 0 | 5 | True | True |

Every project has one plate and is freshly sliced with `--slice 0`, at info level 3 and again at warning level 2. Native completion is awaited, stdout/stderr are captured, input hashes remain unchanged, and repeated commands, annotated motion and configuration match exactly. Benchy and Vulcan600 match the previous baseline exactly. See JSON for the measured X2 command comparison.

X2 differences were aligned within matching layer blocks rather than paired by downstream shifted command indices. All changed annotated records belong to `Support interface`: 14 layer blocks include command or width-annotation changes, and 11 of those contain actual command changes. Model extrusion commands and every other feature's annotated command records remain identical. All layer Z values and the total 1,348,868 command count remain unchanged. Full aligned changes are retained in `validation/x2-path-difference.json`.

The remaining five warning events are the CLI version banner, two legacy BBL compatibility-preset lookups, missing filament-color metadata, and backward-compatible nozzle metadata reconstruction. No warning severity was changed or suppressed.

Normal CLI preset refreshes are unchanged; no explicit model, quality or material overrides were supplied. The earlier uncommitted support-performance source remains byte-for-byte unchanged. Existing installed DLL hashes remain unchanged. One agent performed the work; no install, commit or push was performed.

Fixed DLL SHA-256: `4e9835048b06aaa1610486459dd212227d0ddcf82b73ae29f3e957fdd174c533`. Checkout anchor `a53b59db336d3e87f0cd55206bf49ffe3e81a9f0` identifies the base commit, not the uncommitted fixed source.

## Artifact locations

Machine-readable verification: `docs/evidence/x2-voronoi-repair-verification-20261016.json`.

- x2 G-code: `D:\Work\Nowing\GitHub\InlongSlicer\.agents\runtime\runs\run-cb5f41021b254606a10d628c6f3f75c4\work\x2-fixed\plate_1.gcode`
- Full native log: `D:\Work\Nowing\GitHub\InlongSlicer\.agents\runtime\runs\run-cb5f41021b254606a10d628c6f3f75c4\validation\x2-fixed-native.log`
- Warning/error-only log: `D:\Work\Nowing\GitHub\InlongSlicer\.agents\runtime\runs\run-cb5f41021b254606a10d628c6f3f75c4\validation\x2-warnings-native.log`
- benchy G-code: `D:\Work\Nowing\GitHub\InlongSlicer\.agents\runtime\runs\run-cb5f41021b254606a10d628c6f3f75c4\work\benchy-fixed\plate_1.gcode`
- Full native log: `D:\Work\Nowing\GitHub\InlongSlicer\.agents\runtime\runs\run-cb5f41021b254606a10d628c6f3f75c4\validation\benchy-fixed-native.log`
- Warning/error-only log: `D:\Work\Nowing\GitHub\InlongSlicer\.agents\runtime\runs\run-cb5f41021b254606a10d628c6f3f75c4\validation\benchy-warnings-native.log`
- vulcan600 G-code: `D:\Work\Nowing\GitHub\InlongSlicer\.agents\runtime\runs\run-cb5f41021b254606a10d628c6f3f75c4\work\vulcan600-fixed\plate_1.gcode`
- Full native log: `D:\Work\Nowing\GitHub\InlongSlicer\.agents\runtime\runs\run-cb5f41021b254606a10d628c6f3f75c4\validation\vulcan600-fixed-native.log`
- Warning/error-only log: `D:\Work\Nowing\GitHub\InlongSlicer\.agents\runtime\runs\run-cb5f41021b254606a10d628c6f3f75c4\validation\vulcan600-warnings-native.log`

Native tests and diagnostic captures are retained under `.agents/runtime/runs/run-cb5f41021b254606a10d628c6f3f75c4`.
