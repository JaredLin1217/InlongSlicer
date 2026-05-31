# Skill Authoring

Use before creating or updating project-local skills.

1. Create only repeatable workflows that do not belong in `AGENTS.md`, memory, decisions, runbooks, or policy pack.
2. Store under `.agents/skills/<skill-name>/`.
3. Keep `SKILL.md` concise and include `agents/openai.yaml` metadata from `.agents/docs/agents/schemas.yaml`.
4. Do not create project-specific global Codex skills.

References: `.agents/docs/agents/workflows.yaml`, `.agents/docs/agents/schemas.yaml`.
