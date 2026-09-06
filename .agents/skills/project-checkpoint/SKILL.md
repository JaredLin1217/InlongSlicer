---
name: project-checkpoint
description: Validate changes, prepare evidence, or safely deploy and roll back owned AI Agents files.
---

Use the unified validator: Provider for this distribution, Consumer for installed
rules. Changed checks affected files; Checkpoint checks the owned boundary once.
Run product tests when warranted even if not registered here. Passing tests do
not guarantee all future tasks.

Deploy from a validated Provider using `deploy-agents-workflow.ps1 -TargetPath
<authorized path> -LayoutProfile root-layout|dot-agents-layout -DryRun`. Review
the plan; pass its digest as `-ExpectedPlanDigest` on the write call. Conflicts
stop before writes. Never adopt arbitrary existing files as owned. Only reviewed
ownership with known original hashes permits deletion.

Backups and journals stay in ignored target runtime state. Rollback uses the exact
transaction ID and verifies current owned hashes before restoring. Interrupted
journals block deployment until reconciled. Memory, local configuration and product
files are not deployment assets.

Capture release evidence from committed source. Do not nest validation inside
tests or the collector. Evidence-only commits may touch only the declared evidence
path. Current permissions and remote state always require fresh checks.
