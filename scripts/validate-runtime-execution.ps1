$StandaloneRuntimeExecutionValidation = $null -eq (Get-Command Add-Failure -ErrorAction SilentlyContinue)
if ($StandaloneRuntimeExecutionValidation) {
$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$Failures = New-Object System.Collections.Generic.List[string]
function Write-Check {
param([string] $Status, [string] $Message)
Write-Host ("[{0}] {1}" -f $Status, $Message)
}
function Add-Failure {
param([string] $Message)
$Failures.Add($Message) | Out-Null
Write-Check "FAIL" $Message
}
function Add-Pass {
param([string] $Message)
Write-Check "PASS" $Message
}
function Get-RepoPath {
param([string] $Path)
return Join-Path $RepoRoot $Path
}
function Get-RepoPathHash {
param([string] $Path)
$normalized = [System.IO.Path]::GetFullPath($Path).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar).ToLowerInvariant()
$bytes = [System.Text.Encoding]::UTF8.GetBytes($normalized)
$sha = [System.Security.Cryptography.SHA256]::Create()
try {
$hash = $sha.ComputeHash($bytes)
return -join ($hash[0..5] | ForEach-Object { $_.ToString("x2") })
}
finally {
$sha.Dispose()
}
}
function Get-ValidationProjectKey {
$leaf = Split-Path -Leaf $RepoRoot.Path
$safe = ($leaf.ToLowerInvariant() -replace "[^a-z0-9._-]+", "-").Trim("-._")
if ([string]::IsNullOrWhiteSpace($safe)) {
$safe = "agents"
}
return ("{0}-{1}" -f $safe, (Get-RepoPathHash -Path $RepoRoot.Path))
}
$ValidationRunId = ("runtime-execution-{0}-{1}" -f ((Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssZ")), ([guid]::NewGuid().ToString("N").Substring(0, 8)))
function Get-ValidationTempRoot {
param([string] $Purpose)
$base = [System.IO.Path]::Combine([System.IO.Path]::GetTempPath(), "codex-agent-status", (Get-ValidationProjectKey), $ValidationRunId)
$path = Join-Path $base (($Purpose -replace "[^A-Za-z0-9._-]+", "-").Trim("-._"))
New-Item -ItemType Directory -Path $path -Force | Out-Null
return $path
}
function Get-CanonicalWorkflowVersion {
$text = Get-Content -LiteralPath (Get-RepoPath ".agents/docs/agents/version.yaml") -Raw
$match = [regex]::Match($text, '(?m)^\s+version:\s*"([^"]+)"\s*$')
if (-not $match.Success) {
throw "Unable to read .agents/docs/agents/version.yaml workflow.version."
}
return $match.Groups[1].Value
}
}

function Test-RuntimeExecutionSmoke {
$startFailureCount = $Failures.Count
$expectedWorkflowVersion = Get-CanonicalWorkflowVersion
$runtimeRoot = Get-ValidationTempRoot -Purpose "runtime-execution-validation"
$runId = "readonly-smoke"
try {
$runtimeScript = Get-RepoPath "scripts/agents-runtime.ps1"
& $runtimeScript -Action NewRun -RunId $runId -RuntimeRoot $runtimeRoot -Objective "readonly validation smoke" -Authority "read_only" -Quiet 2>&1 | Out-Null
& $runtimeScript -Action AddStep -RunId $runId -RuntimeRoot $runtimeRoot -Step "read_only" -Authority "read_only" -Quiet 2>&1 | Out-Null
& $runtimeScript -Action AddDeploymentEvidence -RunId $runId -RuntimeRoot $runtimeRoot -EvidenceType "verify" -Summary "readonly verification evidence" -EvidenceRef "run.json" -VerificationRef "scripts/agents-runtime.ps1" -Quiet 2>&1 | Out-Null
& $runtimeScript -Action AddResult -RunId $runId -RuntimeRoot $runtimeRoot -Result "completed" -Summary "readonly smoke completed" -EvidenceRef "summary.json" -VerificationRef "scripts/validate.ps1" -Quiet 2>&1 | Out-Null
& $runtimeScript -Action Collect -RunId $runId -RuntimeRoot $runtimeRoot -Quiet 2>&1 | Out-Null
& $runtimeScript -Action Verify -RunId $runId -RuntimeRoot $runtimeRoot -Quiet 2>&1 | Out-Null
& $runtimeScript -Action Cleanup -RunId $runId -RuntimeRoot $runtimeRoot -Quiet 2>&1 | Out-Null
& $runtimeScript -Action Verify -RunId $runId -RuntimeRoot $runtimeRoot -Quiet 2>&1 | Out-Null
$runPath = Join-Path (Join-Path $runtimeRoot $runId) "run.json"
if (-not (Test-Path -LiteralPath $runPath -PathType Leaf)) {
Add-Failure "Runtime execution smoke did not produce run.json."
}
else {
$run = Get-Content -LiteralPath $runPath -Raw | ConvertFrom-Json
if ([string] $run.version -ne $expectedWorkflowVersion) {
Add-Failure ("Runtime execution run version must be {0}." -f $expectedWorkflowVersion)
}
if ([string] $run.status -ne "cleaned") {
Add-Failure "Runtime execution smoke must end with cleaned status."
}
if (@($run.cleanup_evidence).Count -lt 1) {
Add-Failure "Runtime execution smoke must include cleanup evidence."
}
foreach ($requiredArray in @("verified_assumptions", "remaining_steps", "recovery_pointers", "event_summary", "verification_refs", "risks", "deployment_evidence")) {
if ($null -eq $run.PSObject.Properties[$requiredArray]) {
Add-Failure ("Runtime execution run is missing required array: {0}" -f $requiredArray)
}
}
if ([string]::IsNullOrWhiteSpace([string] $run.resume_pointer)) {
Add-Failure "Runtime execution run must include resume_pointer."
}
foreach ($requiredScalar in @("current_route", "change_boundary")) {
if ([string]::IsNullOrWhiteSpace([string] $run.$requiredScalar)) {
Add-Failure ("Runtime execution run must include {0}." -f $requiredScalar)
}
}
if (@($run.deployment_evidence).Count -lt 1) {
Add-Failure "Runtime execution smoke must include deployment evidence."
}
}
}
catch {
Add-Failure ("Runtime execution helper smoke failed: {0}" -f $_.Exception.Message)
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Runtime execution smoke checks passed."
}
}

if ($StandaloneRuntimeExecutionValidation) {
Push-Location $RepoRoot
try {
Test-RuntimeExecutionSmoke
}
finally {
Pop-Location
}
if ($Failures.Count -gt 0) {
Write-Host ""
Write-Host ("Validation failed: {0} issue(s)." -f $Failures.Count)
exit 1
}
Write-Host ""
Write-Host "Validation passed."
}
