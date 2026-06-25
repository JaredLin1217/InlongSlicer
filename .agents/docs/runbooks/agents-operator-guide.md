# Agents Operator Guide

Use this guide to choose the lightest workflow that proves the claim you need.

## Choose The Lightest Flow

- Answer-only: no repository state claim, no command output, and no durable change.
- Read-only audit: inspect only the assigned files and report findings with the isolation line.
- Edit or maintenance work: inspect current state first, make scoped changes, then run the smallest verification profile that proves the claim.
- Deployment work: require an exact target path and explicit write scope; run dry-run before any target write.
- Release work: capture runtime evidence, run full validation, check whitespace, and commit only expected files.

## Evidence Tiers

- T0 static: rules, schemas, templates, and validation pass.
- T1 dry-run: static proof plus no-write deployment or workflow dry-run evidence.
- T2 current-repo practice: static proof plus repeatable practice evidence in this repository.
- T3 external pilot: authorized external target evidence exists and is reviewed.
- T4 enforced isolation: current runtime, OS, account, or cloud controls prove enforcement.

Do not claim above the captured tier. v2.7 defaults to T2.

## Clean Closeout

- State what changed and what was verified.
- Report any command that could not run.
- Keep runtime logs and raw outputs in status scratch, not in the release package.
- Include GM, GS, XR, and XW in the isolation line.
- Leave the working tree with only intentional files before commit.

## Practice Evidence

Use practice evidence before release when workflow behavior, deployment boundaries, or validation behavior changed. The evidence should bind the source commit, content digest, working tree state, command durations, results, scope, and boundary statement.

Practice evidence is not proof of external target deployment or enforced isolation.
