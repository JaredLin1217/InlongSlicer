# Agents Templates

Source-neutral starter bundle for authorized target repos.

## What This Is

Deployable, source-neutral Agents rules for target repos.

## How To Deploy

Use `.agents/docs/agents/deploy.yaml` and choose one mode:

- `core_bootstrap`: default; router, policy pack, project skill, gitignore fragment, deployment and closeout runbooks.
- `full_workflow`: core plus memory starters, runtime/evidence templates, and remaining runbooks.
- `template_provider_mode`: full workflow plus recursive `.agents/docs/templates/agents/**`.

## What Not To Copy

Do not copy provider status, employee history, memory entries, commits, remotes, tags, runtime files, or filled evidence. Exact target authorization and target inspection are required before writing.
