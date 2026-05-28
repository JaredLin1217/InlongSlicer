# Project Memory Overview

This file is the entry point for InlongSlicer's project-local Codex memory.

Project memory replaces global Codex Memory for normal InlongSlicer work. Keep memory verified, reusable, and specific to this repository.

## Structure

Project memory has two layers:

```text
.agents/docs/memory/index.md
.agents/docs/memory/entries/
```

- `.agents/docs/memory/index.md`: searchable list of memory entries with triggers, keywords, summaries, and links to details.
- `.agents/docs/memory/entries/`: detailed memory entries with context, cause, fix/rule, verification, and reuse conditions.

## Current Status

- Verified InlongSlicer project memory entries are tracked in `.agents/docs/memory/index.md`.
- Global Codex Memory is not used for normal project work.
- Global Memory content must not be imported blindly. Use `.agents/docs/runbooks/global-knowledge-migration.md` and require exact source-path authorization before reading or copying global files.

## Entry Rules

Add an entry only when all are true:

- It is specific to InlongSlicer.
- It is likely to be useful in a future session or employee-agent task.
- It has been verified against current repository files, commands, logs, tests, or observed behavior.
- It is not better represented as an `AGENTS.md` rule, a decision record, a runbook, or product documentation under `InlongSlicer_doc/`.

Do not add:

- Generic preferences.
- One-time observations.
- Guesses about future architecture.
- Lessons from unrelated projects.
- Unverified assumptions.

## Lookup Flow

1. For non-trivial tasks, scan `.agents/docs/memory/index.md` for relevant triggers or keywords.
2. If the index points to a matching entry, read the detailed file under `.agents/docs/memory/entries/`.
3. Verify important facts against current repo files, scripts, logs, tests, or observed behavior before acting.
4. If no matching entry exists, continue from current repository state.

## Add Flow

1. At the end of meaningful work, decide whether a verified reusable lesson exists.
2. Ask the user before adding a project memory entry unless the user explicitly asked to maintain memory.
3. Add a concise index row in `.agents/docs/memory/index.md`.
4. Add the full details in `.agents/docs/memory/entries/YYYY-MM-DD-short-title.md`.

## Entries

See `.agents/docs/memory/index.md`.
