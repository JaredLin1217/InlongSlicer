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
$guidance = @((@($tracked | Where-Object { $_ -match '(^|/)(AGENTS(?:\.override)?|CLAUDE)\.md$' }) +
    @('AGENTS.override.md')) | Sort-Object -Unique)
foreach ($path in $guidance) {
    if (-not (Test-Path -LiteralPath (Join-Path $Root $path) -PathType Leaf)) {
        throw "Missing rule entry point: $path"
    }
}
foreach ($entry in @(@{path='CLAUDE.md';target='@AGENTS.override.md'},
    @{path='tests/CLAUDE.md';target='@AGENTS.md'})) {
    if ((Get-Content -LiteralPath (Join-Path $Root $entry.path) -Raw).Trim() -ne $entry.target) {
        $path = $entry.path
        throw "Claude must share the applicable Agents entry point: $path"
    }
}
if (@($manifest.files | Where-Object path -EQ 'AGENTS.override.md').Count) {
    throw 'The project override must remain outside the deployed managed ownership map.'
}
$override = Get-Content -LiteralPath (Join-Path $Root 'AGENTS.override.md') -Raw
foreach ($reference in @('AGENTS.md', 'docs/agents/consumer-publication-policy.json',
    '-Scope Consumer -Profile Checkpoint')) {
    if (-not $override.Contains($reference)) {
        throw "The project override omits required guidance: $reference"
    }
}

function Assert-PublicationPolicy($Actual, $Expected, [string]$Location = 'publication') {
    if ($Actual -isnot [Collections.IDictionary] -or $Actual.Count -ne $Expected.Count) {
        throw "Invalid Consumer publication policy structure: $Location"
    }
    foreach ($key in $Expected.Keys) {
        if (-not $Actual.Contains($key)) { throw "Missing Consumer publication policy value: $Location.$key" }
        if ($Expected[$key] -is [Collections.IDictionary]) {
            Assert-PublicationPolicy $Actual[$key] $Expected[$key] "$Location.$key"
        } elseif ($Actual[$key].GetType() -ne $Expected[$key].GetType() -or $Actual[$key] -cne $Expected[$key]) {
            throw "Invalid Consumer publication policy value: $Location.$key"
        }
    }
}
$policy = Get-Content -LiteralPath (Join-Path $Root 'docs/agents/consumer-publication-policy.json') -Raw |
    ConvertFrom-Json -AsHashtable
Assert-PublicationPolicy $policy @{
    schema_version='inlong-consumer-publication/v1';project_role='Consumer'
    validation=@{scope='Consumer';profile='Checkpoint';require_product_checks=$true;require_guidance_audit=$true
        require_release_ready=$false;consumer_release_ready_false_requires_exception=$false}
    authorization=@{current_explicit_push_request_authorizes_push=$true;repeat_confirmation_for_approved_push=$false
        commit_only_request_authorizes_push=$false;historical_approval_authorizes_new_push=$false}
    commits=@{require_source_correspondence=$true;prefer_one_coherent_commit=$true;separate_provider_evidence_commit_required=$false}
    remote=@{require_fresh_observation=$true;allow_force_push=$false;verify_remote_head_after_push=$true}
    provider_release=@{applies_to='AI Agents distribution releases from a Provider checkout';scope='Provider';require_release_ready=$true}
}
foreach ($path in @(
    '.agents/skills/project-memory/SKILL.md',
    '.agents/skills/project-checkpoint/SKILL.md',
    '.agents/skills/project-runtime/SKILL.md',
    '.agents/skills/inlong-branding-migration/SKILL.md',
    '.agents/docs/project-structure.md', '.agents/docs/project-memory.md',
    '.agents/docs/runbooks/agents-operator-guide.md',
    'AGENTS.override.md', 'docs/agents/consumer-publication-policy.json',
    '.agents/docs/agents/runtime-policy.json',
    'tests/CATCH2.md', 'tests/fff_print/test_helpers.hpp',
    'scripts/inlong_extra_profile_check.py',
    'InlongSlicer_doc/orcaslicer_to_inlongslicer_migration.md'
)) {
    if (-not (Test-Path -LiteralPath (Join-Path $Root $path) -PathType Leaf)) {
        throw "Guidance references a missing project resource: $path"
    }
}

$projectGuidance = @('AGENTS.override.md', '.agents/docs/project-structure.md', '.agents/docs/project-memory.md',
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
Write-Output 'PASS: Consumer publication, current-request authorization, source verification and non-force remote checks; Provider release gate retained.'
Write-Output 'Boundary: this is guidance validation, not a functional audit of every product or vendor file.'
