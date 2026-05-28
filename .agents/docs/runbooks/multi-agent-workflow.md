# Multi-Agent Workflow

Use this runbook when the user wants InlongSlicer to operate with a controller session and one or more employee agents.

## Validation Status

This workflow has been installed for InlongSlicer and field-tested once with a read-only `explorer` agent on 2026-05-29. The workflow now includes the latest repo-level checkpoint protocol from the controller project. A bounded `worker` trial should be run in this repository before treating worker delegation as fully field-tested here.

## Roles

- Controller session: plans work, assigns agents, reviews results, integrates changes, and reports outcome.
- `explorer`: read-only investigation, file search, architecture mapping, risk analysis.
- `worker`: bounded implementation or documentation work with a clear write scope.

## Hiring Trigger

When the user says `招聘一個員工`, `hire employee`, or `spawn employee`, treat it as permission to create a Codex sub-agent for this repository.

## Required Assignment Fields

Every employee assignment must include:

- Status context: relevant current status from local `.agents/docs/agent-status.md`.
- Role: `explorer` or `worker`.
- Goal: one concrete outcome.
- Allowed scope: files or directories the agent may inspect or edit.
- Forbidden scope: files or directories the agent must not touch.
- Verification: how the result should be checked.
- Final report: exact final report expected.
- Coordination note: tell the agent other work may happen in parallel and not to revert others' changes.

## Default Guardrails

- Do not assign overlapping write scopes to multiple workers.
- Prefer `explorer` for questions and `worker` for implementation.
- Keep immediate blocking work in the controller session when waiting would slow the task.
- Review employee output before integrating it.
- Update or explicitly reconcile local `.agents/docs/agent-status.md` after every employee final report.
- If an employee edited files, inspect the diff before continuing.
- Tell each employee to use repo-local context first and not to write project knowledge to global Codex Memory or global skill folders.
- Employees must not use global/system tools or skills unless the controller assignment explicitly authorizes that use, the user explicitly requested it, or a higher-priority runtime instruction requires it.
- If an employee uses a system/global tool or skill under an allowed exception, require the final report to mention what was used and why.
- Do not allow employees to read or write filesystem paths outside this repository unless the controller has explicit user authorization for exact external paths and actions.

## Assignment Template

```text
Status context:
Role:
Goal:
Allowed scope:
Forbidden scope:
Verification:
Final report:
Coordination:
```

## Status Synchronization

- Before assigning an employee, read local `.agents/docs/agent-status.md` and include relevant current status in the assignment. If it is missing, create it from `.agents/docs/agent-status.template.md`.
- After spawning an employee, add or update an `active` row in local `.agents/docs/agent-status.md` as soon as practical. If waiting to update is more efficient, the controller must reconcile the row before closeout.
- After receiving an employee report, normalize it into the fields required by `.agents/docs/runbooks/session-handoff.md`.
- Treat `.agents/docs/runbooks/session-handoff.md` as the employee final report schema. The closeout fields below are the controller's final report for the whole multi-agent task.
- If the employee omitted a required field, fill it from available context or mark it `unknown`; ask the employee only when the missing field blocks integration.
- The controller owns updates to local `.agents/docs/agent-status.md`.
- Do not close a multi-agent task until local `.agents/docs/agent-status.md` is updated or the closeout states why no update was needed.

## Controller Closeout

At the end of a multi-agent task, the controller reports the integrated result:

- agents used,
- files changed,
- verification performed,
- conflicts or risks,
- system/global Codex resources used, if any,
- Global Memory: used / not used,
- Global Skill: used / not used,
- Project-external reads: none / authorized paths,
- Project-external writes: none / authorized paths,
- local `.agents/docs/agent-status.md`: updated / not updated with reason,
- whether a new `AGENTS.md` rule, project memory entry, runbook, skill, or decision should be added.
