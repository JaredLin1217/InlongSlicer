# Project Operating Rules
Deployable Agents rules. Durable rules/docs/skills/templates are English-only.

## Prefix
- Start visible responses with `$$`, unless higher-priority protocol conflicts.

## Work
- Read `.agents/docs/agents/ai-runtime.yaml`; expand only its named canonical YAML.
- Loop: route -> minimal context -> impact -> execute -> precise verify -> evidence -> durable knowledge.
- Repo work uses `scripts/resolve-agent-context.ps1`; pointers are leads. Dirty, stale, conflicting, unsupported, or failed parsing expands reads and verification.
- Answer-only/no state claim: no commands. Before changes or state claims, inspect needed state and preserve existing work.
- Use the smallest `.agents/docs/agents/verify.yaml` profile. Commit, push, tag, deploy, and release use required checkpoint gates.
- Use `.agents/skills/project-isolation-workflow/SKILL.md` for governed work.
- OpenAI API/Apps SDK/Codex/Agents SDK/model/tool guidance: official docs first.

## Boundaries
- GM off unless requested. GS means intentional global/system `SKILL.md`; project skills are not GS.
- External FS needs exact authorization; `%TEMP%/codex-agent-status/<project-id>/` is scratch. Report XR/XW.
- `.agents/runtime/**` is ignored advisory state, not official DB or deployable source.
- Claim hard isolation only with verified runtime/tool/OS/account/cloud evidence.
- Do not hand-edit `.git/`, generated/cache/build/vendor output, or live Codex state unless targeted.
- Multi-agent rules: `.agents/docs/agents/workflows.yaml`.

## Closeout
Always include:
```text
Isolation: GM <used/not used> | GS <used/not used> | XR <none/paths> | XW <none/paths>
```
