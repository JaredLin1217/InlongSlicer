# Benchy slicing time and exact-path preservation

## Result and scope

The latest support-corrected Release takes about **44 seconds** to read the
supplied project, perform a fresh native slice, and finish exporting G-code on
the measured machine. Three baseline trials took 46.471, 43.542 and 40.554
seconds; their median is **43.542 seconds**.

A small candidate removes repeated contact-collision construction and polygon
copies. Its three trials took 46.409, 39.639 and 42.654 seconds, with a median of
**42.654 seconds**. The observed median reduction is only **2.04%**, with strongly
overlapping ranges. This is not evidence of a stable, substantial end-to-end
speedup, and it does not meet a target of halving slicing time. Sampled process
CPU time decreased from a median 300.641 to 292.516 CPU seconds, about 2.70%.

The candidate preserves every ordered printer command and motion annotation
from the latest baseline. No layer height, model geometry, material parameter or
quality setting was changed. The installed application was not replaced.

## Path baseline selected by the user

The attached reference G-code matches the support algorithm before the two
recent roof-stack and inclined-landing fixes: all **815,669 commands** match a
fresh slice with the older installed DLL. Its raw file SHA-256 is
`0e5952ad879146cc5aef86cf483a96d803b5ef5e9da1ea24cdf9bbf5ab63cd89`.

The user explicitly selected the **latest corrected version** as the path
baseline for this performance task. That baseline contains **811,657 commands**.
Its source checkout is `a53b59db336d3e87f0cd55206bf49ffe3e81a9f0`; the two support
corrections remain present. The supplied 3MF was read unchanged, with SHA-256
`c65b1fcafd39c2c64ed9b906459d4c6ef2b0821b6b938e91fdfb7943bc181ea6`.

The model occupies approximately 240 x 124 x 192 mm and the output contains 1114
global print planes. This is a large scaled Benchy, with independently placed
support planes in addition to the object layers.

Comparison includes the complete ordered command stream after removing comments
and normalizing whitespace. Numeric tokens are not rounded or compared with a
tolerance: travel, extrusion, speed, arc, extrusion-mode and non-motion commands
must all match. A second signature includes motion commands with their role,
width and height annotations. All three candidate outputs match both signatures
and the complete emitted configuration map. Differences in dates and GUI
thumbnail comments are outside printer-path equivalence.

The older reference versus CLI comparison has five metadata differences in its
configuration comments, including material-slot availability metadata. Those
differences do not alter its exact printer command stream. They are not present
between the latest baseline and candidate slices.

## Measurement

Hardware: Intel Core Ultra 7 255H, 16 logical processors, 31.43 GiB usable physical
RAM. The runs use the same project and executable loader, with separate disposable
data/output directories and Release DLLs. Each native process is awaited through
completion; GUI executable launch-return time is not used as slicing time.

| Variant | Trial 1 | Trial 2 | Trial 3 | Median | Range |
| --- | ---: | ---: | ---: | ---: | --- |
| Latest baseline | 46.471 s | 43.542 s | 40.554 s | 43.542 s | 40.554-46.471 s |
| Contact reuse candidate | 46.409 s | 39.639 s | 42.654 s | 42.654 s | 39.639-46.409 s |

The second and third trial pairs were interleaved baseline/candidate. The first
pair was separated by diagnostic work and compilation. No measured native slice
ran concurrently with a build or native regression suite. G-code comparison,
test-runtime copying and compilation are excluded from each reported slice time.
CPU/memory are sampled every 0.2 seconds, so CPU totals and peak observations are
sampled measurements. Power state, host background activity and CPU frequency
were not controlled; these are exploratory results on one machine, not a
statistical speedup guarantee. GUI preview rendering is not included.

The baseline maximum peak working set was 1.589 GiB and the candidate maximum
was 1.569 GiB. The corresponding maximum private-commit peaks were 1.664 and
1.654 GiB. Sampled available system RAM remained above 14.4 GiB. These results
do not show physical-memory capacity pressure.

## Where time is spent

The following are medians from the slicer's existing native stage timers.
Independently computed medians need not sum to a median total.

| Stage | Baseline | Candidate |
| --- | ---: | ---: |
| Overhang detection and avoidance preparation | 2.245 s | 2.103 s |
| Initial contact and tip sampling | 7.092 s | 5.008 s |
| Downward influence propagation | 1.894 s | 2.057 s |
| Node placement | 0.035 s | 0.044 s |
| Branch geometry, output grid and interface assembly | 8.432 s | 9.508 s |
| Support-layer assembly and extrusion-path generation | 7.627 s | 8.036 s |
| Entire organic support stage | 27.152 s | 26.308 s |

Support occupies approximately 62% of the baseline end-to-end time. Other model
processing, loading and G-code export account for the remaining time; they were
not individually timed by this diagnostic. The candidate's smaller initial
sampling cost does not translate into a similarly large whole-pipeline gain.

A separate diagnostic build divided initial sampling into four phases. Summed
elapsed time across parallel layer workers was: contact footprints 1.340 s,
regular-region expansion 2.042 s, roof-component processing 26.065 s and
regular-component processing 32.595 s, with 598 roof and 1673 regular components.
These worker intervals overlap: **they must not be added to wall-clock time**.
They identify component geometry and line generation as the main remaining
sampling work. The instrumented run took 36.778 s and also matched every command;
that single faster run is not included in the primary candidate median or used
to claim another improvement. Its temporary instrumentation has been removed.

## Source change

`convert_contact_lines_to_internal` now builds one collision polygon for its
fixed radius, layer and local clearance settings rather than rebuilding it for
every sampled point. Next-layer evaluation lazily retains one contact polygon
per caller's layer. Its ordinary avoidance/collision alternatives use references
instead of an implicit polygon copy caused by a value-returning conditional arm.

The retained source SHA-256 is
`cd46d7ede5aa154447ac1907f3a0fc3922f2cd3f86de754f49be2716bff1d86f`.
Storage lasts only for the current batch/layer. There is no persistent global
cache or fixed multi-gigabyte allocation, and `SupportCacheBudget` and its
low-memory fallback policy are unchanged. This is not a guarantee that the
mandatory working set of this large model fits a 2 GiB computer.

**84 affected native test cases and 18,107 assertions passed**, including
the roof-continuity, sloped-landing, normal support, raft, belt and support-only
reslice coverage. Randomized order used seed 20261016. Exit code was 0. The
retained source exactly matches the benchmarked candidate bytes after removing
the temporary timing instrumentation.

## Further optimization priorities

1. Profile the component work inside `sample_overhang_area` and support infill
   generation. Check repeated work with identical footprints and fully identical
   fill parameters before adding bounded reuse. Reuse must keep the current
   candidate publication order and local contact rules.
2. Inspect repeated shared-reach copying and footprint clipping across components.
   Reusing a proven identical value can avoid work without changing geometric
   operators, their numeric inputs or path ordering.
3. Separately address branch/output/interface assembly and support extrusion
   generation. They each still take approximately 8 seconds; do not assume that
   increasing the RAM allowance will remove these CPU tasks.
4. Add phase timing to the remaining model and export work before assigning a
   predicted gain to it. The present measurements do not justify a twofold
   end-to-end speedup claim.

At the observed baseline scale, eliminating the entire initial sampling stage
would save only about 16% of total time if everything else stayed constant.
Halving total time using support work alone would require reducing the support
stage by roughly 80%. These are approximate bounds from the measured times,
not promised improvements. A meaningful halving requires broader work across
several stages while maintaining exact-path regression checks.

## Evidence and delivery

Artifacts are registered in
`.agents/runtime/runs/run-1b286a6590ca4fdaa79af9844420f09c/`:

- `validation/inputs.json`: input hashes and host snapshot.
- `validation/baseline-*-result.json` and `optimized-*-result.json`: awaited runs.
- `validation/*-comparison.json`: complete command and annotated-motion checks.
- `validation/performance-summary.json`: per-trial costs, medians and memory.
- `validation/sampling-profile-native.log`: temporary component-stage diagnostic.
- `validation/candidate-regressions-result.json`: affected native regressions.
- `work/optimized-3/plate_1.gcode`: candidate output matching the latest baseline.

The candidate Release DLL SHA-256 is
`858ff538cc66521c133148473cd3b4f265fa1d83df833bf33d99e3783a95f779`;
the baseline DLL SHA-256 is
`040476e09b065225614deed5ce4e3f093edcb659059cc15f28b570e0e993f2c6`.
The native test executable was preserved before temporary profiling compilation,
so the final regression run uses the same retained source as the measured
candidate. One root agent performed this task; no worker agents were used.
No installation, commit or push was performed as part of this performance task.
