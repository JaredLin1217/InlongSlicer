# Repository Maintenance

Use when cleaning, slimming, validating, or restructuring this Agents repo.

1. Inspect git state.
2. Keep `AGENTS.md`, project skills, runbooks, and templates compact.
3. Keep runtime/local status, temp events, environment state, caches, generated output, and filled evidence out of commits unless targeted.
4. Do not run OS permission, ownership, ACL, or `.git` metadata repair commands as an automated maintenance step. If Git cannot write its index or lock files, report a local workspace blocker and wait for a writable repo state.
5. Use `.agents/docs/agents/verify.yaml` profile names as the verification source; ordinary commit/tag uses fast checkpoint, not full audit.

References: `.agents/docs/agents/workflows.yaml`, `.agents/docs/agents/verify.yaml`.
