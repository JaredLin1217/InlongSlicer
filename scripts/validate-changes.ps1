[CmdletBinding()]
param(
    [ValidateSet("Auto", "Fast", "Policy", "Full")]
    [string] $Profile = "Auto",
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

function Get-ChangedPaths {
    if ($Path -and $Path.Count -gt 0) {
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
        $output = @(& git -c core.quotepath=false @arguments)
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to discover changed paths with git $($arguments -join ' ')."
        }
        foreach ($item in $output) {
            if (-not [string]::IsNullOrWhiteSpace($item)) {
                $items.Add((ConvertTo-RepoPath $item)) | Out-Null
            }
        }
    }
    return @($items | Sort-Object -Unique)
}

function Invoke-FastChecks {
    param([string[]] $Paths)
    $failures = New-Object 'System.Collections.Generic.List[string]'
    $scopeArguments = if ($Paths.Count -gt 0) { @("--") + @($Paths) } else { @() }
    $diffArguments = if ($Staged) { @("diff", "--cached", "--check") } else { @("diff", "--check") }
    $diffOutput = @(& git -c core.quotepath=false @diffArguments @scopeArguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        $failures.Add(("git {0} failed: {1}" -f ($diffArguments -join ' '), ($diffOutput -join [Environment]::NewLine))) | Out-Null
    }
    if (-not $Staged) {
        $cachedArguments = @("diff", "--cached", "--check") + $scopeArguments
        $cachedOutput = @(& git -c core.quotepath=false @cachedArguments 2>&1)
        if ($LASTEXITCODE -ne 0) {
            $failures.Add(("git diff --cached --check failed: {0}" -f ($cachedOutput -join [Environment]::NewLine))) | Out-Null
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
    if ($Explain -or -not $Quiet) {
        Write-Host ("[INFO] Validation profile: {0} (requested: {1}, changed paths: {2})" -f $selectedProfile, $Profile, $changedPaths.Count)
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
