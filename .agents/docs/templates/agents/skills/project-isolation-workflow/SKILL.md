---
name: project-isolation-workflow
description: Maintain repo-local Agents rules, templates, memory, deployment, and multi-agent workflows.
---

# Project Isolation Workflow

Use this project-local skill when executing or maintaining isolation, memory, deployment, multi-agent, handoff, skill-authoring, or repo maintenance procedures.

## Route

- Use `.agents/docs/agents/workflows.yaml` for task routing and progress/update rules.
- Use `.agents/docs/agents/policy.yaml` for isolation, authority, hard gates, and closeout.
- Use `.agents/docs/agents/verify.yaml` for verification profile selection.
- Use `.agents/docs/agents/deploy.yaml` before authorized target deployment.
- Read project memory details only from relevant `.agents/docs/memory/index.md` rows.

## Employee Path

- Assignment first; stay inside assigned read/write scope.
- Explorers are read-only and can use the brief explorer schema.
- Workers need exclusive normalized write scope.
- Record recovery-sensitive employee lifecycle in `.agents/runtime/agent-ledger.jsonl`; it is repo-local ignored runtime state and not XR/XW.
- Use temp roster only when external handoff fallback is required; report it as XR/XW.
- Final report is the completion notification; controller reviews before integration.
- After runtime close, run exact authorized Codex App sidebar/history cleanup as one quiet batch: close targets, verify current-project subagent matches, remove matching state/history residues, verify zero hits, record one ledger summary, and report compact XR/XW.
- Fast hiring: run one controller status check, fill available runtime slots first, keep a queue, and refill immediately when one employee completes.
- Do not re-read every runbook for each hire; use the assignment preset and only open canonical files needed by the task.
- Scoring batches: use compact scoring reports, aggregate median-first, stop at 3-5 employees when results converge unless the user explicitly asks for more.

## Guardrails

- Project-specific skills stay under `.agents/skills/`.
- Do not intentionally use global/system skills or global Memory for normal work; project-local `.agents/skills/**` is not GS.
- No external filesystem access without exact authorization, except approved temp handoff cache.
- Do not claim hard isolation without current verified evidence.
- Keep deployable templates source-neutral.

## Closeout

Use `.agents/docs/agents/policy.yaml` closeout rules. Compact answers should be answer plus the required isolation line only unless a claim needs evidence.
