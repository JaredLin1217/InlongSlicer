param(
    [switch]$Full
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $repoRoot

$failures = New-Object System.Collections.Generic.List[string]

function Add-Failure {
    param([string]$Message)
    $failures.Add($Message) | Out-Null
}

function Assert-Path {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        Add-Failure "Missing required path: $Path"
    }
}

function Assert-NoPattern {
    param(
        [string]$Pattern,
        [string[]]$Paths,
        [string]$Message
    )

    $hits = Select-String -Path $Paths -Pattern $Pattern -AllMatches -ErrorAction SilentlyContinue
    if ($hits) {
        $sample = ($hits | Select-Object -First 5 | ForEach-Object { "$($_.Path):$($_.LineNumber)" }) -join ", "
        Add-Failure "$Message ($sample)"
    }
}

function Assert-Mirror {
    param(
        [string]$Source,
        [string]$Mirror
    )

    Assert-Path $Source
    Assert-Path $Mirror
    if ((Test-Path -LiteralPath $Source) -and (Test-Path -LiteralPath $Mirror)) {
        $sourceHash = (Get-FileHash -LiteralPath $Source).Hash
        $mirrorHash = (Get-FileHash -LiteralPath $Mirror).Hash
        if ($sourceHash -ne $mirrorHash) {
            Add-Failure "Mirror drift: $Source != $Mirror"
        }
    }
}

function Assert-NoUtf8Bom {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }

    $bytes = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Path))
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        Add-Failure "UTF-8 BOM present: $Path"
    }
}

$required = @(
    "AGENTS.md",
    ".agents/docs/agents/ai-runtime.yaml",
    ".agents/docs/agents/deploy.yaml",
    ".agents/docs/agents/dispatch.yaml",
    ".agents/docs/agents/model-policy.yaml",
    ".agents/docs/agents/org.yaml",
    ".agents/docs/agents/policy.yaml",
    ".agents/docs/agents/schemas.yaml",
    ".agents/docs/agents/verify.yaml",
    ".agents/docs/agents/version.yaml",
    ".agents/docs/agents/workflows.yaml",
    ".agents/skills/project-isolation-workflow/SKILL.md"
)

$required | ForEach-Object { Assert-Path $_ }

$mirrorPairs = @(
    @(".agents/docs/agents/ai-runtime.yaml", ".agents/docs/templates/agents/agents/ai-runtime.yaml"),
    @(".agents/docs/agents/deploy.yaml", ".agents/docs/templates/agents/agents/deploy.yaml"),
    @(".agents/docs/agents/dispatch.yaml", ".agents/docs/templates/agents/agents/dispatch.yaml"),
    @(".agents/docs/agents/model-policy.yaml", ".agents/docs/templates/agents/agents/model-policy.yaml"),
    @(".agents/docs/agents/org.yaml", ".agents/docs/templates/agents/agents/org.yaml"),
    @(".agents/docs/agents/policy.yaml", ".agents/docs/templates/agents/agents/policy.yaml"),
    @(".agents/docs/agents/schemas.yaml", ".agents/docs/templates/agents/agents/schemas.yaml"),
    @(".agents/docs/agents/verify.yaml", ".agents/docs/templates/agents/agents/verify.yaml"),
    @(".agents/docs/agents/version.yaml", ".agents/docs/templates/agents/agents/version.yaml"),
    @(".agents/docs/agents/workflows.yaml", ".agents/docs/templates/agents/agents/workflows.yaml"),
    @(".agents/docs/runbooks/isolation-audit.md", ".agents/docs/templates/agents/isolation-audit.md"),
    @(".agents/docs/runbooks/multi-agent-workflow.md", ".agents/docs/templates/agents/multi-agent-workflow.md"),
    @(".agents/docs/project-memory.md", ".agents/docs/templates/agents/project-memory.md"),
    @(".agents/docs/project-structure.md", ".agents/docs/templates/agents/project-structure.md"),
    @(".agents/docs/memory-entry.template.md", ".agents/docs/templates/agents/memory-entry.template.md"),
    @(".agents/skills/project-isolation-workflow/SKILL.md", ".agents/docs/templates/agents/skills/project-isolation-workflow/SKILL.md")
)

$mirrorPairs | ForEach-Object { Assert-Mirror $_[0] $_[1] }

$agentPaths = @(
    $required
    ($mirrorPairs | ForEach-Object { $_[1] })
) | ForEach-Object { (Resolve-Path -LiteralPath $_).Path } | Sort-Object -Unique

$agentPaths | ForEach-Object {
    Assert-NoUtf8Bom $_
}

Assert-NoPattern "\.agents/\.agents" $agentPaths "Invalid nested .agents deployment path"
Assert-NoPattern "backup_path|backup_count_match|backup DB/state|move child rollouts" $agentPaths "Legacy backup-based cleanup flow remains"

$aiRuntime = Get-Content -LiteralPath ".agents/docs/agents/ai-runtime.yaml" -Raw
foreach ($needle in @("enterprise_dispatch", "org.yaml", "model-policy.yaml", "dispatch.yaml")) {
    if ($aiRuntime -notmatch [regex]::Escape($needle)) {
        Add-Failure "ai-runtime.yaml missing enterprise dispatch route item: $needle"
    }
}

$deploy = Get-Content -LiteralPath ".agents/docs/agents/deploy.yaml" -Raw
foreach ($needle in @(
    ".agents/docs/templates/agents/agents/org.yaml",
    ".agents/docs/templates/agents/agents/model-policy.yaml",
    ".agents/docs/templates/agents/agents/dispatch.yaml"
)) {
    if ($deploy -notmatch [regex]::Escape($needle)) {
        Add-Failure "deploy.yaml missing enterprise template mapping: $needle"
    }
}

$org = Get-Content -LiteralPath ".agents/docs/agents/org.yaml" -Raw
$modelPolicy = Get-Content -LiteralPath ".agents/docs/agents/model-policy.yaml" -Raw
$dispatch = Get-Content -LiteralPath ".agents/docs/agents/dispatch.yaml" -Raw

foreach ($department in @("executive_office", "pmo", "architecture", "engineering", "devops", "qa", "security", "documentation", "provider_management")) {
    if ($org -notmatch "(?m)^  ${department}:") {
        Add-Failure "org.yaml missing department: $department"
    }
    if ($modelPolicy -notmatch "(?m)^  ${department}:") {
        Add-Failure "model-policy.yaml missing department binding: $department"
    }
}

foreach ($needle in @("controller_target_rule", "department_report", "escalation_record", "level_6", "count_authority_rule", "Project-local .agents/skills/** is not GS")) {
    if ($dispatch -notmatch [regex]::Escape($needle)) {
        Add-Failure "dispatch.yaml missing enterprise guardrail: $needle"
    }
}

if ($failures.Count -gt 0) {
    Write-Host "Agents validation failed:" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host " - $_" -ForegroundColor Red }
    exit 1
}

if ($Full) {
    Write-Host "Agents validation passed (full profile)."
} else {
    Write-Host "Agents validation passed."
}
