---
name: project-checkpoint
description: Plan and validate Agents v5 changes, persist evidence, or safely deploy and roll back owned project files.
---

# Project checkpoint

Use `scripts/agents.ps1 plan` before material work. It is read-only and records
the affected boundary, acceptance conditions and intended checks. Use
`validate changed` while editing and one `validate checkpoint` at handoff. A
receipt is saved before output and reports individual states: passed, failed,
needs_review or not_applicable.

Publication additionally requires clean committed source, matching formal
evidence and `-RequireReleaseReady`. Missing or stale evidence is review-needed;
never replace evidence with a score or a model-behavior claim. Pure receipts may
be reused only when their source digest, command contract and host match.

Deploy an authorized existing Consumer with a dry-run plan. Review ownership,
conflicts and operations, then apply the exact plan digest. Both layouts are
supported. Modified managed files and unowned existing files stop writes.
Rollback validates every original byte and current target hash before restoring.
Read project-runtime for runtime cleanup or interrupted transactions.
