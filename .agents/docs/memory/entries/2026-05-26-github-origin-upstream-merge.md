# 2026-05-26 - GitHub Origin / Upstream Branch Clarification

- Trigger: When GitHub branch UI mentions discarding commits, or when the user asks whether commits were pushed only to the fork or to upstream.
- Context: Imported from authorized global Codex Memory for `D:\inlong\Slicer\GitHub\InlongSlicer`, branch `feature/inlong-slicer-initial`.
- Cause: GitHub comparison UI can make fork commits look like they were pushed to official upstream, and `Discard commits` can be misread as a sync or merge action.
- Fix / Rule: Explain the UI in plain language first, then separate `origin` from `upstream`, then give the exact safe command sequence. `Discard commits` means throw away fork branch commits; it does not merge upstream and does not mean those commits were pushed to official OrcaSlicer.
- Verification: Source memory recorded `origin=https://github.com/JaredLin1217/InlongSlicer.git` and `upstream=https://github.com/OrcaSlicer/OrcaSlicer.git` for this checkout at the time of inspection.
- Reuse when: The user asks about GitHub comparison screens, ahead/behind counts, `Discard commits`, fork-vs-upstream scope, or safe upstream merge commands.

## Safe Merge Pattern

Use this only after checking current branch and git state:

```powershell
git fetch upstream
git checkout feature/inlong-slicer-initial
git merge upstream/release/v2.4
git push origin feature/inlong-slicer-initial
```

State explicitly whether the intended upstream target is `upstream/release/v2.4` or `upstream/main`; the risk and conflict profile differ.

## Source

- Imported from authorized global Codex Memory on 2026-05-29.
- Source category: summarized global memory plus an InlongSlicer GitHub origin/upstream rollout summary.
