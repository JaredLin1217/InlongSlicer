param(
[Parameter(Mandatory = $true)]
[string] $OutputPath,
[switch] $Full,
[switch] $Practice,
[switch] $Quiet
)
$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Write-Info {
param([string] $Message)
if (-not $Quiet) {
Write-Host $Message
}
}

function Get-RepoPath {
param([string] $Path)
return Join-Path $RepoRoot $Path
}

function Get-StringSha256 {
param([string] $Value)
$sha = [System.Security.Cryptography.SHA256]::Create()
try {
$bytes = [System.Text.Encoding]::UTF8.GetBytes($Value)
$hash = $sha.ComputeHash($bytes)
return ([System.BitConverter]::ToString($hash)).Replace("-", "").ToLowerInvariant()
}
finally {
$sha.Dispose()
}
}

function Get-RepoPathHash {
param([string] $Path)
$normalized = [System.IO.Path]::GetFullPath($Path).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar).ToLowerInvariant()
return (Get-StringSha256 -Value $normalized).Substring(0, 12)
}

function Get-EvidenceProjectKey {
$leaf = Split-Path -Leaf $RepoRoot
$safe = ($leaf.ToLowerInvariant() -replace "[^a-z0-9._-]+", "-").Trim("-._")
if ([string]::IsNullOrWhiteSpace($safe)) {
$safe = "agents"
}
return ("{0}-{1}" -f $safe, (Get-RepoPathHash -Path $RepoRoot))
}

function Get-WorkflowVersion {
$text = Get-Content -LiteralPath (Get-RepoPath ".agents/docs/agents/version.yaml") -Raw
$match = [regex]::Match($text, '(?m)^\s+version:\s*"([^"]+)"\s*$')
if (-not $match.Success) {
throw "Unable to resolve workflow version."
}
return $match.Groups[1].Value
}

function Invoke-GitLines {
param([string[]] $Arguments)
$output = & git -c core.quotepath=false -C $RepoRoot @Arguments 2>$null
if ($LASTEXITCODE -ne 0 -or $null -eq $output) {
return @()
}
return @($output | ForEach-Object { [string] $_ })
}

function Get-RepoCommit {
$commit = @(Invoke-GitLines -Arguments @("rev-parse", "HEAD"))
if ($commit.Count -eq 0 -or [string]::IsNullOrWhiteSpace($commit[0])) {
return "unavailable"
}
return [string] $commit[0]
}

function Get-WorkingTreeStatus {
$porcelain = @(Invoke-GitLines -Arguments @("status", "--porcelain=v1", "--untracked-files=all"))
$sanitized = @($porcelain | ForEach-Object { ([string] $_).Replace("\", "/") })
return [ordered]@{
state = $(if ($sanitized.Count -eq 0) { "clean" } else { "dirty" })
porcelain = $sanitized
entry_count = $sanitized.Count
}
}

function Test-ContentDigestExcludedPath {
param([string] $Path)
$rel = $Path.Replace("\", "/").TrimStart("/")
$lower = $rel.ToLowerInvariant()
if ($lower.StartsWith(".git/")) { return $true }
if ($lower.StartsWith(".agents/runtime/")) { return $true }
if ($lower.StartsWith(".workflow/")) { return $true }
if ($lower.StartsWith(".codex/")) { return $true }
if ($lower.StartsWith("docs/evidence/raw/")) { return $true }
if ($lower.StartsWith("docs/evidence/releases/") -and $lower.EndsWith(".json")) { return $true }
return $false
}

function Get-ValidatedContentDigest {
$tracked = @(Invoke-GitLines -Arguments @("ls-files"))
$untracked = @(Invoke-GitLines -Arguments @("ls-files", "--others", "--exclude-standard"))
$paths = @($tracked + $untracked |
ForEach-Object { ([string] $_).Replace("\", "/").TrimStart("/") } |
Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
Sort-Object -Unique)
$entries = New-Object System.Collections.Generic.List[string]
foreach ($path in $paths) {
if (Test-ContentDigestExcludedPath -Path $path) {
continue
}
$platformPath = $path.Replace("/", [string] [System.IO.Path]::DirectorySeparatorChar)
$fullPath = Join-Path $RepoRoot $platformPath
if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
continue
}
$fileHash = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash.ToLowerInvariant()
$entries.Add(("{0}`t{1}" -f $path, $fileHash)) | Out-Null
}
$payload = ($entries.ToArray() -join "`n")
return [ordered]@{
algorithm = "sha256"
value = Get-StringSha256 -Value $payload
intended_file_count = $entries.Count
}
}

function Get-PowerShellExecutable {
$pwsh = Get-Command pwsh -ErrorAction SilentlyContinue
if ($pwsh) {
return $pwsh.Source
}
return "powershell.exe"
}

function Invoke-EvidenceCommand {
param(
[string] $Name,
[string] $DisplayCommand,
[string] $ScriptPath,
[string[]] $Arguments = @(),
[string] $RawLogPath,
[string] $RawOutputRef,
[hashtable] $Environment
)
$timer = [System.Diagnostics.Stopwatch]::StartNew()
$previous = @{}
if ($Environment) {
foreach ($entry in $Environment.GetEnumerator()) {
$previous[$entry.Key] = [Environment]::GetEnvironmentVariable($entry.Key, "Process")
[Environment]::SetEnvironmentVariable($entry.Key, [string] $entry.Value, "Process")
}
}
try {
$powerShell = Get-PowerShellExecutable
$processArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $ScriptPath) + $Arguments
$output = & $powerShell @processArgs 2>&1
$exitCode = if ($null -eq $LASTEXITCODE) { 0 } else { [int] $LASTEXITCODE }
}
catch {
$output = @($_.Exception.Message)
$exitCode = 1
}
finally {
if ($Environment) {
foreach ($key in $Environment.Keys) {
[Environment]::SetEnvironmentVariable($key, $previous[$key], "Process")
}
}
$timer.Stop()
}
$outputText = ($output | ForEach-Object { [string] $_ }) -join [Environment]::NewLine
Set-Content -LiteralPath $RawLogPath -Value $outputText -Encoding UTF8
$record = [ordered]@{
name = $Name
command = $DisplayCommand
result = $(if ($exitCode -eq 0) { "passed" } else { "failed" })
exit_code = $exitCode
duration_ms = [int64] $timer.ElapsedMilliseconds
retry_count = 0
raw_output_ref = $RawOutputRef
}
if ($Name -eq "full_validation") {
$scoreMatch = [regex]::Match($outputText, 'Overall:\s*([0-9.]+)\s*/\s*100')
$record["overall_score"] = $(if ($scoreMatch.Success) { [decimal] $scoreMatch.Groups[1].Value } else { $null })
$record["evidence_capture_mode"] = "runtime-evidence-bootstrap"
}
return [pscustomobject] $record
}

function Add-EvidenceCommand {
param(
[System.Collections.Generic.List[object]] $Commands,
[string] $Name,
[string] $ScriptName,
[string] $DisplayCommand,
[string[]] $Arguments = @(),
[string] $ScratchRoot,
[hashtable] $Environment
)
if ([string]::IsNullOrWhiteSpace($DisplayCommand)) {
$DisplayCommand = ".\scripts\$ScriptName"
if ($Arguments.Count -gt 0) { $DisplayCommand += " " + ($Arguments -join " ") }
}
$commands.Add((Invoke-EvidenceCommand `
-Name $Name `
-DisplayCommand $DisplayCommand `
-ScriptPath (Get-RepoPath "scripts/$ScriptName") `
-Arguments $Arguments `
-RawLogPath (Join-Path $ScratchRoot ("{0}.log" -f $Name)) `
-RawOutputRef ("%TEMP%/codex-agent-status/<project-id>/<run-id>/{0}.log" -f $Name) `
-Environment $Environment)) | Out-Null
}

$runStarted = (Get-Date).ToUniversalTime()
$workflowVersion = Get-WorkflowVersion
$repoCommit = Get-RepoCommit
$sourceCommit = $repoCommit
$contentDigest = Get-ValidatedContentDigest
$workingTreeStatus = Get-WorkingTreeStatus
$excludedPaths = @(
".git/**",
".agents/runtime/**",
".workflow/**",
".codex/**",
"docs/evidence/releases/*.json",
"docs/evidence/raw/**"
)
$projectKey = Get-EvidenceProjectKey
$runId = ("runtime-evidence-{0}-{1}" -f $runStarted.ToString("yyyyMMddTHHmmssZ"), ([guid]::NewGuid().ToString("N").Substring(0, 8)))
$scratchRoot = [System.IO.Path]::Combine([System.IO.Path]::GetTempPath(), "codex-agent-status", $projectKey, $runId)
New-Item -ItemType Directory -Path $scratchRoot -Force | Out-Null
$commands = New-Object System.Collections.Generic.List[object]

Push-Location $RepoRoot
try {
Add-EvidenceCommand -Commands $commands -Name "deployment_self_test" -ScriptName "deploy-agents-workflow.ps1" -Arguments @("-SelfTest", "-Quiet") -ScratchRoot $scratchRoot
Add-EvidenceCommand -Commands $commands -Name "current_project_dry_run" -ScriptName "deploy-agents-workflow.ps1" -Arguments @("-TargetPath", ".", "-Mode", "template_provider_mode", "-LayoutProfile", "auto", "-DryRun", "-Quiet") -ScratchRoot $scratchRoot
if ($Practice) {
Add-EvidenceCommand -Commands $commands -Name "disposable_target_rollback_drill" -ScriptName "deploy-agents-workflow.ps1" -Arguments @("-SelfTest", "-Quiet") -ScratchRoot $scratchRoot
Add-EvidenceCommand -Commands $commands -Name "runtime_lifecycle_smoke" -ScriptName "validate-runtime-execution.ps1" -ScratchRoot $scratchRoot
$releaseOutput = Join-Path $scratchRoot "release-package"
Add-EvidenceCommand -Commands $commands -Name "release_package_export" -ScriptName "export-release-package.ps1" -DisplayCommand ".\scripts\export-release-package.ps1 -OutputPath %TEMP%/codex-agent-status/<project-id>/<run-id>/release-package -Quiet" -Arguments @("-OutputPath", $releaseOutput, "-Quiet") -ScratchRoot $scratchRoot
Add-EvidenceCommand -Commands $commands -Name "route_pack_deterministic" -ScriptName "validate-route-pack.ps1" -ScratchRoot $scratchRoot
Add-EvidenceCommand -Commands $commands -Name "context_intelligence_practice" -ScriptName "test-context-intelligence.ps1" -Arguments @("-Quiet") -ScratchRoot $scratchRoot
}
if ($Full -or $Practice) {
Add-EvidenceCommand -Commands $commands -Name "full_validation" -ScriptName "validate-changes.ps1" -Arguments @("-Profile", "Full", "-ContextMode", "Required", "-Score") -ScratchRoot $scratchRoot -Environment @{ "AGENTS_RUNTIME_EVIDENCE_CAPTURE_ACTIVE" = "1" }
}
}
finally {
Pop-Location
}

$runFinished = (Get-Date).ToUniversalTime()
$duration = [int64] ($runFinished - $runStarted).TotalMilliseconds
$commandArray = @($commands.ToArray())
$failedCommands = @($commandArray | Where-Object { [string] $_.result -ne "passed" })
$contextPracticeReportPath = Get-RepoPath ".agents/runtime/context-intelligence/practice-report.json"
$contextIntelligence = [ordered]@{
captured = [bool] $Practice
resolver_schema = "agents-context-evidence/v1"
practice_report_schema = $(if ($Practice) { "agents-context-practice-report/v1" } else { "not_run" })
runtime_report_ref = ".agents/runtime/context-intelligence/practice-report.json"
result = $(if ($Practice) { "failed" } else { "not_run" })
}
if ($Practice -and (Test-Path -LiteralPath $contextPracticeReportPath -PathType Leaf)) {
try {
$contextReport = Get-Content -LiteralPath $contextPracticeReportPath -Raw | ConvertFrom-Json
$contextIntelligence["practice_report_schema"] = [string] $contextReport.schema_version
$contextIntelligence["baseline"] = $contextReport.baseline
$contextIntelligence["candidate"] = $contextReport.candidate
$contextIntelligence["metrics"] = $contextReport.metrics
$contextIntelligence["thresholds"] = $contextReport.thresholds
$contextIntelligence["fallback_tests"] = @($contextReport.fallback_tests)
$contextIntelligence["scope"] = $contextReport.scope
$contextIntelligence["result"] = [string] $contextReport.result
}
catch {
$contextIntelligence["error"] = "Practice report could not be parsed."
}
}
elseif ($Practice) {
$contextIntelligence["error"] = "Practice report was not produced."
}
$contextCapturePassed = (-not $Practice) -or ([string] $contextIntelligence.result -eq "pass")
$evidence = [ordered]@{
schema_version = "agents-runtime-evidence/v4"
workflow_version = $workflowVersion
repo_commit = $repoCommit
source_commit = $sourceCommit
validated_content_digest = $contentDigest
content_digest_excluded_paths = $excludedPaths
working_tree_status_at_capture = $workingTreeStatus
run_started_utc = $runStarted.ToString("o")
run_finished_utc = $runFinished.ToString("o")
duration_ms = $duration
commands = $commandArray
context_intelligence = $contextIntelligence
claims = [ordered]@{
static_validation = "Full validation covers repository contracts, schema checks, package checks, and release evidence."
runtime_evidence = "Runtime evidence is captured from repeatable local commands."
practice_evidence = $(if ($Practice) { "T2 current-repo practice suite captured." } else { "Practice suite was not requested." })
context_intelligence = $(if ($Practice) { "Live v2.9.1 context resolver A/B and fallback practice passed." } else { "Context intelligence practice was not requested." })
evidence_tier = $(if ($Practice) { "T2 current-repo practice" } else { "T1 dry-run" })
hard_isolation = "No hard isolation claim is made without current runtime, tool, OS, account, or cloud enforcement evidence."
external_pilot_boundary = "No external project pilot was executed for this release evidence."
}
scope = [ordered]@{
target = "current repository and disposable self-test target"
external_target_deployment = $false
external_pilot = "not executed"
codegraph_runtime = "not installed or used"
raw_output_storage = "%TEMP%/codex-agent-status/<project-id>/<run-id>/"
practice = [bool] $Practice
disposable_target = "deployment self-test temporary targets only"
}
xr_xw = [ordered]@{
external_reads = @()
external_writes = @("%TEMP%/codex-agent-status/<project-id>/<run-id>/ raw command logs and release package scratch")
}
evidence_tier = [ordered]@{
current = $(if ($Practice) { "T2 current-repo practice" } else { "T1 dry-run" })
maximum_claimed = $(if ($Practice) { "T2 current-repo practice" } else { "T1 dry-run" })
scale = @("T0 static", "T1 dry-run", "T2 current-repo practice", "T3 external pilot", "T4 enforced isolation")
}
performance = [ordered]@{
command_count = $commandArray.Count
total_duration_ms = $duration
token_usage = "unavailable"
token_usage_reason = "Repository validation scripts do not receive Codex token accounting."
}
result = $(if ($failedCommands.Count -eq 0 -and $contextCapturePassed) { "passed" } else { "failed" })
release = ("v{0}" -f $workflowVersion)
}

if ([System.IO.Path]::IsPathRooted($OutputPath)) {
$resolvedOutputPath = [System.IO.Path]::GetFullPath($OutputPath)
}
else {
$resolvedOutputPath = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputPath))
}
$outputDir = Split-Path -Parent $resolvedOutputPath
if (-not [string]::IsNullOrWhiteSpace($outputDir)) {
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}
$json = ($evidence | ConvertTo-Json -Depth 20 -Compress) -replace "`r`n", "`n"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($resolvedOutputPath, ($json + "`n"), $utf8NoBom)
. (Get-RepoPath "scripts/validate-score.ps1")
$verifyText = Get-Content -LiteralPath (Get-RepoPath ".agents/docs/agents/verify.yaml") -Raw
$repoLimitMatch = [regex]::Match($verifyText, "(?m)^\s*tracked_repo_kib:\s*(\d+)\s*$")
if (-not $repoLimitMatch.Success) {
throw "Repository size limit is unavailable after evidence capture."
}
$intendedBytes = Get-RepoFilesSize -Paths (Get-IntendedRepoFiles)
$repoLimitBytes = [int64]$repoLimitMatch.Groups[1].Value * 1024
if ($intendedBytes -gt $repoLimitBytes) {
throw "Runtime evidence exceeds the intended repository size gate: $intendedBytes > $repoLimitBytes bytes."
}
Write-Info ("Runtime evidence written: {0}" -f $OutputPath)
if ($failedCommands.Count -gt 0 -or -not $contextCapturePassed) {
exit 1
}
