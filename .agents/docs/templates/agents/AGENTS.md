# Project Operating Rules

Repo-local Codex rules for an isolated, deployable Agents workflow. Durable rules, docs, skills, and templates are English-only. Canonical detail lives in `.agents/docs/agents/*.yaml`.

## Route

- Classify first with `.agents/docs/agents/workflows.yaml` common task matrix.
- Answer-only with no current repo-state claim: run no commands and keep closeout compact.
- Before edits, git/deploy actions, or current-state claims, inspect only the needed state and protect existing changes.
- Use the smallest profile in `.agents/docs/agents/verify.yaml`; ordinary commit/tag/branch-push uses fast checkpoint gates.
- Use `.agents/skills/project-isolation-workflow/SKILL.md` for executing or maintaining isolation, memory, deployment, multi-agent, skill, or maintenance procedures.

## Boundaries

- Global Memory: not used unless the user explicitly requests it.
- Global/system Skills: GS means intentional global/system `SKILL.md` use; `.agents/skills/**/SKILL.md` is project-local and does not count as GS.
- External filesystem: no access outside this repo unless the user authorizes exact path/action, except `%TEMP%/codex-agent-status/<project-id>/`.
- Report temp/external access as XR/XW.
- Project-local agent ledger: `.agents/runtime/agent-ledger.jsonl` is ignored runtime state, not Codex's official DB, not deployable, and not XR/XW.
- Repo rules are behavioral, not a sandbox. Claim hard isolation only with verified runtime/tool/OS/account/cloud evidence.
- Do not hand-edit `.git/`, generated output, caches, build output, vendored files, runtime copies, or live Codex environment state unless explicitly targeted.

## Multi-Agent

- Use `.agents/docs/agents/workflows.yaml` for employee modes, ownership, project-local ledger, roster fallback, scoring batches, and recovery.

## Closeout

Always include:

```text
Isolation: GM <used/not used> | GS <used/not used> | XR <none/paths> | XW <none/paths>
```

Expand only for file changes, verification, risks, external access, durable knowledge, or explicit claim scope.
