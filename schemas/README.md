# Schemas
This directory owns machine-readable contracts for the Agents governance rules.
Initial scope:
- `.agents/docs/agents/workflows.yaml`
- `.agents/docs/agents/verify.yaml`
- `.agents/docs/agents/policy.yaml`
- `.agents/docs/agents/schemas.yaml`
- `.agents/docs/agents/version.yaml`
- `.agents/docs/agents/deploy.yaml`
- `.agents/docs/agents/openai-foundations.yaml`
- `.agents/docs/agents/org.yaml`
- `.agents/docs/agents/model-policy.yaml`
- `.agents/docs/agents/dispatch.yaml`
- `.agents/docs/agents/workflow-artifacts.yaml`
- `.agents/docs/agents/context-compact.yaml`
- `.agents/docs/agents/collaborators.yaml`
- `.agents/docs/agents/core-system.yaml`
- `.agents/docs/agents/runtime-execution.yaml`
- `.agents/docs/agents/provider-adapters.yaml`
- `.agents/docs/agents/route-packs.yaml`
- `.agents/docs/agents/knowledge-footprint.yaml`
Schema contracts are standard JSON Schema documents with a small supported
subset used by `scripts/validate.ps1`:
- top-level `required`
- `properties.schema.const`
- `x-required-paths`
- `x-required-values`
- `x-required-contains`
This gives the repo an immediate contract gate without adding package
dependencies. A fuller JSON Schema validator can replace the lightweight
runner without changing the schema ownership model.
The enterprise dispatch schemas keep organization structure, model tier policy,
and dispatch protocol machine-checkable without tying the workflow to one
future-sensitive model ID.
The workflow artifact, context compact, collaborator, foundation creation,
runtime execution, provider adapter, route pack, and knowledge footprint schemas
keep local packets, approval gates, collection reports, compact resume state,
named thread window assignments, state boundaries, latency controls, evaluation
gates, and close evidence machine-checkable while runtime evidence stays local.
