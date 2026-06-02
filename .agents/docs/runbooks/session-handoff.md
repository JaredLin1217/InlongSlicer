# Session Handoff
Use when work may continue across sessions, windows, employees, or app restarts.
Trigger on session/window/app restart, employee recovery, or uncertain continuation state.
1. Inspect git state.
2. If employees may exist, inspect `.agents/runtime/agent-ledger.jsonl` first.
3. Inspect the exact temp handoff cache only when external fallback is required, and report XR.
4. Treat ledger and temp roster as advisory; recover from git, runtime handles, final reports, temp events, and controller lease.
5. If recovery is incomplete, mark state `unknown`; do not close by nickname alone.
6. Report recovered sources, unknowns, ledger status, and XR for temp cache access.
References: `.agents/docs/agents/workflows.yaml`, `.agents/docs/agents/schemas.yaml`.
