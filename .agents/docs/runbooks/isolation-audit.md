# Isolation Audit

Use this runbook to make InlongSlicer's project isolation auditable without pretending `AGENTS.md` is a technical sandbox.

## Known Boundary

- `AGENTS.md` and `.agents/docs/` are behavior rules, not runtime enforcement.
- Codex system instructions, tools, plugins, and global/system skills can still exist in the session.
- The project rule is to avoid using global Memory and global/system skills for normal InlongSlicer work, keep durable knowledge inside this repo, and report any exception.
- This runtime boundary is an accepted project limitation. Do not count it as a failed audit by itself.
- Count an audit failure when an assistant claims this repo technically disables runtime capabilities, omits required isolation reporting, uses global Memory without explicit user approval, intentionally uses a global/system skill without an allowed exception, or reads/writes project-external filesystem paths without exact user authorization.

## Skill Source Classification

Before intentionally using a skill, classify it:

- `project-local`: the `SKILL.md` path is under this repository's `.agents/skills/`.
- `global/system`: the `SKILL.md` path is outside this repository, including Codex system skills, user global skills, and plugin skills.
- `none`: no skill was used.

Project-local skills are allowed for normal project work. Global/system skills require either an explicit user request or a higher-priority runtime instruction. If used, report the skill name and why it was required.

Classify from the skill path already visible in the session context. Do not read external skill files only to classify them.

## Filesystem Access Rule

Repository root:

```text
D:\inlong\Slicer\GitHub\InlongSlicer
```

Do not read, list, create, edit, delete, move, stage, commit, or configure filesystem paths outside the repository root unless the user explicitly authorizes the exact external path and action.

If external access is needed, stop and ask before accessing the path.

## Closeout Format

Every non-trivial Codex reply in this repository must include either the expanded form:

```text
Global Memory: used / not used
Global Skill: used / not used
Project-external reads: none / authorized paths
Project-external writes: none / authorized paths
```

Or the compact equivalent:

```text
Isolation: GM used/not used | GS used/not used | XR none/paths | XW none/paths
```

If any field is not `none` or `not used`, include the exact source, path, action, and reason.

## No-Script Verification

Use this checklist after isolation rule changes. Do not rely on project-specific verification scripts for this operating layer.

Required checks:

- `git status -sb --untracked-files=all`
- `git diff --check`
- `rg --files .agents AGENTS.md`
- `rg -n "Global Skill|Project-external|Isolation: GM|招聘一個員工|hire employee|spawn employee|task-closeout|Status Sync Checkpoints|global Codex Memory" AGENTS.md .agents`
- `rg -n "TODO|\[TODO\]" AGENTS.md .agents`

If `rg` is unavailable on a device, use an equivalent text search and report the substituted command.
If terminal output appears corrupted, verify important phrases by searching for the expected text, such as `招聘一個員工`, `hire employee`, or `spawn employee`, before editing.

## Validation Checklist

For isolation-related changes:

1. Run `git status -sb --untracked-files=all`.
2. Run `git diff --check`.
3. Run the no-script verification searches above.
4. Confirm any new durable knowledge is placed in the right layer:
   - every-session rule: `AGENTS.md`,
   - reusable verified lesson: `.agents/docs/memory/`,
   - repeatable workflow: `.agents/docs/runbooks/` or `.agents/skills/`,
   - durable decision: `.agents/docs/decisions/`.
5. Report whether global Memory, global/system skills, project-external reads, or project-external writes were used.
6. Treat the repo-level/runtime boundary as a known limitation. Only report it as a remaining risk when the task depends on technical enforcement rather than documented behavior and closeout reporting.
