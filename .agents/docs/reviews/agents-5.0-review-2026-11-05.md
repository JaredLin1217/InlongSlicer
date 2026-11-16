# Agents 5.0 review

Reviewed on 2026-11-05 as a v5 release review. The v4 checkout at `46d132f`
was the baseline used for the historical comparison; the resulting release is
the `agents-project/v5` contract. The project purpose remains a portable
governance workflow for authorized engineering: versioned rules, checked
knowledge, recoverable work and owned deployment changes.

## Decisions

| Area | Keep | Remove or replace |
|---|---|---|
| Project purpose | Rules, knowledge, task recovery and deployment ownership | Hosted multi-tenant service, billing and background memory service are outside this release |
| Instructions | Short root guidance, scoped skills and native precedence | Retired orchestration, host pinning, worker-count, compaction and scoring rules |
| Memory | Immutable source-checked records and explicit retirement | Native memory as an authority or permission source |
| Tasks | Revisioned goals, acceptance, receipts, Git/file snapshots and action history | Silent v3/v4 fallback in the active v5 reader |
| Runtime | Registered runs, state, ledger, retention, cleanup preview and tombstones | Wholesale runtime clearing and unregistered payloads |
| Distribution | Cross-platform PowerShell CLI and Codex plugin | Implicit hook activation, telemetry and secrets in packages |
| Validation | Plan, affected checks, one delivery checkpoint and committed evidence | Full regression on every edit and synthetic quality scores |

## History and drift

Agents 1.x established isolation and reusable rules. Agents 2.x added ledgers,
workflow coordination and deployment readiness. Agents 3.x removed mandatory
routing, host probes, comparison runners and synthetic scoring in favor of native
Codex skills. Agents 4.x added immutable knowledge, task revisions, runtime
retention and release evidence. v5 keeps the purpose while removing the remaining
legacy readers and packaging the workflow for repeatable cross-project use.

The baseline lacked a unified CLI, portable plugin manifest and v5 migration
boundary. The v5 release adds those contracts, keeps historical schemas under
`docs/history` and `migrations`, and refreshes the official content hashes as
of 2026-11-04 UTC. Final source and release readiness are bound to the checked-in
v5 release evidence rather than this baseline description.

## Official boundary

GPT-6.1 Sol supports low, medium, high, xhigh and max reasoning and requires the
Responses API for tool calling. These are API facts, not a forced Codex host
configuration. The GPT-6 prompting guide is a family starting point based on
Astra observations; it is not measured Sol behavior.

Codex instruction files merge by directory and support overrides. Skills use
progressive disclosure and concise descriptions. Native memory is a local,
generated recall layer with host storage controls. Hooks use trusted JSON event
handlers and must not depend on transcript internals. Plugins use a portable root
manifest with a Codex compatibility overlay; lifecycle-hook bundles remain local
opt-in artifacts rather than public plugin content.

## Commercial readiness boundary

v5 makes the package supportable and distributable without pretending to be a
hosted SaaS product. The release must provide a stable CLI, JSON/exit contracts,
v4 migration, root/dot layout support, rollback, deterministic plugin packaging,
platform support evidence, privacy-safe diagnostics, no telemetry by default and
clear support/security documents. External Consumer upgrades remain explicit.
