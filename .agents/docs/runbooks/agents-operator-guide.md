# Operator guide

## Validation and publication

Inspect relevant files and Git before edits. Provider validates this distribution;
Consumer validates the installed ownership map and registered project wrappers.
Locate Consumer scripts by source in `.agents/managed.json`, rather than assuming
one layout. Product changes still need appropriate product checks.

Use Changed during iteration and one Checkpoint for the delivery boundary. Narrow
Path narrows the claim; it does not prove complete impact. Receipts are saved under
registered runtime runs before results are printed. `passed` means no required
check failed; `result=needs_review` identifies outstanding advisory review and
`release_ready` is a separate publication gate. Expired official references need
fresh reads before their review dates change. Current remote state, authority and
external side effects cannot be reused from prior receipts.

Publication sequence:

1. Run Provider Checkpoint, review individual results and commit source.
2. From clean committed source run `scripts/capture-runtime-evidence.ps1`. The
   collector uses the same registry without nesting another validator. Its own
   evidence check is not_applicable during capture; all source checks must pass.
3. Generation rechecks source stability, validates schema and binds content to
   the commit, including only the declared evidence-file exclusion.
4. Commit only `docs/evidence/releases/v4.0.0-runtime-evidence.json` as evidence.
5. Run Checkpoint -RequireReleaseReady before publication and freshly verify its
   actual target/remote permissions. Evidence freshness is 30 days, independent
   of retaining the versioned file. Missing/stale/mismatched evidence never passes
   that gate. Offline success does not measure model task accuracy or token savings.

## Knowledge

Use `project-memory.ps1 -Action Recall` before relying on a past finding. Query is
a string array; Module, Tag, File and Scope narrow it. Expand returns source and
verification details. Index rebuilds an advisory cache from records.
Recall checks file hashes and review deadlines by default; official knowledge is
an as-of snapshot. Add VerifyOfficial for a live content comparison before making
current OpenAI claims. A network failure suppresses that finding visibly.

Prepare a reviewed v4 proposal using `docs/templates/knowledge-entry.json` inside
a registered draft artifact. Fill actual hashes, locators, verification and UTC
time. `Promote -Automatic` saves only reusable nonsensitive verified knowledge.
The reviewer must check semantic support; JSON validation alone is insufficient.
Official sources use an allowed HTTPS URL, durable summary snapshot, remote content
hash and review deadline. Automatic promotion adds VerifyOfficial for a live check.
User decisions use an explicit recorded project decision with locator/hash, never
an implied authorization from memory. File commit fields are checkout anchors;
the file SHA-256 proves the reviewed bytes, including an uncommitted snapshot.

Never overwrite an entry. Migrate publishes a reviewed v4 replacement with legacy
IDs in supersedes. Retract publishes a new retracted record with retracts IDs.
Drift/expiry suppresses active facts. Conflicts and duplicate active keys pause
recall until explicit resolution. Retirement survives stale or unreadable
replacements. Invalid JSON suspends recall because its retirement scope is unknown.
Required rules belong in versioned guidance; native generated memories are optional
recall help and must not be edited by Agents. No automatic knowledge sharing.

## Task continuation

Copy task-input into a registered draft and fill the latest authorized objective,
adjustment, acceptance criteria, boundaries, completed work, issues, next steps,
knowledge IDs, persisted receipt references and external-action observations.
Save with InputPath and ExpectedRevision 0 for a new task, then the exact current
revision for updates. Parallel or interrupted overwrites stop. Requirement changes
append history. Each checkpoint revision is immutable; its pointer is atomically
updated. Completed external-action records cannot be rewritten or replayed.
An interrupted pointer is not silently overwritten. Inspect the candidate immutable
revision, then use task Reconcile with its InputPath, Id, ExpectedRevision,
ExpectedDigest and a reviewed Reason. Recovery restores only the pointer and
reports actual state drift; retired v4 tasks never fall back to an old v3 snapshot.

Resume with an explicit Id. Without Id, multiple active tasks are listed rather
than chosen by time. Inspect HEAD, index digest, worktree status, boundary hashes,
receipt hashes and knowledge freshness before action. List does not resume work.
Complete requires the latest revision and reviewed input, closes the task and
starts 90 days. Registered receipt dependencies survive their shorter 30-day
window until all dependent tasks are removed. v3 state remains read-only until
explicit Migrate; it is retained as migration provenance.
Migration atomically archives the exact v3 bytes inside the task's registered run,
then removes only the hash-matching old copy after the v4 pointer is saved. Original
location/hash stay in the lineage; the archive follows that task's 90-day lifecycle.

## Runtime inventory and cleanup

Payloads use `runs/<run-id>`; `state` holds pointers, indexes, locks and atomic-write
staging; `ledger` holds provenance and deletion events. Every artifact records
creator, purpose, task/run, path, timestamps, status, tree hash, dependencies and
retention reasons. JSON writes use runtime staging with flushed immutable start/end
journal frames, so interruption does not leave an untracked adjacent temp file.
Frames are the bootstrap journal; they are written exclusively and incomplete
frames require review. The ledger persists after payloads are deleted.

Create a run, Register each run-local payload, then Complete with completed, failed
or interrupted status. Agents validators, tests, packages and deployment journals
use this module. Invoke-AgentChild routes supported subprocess TEMP/TMP/TMPDIR to
a registered directory and restores process settings afterward. Unregistered
external caches, Git and native Codex state are explicitly outside scope.

| Artifact | Retention after completion |
|---|---|
| Scratch and rebuildable package | 7 days |
| Receipt and diagnostics | 30 days |
| Completed task revisions | 90 days |
| Active work or unresolved failure | Protected until resolved |
| Rollback backup or interrupted transaction | Protected until explicit retirement |
| Durable knowledge and formal release evidence | No temporary TTL |

List returns compact metadata; add Expand to inspect per-file footprints.
Inventory walks every path for links and unregistered content, using a per-run
ownership lookup. PlanCleanup only inspects and returns a digest. Save the JSON in
a registered scratch artifact and pass InputPath plus ExpectedDigest to Cleanup. Execution
checks the plan, all candidates, ownership, current hashes, dependencies and locks
before deleting payloads, and records intent and tombstones. Unknown/changed
content, links and unreconciled staging stop cleanup. No command clears runtime
wholesale. This is protection against accidental deletion, not a security boundary
against an adversarial process racing writes. Production uses the actual UTC clock;
fixed clocks are for disposable contract tests.

New footprint digests sort explicit path values before hashing. Earlier traversal
order digests remain readable only when the saved digest and every current path,
kind and file hash match. This compatibility check never adopts changed content.

Retire requires a reviewed resolution reason and restarts the artifact's window.
Interrupted deployment backups cannot retire before rollback/reconciliation.
Reconcile without arguments lists unknown state. Explicit staging or abandoned-lock
recovery needs Id, observed digest and reason; an active lock owner blocks removal.
Interrupted deletions reconcile only a recorded deletion intent and remaining
unchanged owned children, preserving unknown or changed payloads.
Legacy packages require complete manifest/hash ownership proof before migration.
The complete catalog must also match a known local source commit; a self-written
manifest alone is not origin proof.
Unknown legacy data is preserved for review, never inferred from its filename.

## Deployment and rollback

Use deploy-agents-workflow.ps1 against an authorized existing target with DryRun.
Inspect creates, updates, removals and conflicts; apply with its ExpectedPlanDigest.
Both layouts preserve byte-identical script sources. Unowned existing files and
modified managed files stop writes. Repeated deployment is a no-op, but pending
transactions and locks still block it. No business code, README, knowledge, global
settings or local Codex configuration is managed.

Journals and original bytes are registered pinned backups. Rollback uses the
transaction Id, validates every original backup and current target hash before
writes, then restores the full owned boundary. Do not remove a live lock.
After rollback or deliberate retirement of a committed rollback option, explicitly
Retire the artifact. v3 journal paths remain readable for recovery.

## Optional native hooks

`docs/templates/codex-hooks.json` is a template, never automatically installed or
trusted. Review it and merge into project `.codex/hooks.json` only when intentionally
enabling integration. Use native `/hooks` trust review; changed definitions need
fresh review. Do not bypass trust. The template resolves the Git root from any
subdirectory and finds tools through the managed manifest in both layouts.

SessionStart returns a bounded source-checked summary and task candidates.
PreCompact inspects saved state; compact SessionStart recalls it before the next
model call. Stop checks checkpoint/artifact omissions, requests at most one
continuation per session/turn, and respects stop_hook_active. No event parses full
transcripts, promotes knowledge, cleans artifacts or replays external actions.
Plan/unknown permissions inspect only; use `AGENTS_HOOK_READ_ONLY=1` or the
ReadOnly switch for a read-only host that does not expose its mode in event JSON.
These environment settings apply only to that hook process, not global Codex state.
Hook failures are advisory and visible; native activation is a separate trust step.

Manual equivalents remain available without hooks:

```powershell
pwsh -NoProfile -File scripts/project-memory.ps1 -Action Recall -Expand
pwsh -NoProfile -File scripts/task-state.ps1 -Action Resume -Id <task>
pwsh -NoProfile -File scripts/runtime-artifacts.ps1 -Action List
pwsh -NoProfile -File scripts/project-hooks.ps1 -Event SessionStart -InputJson '{}' -ReadOnly
```
