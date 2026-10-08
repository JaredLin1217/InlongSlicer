#requires -Version 7.0
param([string]$Root = (Split-Path -Parent $PSScriptRoot))

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath($Root)
$manifest = Get-Content -LiteralPath (Join-Path $Root '.agents/managed.json') -Raw |
    ConvertFrom-Json -AsHashtable
if ($manifest.version -ne '4.0.0' -or $manifest.layout -ne 'dot-agents-layout') {
    throw 'This check expects the reviewed Agents 4.0 dot-layout deployment.'
}
if (Test-Path -LiteralPath (Join-Path $Root 'agents.json')) {
    throw 'This product checkout must remain a Consumer, not a distribution Provider.'
}

$tracked = @(& git -C $Root -c core.quotepath=false ls-files)
if ($LASTEXITCODE -ne 0) { throw 'Cannot inventory the Git source tree.' }
$guidance = @($tracked | Where-Object { $_ -match '(^|/)(AGENTS|CLAUDE)\.md$' })
foreach ($path in $guidance) {
    if (-not (Test-Path -LiteralPath (Join-Path $Root $path) -PathType Leaf)) {
        throw "Missing rule entry point: $path"
    }
}
foreach ($path in @('CLAUDE.md', 'tests/CLAUDE.md')) {
    if ((Get-Content -LiteralPath (Join-Path $Root $path) -Raw).Trim() -ne '@AGENTS.md') {
        throw "Claude must share the applicable Agents entry point: $path"
    }
}
foreach ($path in @(
    '.agents/skills/project-memory/SKILL.md',
    '.agents/skills/project-checkpoint/SKILL.md',
    '.agents/skills/project-runtime/SKILL.md',
    '.agents/skills/inlong-branding-migration/SKILL.md',
    '.agents/docs/project-structure.md', '.agents/docs/project-memory.md',
    '.agents/docs/runbooks/agents-operator-guide.md',
    '.agents/docs/agents/runtime-policy.json',
    'tests/CATCH2.md', 'tests/fff_print/test_helpers.hpp',
    'scripts/inlong_extra_profile_check.py',
    'InlongSlicer_doc/orcaslicer_to_inlongslicer_migration.md'
)) {
    if (-not (Test-Path -LiteralPath (Join-Path $Root $path) -PathType Leaf)) {
        throw "Guidance references a missing project resource: $path"
    }
}

$projectGuidance = @('.agents/docs/project-structure.md', '.agents/docs/project-memory.md',
    '.claude/commands/commit-push-pr.md')
foreach ($path in $projectGuidance) {
    $text = Get-Content -LiteralPath (Join-Path $Root $path) -Raw
    if ($text -match 'AI Agents v3|ai-runtime\.yaml|knowledge-footprint\.yaml|workflows\.yaml|\.agents/docs/memory/index\.md|MUST do all of the above in a single message') {
        throw "Obsolete operational guidance remains: $path"
    }
}
foreach ($path in @('.agents/docs/memory/index.md',
    '.agents/docs/memory/entries/README.md',
    '.claude/loopspec/sketch-focus-arbiter.spec.md')) {
    if (Test-Path -LiteralPath (Join-Path $Root $path)) {
        throw "Retired guidance was reintroduced: $path"
    }
}

Write-Output "PASS: inventory of $($tracked.Count) tracked source paths; $($guidance.Count) rule entry points."
Write-Output 'PASS: Consumer layout, shared Claude rules, project resources and retired-guidance checks.'
Write-Output 'Boundary: this is guidance validation, not a functional audit of every product or vendor file.'
