# Project-Local Skill Authoring

Use this runbook before creating or updating any project-local skill under `.agents/skills/`.

## When To Create A Skill

Create a project-local skill only when a workflow is likely to repeat and benefits from procedural instructions.

Prefer other layers when they fit better:

- `AGENTS.md`: every-session rules.
- `.agents/docs/memory/index.md` plus `.agents/docs/memory/entries/`: verified reusable lessons.
- `.agents/docs/decisions/`: durable decisions and reasons.
- `.agents/docs/runbooks/`: longer SOPs that do not need skill triggering.
- `.agents/skills/`: concise workflow instructions that should be invoked as a capability.
- `InlongSlicer_doc/`: product, migration, release, and functional change documentation.

Do not create a skill for one-off observations, generic preferences, or unverified guesses.

## Required Layout

```text
.agents/skills/
`-- <skill-name>/
    |-- SKILL.md
    `-- agents/
        `-- openai.yaml
```

Optional resource folders may be added only when needed:

```text
scripts/      deterministic helpers that should be executed
references/   detailed docs loaded only when needed
assets/       templates or files used as output resources
```

Do not add `README.md`, `CHANGELOG.md`, `INSTALLATION_GUIDE.md`, or other auxiliary docs inside a skill folder.

## Naming

- Use lowercase letters, digits, and hyphens only.
- Keep names short and action-oriented.
- Keep the folder name and YAML `name` identical.
- Use project-specific names only when the skill belongs to this repo.

## SKILL.md Requirements

Every `SKILL.md` must start with frontmatter containing only `name` and `description`:

```md
---
name: skill-name
description: What this skill does. Use when specific trigger, context, file type, or workflow applies.
---
```

Description rules:

- Include what the skill does.
- Include when to use it.
- Include concrete triggers or contexts.
- Keep it specific enough to avoid accidental use.
- Do not rely on a body section called "When to use"; the description is the trigger surface.

Body rules:

- Start with one short overview.
- Use imperative, procedural language.
- Include the workflow steps.
- Include file placement or ownership rules when relevant.
- Include validation and closeout requirements.
- Keep long explanations in `.agents/docs/`, not in `SKILL.md`.
- Keep `SKILL.md` concise; target under 150 lines and avoid exceeding 500 lines.

## agents/openai.yaml Requirements

Add UI metadata in `agents/openai.yaml`:

```yaml
interface:
  display_name: "Human Friendly Name"
  short_description: "25 to 64 character summary."
  default_prompt: "Use $skill-name to perform the intended workflow."

policy:
  allow_implicit_invocation: true
```

Rules:

- `default_prompt` must mention `$skill-name`.
- `short_description` must be 25-64 characters.
- Keep strings quoted.
- Add icons or brand color only when explicitly needed.

## Validation

After creating or updating a skill:

1. Run `git status -sb --untracked-files=all`.
2. Confirm `SKILL.md` frontmatter has `name` and `description`.
3. Confirm folder name matches frontmatter `name`.
4. Search for unresolved placeholders:
   ```powershell
   rg -n "TODO|\[TODO\]" .agents/skills
   ```
5. Do not rely on global validator scripts for normal project work.
6. If the user explicitly authorizes a global validator path and action, report that access in closeout.

## Update Checklist

When adding a new project-local skill, update these if relevant:

- `AGENTS.md`
- `.agents/README.md`
- `.agents/docs/project-structure.md`
- `.agents/docs/runbooks/`
- `.agents/docs/decisions/`
- `.agents/docs/memory/index.md` only if the work produced a verified reusable lesson

## Closeout

Report:

- skill path,
- why a skill was warranted instead of a runbook or memory entry,
- validation performed,
- whether any project rules or docs were updated.
