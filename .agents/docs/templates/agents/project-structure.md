# Source Repo Structure

This source repo owns the canonical AI Agents workflow: repo-local routing,
deployable governance, project-local skills, runtime boundaries, validation
gates, deployment templates, and extension points.

## Read Order

1. `AGENTS.md`
2. `.agents/docs/agents/ai-runtime.yaml`
3. `.agents/docs/agents/workflows.yaml` only when routed
4. `.agents/docs/agents/policy.yaml` only when routed
5. `.agents/docs/agents/verify.yaml` for the selected profile
6. `.agents/docs/agents/schemas.yaml` for assignments, reports, status, or templates
7. `.agents/docs/agents/org.yaml`, `.agents/docs/agents/model-policy.yaml`, and `.agents/docs/agents/dispatch.yaml` only for enterprise dispatch
8. `.agents/docs/agents/mcp.yaml` when optional integrations matter
9. `.agents/docs/agents/version.yaml` for compatibility
10. `.agents/docs/agents/deploy.yaml` for authorized target deployment

## Role Matrix

| Area | Role | Deploy policy |
|---|---|---|
| `AGENTS.md` | session router | deploy |
| `.agents/skills/project-isolation-workflow/` | project-local skill | deploy |
| `.agents/runtime/` | ignored coordination/runtime state | never deploy |
| `.agents/docs/agents/*.yaml` | canonical governance rules | deploy |
| `.agents/docs/runbooks/*.md` | procedure entry points | mode-based deploy |
| `.agents/docs/templates/agents/` | source-neutral deploy bundle | `template_provider_mode` only |
| `.agents/docs/memory/`, `.agents/docs/decisions/` | provider-local knowledge | target-owned / do not deploy rows |
| `.agents/docs/decisions/` | workflow structure decisions | provider source only |
| `schemas/`, `scripts/`, `tests/`, `mcp/` | contracts, checks, fixtures, capability registry | provider source only until explicitly deployed |
| `artifacts/`, `.github/workflows/` | audits/evals and CI | provider source only |
| `.codex/`, status, validation records | local/runtime state | never deploy |

Do not deploy source `.agents/runtime/`, `.codex/config.toml`,
`.codex/environments/environment.toml`, source memory rows, decisions, status,
or validation history by default.

## V2 Structure Rule

V2 structure changes must preserve mirror pairs and deployment rules until drift
checks are updated. Large moves of runbooks, templates, decisions, or memory docs
belong in dedicated changes, not mixed with validation, MCP, or CI work.

## Current Agents Tree

```text
AGENTS.md
.agents/
  docs/
    agents/
      ai-runtime.yaml
      policy.yaml
      workflows.yaml
      verify.yaml
      schemas.yaml
      deploy.yaml
      mcp.yaml
      version.yaml
      org.yaml
      model-policy.yaml
      dispatch.yaml
    runbooks/
      agents-deployment.md
      isolation-audit.md
      multi-agent-workflow.md
      repository-maintenance.md
      session-handoff.md
      skill-authoring.md
      task-closeout.md
    templates/agents/
      AGENTS.md
      agents/*.yaml
      runbook mirrors
      template mirrors
      skills/project-isolation-workflow/
    memory/
      index.md
      entries/
    decisions/
    *.template.md
    project-memory.md
    project-structure.md
  skills/
    project-isolation-workflow/
  runtime/
    agent-ledger.jsonl  # ignored, advisory only
```

## Flow Summary

1. `AGENTS.md` gives the compact always-on rules and points to `ai-runtime.yaml`.
2. `ai-runtime.yaml` classifies the request and expands only named canonical YAML.
3. `workflows.yaml` owns task flow, progress updates, multi-agent lifecycle, scoring, handoff, deployment delegation, and maintenance routing.
4. `policy.yaml` owns authority, boundaries, project-local knowledge layers, template cleanliness, and closeout.
5. `verify.yaml` selects the smallest proof profile; commit and branch push use `commit_tag_checkpoint`.
6. `org.yaml`, `model-policy.yaml`, and `dispatch.yaml` add the optional enterprise dispatch overlay: controller to department leaders, leaders to internal workers, and department reports back to controller.
7. `deploy.yaml` builds the allowlisted `deployed_file_set`, preserves target-owned state, and rewrites paths only for the selected target layout.
8. Template mirrors under `.agents/docs/templates/agents/` must match canonical sources unless marked template-specific.
