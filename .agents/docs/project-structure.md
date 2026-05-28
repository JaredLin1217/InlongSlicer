# Codex Project File Structure

InlongSlicer keeps Codex operating knowledge inside the repository.

## Current Structure

```text
.
|-- AGENTS.md
|-- CLAUDE.md
|-- .codex/
|   |-- config.toml
|   `-- environments/
|       `-- environment.toml
|-- .agents/
|   |-- README.md
|   |-- docs/
|   |   |-- agent-status.md
|   |   |-- codex-memory.md
|   |   |-- project-memory.md
|   |   |-- project-structure.md
|   |   |-- memory/
|   |   |   |-- index.md
|   |   |   `-- entries/
|   |   |       `-- README.md
|   |   |-- decisions/
|   |   |   `-- 0001-project-isolated-knowledge.md
|   |   `-- runbooks/
|   |       |-- global-knowledge-migration.md
|   |       |-- isolation-audit.md
|   |       |-- multi-agent-workflow.md
|   |       |-- session-handoff.md
|   |       `-- skill-authoring.md
|   `-- skills/
|       |-- inlong-branding-migration/
|       |-- project-isolation-workflow/
|       |-- source-command-dedupe/
|       `-- source-command-oncall-triage/
|-- .claude/
|   `-- commands/
`-- InlongSlicer_doc/
```

## Directory Rules

- `AGENTS.md`: compact repository-wide rules loaded at the start of project work.
- `.agents/README.md`: local index for Codex agent assets.
- `.agents/docs/`: Codex memory, runbooks, decisions, and handoff status.
- `.agents/docs/agent-status.md`: current status board for controller and employee agents.
- `.agents/docs/project-memory.md`: overview of the project-local memory system.
- `.agents/docs/memory/index.md`: searchable memory index with triggers, keywords, summaries, and links.
- `.agents/docs/memory/entries/`: detailed memory entries.
- `.agents/docs/decisions/`: durable Codex/project operating decisions.
- `.agents/docs/runbooks/`: repeatable procedures that are longer than `AGENTS.md` should be.
- `.agents/skills/`: project-local skills. Do not place InlongSlicer-specific skills in global Codex skill folders.
- `.codex/`: Codex App project settings and local environment setup. Let Codex App generate environment files when possible.
- `.claude/commands/`: legacy Claude command prompts. Keep matching behavior aligned with migrated Codex skills when both exist.
- `InlongSlicer_doc/`: product documentation, migration records, and functional change log.

System/global Codex locations are not part of this project structure. Use them only when the user explicitly authorizes exact global paths and actions.
