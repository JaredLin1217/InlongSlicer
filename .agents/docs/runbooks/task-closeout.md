# Task Closeout

Use the smallest closeout that proves the claim.

1. Choose the smallest `.agents/docs/agents/verify.yaml` profile.
2. No-change/no-current-state answer: no command, compact closeout.
3. Repo/deploy/git/runtime state claims such as clean, dirty, online, committed, pushed, tagged, deployed, active, or completed require only the named state check; required report status labels such as completed do not trigger extra checks by themselves.
4. File changes: report changed files, verification, risks, external access, durable-knowledge impact, and claim scope when relevant.
5. Ordinary commit/tag/branch-push uses fast checkpoint plus push result when applicable.
6. Full audit is for release, deploy, protected/main push, broad audit, no-deduction, or explicit full verification.

Closeout shapes:

- No-change: answer/result, optional caveat, required isolation line from `.agents/docs/agents/policy.yaml`.
- Read-only explorer: score/result, up to 3 issues, up to 3 fixes, required isolation line.
- File change: changed files, verification, risk, required isolation line.
- Deploy/audit: claim scope, checks, failures/risks, required isolation line.
