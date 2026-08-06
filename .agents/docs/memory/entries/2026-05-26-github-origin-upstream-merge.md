# GitHub Origin And Upstream Merge Boundary
ID: M004
Date: 2026-05-26
Title: GitHub origin and upstream merge boundary
Status: active
Confidence: high
Source Commit: 48d061e9c54a79704079e2e20488e6c686c99145
Content Hash: 7e3977fd96f864a254bc95a284a05f3083b27fedbb6f9b843c11bba3c74fd3b7
Checked At: 2026-08-06T04:48:26Z
Last Verified: 2026-08-06
Next Review Due: 2026-11-04
Update Trigger: Remote URLs, default upstream branch, or integration branch changes
Supersedes: none
Boundary: This checkout and its current remotes; no authorization to push to upstream
Source Refs: .git/config; .agents/skills/inlong-branding-migration/SKILL.md; InlongSlicer_doc/orcaslicer_to_inlongslicer_migration.md

## Trigger
Use this lesson before fetching, merging, comparing, or publishing fork changes.

## Context
The current branch is `feature/inlong-slicer-initial`; `origin` is the InlongSlicer fork and `upstream` is OrcaSlicer.

## Cause
GitHub comparison language can blur the difference between discarding fork commits, merging upstream, and pushing to the official repository.

## Fix / Rule
Inspect branch, status, and remotes first. For the current upgrade path, fetch and merge the explicitly requested `upstream/main`; never reuse a historical release branch by default. Push only the intended local ref to `origin` after separate authorization and validation.

## Verification
Current remote inspection records `origin=https://github.com/JaredLin1217/InlongSlicer.git`, `upstream=https://github.com/OrcaSlicer/OrcaSlicer.git`, and branch `feature/inlong-slicer-initial`.

## Evidence
Use `git remote -v`, `git branch --show-current`, `git status --short`, and an explicit upstream ref before merge or publication.

## Reuse when
Explaining GitHub fork UI, selecting an upstream ref, protecting local changes during merge, or verifying publication scope.

## Index Row
| ID | Date | Title | Trigger | Keywords | Summary | Entry | Status | Confidence | Source Commit | Content Hash | Checked At | Last Verified | Next Review Due | Update Trigger | Supersedes | Boundary | Source Refs |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| M004 | 2026-05-26 | GitHub origin and upstream merge boundary | Fetch, merge, compare, or publish work | git, origin, upstream, merge | Inspect remotes and use the explicitly requested upstream ref | `entries/2026-05-26-github-origin-upstream-merge.md` | active | high | 48d061e9c54a79704079e2e20488e6c686c99145 | 7e3977fd96f864a254bc95a284a05f3083b27fedbb6f9b843c11bba3c74fd3b7 | 2026-08-06T04:48:26Z | 2026-08-06 | 2026-11-04 | Remote URLs, default upstream branch, or integration branch changes | none | This checkout and its current remotes; no authorization to push to upstream | .git/config; .agents/skills/inlong-branding-migration/SKILL.md; InlongSlicer_doc/orcaslicer_to_inlongslicer_migration.md |
