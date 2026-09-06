# Operator Guide

## Everyday Work
Ask for the result in your normal language. Known local fixes need relevant code
and tests, not a repository-wide scan. Use the optional context helper when paths
or cross-module dependencies are unclear. Its output is a starting point, not a
complete impact graph or an automated test exemption.

Run `scripts/validate.ps1 -Scope Provider -Profile Changed -Path <files>` during
iteration. For an installed project use Consumer, with the path from
`.agents/managed.json`. Consumer checks only owned rules and explicitly registered
project tests. Business changes still require their normal product tests.

## Checkpoint and Release
Before commit, push, tag, release or deploy, run the Checkpoint profile. One
invocation uses one check registry and records each check once. A changed input
invalidates prior results; current permissions, remote state and side effects
always need fresh observation. There is no composite quality score.

Commit source, then run `scripts/capture-runtime-evidence.ps1 -OutputPath
docs/evidence/releases/v3.0.0-runtime-evidence.json`. The collector runs the registry
directly, without nesting another validator. Only that declared output is excluded
from the content digest. Commit evidence separately; no unrelated source changes
may be included in the evidence-only commit.

## Knowledge and Recovery
Project facts live in `docs/memory/entries/*.json`, not in a model's recall. Use
the project-memory skill to recall, promote or supersede a verified finding.
The runtime index can always be rebuilt. Changed sources make entries unusable
until reverified. Conflicted entries suspend their scope. Mechanical validation
does not prove a conclusion; a person or agent must check the cited evidence.
Replacing a fact permanently retires its earlier entries. A stale or missing
replacement source does not revive old conclusions; reverify the replacement or
promote a newly reviewed entry. Explicit conflicts still need a fresh resolution.
Unreadable knowledge records suspend recall until repaired, because their affected
scopes and retirement relationships cannot be established safely.

Use task-state checkpoints for long work. Resume checks the actual commit and
file hashes and identifies required reinspection. Completed external actions must
not be replayed just because a prior summary says they were planned. Runtime state
stays local and ignored; durable knowledge stays versioned and project-specific.

## Deployment
The Provider offers two layouts: `root-layout` and `dot-agents-layout`. Deployment
does not edit script source text. Every asset has an explicit destination; scripts
discover their locations through the managed manifest.

Run a dry-run against an explicitly authorized existing target. Inspect conflicts,
creates, updates and removals. Pass `-ExpectedPlanDigest <digest>` to apply that
exact preview. Unowned existing files and modified managed files stop all writes.
Repeated deployment is a no-op. The `.agents/managed.json` manifest is versioned;
keep it with the deployed files. Product code, README, local configuration and
knowledge are never deployable assets.

Rollback with `-TargetPath <target> -Rollback <transaction-id>`. It verifies current
hashes and backup integrity first. Interrupted transactions must be reconciled or
rolled back before new deployment. Never remove a lock while a deployment process
is still active. These guards are behavioral/file ownership controls, not a hard
security sandbox against an adversarial process racing file writes.

## Evidence Boundaries
Static checks establish syntax and contracts. Core offline regression exercises
both layouts, ownership conflicts, deployment rollback, source-checked memory and
task recovery. These run through the same checkpoint registry, not separate nested
validation commands. No model comparison is a release prerequisite. Passing these
checks does not establish model task accuracy, token savings, external project
compatibility or enforced OS isolation. Token usage remains unavailable.

Sources are reviewed when used or released: model/configuration references
every 30 days, stable workflow references every 90. Stale claims need fresh official
documentation; unrelated local edits are not blocked merely by a stale reference.
