# Agent Status

This file is the InlongSlicer-local runtime status board for controller and employee agents.

Copy this template to `.agents/docs/agent-status.md` when starting or taking over local multi-session or multi-agent work. Keep the live `.agents/docs/agent-status.md` local to this repository instance; do not copy it into other projects as part of an Agents deployment.

## Current Controller Status

- Status: idle
- Last reviewed: YYYY-MM-DD
- Current branch: `unknown`
- Latest known commit: `unknown`
- Git state: unknown
- Remote state: unknown
- Release marker: unknown
- Current focus: none
- Next action: none

## Employee Agents

| Agent | Role | Task | Status | Files inspected | Files changed | Stopping point | Next action |
|---|---|---|---|---|---|---|---|

## Isolation Log

| Date | Actor | Global Memory | Global Skill | Project-external reads | Project-external writes |
|---|---|---|---|---|---|

## Handoff Notes

- This status board is repository-local runtime state, not an Agents deployment artifact.
- The controller owns updates to this file.
- Employee agents should report their status to the controller unless explicitly assigned this file.
- If this file and git state disagree, trust current git state and update this file after review.
