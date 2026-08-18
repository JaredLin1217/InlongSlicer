[CmdletBinding()]
param(
    [string]$OutputPath = ".agents/runtime/context-intelligence/practice-report.json",
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-Median {
    param([double[]]$Values)
    $ordered = @($Values | Sort-Object)
    if ($ordered.Count -eq 0) {
        return 0.0
    }
    $middle = [Math]::Floor($ordered.Count / 2)
    if (($ordered.Count % 2) -eq 1) {
        return [double]$ordered[$middle]
    }
    return ([double]$ordered[$middle - 1] + [double]$ordered[$middle]) / 2.0
}

function Invoke-ResolverCase {
    param(
        [string]$ResolverPath,
        [object]$Case
    )
    $invoke = @{
        Task        = [string]$Case.task
        MaxFiles    = 3
        BudgetBytes = 8192
        Format      = "Json"
    }
    if (@($Case.changed_paths).Count -gt 0) {
        $invoke.ChangedPath = @($Case.changed_paths)
    }
    $json = & $ResolverPath @invoke
    if ($LASTEXITCODE -ne 0) {
        throw "Context resolver failed for case $($Case.id)."
    }
    return ($json | ConvertFrom-Json)
}

function Get-BaselineBytes {
    param(
        [string]$Root,
        [string]$Commit,
        [string[]]$Files
    )
    $total = 0L
    foreach ($file in $Files) {
        $content = @(& git -C $Root show "$Commit`:$file" 2>$null)
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to read v2.8 baseline file $file at $Commit."
        }
        $total += [System.Text.Encoding]::UTF8.GetByteCount(($content -join "`n"))
    }
    return $total
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$resolverPath = Join-Path $PSScriptRoot "resolve-agent-context.ps1"
$casePath = Join-Path $repoRoot "tests/context-intelligence/cases.json"
$casesDocument = Get-Content -LiteralPath $casePath -Raw | ConvertFrom-Json
$baselineCommit = [string]$casesDocument.baseline_commit
$runsPerGroup = [int]$casesDocument.runs_per_group

& git -C $repoRoot cat-file -e "$baselineCommit^{commit}" 2>$null
if ($LASTEXITCODE -ne 0) {
    throw "The v2.8 baseline commit is unavailable: $baselineCommit"
}

$runRecords = [System.Collections.Generic.List[object]]::new()
$newContextBytes = [System.Collections.Generic.List[double]]::new()
$baselineContextBytes = [System.Collections.Generic.List[double]]::new()
$newToolCalls = [System.Collections.Generic.List[double]]::new()
$baselineToolCalls = [System.Collections.Generic.List[double]]::new()
$correctRuns = 0
$criticalHits = 0
$criticalTotal = 0
$relevantSelections = 0
$totalSelections = 0
$deterministicPass = $true

foreach ($case in @($casesDocument.cases)) {
    $baselineCalls = @($case.v2_8_baseline_files).Count
    $signatures = [System.Collections.Generic.List[string]]::new()

    for ($run = 1; $run -le $runsPerGroup; $run++) {
        $baselineBytes = Get-BaselineBytes -Root $repoRoot -Commit $baselineCommit -Files @($case.v2_8_baseline_files)
        [void]$baselineContextBytes.Add([double]$baselineBytes)
        [void]$baselineToolCalls.Add([double]$baselineCalls)
        [void]$runRecords.Add([pscustomobject][ordered]@{
            case_id       = [string]$case.id
            group         = "v2.8-baseline"
            run           = $run
            route         = [string]$case.expected_route
            context_bytes = $baselineBytes
            tool_calls    = $baselineCalls
            evidence_read = $false
            result        = "pass"
        })

        $evidence = Invoke-ResolverCase -ResolverPath $resolverPath -Case $case
        $selectedPaths = @($evidence.relevant_files | ForEach-Object { [string]$_.path })
        $routePass = ([string]$evidence.route -eq [string]$case.expected_route)
        $caseCriticalHits = 0
        foreach ($criticalPath in @($case.critical_paths)) {
            $criticalTotal++
            if ($selectedPaths -contains [string]$criticalPath) {
                $criticalHits++
                $caseCriticalHits++
            }
        }
        $criticalPass = ($caseCriticalHits -eq @($case.critical_paths).Count)
        if ($routePass -and $criticalPass) {
            $correctRuns++
        }

        foreach ($selectedPath in $selectedPaths) {
            $totalSelections++
            if (@($case.relevant_paths) -contains $selectedPath) {
                $relevantSelections++
            }
        }
        $signature = "{0}|{1}|{2}" -f $evidence.route, ($selectedPaths -join ","), (@($evidence.dependency_paths) -join ",")
        [void]$signatures.Add($signature)
        [void]$newContextBytes.Add([double]$evidence.budget.selected_bytes)
        [void]$newToolCalls.Add(1.0)
        [void]$runRecords.Add([pscustomobject][ordered]@{
            case_id       = [string]$case.id
            group         = "v2.9-live"
            run           = $run
            route         = [string]$evidence.route
            context_bytes = [int64]$evidence.budget.selected_bytes
            tool_calls    = 1
            critical_pass = $criticalPass
            result        = if ($routePass -and $criticalPass) { "pass" } else { "fail" }
        })
    }
    if (@($signatures | Sort-Object -Unique).Count -ne 1) {
        $deterministicPass = $false
    }
}

$fallbackRecords = [System.Collections.Generic.List[object]]::new()
foreach ($state in @("stale", "degraded", "unsupported", "parse_error", "conflict", "dirty")) {
    $json = & $resolverPath -Task "validate fallback behavior" -ChangedPath "scripts/validate-changes.ps1" -Format Json -Simulation $state
    if ($LASTEXITCODE -ne 0) {
        throw "Context resolver fallback simulation failed for $state."
    }
    $evidence = $json | ConvertFrom-Json
    $matchedGap = @($evidence.gaps | Where-Object { ([string]$_).StartsWith("$state`:", [System.StringComparison]::OrdinalIgnoreCase) }).Count -gt 0
    $passed = $matchedGap -and [string]$evidence.verification_recommendation.profile -eq "Full" -and [bool]$evidence.verification_recommendation.expand_context
    [void]$fallbackRecords.Add([pscustomobject][ordered]@{
        state  = $state
        result = if ($passed) { "pass" } else { "fail" }
    })
}

$totalLiveRuns = @($casesDocument.cases).Count * $runsPerGroup
$taskAccuracy = if ($totalLiveRuns -gt 0) { $correctRuns / [double]$totalLiveRuns } else { 0.0 }
$criticalRecall = if ($criticalTotal -gt 0) { $criticalHits / [double]$criticalTotal } else { 1.0 }
$impactPrecision = if ($totalSelections -gt 0) { $relevantSelections / [double]$totalSelections } else { 1.0 }
$newContextMedian = Get-Median @($newContextBytes)
$baselineContextMedian = Get-Median @($baselineContextBytes)
$contextReduction = if ($baselineContextMedian -gt 0) { 1.0 - ($newContextMedian / $baselineContextMedian) } else { 0.0 }
$newToolMedian = Get-Median @($newToolCalls)
$baselineToolMedian = Get-Median @($baselineToolCalls)
$toolReduction = if ($baselineToolMedian -gt 0) { 1.0 - ($newToolMedian / $baselineToolMedian) } else { 0.0 }
$fallbackPassCount = @($fallbackRecords | Where-Object { $_.result -eq "pass" }).Count
$fallbackPassRate = if ($fallbackRecords.Count -gt 0) { $fallbackPassCount / [double]$fallbackRecords.Count } else { 0.0 }

$thresholds = [ordered]@{
    task_accuracy                 = [bool]($taskAccuracy -ge 1.0)
    critical_impact_recall        = [bool]($criticalRecall -ge 1.0)
    impact_precision              = [bool]($impactPrecision -ge 0.85)
    context_median_reduction      = [bool]($contextReduction -ge 0.50)
    tool_call_reduction           = [bool]($toolReduction -ge 0.30)
    fallback_pass_rate            = [bool]($fallbackPassRate -ge 1.0)
    deterministic                = [bool]$deterministicPass
}
$overallPass = @($thresholds.Values | Where-Object { -not $_ }).Count -eq 0

$report = [ordered]@{
    schema_version = "agents-context-practice-report/v1"
    workflow_version = "2.9.1"
    baseline = [ordered]@{
        version       = "2.8.0"
        source_commit = $baselineCommit
        method        = "Replay canonical v2.8 route-expansion files without reading current context evidence."
        runs_per_task = $runsPerGroup
    }
    candidate = [ordered]@{
        version       = "2.9.1"
        method        = "Live agents-context-evidence/v1 resolver execution."
        runs_per_task = $runsPerGroup
    }
    metrics = [ordered]@{
        task_accuracy                   = [Math]::Round($taskAccuracy, 4)
        critical_impact_recall          = [Math]::Round($criticalRecall, 4)
        impact_precision                = [Math]::Round($impactPrecision, 4)
        baseline_context_median_bytes   = [Math]::Round($baselineContextMedian, 2)
        candidate_context_median_bytes  = [Math]::Round($newContextMedian, 2)
        context_median_reduction        = [Math]::Round($contextReduction, 4)
        baseline_tool_calls_median      = [Math]::Round($baselineToolMedian, 2)
        candidate_tool_calls_median     = [Math]::Round($newToolMedian, 2)
        tool_call_reduction             = [Math]::Round($toolReduction, 4)
        fallback_pass_rate              = [Math]::Round($fallbackPassRate, 4)
    }
    thresholds = $thresholds
    fallback_tests = @($fallbackRecords)
    runs = @($runRecords)
    result = if ($overallPass) { "pass" } else { "fail" }
    scope = [ordered]@{
        practice_level = "T2 current-repo practice"
        external_pilot = "not executed"
        codegraph_runtime = "not installed or used"
    }
}

$absoluteOutputPath = if ([System.IO.Path]::IsPathRooted($OutputPath)) { $OutputPath } else { Join-Path $repoRoot $OutputPath }
$outputDirectory = Split-Path -Parent $absoluteOutputPath
if (-not [string]::IsNullOrWhiteSpace($outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $absoluteOutputPath -Encoding UTF8

if (-not $Quiet) {
    Write-Host ("Context intelligence practice: {0}" -f $report.result)
    Write-Host ("Accuracy={0:P1} Recall={1:P1} Precision={2:P1}" -f $taskAccuracy, $criticalRecall, $impactPrecision)
    Write-Host ("Context reduction={0:P1} Tool-call reduction={1:P1} Fallback={2:P1}" -f $contextReduction, $toolReduction, $fallbackPassRate)
    Write-Host ("Report: {0}" -f $absoluteOutputPath)
}

if (-not $overallPass) {
    exit 1
}
