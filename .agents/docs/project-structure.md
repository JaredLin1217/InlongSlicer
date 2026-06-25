# Source Repo Structure
This source repo owns the canonical AI Agents workflow: repo-local routing,
deployable governance, project-local skills, runtime boundaries, validation
gates, deployment templates, and extension points.
## Read Order
1. `AGENTS.md`
2. `.agents/docs/agents/ai-runtime.yaml`
3. `.agents/docs/agents/workflows.yaml` when routing or multi-agent behavior matters
4. `.agents/docs/agents/org.yaml`, `.agents/docs/agents/model-policy.yaml`, and
   `.agents/docs/agents/dispatch.yaml` only for enterprise dispatch work
5. `.agents/docs/agents/workflow-artifacts.yaml` only for artifact-backed workflow work
6. `.agents/docs/agents/context-compact.yaml` only for compaction or resume work
7. `.agents/docs/agents/collaborators.yaml` only for Codex collaborator window work
8. `.agents/docs/agents/core-system.yaml` only for core runtime boundary work
9. `.agents/docs/agents/runtime-execution.yaml` only for execution evidence work
10. `.agents/docs/agents/provider-adapters.yaml` only for provider capability work
11. `.agents/docs/agents/route-packs.yaml` only for route pack work
12. `.agents/docs/agents/knowledge-footprint.yaml` only for cross-window resume work
13. `.agents/docs/agents/policy.yaml` for isolation or boundary claims
14. `.agents/docs/agents/verify.yaml` before claims, commits, deployments, or releases
15. `.agents/docs/agents/schemas.yaml` for assignments, reports, status, or templates
16. `.agents/docs/agents/openai-foundations.yaml` when foundation creation matters
17. `.agents/docs/agents/version.yaml` for core runtime version metadata
18. `.agents/docs/agents/deploy.yaml` for authorized target deployment
## Role Matrix
| Area | Role | Deploy policy |
|---|---|---|
| `AGENTS.md` | session router | deploy |
| `.agents/skills/project-isolation-workflow/` | project-local skill | deploy |
| `.agents/runtime/` | ignored coordination/runtime state | never deploy |
| `.agents/runtime/workflows/`, `.workflow/` | local workflow artifacts and import alias | never deploy |
| `.agents/runtime/collaborators.jsonl` | local collaborator window/thread state | never deploy |
| `.agents/docs/agents/*.yaml` | canonical governance rules | deploy |
| `.agents/docs/agents/org.yaml`, `.agents/docs/agents/model-policy.yaml`, `.agents/docs/agents/dispatch.yaml` | enterprise dispatch runtime | deploy |
| `.agents/docs/agents/workflow-artifacts.yaml` | supervised workflow artifact route | deploy |
| `.agents/docs/agents/context-compact.yaml` | context compact route | deploy |
| `.agents/docs/agents/collaborators.yaml` | collaborator window dispatch route | deploy |
| `.agents/docs/agents/core-system.yaml` | core runtime system boundary | deploy |
| `.agents/docs/agents/runtime-execution.yaml` | execution run evidence route | deploy |
| `.agents/docs/agents/provider-adapters.yaml` | provider capability and tier map route | deploy |
| `.agents/docs/agents/route-packs.yaml` | deterministic minimal route pack route | deploy |
| `.agents/docs/agents/knowledge-footprint.yaml` | cross-window resume evidence route | deploy |
| `.agents/docs/agents/openai-foundations.yaml` | foundation creation route | deploy |
| `.agents/docs/runbooks/*.md` | procedure entry points | mode-based deploy |
| `.agents/docs/templates/agents/` | source-neutral deploy bundle | `template_provider_mode` only |
| `docs/memory/`, `docs/decisions/` | provider-local knowledge | target-owned / do not deploy rows |
| `.agents/docs/agents/decisions/` | workflow structure decisions | provider source only |
| `schemas/`, `scripts/`, `tests/` | contracts, checks, and fixtures | provider source only until explicitly deployed |
| `artifacts/`, `.github/workflows/` | audits/evals and CI | provider source only |
| `.codex/`, status, validation records | local/runtime state | never deploy |
Do not deploy source `.agents/runtime/`, `.agents/runtime/workflows/`,
`.agents/runtime/executions/`, `.agents/runtime/knowledge/`,
`.agents/runtime/route-packs/`, `.agents/runtime/tool-evidence/`,
`.agents/runtime/deployments/`, `.agents/runtime/collaborators.jsonl`,
`.workflow/`, `.codex/config.toml`,
`.codex/environments/*.toml`, source memory rows, decisions, status,
live thread ids, collaborator window state, or validation history by default.
## Core Runtime Structure Rule
Core runtime structure changes must preserve mirror pairs, deployment rules,
schema contracts, runtime blocklists, and route-pack compactness until drift
checks are updated. Large moves of runbooks, templates, decisions, or memory docs
belong in dedicated changes, not mixed with validation, foundation, or CI work.
Simple answer, scoped edit, plain deploy, and release tasks still use the
minimal route from `.agents/docs/agents/ai-runtime.yaml` and should not load
organization, workflow artifact, context compact, collaborator, core-system,
runtime-execution, provider-adapter, route-pack, knowledge-footprint, or
foundation-creation files
unless the task is about those named routes.
