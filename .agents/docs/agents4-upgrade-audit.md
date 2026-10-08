# Agents 4.0 Upgrade Audit

## Reviewed Source And Deployment

- Source: `https://github.com/JaredLin1217/Agents`, version `4.0.0`.
- Reviewed commit: `121f6e8559b4c8d4a4bb08322c8c53c7d9687de6`.
- Target: this InlongSlicer Consumer, using `dot-agents-layout`.
- Deployment transaction: `deploy-7fa8d6aa9f2141968e3234efaa855500`.
- Reviewed plan digest:
  `8d3473be95600e9df76641aa496ac7425a5b926a15e996c4f6fe6d1f2a0ed43c`.
- Managed ownership covers 29 assets. The manifest retains the compatible
  `agents-managed/v3` schema; its deployed product version is `4.0.0`.
- No native hooks, global settings or local Codex environment were changed.
- No Git commit, push, product rebuild or installation was requested or performed.

## Source Verification

The Provider Checkpoint passed syntax, JSON schema, ownership, knowledge,
sources, size measurement, regressions, v4 regressions, package and diff checks.
The offline regression suite passed 153 assertions; the v4 regression suite
passed 143 assertions. These are Agents workflow tests, not slicer product tests.

The original Checkpoint receipt failed only the release-evidence ancestry check:
the shallow source clone did not contain its referenced source ancestor
`b9ac8f2db2b5fcbb9f541dfd0031554032a28ef9`. Fetching the missing history changed
no source files. Rechecking only this failed check passed, with unchanged input
digest `2cff2cb38012808a1ea555d9c07e2d05786aa7711cfbce84d34c956ad6730002`.
The original receipt and separate successful recheck are retained without rewriting
the original failed result or rerunning unchanged checks.

## Project Guidance Review

The source inventory contained 20,806 tracked paths and four applicable
`AGENTS.md`/`CLAUDE.md` entry points. Root and test Claude entry points continue
to refer to their shared Agents rules. Project-owned resource and build guidance
were checked against the actual checkout, rather than importing Provider-only
rules into the product.

- Updated project structure and memory guidance for immutable reviewed JSON
  knowledge, registered runtime artifacts and optional, inactive native hooks.
- Removed the obsolete hand-maintained memory index and its referring README.
- Removed the external `orca_cad` focus-arbiter loop instruction, which referred
  to a different host, checkout and delegation workflow.
- Replaced the publication command's single-message/tool restrictions with
  explicit user authorization, scoped staging, validation and remote verification.
- Retained test rules, Inlong branding safeguards, product/CAD documentation,
  reference assets and historical Markdown knowledge. Historical records do not
  become current evidence or mandatory rules merely because they were retained.

Run `pwsh -NoProfile -File scripts/check-agents-guidance.ps1` to repeat the
project-owned entry-point and retired-guidance checks. Run the deployed validator
with `-Scope Consumer -Profile Checkpoint` for the full managed boundary.
Consumer checks do not claim Provider release readiness.

This was a repository-wide guidance and artifact inventory, not a functional
audit of every product source file, profile, dependency or installed binary.
Eighteen pre-existing modified/untracked product files were protected by SHA256
snapshots. Product tests were not rebuilt or rerun for this tooling-only change.

## Cleanup Boundary

The user explicitly authorized deletion of old chat-created verification payloads,
backups, twelve legacy task recovery records and legacy rollback journals.
Deleting those task records does not resolve or reclassify their historical
unfinished GUI or hardware checks.

Each precise target is previewed with a complete file/tree hash and rechecked for
source ownership, links, active use, locks, protected product files and recovery
dependencies immediately before removal. Explicit user-directed legacy removal
is recorded separately from the native 7/30/90-day retention policy; no clock,
policy or eligibility date is falsified. Native provenance and current validation
receipts survive payload deletion.

Tracked retired guidance is recoverable from Git. Deleted ignored temporary
payloads and retired rollback data are not backed up elsewhere by this cleanup.
Original user projects, source fixtures, active programs, generated runtime
resources, build/dependency output and local Codex state are outside the cleanup.

The completed legacy removal deleted 5,564 files (159,884,879 bytes), including
the twelve old task records, both legacy deployment journals and this upgrade's
isolated Provider clone. A separate checked removal deleted 17 Python bytecode
cache files (262,180 bytes); current or historical tracked source and bytecode
headers were verified. Exact target hashes and deletion times remain in the
runtime ledger. Old runtime groups and root `00000.log` no longer exist.

Local logo design material and `scripts/flatpak/deps.tar` were preserved: the
inventory alone does not prove that these are expendable chat-task payloads.
No blanket Git clean or recursive deletion of a workspace/build root was used.
The final current-run scratch and retired upgrade rollback payload have separate
reviewed footprints and deletion events; their bookkeeping is not old test data.
