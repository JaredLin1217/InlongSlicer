# Upstream G-code statistics integration - 2026-10-17

Merged upstream main `c6804fbb6836493c71d3669e0404ddb85e7d2f6e` into the local project based on `b186cf39341272ee53c39184a90f09a6abb4979b`. Upstream has no master branch. One incoming commit touches four files and merged without conflicts. No installation or remote push was requested or performed.

The change makes used_filament, print_time, normal_print_time, silent_print_time, total_weight, total_cost, total_wipe_tower_cost and total_wipe_tower_filament available in custom G-code. Reserved tags are replaced after complete export, when final statistics are known. Running extruded_* numeric totals retain their previous behavior. Inlong identity and the 2.5.0-dev label remain unchanged.

## Verification

- Windows Release core and fff_print_tests built successfully. The focused Print/GCodeProcessor/GCodeTiming/GCodeStatistics suite passed 81 cases and 6,471 assertions. JUnit confirms execution of the new final-statistics regression; its six generated combinations cover file/start/end fields and both Bambu and non-Bambu modes.
- A new application DLL was linked in the registered disposable runtime with project references and pre/post-build events disabled. The previous GUI archive was reused: the only header change appends enum members and preserves existing values and class layout. No GUI source changed. No new GUI-library rebuild or manual interactive verification is claimed.
- Branding filename/content scans and the color guard passed. Existing historical, service/protocol, plugin-ABI, producer-detection and dependency references remain classified; no new Orca identity was introduced.

| Project | Total seconds | Ordered commands | Latest fixed baseline |
| --- | ---: | ---: | --- |
| benchy | 23.867 | 811,657 | Complete commands, annotated motion and emitted configuration identical |
| vulcan600 | 33.005 | 1,211,052 | Complete commands, annotated motion and emitted configuration identical |
| x2 | 64.989 | 1,348,868 | Complete commands, annotated motion and emitted configuration identical |

Each supplied input was hash-checked before execution and remained unchanged. Every process exited zero, exported one fresh plate, and logged use_cache=0 and one object requiring slicing. All three retained five known initialization/compatibility warning events, with no prior Voronoi, MedialAxis or support-branch failure strings. Raw metadata such as export timestamps is excluded from the command/path comparison. Timings overlapped native regression work and are not performance benchmarks.

## Preservation and limits

- Exact file sets and hashes of all three current portable data directories are unchanged. All six installed EXE/DLL hashes are unchanged. No portable reset/install target or installed GUI was invoked.
- All five protected support/X2 source and regression files retain their previous hashes; no profile source changed. One agent performed the work.
- Existing compiler/dependency warnings remain. Passing negative test fixtures can emit error diagnostics. macOS/Linux and interactive GUI behavior are unverified. Consumer checks do not establish Provider publication readiness.

Machine-readable evidence: `docs/evidence/upstream-gcode-statistics-verification-20261017.json`.
Raw logs: `.agents/runtime/runs/run-81cf153b81114b5398a62074617cb7b4/validation`.
The source hashes bind the validated worktree bytes. The checkout anchor is pre-merge; final local commit and Consumer receipt references are stored in runtime task completion evidence.
