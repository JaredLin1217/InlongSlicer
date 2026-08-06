[CmdletBinding()]
param(
    [ValidateSet("Auto", "Fast", "Policy", "Full")]
    [string] $Profile = "Auto",
    [ValidateSet("Auto", "Off", "Required")]
    [string] $ContextMode = "Auto",
    [string[]] $Path,
    [switch] $Staged,
    [switch] $Score,
    [switch] $Quiet,
    [switch] $Explain,
    [switch] $SelfTest
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function ConvertTo-RepoPath {
    param([string] $Value)
    $normalized = $Value.Trim() -replace '\\', '/'
    while ($normalized.StartsWith('./', [System.StringComparison]::Ordinal)) {
        $normalized = $normalized.Substring(2)
    }
    return $normalized.TrimStart('/')
}

function Get-PathProfile {
    param([string] $Value)
    $normalized = ConvertTo-RepoPath $Value
    if ($normalized -match '^(schemas/|(?:\.agents/)?docs/(?:templates/agents/schemas/))' -or
        $normalized -match '^(?:\.agents/)?docs/(?:agents/version\.yaml)$' -or
        $normalized -match '^(?:\.agents/)?docs/(?:evidence/releases/)' -or
        $normalized -match '^\.github/workflows/' -or
        $normalized -match '^scripts/(validate[^/]*|deploy-agents-workflow|capture-runtime-evidence|export-release-package|invoke-agent-runtime|agents-cleanup)\.ps1$') {
        return "Full"
    }
    if ($normalized -eq "AGENTS.md" -or
        $normalized -match '^\.agents/skills/' -or
        $normalized -match '^(?:\.agents/)?docs/(agents/|templates/agents/|runbooks/|memory/|project-memory\.md$|memory-entry\.template\.md$)') {
        return "Policy"
    }
    return "Fast"
}

function Select-AutoProfile {
    param([string[]] $Paths)
    $rank = @{ Fast = 1; Policy = 2; Full = 3 }
    $selected = "Fast"
    foreach ($item in $Paths) {
        $candidate = Get-PathProfile $item
        if ($rank[$candidate] -gt $rank[$selected]) {
            $selected = $candidate
        }
    }
    return $selected
}

function Select-HigherProfile {
    param(
        [string] $Current,
        [string] $Recommended
    )
    $rank = @{ Fast = 1; Policy = 2; Full = 3 }
    if (-not $rank.ContainsKey($Recommended)) {
        return $Current
    }
    if ($rank[$Recommended] -gt $rank[$Current]) {
        return $Recommended
    }
    return $Current
}

function New-DegradedContextEvidence {
    param([string] $Gap)
    return [pscustomobject]@{
        route = "degraded"
        confidence = "low"
        gaps = @($Gap)
        verification_recommendation = [pscustomobject]@{
            profile = "Full"
            expand_context = $true
        }
        budget = [pscustomobject]@{
            selected_files = 0
            selected_bytes = 0
        }
    }
}

function Get-ContextEvidence {
    param([string[]] $Paths)
    if ($ContextMode -eq "Off") {
        return $null
    }

    $resolver = Join-Path $PSScriptRoot "resolve-agent-context.ps1"
    if (-not (Test-Path -LiteralPath $resolver -PathType Leaf)) {
        if ($ContextMode -eq "Required") {
            throw "Context evidence is required, but scripts/resolve-agent-context.ps1 is missing."
        }
        if (-not $Quiet) {
            Write-Host "[WARN] Context resolver is missing; expanding validation to Full."
        }
        return New-DegradedContextEvidence -Gap "degraded:resolver-missing"
    }

    $task = if (@($Paths).Count -gt 0) {
        "Validate impact for {0} changed paths" -f @($Paths).Count
    }
    else {
        "Validate repository changes"
    }
    try {
        $json = & $resolver -Task $task -ChangedPath $Paths -MaxFiles 3 -BudgetBytes 8192 -Format Json
        if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace(($json -join ""))) {
            throw "Context resolver returned no evidence."
        }
        $evidence = ($json -join [Environment]::NewLine) | ConvertFrom-Json
        if ([string]$evidence.schema_version -ne "agents-context-evidence/v1") {
            throw "Unexpected context evidence schema: $($evidence.schema_version)"
        }
        if (@($evidence.relevant_files).Count -gt 3 -or [int64]$evidence.budget.selected_bytes -gt 8192) {
            throw "Context evidence exceeded the 3-file or 8192-byte budget."
        }
        if ([string]::IsNullOrWhiteSpace([string]$evidence.route) -or
            [string]::IsNullOrWhiteSpace([string]$evidence.verification_recommendation.profile)) {
            throw "Context evidence is missing route or verification recommendation."
        }
        return $evidence
    }
    catch {
        if ($ContextMode -eq "Required") {
            throw
        }
        if (-not $Quiet) {
            Write-Host ("[WARN] Context resolver degraded; expanding validation to Full: {0}" -f $_.Exception.Message)
        }
        return New-DegradedContextEvidence -Gap "degraded:resolver-failure"
    }
}

function Get-ChangedPaths {
    if ($Path -and @($Path).Count -gt 0) {
        return @($Path | ForEach-Object { ConvertTo-RepoPath $_ } | Sort-Object -Unique)
    }
    $items = New-Object 'System.Collections.Generic.List[string]'
    $commands = if ($Staged) {
        @(@("diff", "--cached", "--name-only", "--diff-filter=ACMRTUXB"))
    }
    else {
        @(
            @("diff", "--name-only", "--diff-filter=ACMRTUXB"),
            @("diff", "--cached", "--name-only", "--diff-filter=ACMRTUXB"),
            @("ls-files", "--others", "--exclude-standard")
        )
    }
    foreach ($arguments in $commands) {
        $result = Invoke-GitCommand -GitArguments $arguments -SuppressStandardError
        if ($result.ExitCode -ne 0) {
            throw "Unable to discover changed paths with git $($arguments -join ' ')."
        }
        foreach ($item in @($result.Output)) {
            if (-not [string]::IsNullOrWhiteSpace($item)) {
                $items.Add((ConvertTo-RepoPath $item)) | Out-Null
            }
        }
    }
    return @($items | Sort-Object -Unique)
}

function Invoke-GitCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string[]] $GitArguments,
        [switch] $SuppressStandardError
    )
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        if ($SuppressStandardError) {
            $output = @(& git -c core.quotepath=false -c core.safecrlf=false @GitArguments 2>$null)
        }
        else {
            $output = @(& git -c core.quotepath=false -c core.safecrlf=false @GitArguments 2>&1)
        }
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    return [pscustomobject]@{
        Output = @($output)
        ExitCode = $exitCode
    }
}

function Invoke-FastChecks {
    param([string[]] $Paths)
    $failures = New-Object 'System.Collections.Generic.List[string]'
    $scopeArguments = if (@($Paths).Count -gt 0) { @("--") + @($Paths) } else { @() }
    $diffArguments = if ($Staged) { @("diff", "--cached", "--check") } else { @("diff", "--check") }
    $diffResult = Invoke-GitCommand -GitArguments (@($diffArguments) + @($scopeArguments))
    if ($diffResult.ExitCode -ne 0) {
        $failures.Add(("git {0} failed: {1}" -f ($diffArguments -join ' '), (@($diffResult.Output) -join [Environment]::NewLine))) | Out-Null
    }
    if (-not $Staged) {
        $cachedArguments = @("diff", "--cached", "--check") + $scopeArguments
        $cachedResult = Invoke-GitCommand -GitArguments $cachedArguments
        if ($cachedResult.ExitCode -ne 0) {
            $failures.Add(("git diff --cached --check failed: {0}" -f (@($cachedResult.Output) -join [Environment]::NewLine))) | Out-Null
        }
    }

    foreach ($repoPath in $Paths) {
        $fullPath = Join-Path $RepoRoot ($repoPath -replace '/', [System.IO.Path]::DirectorySeparatorChar)
        if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
            continue
        }
        switch ([System.IO.Path]::GetExtension($fullPath).ToLowerInvariant()) {
            ".json" {
                try {
                    Get-Content -LiteralPath $fullPath -Raw | ConvertFrom-Json | Out-Null
                }
                catch {
                    $failures.Add(("JSON parse failed for {0}: {1}" -f $repoPath, $_.Exception.Message)) | Out-Null
                }
            }
            ".ps1" {
                $tokens = $null
                $errors = $null
                [System.Management.Automation.Language.Parser]::ParseFile($fullPath, [ref] $tokens, [ref] $errors) | Out-Null
                foreach ($parseError in @($errors)) {
                    $failures.Add(("PowerShell parse failed for {0}: {1}" -f $repoPath, $parseError.Message)) | Out-Null
                }
            }
        }
    }
    if ($failures.Count -gt 0) {
        foreach ($failure in $failures) {
            Write-Host ("[FAIL] {0}" -f $failure)
        }
        return $false
    }
    if (-not $Quiet) {
        Write-Host "[PASS] Changed files passed diff and syntax checks."
    }
    return $true
}

function Test-ProfileSelection {
    $cases = @(
        @{ Paths = @("src/app.ts"); Expected = "Fast" },
        @{ Paths = @("AGENTS.md"); Expected = "Policy" },
        @{ Paths = @(".agents/docs/memory/index.md"); Expected = "Policy" },
        @{ Paths = @(".agents/docs/memory/index.md"); Expected = "Policy" },
        @{ Paths = @("schemas/example.schema.json"); Expected = "Full" },
        @{ Paths = @("src/app.ts", ".agents/docs/agents/version.yaml"); Expected = "Full" },
        @{ Paths = @(".agents/docs/agents/version.yaml"); Expected = "Full" },
        @{ Paths = @(".agents/docs/templates/agents/schemas/agents-version.schema.json"); Expected = "Full" },
        @{ Paths = @("scripts/validate-foundation.ps1"); Expected = "Full" }
    )
    foreach ($case in $cases) {
        $actual = Select-AutoProfile $case.Paths
        if ($actual -ne $case.Expected) {
            throw "Profile selection self-test failed: expected $($case.Expected), got $actual for $($case.Paths -join ', ')."
        }
    }
    if (-not $Quiet) {
        Write-Host "[PASS] Change-aware profile selection self-test passed."
    }
}

Push-Location $RepoRoot
try {
    if ($SelfTest) {
        Test-ProfileSelection
        exit 0
    }
    $changedPaths = @(Get-ChangedPaths)
    $selectedProfile = if ($Profile -eq "Auto") { Select-AutoProfile $changedPaths } else { $Profile }
    $contextEvidence = Get-ContextEvidence $changedPaths
    if ($null -ne $contextEvidence) {
        $selectedProfile = Select-HigherProfile `
            $selectedProfile `
            ([string] $contextEvidence.verification_recommendation.profile)
    }
    if ($Explain -or -not $Quiet) {
        Write-Host ("[INFO] Validation profile: {0} (requested: {1}, context: {2}, changed paths: {3})" -f $selectedProfile, $Profile, $ContextMode, @($changedPaths).Count)
        if ($null -ne $contextEvidence) {
            Write-Host ("[INFO] Context route: {0}; confidence: {1}; recommendation: {2}" -f `
                $contextEvidence.route, `
                $contextEvidence.confidence, `
                $contextEvidence.verification_recommendation.profile)
        }
        if ($Explain) {
            foreach ($item in $changedPaths) {
                Write-Host ("  {0}: {1}" -f (Get-PathProfile $item), $item)
            }
        }
    }
    if ($selectedProfile -eq "Fast") {
        if (-not (Invoke-FastChecks $changedPaths)) {
            exit 1
        }
        exit 0
    }

    & (Join-Path $PSScriptRoot "validate.ps1") `
        -Full:($selectedProfile -eq "Full") `
        -Score:$Score `
        -Quiet:$Quiet
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
