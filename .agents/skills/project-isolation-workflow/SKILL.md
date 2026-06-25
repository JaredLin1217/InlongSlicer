---
name: project-isolation-workflow
description: Use for repo Agents workflow: routing, memory, deploy, multi-agent, skill, or maintenance.
---

# Project Workflow
Read `.agents/docs/agents/ai-runtime.yaml`; expand only named canonical YAML. Durable rules/docs/skills/templates: English-only.

## Routes
- `ai-runtime`: classify task, pick scope, stop unless another file is named.
- `quick_memory`: read/update `.agents/docs/agents/memory.yaml`.
- `workflows`: multi-agent modes, ownership, ledger, roster fallback, scoring, recovery.
- `skills`: repo-local skill rules and hard/behavioral isolation evidence.
- `deploy`: template provider, dry-run/copy, target state, rollback.
- `verify`: smallest matching profile first; fast gates before commit/tag/branch-push.
- `maintenance`: compaction, residue, old-layer cleanup, version alignment.
- OpenAI/API/model/tool guidance: official docs first.

## Guardrails
- Treat `.agents/skills/**/SKILL.md` as project-local, not GS.
- GM off unless explicitly requested.
- External FS is XR/XW unless exact path/action is authorized; `%TEMP%/codex-agent-status/<project-id>/` is status scratch.
- `.agents/runtime/agent-ledger.jsonl` is ignored advisory state, not official DB, deployable, or XR/XW.
- Claim hard isolation only with verified tool/OS/account/cloud evidence.
- Do not edit `.git/`, generated/cache/build/vendor output, runtime copies, or live Codex state unless targeted.

## Closeout
Always report:
```text
Isolation: GM <used/not used> | GS <used/not used> | XR <none/paths> | XW <none/paths>
```
