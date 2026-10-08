# Project memory

Use the deployed project-memory skill and resolve tools through
`.agents/managed.json`. Run `.agents/scripts/project-memory.ps1 -Action Recall
-Query <terms>` before relying on a previous finding. Add `-Expand` when source
and verification details are needed; an index is never an authority.

Reviewed reusable findings use immutable v4 JSON records under
`docs/memory/entries/`, with source locators, hashes, verification and freshness.
Older Markdown entries in that directory are historical references only. They
are not automatically promoted or treated as verified current knowledge.
Reinspect their original sources before preparing a reviewed replacement.

Keep assumptions and unfinished work in registered `.agents/runtime/runs/`
payloads. Save task revisions through `.agents/scripts/task-state.ps1` with
acceptance criteria and an expected revision. Resume rechecks actual Git and
files; it never authorizes replaying an external action.

Follow `.agents/docs/runbooks/agents-operator-guide.md` for migration and the
versioned `.agents/docs/agents/runtime-policy.json` for normal retention.
Do not edit native/global Codex memory or settings, and do not automatically
share project knowledge.
