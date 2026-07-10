# Multi-Agent Workflow
Use only after an explicit hire, spawn, delegate, parallel-agent, scoring, or
equivalent request. Canonical details live in `.agents/docs/agents/workflows.yaml`.
## Fast Path
1. Run one controller status check when repo state or edits matter.
2. Create one reusable brief per role type.
3. Fill current runtime slots, queue the rest, and refill as employees finish.
4. Do not re-read every canonical file for each employee.
5. Record recovery-sensitive lifecycle events in `.agents/runtime/agent-ledger.jsonl`.
6. Capture final reports, close completed employees, then summarize compactly.
## Dispatch
Before launch, assign role, task, read scope, write scope, allowed commands,
report schema, and close condition. Normalize write scopes and block overlaps.
Use read-only explorers for scoring/review/investigation; use workers only with
exclusive normalized write scope.
During work, poll only when controller progress depends on a result or an owned
scope may be affected. Record each final report once, dedupe by report hash for
scoring batches, and stop expansion at convergence or cap.
After work, close runtimes after report capture. When the user explicitly asks
to dismiss employees or clean up the roster, run authorized Codex App
sidebar/history cleanup and zero-hit verification before claiming the roster is
clean. Reconcile git status and owned scopes, and keep `.agents/runtime/**`,
temp roster, status, and filled validation records unstaged and undeployed.
## Batch Validation
For smoke/load batches, precompute the expected id set and compact ack schema
before launch. Capture requested, spawned, completed, and closed counts; then
normalize received ids once and separate missing, duplicate, invalid-format,
wrong-id, failed, running, and unclosed results. Report protocol success
separately from deploy, edit, or test success, and do not claim closed employees
without close results or official runtime status.
## Close And Cleanup
Runtime close comes first; DB deletion is never a substitute. Sidebar/history
cleanup is destructive and no-backup; it requires explicit cleanup wording for
exact runtime ids or standalone exact cleanup authorization.
Prefer official controls first: runtime close, `thread/list` by cwd/source kind,
`thread/loaded/list`, archive, and unsubscribe when available. Do not assume an
official hard-delete thread API or sidebar refresh API.
If authorized cleanup is needed, match only current-project closed subagent
rows/files after normalizing Windows paths. Do not create backup copies; record
pre-delete counts. Use `scripts/agents-cleanup.ps1 -Action Verify` to capture
current residue and `-Action Cleanup -Force` only after exact authorization. In
`%USERPROFILE%/.codex/state_<n>.sqlite`, delete matching
child `thread_spawn_edges` before matching child `threads`; preserve the
parent/controller/user thread. Remove matching rows from
`%USERPROFILE%/.codex/session_index.jsonl`. Clear matching unread ids from
`%USERPROFILE%/.codex/.codex-global-state*.json` and matching child rollout
files under `%USERPROFILE%/.codex/sessions` and `archived_sessions`. Because the
app can rewrite unread state once, repeat unread cleanup if it reappears, then
run delayed zero-hit verification. Treat these as external runtime state; report
reads/writes/deletes as XR/XW. Never delete parent/controller/user threads,
unrelated rollout files, backup/copy DB files, or unrelated project history.
A clean roster claim requires runtime close/inactive proof, official-list zero,
sqlite edge/thread zero, session_index zero, global unread zero, rollout zero,
and delayed zero verification. Cleanup targets exact runtime ids, never sidebar
nicknames. If sidebar residue remains after those proofs, reload/restart Codex
UI before any cache-cleanup claim. Shutdown/cache cleanup needs explicit
authorization.
## Scoring Batches
Default to 3 read-only scorers. Expand to 5 only if score spread exceeds 10
points or top issues conflict; hard cap at 7, or 10 with explicit approval.
Before launching more than 5, state the unresolved disagreement or coverage gap.
Aggregate median-first with mean/range/confidence, dedupe findings by root
cause, and report disagreement instead of spawning beyond cap.
## Deployment Workers
Use one `deployment_worker` per exact target path. Assignment must name target
path, mode, dry-run/write scope, and deployed file set boundary. Worker may
inspect target layout and run the deployment script, but must not edit target app
code, copy provider runtime state, repair OS permissions, or modify `.git`
metadata. Controller reviews the dry-run/write report before claiming completion.
References: `.agents/docs/agents/workflows.yaml`, `.agents/docs/agents/schemas.yaml`,
`.agents/docs/agents/verify.yaml`.
