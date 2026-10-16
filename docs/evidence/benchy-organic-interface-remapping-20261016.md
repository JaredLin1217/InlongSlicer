# Organic support interface continuity on an independent output grid

## Reproduction

The reported Benchy preview at global layer 886 (Z 146.100 mm) shows the support
printed on global layer 885 (Z 145.845 mm). Layer 886 itself contains model
extrusions. The scattered circles are present in the supplied G-code, so this is
an actual toolpath defect rather than a preview rendering defect.

Inputs were read without modification:

| Input | SHA-256 |
| --- | --- |
| 3DBenchy.3mf | c65b1fcafd39c2c64ed9b906459d4c6ef2b0821b6b938e91fdfb7943bc181ea6 |
| Supplied Benchy G-code | 24083b721568a683788f3b248470e2f318c5402475302023104cb03f0879e09f |

The project uses Organic tree support, 0.300 mm object layers, a 0.255 mm top
distance, four top contact/interface layers, both independent height options,
rectilinear interfaces at 0.400 mm spacing and concentric top contacts at
0.050 mm spacing. The model is scaled by four. No slicing settings were changed
for the full-project comparison.

## Cause and correction

`InterfacePlacer` previously combined lower roof footprints by their topology
layer index. `organic_draw_branches` then mapped those combined layers to the
independent output grid by rounding their old print Z downward. The exact top
contact moved to its requested Z, but its lower interface stack lost its contact
association and depth. A required output plane could contain only branch-tip
sections, while a full interface was emitted too far below the contact.

Lower interface footprints now remain separate, with their source contact and
depth, until the output grid is known. They are moved, not copied, into temporary
placements. Each footprint is published at its contact's output index minus its
roof depth. Truncated stacks without an available parent retain their source-Z
mapping. Synchronized fallback publishes the original topology indices. Contact
anchors, height planning, footprint generation, local tip exceptions and infill
settings retain their existing rules.

## Full-project verification

Both the installed baseline and the corrected Release DLL sliced the original
project successfully. The corrected DLL was linked to a task-owned directory
with project-reference builds and pre/post-build events disabled. This avoided
the portable data-directory reset and left the running user application open.
Each native process was awaited through Python `subprocess.run`.

| Measurement | Baseline | Corrected |
| --- | ---: | ---: |
| Slicing exit code | 0 | 0 |
| Elapsed seconds, single run | 54.984 | 55.828 |
| Global print planes | 1114 | 1114 |
| Interface paths at Z 145.845 mm | 306 | 32 |
| Closed interface paths at that Z | 286 | 11 |
| Interface coverage in the central roof ROI | 8.16% | 63.26% |

The ROI is X 280-340 mm, Y 235-275 mm. Coverage is a raster estimate from actual
extrusion centerlines and emitted widths at 20 pixels/mm, not a mechanical
strength measurement. Rectilinear interface spacing intentionally leaves gaps.

| Z, mm | Baseline interface coverage | Corrected interface coverage |
| --- | ---: | ---: |
| 144.945 | 63.27% | 0.00% |
| 145.245 | 63.26% | 63.27% |
| 145.545 | 63.26% | 63.26% |
| 145.845 | 8.16% | 63.26% |
| 146.145 | 93.97% | 93.85% |

The corrected stack contains three consecutive lower interfaces and one top
contact, as configured. It does not add an interface layer. The formerly misplaced
lowest interface becomes the ordinary support body below the corrected stack.

The complete emitted configuration blocks are byte-identical (normalized UTF-8
SHA-256 `dbfe95ccd85246acf8268d8e6ded5f7d5ad13bd3ae49c76461d66d5c1ddcd704`).
Canonical positive-extrusion motion records for every non-support role are
identical, including coordinates, arc centers, extrusion amounts, widths and
heights. Only support and support-interface motion records change.

The top contact print plane remains Z 146.145 mm. The sliced roof underside is
Z 146.400 mm, retaining the configured nominal 0.255 mm clearance. The original
mesh's downward roof faces are at approximately Z 146.455994 mm, giving 0.310994 mm
to the contact plane; the difference comes from object-layer discretization.
The first bridge prints at Z 146.700 mm with emitted height 0.315 mm, giving a
nominal deposited-bead envelope gap of 0.240 mm. These are distinct measurements;
none indicates an added 0.300 mm skipped layer. Actual printed clearance depends
on the deposited bead shape and has not been measured physically.

## Native regression scope

The new regression measures both selected roof-footprint continuity and actual
extrusion coverage on every required interface/contact plane. It covers
independent-body on/off, with 0.200 and 0.255 mm top distances. Its reference is
the footprint selected by the overhang detector, not the whole underside, which
need not all require support.

The affected native support, tree, raft, belt and support-only reslice cases passed
together in randomized order with seed 20261016: **83 test cases and 18,021
assertions**, exit code 0, elapsed 434.784 seconds. Results and checkpoint receipts
are recorded with the task artifacts. This includes the newly added roof-stack
regression and the existing gap, height-limit and synchronized-fallback cases.

## Remaining observations and delivery boundary

Both native slices emit the same thirty potential lost-branch warnings at topology
layer 301 below parent layer 302 (Z 90.6/90.9 mm on this grid). The initial summary
under-counted these warnings; the follow-up reviewed the complete logs. The failed
influence-area expansion uses a 1.0 mm branch radius and model-resting candidates.
This happens before output-grid remapping and is unchanged by the correction.
It is separate from the reproduced Z 145.845 mm interface omission. The warning
does not by itself establish the final printed condition of those local branches;
the entire part has not been physically qualified for printing. The subsequent
[branch-continuation investigation](benchy-organic-branch-continuation-20261016.md)
corrects the candidate classification and removes these warnings on the original
project; its verification is separate from the measurements above.

The running user executable and installed application were not replaced. This
change requires a subsequent normal application build/install to appear in that
window. No commit, push or installer publication was performed for this task.
One root agent performed the work; no worker agents were used.

## Evidence location

Task artifacts are registered under
`.agents/runtime/runs/run-63d75248616d4ec09af26466d3c62ece/`:

- `validation/baseline-sync-slice-result.json`
- `validation/roof-fix-slice-result.json`
- `validation/motion-and-coverage.json`
- `validation/config-comparison.json`
- `validation/roof-gap.json`
- `validation/benchy-interface-before-after.png`
- `validation/support-regressions.log`
- `validation/support-regressions-result.json`
- `work/roof-fix/plate_1.gcode`

The source and this report preserve the reusable conclusion after temporary
diagnostics expire. Native timing is a single comparison, not a performance claim.
