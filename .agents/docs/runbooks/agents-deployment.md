# Agents Deployment

Use when copying this workflow into an explicitly authorized target repo.

## Fast Path

1. Confirm exact target path and write action.
2. Inspect target state before writing.
3. Choose `core_bootstrap`, `full_workflow`, or `template_provider_mode`; default to `core_bootstrap` unless the user asks for more.
4. Copy/adapt only the `deployable_by_mode` groups listed by `mode_composition` for the chosen mode in `.agents/docs/agents/deploy.yaml`.
5. Append/adapt `gitignore.fragment`; do not replace target `.gitignore` wholesale.
6. Validate source-neutral templates and report exact target reads/writes.

## Modes

- `core_bootstrap`: `AGENTS.md`, policy pack, project skill, gitignore fragment, deployment and closeout runbooks.
- `full_workflow`: `core_bootstrap` plus memory starters, runtime/evidence templates, and remaining runbooks.
- `template_provider_mode`: `full_workflow` plus recursive `.agents/docs/templates/agents/**` so the target can redeploy this workflow.

Hard fail on missing exact authorization, blocklisted copy, uninspected target-specific claim, or provider state in deployable templates.

Keep target state target-owned. Do not copy source status, employee history, memory entries, commits, remotes, tags, or runtime files.

References: `.agents/docs/agents/deploy.yaml`, `.agents/docs/agents/verify.yaml`.
