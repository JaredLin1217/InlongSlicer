# InlongSlicer project instructions

Read `AGENTS.md` and retain its shared rules. This project-owned entry point
replaces only the generic publication routing with the Consumer policy below.
Read `docs/agents/consumer-publication-policy.json` before commit or push work.
This policy also takes precedence over Provider publication sections in the
project-checkpoint skill and the managed operator guide for routine product work.

## Routine commits and pushes

InlongSlicer is an AI Agents Consumer. Run the relevant product checks,
`pwsh -NoProfile -File scripts/check-agents-guidance.ps1`, and the deployed
`validate.ps1 -Scope Consumer -Profile Checkpoint`. Preserve individual receipts
and verify that tested source matches the proposed or committed changes.

A current human request that explicitly includes push authorizes that push.
Complete the authorized review, commit and push in one continuous workflow when
checks pass. Do not request another confirmation merely because the Consumer
report has `release_ready=false`. Commit-only requests and historical approvals
do not authorize a new push.

For routine product commits and pushes, do not invoke `-RequireReleaseReady`,
request a Provider-gate exception, or require a separate Provider evidence commit.
Prefer one coherent commit when practical; retain product verification receipts
under the registered runtime and bind them to source hashes or the Git tree.
Create additional commits only when the change scope or an actual product
release requirement warrants them.

Before every authorized push, freshly inspect the remote and branch, preserve
unrelated work, push without force, and verify the resulting remote HEAD. Failed
checks, unexpected divergence, or unclear scope still require resolution.

## Provider boundary

`-RequireReleaseReady`, the Provider runtime-evidence collector and its separate
evidence commit apply only to an actual AI Agents distribution release from its
Provider checkout. Keep those controls intact. Do not create `agents.json`, edit
managed hashes, or relabel a Consumer as a Provider to make a check pass.

The root `CLAUDE.md` imports this same entry point. Nested instructions, including
`tests/AGENTS.md`, retain their normal scope. Shared managed files remain owned
by `.agents/managed.json`; project policy is maintained outside that manifest.
