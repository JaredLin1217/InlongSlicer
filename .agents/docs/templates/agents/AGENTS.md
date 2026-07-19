# Project Operating Rules
Repo rules for deployable Agents workflow. Durable rules/docs/skills/templates are English-only. Read `.agents/docs/agents/ai-runtime.yaml`; expand only named canonical YAML.

## Prefix
- Start visible responses with `$$`, unless higher-priority protocol conflicts.

## Route
- Classify with `.agents/docs/agents/ai-runtime.yaml`; read only route files.
- Answer-only/no repo-state claim: no commands; compact closeout.
- Before edits, git/deploy, or state claims: inspect needed state and protect existing changes.
- Verify with the smallest `.agents/docs/agents/verify.yaml` profile; commit/tag/branch-push use fast checkpoint gates.
- Use `.agents/skills/project-isolation-workflow/SKILL.md` for isolation, memory, deployment, multi-agent, skill, or maintenance work.
- OpenAI API/Apps SDK/Codex/Agents SDK/model/tool guidance: official docs first.

## Boundaries
- GM off unless explicitly requested.
- GS means intentional global/system `SKILL.md`; `.agents/skills/**/SKILL.md` is project-local, not GS.
- External filesystem: no access outside repo unless exact path/action is authorized; `%TEMP%/codex-agent-status/<project-id>/` is status scratch. Report XR/XW.
- `.agents/runtime/agent-ledger.jsonl` is ignored advisory state, not official DB, deployable, or XR/XW.
- Rules are behavioral; claim hard isolation only with verified runtime/tool/OS/account/cloud evidence.
- Do not hand-edit `.git/`, generated/cache/build/vendor output, runtime copies, or live Codex state unless targeted.
- Multi-agent: use `.agents/docs/agents/workflows.yaml` for modes, ownership, ledger, roster fallback, scoring, recovery.

## Closeout
Always include:
```text
Isolation: GM <used/not used> | GS <used/not used> | XR <none/paths> | XW <none/paths>
```
Add detail only for changes, verification, risks, external access, durable knowledge, or claim scope.
