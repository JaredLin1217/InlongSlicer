# Project Operating Rules
Repo-local rules for isolated deployable Agents workflow. Durable rules/docs/skills/templates are English-only. Read `.agents/docs/agents/ai-runtime.yaml` first; expand only named canonical YAML.
## Prefix
- Start every visible assistant response with `$$`, unless higher-priority tool/system protocol conflicts.
## Route
- Classify with `.agents/docs/agents/ai-runtime.yaml`; read only its route files.
- Answer-only with no repo-state claim: no commands; compact closeout.
- Before edits, git/deploy, or current-state claims: inspect needed state and protect existing changes.
- Verify with the smallest `.agents/docs/agents/verify.yaml` profile; commit/tag/branch-push use fast checkpoint gates.
- Use `.agents/skills/project-isolation-workflow/SKILL.md` for isolation, memory, deployment, multi-agent, skill, or maintenance work.
- OpenAI API/Apps SDK/Codex/Agents SDK/MCP/model/tool guidance: official OpenAI developer docs first.
## Boundaries
- GM off unless explicitly requested.
- GS means intentional global/system `SKILL.md`; `.agents/skills/**/SKILL.md` is project-local, not GS.
- External filesystem: no access outside repo unless exact path/action is authorized, except `%TEMP%/codex-agent-status/<project-id>/`.
- Report temp/external access as XR/XW.
- `.agents/runtime/agent-ledger.jsonl` is ignored advisory runtime state, not official DB, not deployable, not XR/XW.
- Rules are behavioral; claim hard isolation only with verified runtime/tool/OS/account/cloud evidence.
- Do not hand-edit `.git/`, generated/cache/build/vendor output, runtime copies, or live Codex state unless targeted.
- Multi-agent: use `.agents/docs/agents/workflows.yaml` for modes, ownership, ledger, roster fallback, scoring, recovery.
## Closeout
Always include:
```text
Isolation: GM <used/not used> | GS <used/not used> | XR <none/paths> | XW <none/paths>
```
Add detail only for file changes, verification, risks, external access, durable knowledge, or explicit claim scope.