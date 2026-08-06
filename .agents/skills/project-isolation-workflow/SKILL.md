---
name: project-isolation-workflow
description: Route repo Agents work with bounded context, evidence, and isolation.
---

# Project Workflow
Read `.agents/docs/agents/ai-runtime.yaml`; expand only named canonical YAML. Durable content is English-only.

## Loop
1. Classify the route and scope.
2. For repo work, run `resolve-agent-context.ps1`; cap output at three files and 8192 bytes.
3. Confirm impact from files, Git, schemas, AST, mappings, and tests. Heuristics are discovery only.
4. Dirty, stale, conflict, unsupported input, or parse failure expands reads and verification.
5. Execute inside the authorized boundary and preserve unrelated work.
6. Use the smallest safe verify profile; checkpoint operations use required gates.
7. Close with evidence pointers. Durable knowledge requires v3 provenance and freshness metadata.

Use canonical routes. OpenAI guidance uses official docs first.

## Guardrails
- GM off unless requested. Project-local skills are not GS.
- External FS needs exact authorization; `%TEMP%/codex-agent-status/<project-id>/` is scratch.
- `.agents/runtime/**` is ignored advisory state and never deployable.
- Hard-isolation claims need verified enforcement evidence.
- Do not edit `.git/`, generated output, or live Codex state unless targeted.

## Closeout
```text
Isolation: GM <used/not used> | GS <used/not used> | XR <none/paths> | XW <none/paths>
```
