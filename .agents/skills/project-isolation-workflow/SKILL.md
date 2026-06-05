---
name: project-isolation-workflow
description: Repo-local Agents rules, templates, memory, deploy, multi-agent.
---
# Project Isolation Workflow
Token-min router for isolation, memory, deploy, agents, handoff, skills, maintenance. Start `.agents/docs/agents/ai-runtime.yaml`.
## Route
- `.agents/docs/agents/ai-runtime.yaml`: minimal files.
- `.agents/docs/agents/workflows.yaml`: route, progress, employees.
- `.agents/docs/agents/policy.yaml`: isolation, authority, closeout.
- `.agents/docs/agents/verify.yaml`: proof profile.
- `.agents/docs/agents/deploy.yaml`: authorized deployment.
- Deploy: preserve target layout; write only `deployed_file_set`; report historical files separately.
- Memory: read relevant `.agents/docs/memory/index.md` rows only.
## Employee Summary
- Exact read/write scope; explorers read-only; workers need exclusive normalized write scope.
- Use `.agents/runtime/agent-ledger.jsonl` only for recovery; temp roster only when authorized.
- Runtime close/sidebar cleanup/hiring/scoring: expand `.agents/docs/agents/workflows.yaml`; closeout runs `runtime.quiet_cleanup` and `scripts/agents-cleanup.ps1` for current parent/cwd closed subagent runtime ids, never sidebar nicknames; clear sqlite, session index, unread state, rollouts; no backups.
- Broader dismissal/cleanup follows `runtime.sidebar_cleanup_success_path` with exact authorization and evidence.
## Guardrails
- Project skills stay under `.agents/skills/`.
- No global Memory or global/system skills by default; project-local `.agents/skills/**` is not GS.
- No external filesystem access without exact authorization, except approved temp cache.
- Do not claim hard isolation without current verified evidence.
- Keep deployable templates source-neutral.
## Closeout
Use policy closeout. Compact answers: answer plus isolation line unless evidence is needed.
