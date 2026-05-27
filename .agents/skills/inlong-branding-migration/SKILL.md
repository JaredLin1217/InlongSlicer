# Inlong Branding Migration

Use this skill whenever work may bring upstream OrcaSlicer naming, assets,
colors, profiles, packaging, or runtime identity back into InlongSlicer.

## Required Reference

Before editing, read:

- `InlongSlicer_doc/orcaslicer_to_inlongslicer_migration.md`
- `AGENTS.md`

Treat the migration document as the source of truth. This skill is only the
entrypoint that reminds agents to use it.

## Workflow

1. Identify whether the task touches branding, build targets, package identity,
   profiles, translations, web resources, icons, colors, or source symbols.
2. Check the canonical identity table in the migration document.
3. Keep allowed Orca references intact unless the user explicitly approves
   changing external cloud/wiki/protocol behavior, dependency mirrors, or git
   base branch names.
4. Run the filename and content scans listed in the migration document.
5. For profile edits, run `scripts/inlong_extra_profile_check.py` and the
   `InlongSlicer_profile_validator` target when available.
6. For code/build edits, run the narrowest build target that proves the change.
7. Summarize any remaining Orca hits by category: replaced, allowed, generated
   stale output, or false positive.

## Brand Constants

- Product compact name: `InlongSlicer`
- Product display name: `Inlong Slicer`
- Command name: `inlong-slicer`
- App id: `io.github.JaredLin1217.InlongSlicer`
- Accent color: `#D66C47`, RGB `214, 108, 71`
- Dependency prefix: `InlongSlicer_dep`

Never rely on `build`, `deps/build`, or `.git` search results alone to decide
whether source files are still Orca branded.
