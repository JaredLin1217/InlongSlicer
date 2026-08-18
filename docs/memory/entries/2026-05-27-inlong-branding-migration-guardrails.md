# Inlong Branding Migration Guardrails
ID: M005
Date: 2026-05-27
Title: Inlong branding migration guardrails
Status: active
Confidence: high
Source Commit: 48d061e9c54a79704079e2e20488e6c686c99145
Content Hash: a1ad9506783b6b193a9e0fe1b143bf96aa74abbbfdbf7d20361cc6941646a602
Checked At: 2026-08-06T04:48:26Z
Last Verified: 2026-08-06
Next Review Due: 2026-11-04
Update Trigger: Product identity, external service contracts, packaging names, or branding migration policy changes
Supersedes: none
Boundary: InlongSlicer branding and upstream migration surfaces; documented external Orca contracts remain exempt
Source Refs: .agents/skills/inlong-branding-migration/SKILL.md; InlongSlicer_doc/orcaslicer_to_inlongslicer_migration.md; version.inc; CMakeLists.txt; src/CMakeLists.txt

## Trigger
Use this lesson before upstream syncs or changes to branding, profiles, packaging, assets, translations, or runtime resources.

## Context
Inlong identity spans source, build targets, profiles, localization, packaging, and generated resources, while some Orca names are required external contracts or dependency references.

## Cause
Blind text replacement can break service endpoints, package identity, profile inheritance, generated resources, or upstream traceability.

## Fix / Rule
Read the project-local branding skill and migration record before editing. Classify Orca occurrences as replacement, documented allowlist, generated state, or false positive. Preserve allowlisted external contracts unless a separate product change supplies a replacement plan.

## Verification
The current checkout contains the branding skill and migration record. Current build contracts include `InlongSlicer`, `InlongSlicer_profile_validator`, and `install_only_inlong` targets.

## Evidence
Use the skill's scoped scan and validation sequence, including the Inlong profile checker and validator for profile edits and staged-runtime verification for packaging or resource changes.

## Reuse when
Merging upstream, auditing residual Orca names, changing product identity, packaging, profiles, translations, icons, colors, or web resources.

## Index Row
| ID | Date | Title | Trigger | Keywords | Summary | Entry | Status | Confidence | Source Commit | Content Hash | Checked At | Last Verified | Next Review Due | Update Trigger | Supersedes | Boundary | Source Refs |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| M005 | 2026-05-27 | Inlong branding migration guardrails | Upstream sync or branding-sensitive edit | branding, upstream, packaging, identity | Use the project skill and preserve documented external contracts | `entries/2026-05-27-inlong-branding-migration-guardrails.md` | active | high | 48d061e9c54a79704079e2e20488e6c686c99145 | a1ad9506783b6b193a9e0fe1b143bf96aa74abbbfdbf7d20361cc6941646a602 | 2026-08-06T04:48:26Z | 2026-08-06 | 2026-11-04 | Product identity, external service contracts, packaging names, or branding migration policy changes | none | InlongSlicer branding and upstream migration surfaces; documented external Orca contracts remain exempt | .agents/skills/inlong-branding-migration/SKILL.md; InlongSlicer_doc/orcaslicer_to_inlongslicer_migration.md; version.inc; CMakeLists.txt; src/CMakeLists.txt |
