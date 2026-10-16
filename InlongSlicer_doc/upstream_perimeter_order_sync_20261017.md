# Upstream perimeter order integration - 2026-10-17

This task integrates two upstream commits. The statistics fix c6804fbb68 was separately verified and committed locally as 221f64e305. A final remote observation found the additional 0acf1e6680 perimeter fix, which is the selected second fetched snapshot for this integration. Upstream uses main and has no master.

The perimeter update fixes Classic and Arachne wall order/direction on narrow tubes, shared odd centre lines and thin holes. Explicit outer, outer-and-inner and belt leading-edge brims put outer walls first only on layer zero when width is positive. Changing brim type or width invalidates perimeter generation when that outer-first decision changes. Auto, ear and painted brims retain the configured wall order.

Three overlapping comment/condition hunks in PerimeterGenerator.cpp were resolved by preserving Inlong identity and importing the upstream narrow-hole condition. The belt leading-edge case remains in the shared brim helper. No model, quality, material, profile or version-label override was applied.

## Validation

- Perimeter and selected belt/support regressions passed 23 cases and 6,319 assertions. JUnit confirms all seven new wall-order/direction/brim fixtures and both preserved belt/support fixtures ran.
- The combined candidate reran the statistics regression: 1 case and 138 assertions passed. The earlier complete statistics/export/processor/timing sweep passed 81 cases and 6,471 assertions; its implementation/test hashes remain unchanged.
- Focused Windows Release core/FFF builds and disposable application DLL linking passed. The previous GUI archive was reused: public header additions preserve existing enum values/class layout and only add nonvirtual/free functions. No new GUI-library rebuild or manual GUI verification is claimed.
- One imported line had mixed space/tab indentation. It was corrected after compilation; the persisted one-line byte comparison verifies that only leading whitespace changed, with identical C++ tokens. Native behavior checks are reused for this formatting-only cleanup.
- Required filename/content scans and the palette guard passed. Existing provenance, plugin ABI, protocol, legacy producer/import and dependency references remain classified.

| Project | Total seconds | Ordered commands | Complete commands / motion / configuration |
| --- | ---: | ---: | --- |
| benchy | 22.724 | 811,657 | Identical |
| vulcan600 | 32.333 | 1,211,052 | Identical |
| x2 | 58.889 | 1,348,868 | Identical |

The native inputs were hash-checked and unchanged. Every process exited zero and exported one fresh plate (use_cache=0). Emitted configurations match the statistics candidate. The previous Voronoi/MedialAxis/support-branch failure strings are absent; each log still has five known initialization/compatibility warning events. Timings overlapped regression work and are not a controlled performance benchmark.

All three current portable data directories have exactly the same file sets and hashes as at task start. All six installed EXE/DLL hashes and all five protected support/X2 source hashes are unchanged. No install, reset or remote push target was invoked. One agent executed six native slices across both candidates.

The first Consumer Checkpoint exposed a stale support-knowledge hash because the unrelated statistics fixture changed test_print.cpp. The existing support fixtures and implementation were reviewed, the immutable record was migrated, and the repaired checkpoint passed before committing 221f64e305. Earlier performance timings remain historical observations.

Machine-readable evidence: `docs/evidence/upstream-perimeter-order-verification-20261017.json`.
Statistics component: `docs/evidence/upstream-gcode-statistics-verification-20261017.json`.
Raw native/build/JUnit evidence: `.agents/runtime/runs/run-81cf153b81114b5398a62074617cb7b4/validation`.
Source hashes identify validated worktree bytes; checkout anchors do not claim pending bytes were already committed. Final local merge identity and Consumer receipts are recorded in task completion evidence. No Provider release-readiness, manual GUI or cross-platform claim is made.
