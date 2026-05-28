# Agent Status Handoff

Use this runbook when more than one Codex session, window, or sub-agent may touch this repository. The goal is that each agent can know where other agents stopped.

## Status Board

Use `.agents/docs/agent-status.md` as the project-local status board.

The controller session owns this file. Employee agents should report their final status to the controller; the controller updates the status board after reviewing the report.

Do not use global Memory for agent status.

`.agents/docs/agent-status.md` is not live shared state. It is a repo-local snapshot that must be refreshed from current git state and employee reports.

## Status Sync Checkpoints

Because `.agents/docs/agent-status.md` is repo-local snapshot state, not automatic live shared state, reconcile it at these points:

- Session start / takeover: read the status board and current git state; update `Current Controller Status` after review if they differ.
- Before assigning an employee: refresh the controller snapshot and include the relevant status in the assignment.
- Immediately after spawning, or as soon as practical: add or update the employee's `active` row in `Employee Agents`; if delayed, reconcile it before closeout.
- After an employee final report: review the report and any diff, then update `Employee Agents` and `Isolation Log`.
- Before final closeout for any task involving multiple sessions, windows, or agents: confirm `Current Controller Status`, `Employee Agents`, and `Isolation Log` reflect the final reviewed state, or explain why no update was needed.

## Controller Snapshot Fields

Keep the top section of `.agents/docs/agent-status.md` current enough for handoff:

- `Last reviewed`: update when the controller reconciles the board.
- `Current branch`: update when the branch changes.
- `Latest known commit`: record the short hash and subject from `git log --oneline --decorate -1`.
- `Git state`: summarize whether the repo is clean, has uncommitted changes, or is ahead/behind.
- `Current focus`: name the active task, not a broad project aspiration.
- `Next action`: one concrete next step for the next session.

## Before Assigning An Agent

The controller should inspect:

```powershell
git status -sb --untracked-files=all
git log --oneline --decorate -5
```

Then read `.agents/docs/agent-status.md` before writing the new assignment.

The assignment must tell the employee:

- current relevant status from `.agents/docs/agent-status.md`,
- role,
- goal,
- allowed scope,
- forbidden scope,
- whether it may edit files,
- final report format,
- isolation requirements.

## Employee Final Report

Each employee report must include:

```text
Agent:
Role:
Status: completed / blocked / stopped
Task:
Files inspected:
Files changed:
Current stopping point:
Findings:
Verification:
Risks:
Recommended next action:
Global Memory:
Global Skill:
Project-external reads:
Project-external writes:
```

If the employee edited files, it must list exact paths.

## Non-Compliant Reports

If an employee final report is missing fields:

- The controller normalizes the report into the required format before updating `.agents/docs/agent-status.md`.
- Missing values should be filled from available context when safe.
- Unknown values must be marked `unknown`, not guessed.
- Ask the employee for a follow-up only when missing information blocks integration or risks overwriting work.

## Updating Agent Status

After receiving an employee report, the controller updates `.agents/docs/agent-status.md` with:

- latest controller status,
- employee id or nickname,
- role,
- task,
- final status,
- files inspected,
- files changed,
- stopping point,
- next action.

Record per-employee work state in the `Employee Agents` table.

Record per-run isolation fields in the `Isolation Log` table:

- Global Memory,
- Global Skill,
- Project-external reads,
- Project-external writes.

If an employee omitted an isolation field, normalize it from available context when safe. Otherwise record `unknown` instead of guessing.

Do not mark an employee task complete until the controller has reviewed the report and, if files changed, inspected the diff.

Do not close a multi-agent task until `.agents/docs/agent-status.md` is updated or the closeout explains why no update was needed.

## Session Start

Any session taking over this project should read:

1. `AGENTS.md`
2. `.agents/docs/agent-status.md`
3. `git status -sb --untracked-files=all`
4. `git log --oneline --decorate -5`

If `.agents/docs/agent-status.md` and git state disagree, trust current git state and update the status board after review.

## Snapshot Rules

- Git status is the source of truth for file changes.
- `.agents/docs/agent-status.md` is the source of truth for agent stopping points and handoff notes.
- If the two disagree, reconcile by inspecting current git state and then updating `.agents/docs/agent-status.md`.
- Do not rely on an agent's remembered state if it is not reflected in the status board or current git state.

## Conflict Handling

- If two agents touched the same file, stop and inspect diffs before continuing.
- Prefer preserving both useful changes with a small manual merge.
- Do not use destructive git commands to resolve conflicts unless the user explicitly requests the exact action.
