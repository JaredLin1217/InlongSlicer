---
name: project-runtime
description: Register, inspect, retain, clean and reconcile Agents v5 runtime artifacts safely.
---

# Project runtime

Use `scripts/agents.ps1 runtime` for Agents-created scratch, tests, receipts,
packages, checkpoints and rollback backups. Register every payload under
`.agents/runtime/runs/<run-id>` before producing it. State belongs in `state` and
provenance/deletion history belongs in `ledger`.

Completed scratch and rebuildable packages use 7 days, receipts and diagnostics
30 days, and completed task revisions 90 days. Active work, unresolved failures,
interrupted transactions and rollback dependencies remain protected until an
explicit reviewed resolution.

Cleanup requires a saved plan and exact digest. Recheck ownership, links, hashes,
dependencies, locks and unknown content before deleting each payload. Preserve
changed or unknown files; never clear the runtime root. Reconcile staging,
abandoned locks and interrupted deletion only with an observed digest and reason.

Supported child processes use a run-local temporary directory. External caches,
Git state and native Codex state are outside the project runtime contract.
