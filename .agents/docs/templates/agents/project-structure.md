# Target Repo Structure

Agents rules live inside the target repo.

## Read Order

1. `AGENTS.md`
2. `.agents/docs/agents/workflows.yaml`
3. `.agents/docs/agents/policy.yaml`
4. `.agents/docs/agents/verify.yaml`
5. `.agents/docs/agents/schemas.yaml` only for assignments, reports, status, or templates
6. `.agents/docs/agents/deploy.yaml` only for authorized redeployment

## Role Matrix

| Area | Role | Deploy policy |
|---|---|---|
| `AGENTS.md` | Target router | deploy |
| `.agents/skills/project-isolation-workflow/` | Target-local skill | deploy |
| `.agents/docs/agents/*.yaml` | Canonical policy pack | deploy |
| `.agents/docs/runbooks/*.md` | Task entry points | mode-based deploy |
| `.agents/docs/templates/agents/` | Optional redeploy bundle | `template_provider_mode` only |
| `.agents/docs/memory/`, `.agents/docs/decisions/` | Target-local knowledge | target-owned |
| `.agents/runtime/`, `.codex/`, status, validation records | Local runtime state | never deploy |

- `AGENTS.md`: every-session router.
- `.agents/skills/project-isolation-workflow/`: project-local skill.
- `.agents/docs/agents/*.yaml`: canonical policy pack.
- `.agents/docs/runbooks/*.md`: short entry points.
- `.agents/docs/memory/`: target-local lessons.
- `.agents/docs/templates/agents/`: optional template-provider bundle for redeployment.

Target memory, decisions, agent ledger, status, Codex App config, local environment state, and validation history stay target-owned.
