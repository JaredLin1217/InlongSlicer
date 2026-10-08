---
name: project-runtime
description: Register Agents temporary and verification artifacts, preview retention cleanup, or reconcile interrupted runtime work.
---

# Project runtime

Use this for Agents-created scratch, test data, diagnostics, packages, checkpoints
and rollback backups. Resolve tools through `.agents/managed.json` in a Consumer.
Read the operator guide's runtime section only when cleaning or reconciling.

Create a run with `runtime-artifacts.ps1 -Action Create -Purpose <purpose>`.
Register each payload under its `runs/<run-id>` directory before producing data.
Record dependencies and retention reasons. Close the run with `Complete`, marking
failed/interrupted work accurately. Supported child processes inherit run-local
TEMP/TMP through `Invoke-AgentChild`; arbitrary external caches remain outside scope.

Cleanup requires `PlanCleanup`, a saved plan, and its exact digest. `Cleanup`
rechecks hashes, ownership, links, dependencies and locks before deleting any
eligible payload. Never clear the runtime root, force changed files into ownership,
or delete unknown content. Keep active tasks, unresolved failures and rollback
dependencies pinned. `Retire` requires a reviewed reason after resolution; it
starts a new retention window. Plan/read-only work may inspect but never clean.

Use `Reconcile` without mutation arguments first. Migrate legacy packages only
when their managed manifest and complete file list prove ownership. Recover staging
transactions or abandoned locks with the observed digest and reason; a running
owner blocks lock removal. See `docs/agents/runtime-policy.json` for 7/30/90 days.
