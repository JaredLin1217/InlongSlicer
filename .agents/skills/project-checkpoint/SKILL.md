---
name: project-checkpoint
description: Validate affected work, persist checkpoint evidence, or safely deploy and roll back owned AI Agents files.
---

# Project checkpoint

Use validate.ps1: Provider for this distribution, Consumer for installed rules.
Changed checks affected files; Checkpoint checks the complete owned boundary once.
Run warranted product tests through registered project wrappers. A saved receipt
precedes output and reports passed, failed, needs_review or not_applicable.
Check passed separately from release_ready: missing/stale release evidence remains
needs_review. RequireReleaseReady fails the publication gate. No composite score,
model-accuracy guarantee or inferred test exemption is provided.

Capture official evidence only from clean committed source using the collector;
verify its schema and content digest after generation. Commit only the declared
evidence path in the evidence commit. Read the operator guide for that sequence.
Current remote state, permissions and side effects always need fresh checks.
Do not nest another Provider validator or collector inside the regression suite.

Deploy from a validated Provider to an authorized existing target. DryRun returns
an exact digest without state writes. Review conflicts, operations and ownership;
apply with ExpectedPlanDigest. Both layouts retain explicit ownership. Modified
managed files or unowned existing files stop writes; never force adoption. Consumer
business code, README, knowledge and local Codex settings are outside deployment.

Rollback uses the transaction ID and validates current hashes and original bytes
before writes. Journals/backups are registered and pinned in the target runtime.
Interrupted transactions block new deployment, including no-ops. Reconcile or
roll back before retirement. Keep rollback dependencies until explicitly retired
with a reviewed reason. Read project-runtime when cleaning or recovering locks.
