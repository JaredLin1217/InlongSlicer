# Organic contact tips and sloped model landings

## Reproduction and diagnosis

The original Benchy 3MF from the interface-remapping investigation also generates
failed branch-expansion warnings near Z 90 mm. Complete native logs contain
**30 failed candidates**, rather than the ten stated in the earlier report's
initial summary. The source topology parent index is 302 (Z 90.9 mm on this
0.3 mm grid); the attempted child index is 301 (Z 90.6 mm).

An opt-in diagnostic build recorded each failed target and its ordinary collision,
local contact collision, avoidance and model-placement polygons. All 30 targets
are outside ordinary collision polygons on both the parent and child planes,
but inside the avoidance polygons that require a flat model landing. None is in
the flat placeable area at the parent plane. Thus they fit the configured normal
clearances but have not established a route to a flat landing.

`convert_contact_lines_to_internal` returns `CONTACT_TO_MODEL` after a local
collision test. `RichInterfacePlacer::add_point_as_influence_area` incorrectly
treated this status as proof of a flat, or "gracious", landing. Subsequent branch
propagation therefore insisted on flat-landing avoidance, failed all expansion
attempts, and removed the unsupported tip.

## Correction

Only avoidance-classified `TO_MODEL_GRACIOUS`, `TO_MODEL_GRACIOUS_SAFE` and
build-plate candidates set the gracious flag. `CONTACT_TO_MODEL` now uses the
existing non-gracious model-resting branch path, which clips against ordinary
collision polygons and can land on an inclined model surface. A later merge may
promote a branch to gracious only when the existing avoidance calculation proves
that placement is available.

No clearance parameter, global collision mask, warning filter or output-path
filter was relaxed. Temporary diagnostic code was removed after recording the
counterexample. The earlier contact-relative roof-stack mapping remains intact.

## Behavioral regression

The new test puts a suspended ceiling above a pyramid. A branch below the ceiling
can fit ordinary collision constraints while having no sufficiently wide flat
landing on the pyramid. The old implementation fails this fixture with the same
influence-area expansion warning. The test also requires actual support between
the pyramid and ceiling and checks that every emitted branch island in the air
below the interface stack overlaps material on the preceding output plane.

Scoped logging is limited to the disposable native test process and restored by
RAII. The regression checks the resulting slicing behavior, not a direct copy of
the gracious-flag expression.

## Original-project verification

The supplied 3MF was sliced with an isolated Release DLL. The native process was
awaited, exited with code 0, and completed in 43.000 seconds in the measured run.
The emitted configuration block is identical to the preceding corrected-roof
baseline. Failed branch-expansion warnings decreased from 30 to zero, with no
new uninitialized-element or invalid-placement warnings.

The complete model extrusion geometry is identical, including arc centers,
extrusion amounts, widths and heights. Support, skirt and brim records change;
the latter two follow the resulting support footprint. There are still 1114
global print planes. The repaired central roof keeps its three lower interfaces
at Z 145.245, 145.545 and 145.845 mm, each with 63.26% rasterized bead coverage,
and its dense contact at Z 146.145 mm. The preceding interface omission does not
return.

Actual G-code beside the failed targets has additional branch sections on
successive output planes. In the examined X 346-353, Y 225-281 mm region, paths
intersecting the window at Z 90.645 mm increase from 11 to 19; their full path
length increases from 63.45 to 112.99 mm. These are emitted material paths, not
just changed log messages. See `validation/branch-paths-before-after.png` and
`validation/branch-region-paths.json`; the length counts entire selected paths,
not only the portions clipped to the window.

The affected native support, tree, raft, belt and support-only reslice suite passed
**84 test cases and 18,107 assertions**, including the new sloped-landing test, in
randomized order with seed 20261016. Native results and Consumer checkpoint
receipts are preserved in the registered task artifacts. This is software and
G-code validation; an actual printed sample was not measured.

## Evidence and delivery

The registered run is
`.agents/runtime/runs/run-25e0017a1dd046f6b25e541fdf44de2d/`:

- `work/branches/*.json`: the original failing target polygons.
- `validation/branch-targets.json`: collision and avoidance membership.
- `validation/branch-red-result.json`: the original-algorithm regression failure.
- `validation/fixed-slice-result.json`: successful original-project slicing.
- `validation/full-slice-branch-result.json`: warning and configuration checks.
- `validation/motion-and-coverage.json`: model motion and roof-stack verification.
- `validation/branch-paths-before-after.png`: emitted branches on consecutive planes.
- `validation/branch-fixed-regressions-result.json`: affected native results.
- `work/fixed/plate_1.gcode`: the corrected output.

The source tree retains the preceding uncommitted roof correction. No commit,
push or installed-application replacement is part of this follow-up. One root
agent performed the work; no worker agents were used.
