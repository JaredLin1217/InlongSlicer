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

Do not claim above the captured tier. v2.9 defaults to T2 for this repository; external pilot and enforced isolation remain unclaimed.

## Validate By Change Risk

- Routine source or prose change: run `.\scripts\validate-changes.ps1 -Profile Auto -ContextMode Auto -Explain`; expected profile is `Fast` when context evidence has no wider-risk recommendation.
- Canonical policy, instruction, skill, template, or memory change: expected profile is `Policy`.
- Schema, version, release evidence, CI, deploy, or validator change: expected profile is `Full`.
- Commit, push, deployment, or release claim: use the profile required by `.agents/docs/agents/verify.yaml`; release evidence requires `-ContextMode Required`.

Auto-selection minimizes repeated work; it does not weaken the evidence needed for a broader claim.

## Recall Durable Knowledge

1. Read `.agents/docs/memory/index.md`, not every memory entry.
2. Filter by route, status, `next_review`, source commit, content hash, and boundary.
3. Load at most three relevant details within the context byte budget.
4. Verify remembered facts against current repository or tool evidence before acting.
5. Update, supersede, or retire stale and conflicting memory after durable facts change.

Checked-in memory supports recall. Canonical YAML and current evidence remain authoritative.

## Keep Context Small

- Keep stable instructions before dynamic task state so exact prefixes remain reusable.
- Expand only the route files named by `.agents/docs/agents/ai-runtime.yaml`.
- Use `.\scripts\resolve-agent-context.ps1 -Task <text>` for repository work; consume its file and line pointers before broader scans.
- Treat provenance, confidence, hashes, and freshness as evidence quality. Heuristic links never justify a change or skipped validation by themselves.
- Dirty, stale, degraded, conflicting, unsupported, and parse-failure states require broader inspection and validation.
- Summarize long tool output to the evidence needed for the next decision.
- Delegate only when parallel benefit exceeds context, coordination, and integration cost.
- Compact at task boundaries or context pressure; preserve decisions, evidence, risks, and the next executable step.

## Clean Closeout

- State what changed and what was verified.
- Report any command that could not run.
- Keep runtime logs and raw outputs in status scratch, not in the release package.
- Include GM, GS, XR, and XW in the isolation line.
- Leave the working tree with only intentional files before commit.

## Practice Evidence

Use practice evidence before release when workflow behavior, deployment boundaries, context selection, or validation behavior changed. The evidence must bind the source commit, content digest, working tree state, command durations, A/B metrics, fallback results, scope, and boundary statement.

Practice evidence is not proof of external target deployment or enforced isolation.
