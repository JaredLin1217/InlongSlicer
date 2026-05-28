# Decision 0001: Project-Isolated Codex Knowledge

## Decision

InlongSlicer keeps Codex working knowledge inside this repository instead of using global Codex Memory or global project-specific skill folders.

## Reason

InlongSlicer is a forked, cross-platform slicer project with high-risk areas such as branding, package identity, profiles, translations, CI, and release artifacts. Old assumptions from unrelated projects can cause incorrect edits. Repo-local rules and docs make Codex behavior auditable, portable, and reviewable.

## Rules

- Keep every-session rules in `AGENTS.md`.
- Keep project memory intake rules in `.agents/docs/codex-memory.md`.
- Keep project memory overview in `.agents/docs/project-memory.md`.
- Keep project memory index in `.agents/docs/memory/index.md`.
- Keep detailed project memory entries in `.agents/docs/memory/entries/`.
- Keep repeatable workflow SOPs in `.agents/docs/runbooks/` or `.agents/skills/`.
- Keep durable Codex/project operating decisions in `.agents/docs/decisions/`.
- Keep product, migration, release, and functional change docs in `InlongSlicer_doc/`.
- Do not write to global Codex Memory unless the user explicitly asks to re-enable or use it for exact paths.
- Treat Codex system skills, plugins, and global instructions as runtime capabilities, not as project knowledge stores.
- Do not intentionally use global/system skills for normal project work unless the user explicitly requests that capability or a higher-priority runtime instruction requires it.
- If a system/global capability is used, keep project-specific results in this repository and report the usage.
- For normal project work, project-external filesystem access is not allowed. Any exception requires explicit user authorization for the exact path and action.
- Every non-trivial Codex reply in this repository must report global Memory usage, global Skill usage, project-external reads, and project-external writes.
- Use `.agents/docs/runbooks/isolation-audit.md` as the operational checklist for this decision.

## Consequences

- New sessions must rely on repo files for project context.
- Useful lessons should be written as `.agents/docs/memory/index.md` plus `.agents/docs/memory/entries/`, repo docs, decisions, or project-local skills.
- The project can be moved or shared without depending on `C:\Users\v_jar\.codex\memories\`.
- This decision cannot technically disable Codex runtime capabilities by itself; it defines the project boundary and reporting rule.
- The repo-level boundary is an accepted limitation, not an unresolved defect. The required mitigation is auditable behavior: avoid global Memory and global/system skills for normal work, require exact authorization for project-external filesystem access, and report every exception.
- Closeout reports may be slightly longer, but they make isolation auditable.
