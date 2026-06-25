$StandaloneRoutePackValidation = $null -eq (Get-Command Add-Failure -ErrorAction SilentlyContinue)
if ($StandaloneRoutePackValidation) {
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
$ValidationRunId = ("route-pack-{0}-{1}" -f ((Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssZ")), ([guid]::NewGuid().ToString("N").Substring(0, 8)))
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

function Test-RoutePackDeterminism {
$startFailureCount = $Failures.Count
$expectedWorkflowVersion = Get-CanonicalWorkflowVersion
$routePackRoot = Get-ValidationTempRoot -Purpose "route-pack-validation"
$routePackA = Join-Path $routePackRoot "core-system-a.json"
$routePackB = Join-Path $routePackRoot "core-system-b.json"
$routePackAnswerA = Join-Path $routePackRoot "answer-only-a.json"
$routePackAnswerB = Join-Path $routePackRoot "answer-only-b.json"
try {
$outputA = & (Get-RepoPath "scripts/export-route-pack.ps1") -RouteId "core_system" -OutputPath $routePackA -Quiet 2>&1
$outputB = & (Get-RepoPath "scripts/export-route-pack.ps1") -RouteId "core_system" -OutputPath $routePackB -Quiet 2>&1
$outputAnswerA = & (Get-RepoPath "scripts/export-route-pack.ps1") -RouteId "answer_only" -OutputPath $routePackAnswerA -Quiet 2>&1
$outputAnswerB = & (Get-RepoPath "scripts/export-route-pack.ps1") -RouteId "answer_only" -OutputPath $routePackAnswerB -Quiet 2>&1
if (@($outputA).Count -gt 0 -or @($outputB).Count -gt 0 -or @($outputAnswerA).Count -gt 0 -or @($outputAnswerB).Count -gt 0) {
Add-Failure "Route pack export quiet mode produced output during deterministic check."
}
if (-not (Test-Path -LiteralPath $routePackA -PathType Leaf) -or -not (Test-Path -LiteralPath $routePackB -PathType Leaf) -or -not (Test-Path -LiteralPath $routePackAnswerA -PathType Leaf) -or -not (Test-Path -LiteralPath $routePackAnswerB -PathType Leaf)) {
Add-Failure "Route pack deterministic check did not produce both manifests."
}
else {
$hashA = (Get-FileHash -LiteralPath $routePackA -Algorithm SHA256).Hash
$hashB = (Get-FileHash -LiteralPath $routePackB -Algorithm SHA256).Hash
if ($hashA -ne $hashB) {
Add-Failure "Route pack deterministic check produced different manifest hashes."
}
$answerHashA = (Get-FileHash -LiteralPath $routePackAnswerA -Algorithm SHA256).Hash
$answerHashB = (Get-FileHash -LiteralPath $routePackAnswerB -Algorithm SHA256).Hash
if ($answerHashA -ne $answerHashB) {
Add-Failure "Answer-only route pack deterministic check produced different manifest hashes."
}
$manifest = Get-Content -LiteralPath $routePackA -Raw | ConvertFrom-Json
if ([string] $manifest.route_id -ne "core_system") {
Add-Failure "Route pack manifest route_id mismatch."
}
if ([string] $manifest.version -ne $expectedWorkflowVersion) {
Add-Failure ("Route pack manifest version must be {0}." -f $expectedWorkflowVersion)
}
if ($manifest.PSObject.Properties["files"]) {
Add-Failure "Route pack manifest must use required_files, not files."
}
$requiredFiles = $manifest.PSObject.Properties["required_files"]
if (-not $requiredFiles -or $null -eq $requiredFiles.Value -or @($requiredFiles.Value).Count -eq 0) {
Add-Failure "Route pack manifest must contain non-empty required_files for core_system."
}
if (-not $manifest.PSObject.Properties["manifest_hash"] -or [string]::IsNullOrWhiteSpace([string] $manifest.manifest_hash)) {
Add-Failure "Route pack manifest must include manifest_hash."
}
$answerManifest = Get-Content -LiteralPath $routePackAnswerA -Raw | ConvertFrom-Json
if ([string] $answerManifest.route_id -ne "answer_only") {
Add-Failure "Answer-only route pack manifest route_id mismatch."
}
if ([string] $answerManifest.tool_surface -ne "no_file_read") {
Add-Failure "Answer-only route pack must use no_file_read tool_surface."
}
$answerRequiredFiles = $answerManifest.PSObject.Properties["required_files"]
if (-not $answerRequiredFiles) {
Add-Failure "Answer-only route pack manifest must include required_files."
}
elseif ($null -ne $answerRequiredFiles.Value -and @($answerRequiredFiles.Value).Count -ne 0) {
Add-Failure "Answer-only route pack required_files must be empty."
}
if (-not $answerManifest.PSObject.Properties["manifest_hash"] -or [string]::IsNullOrWhiteSpace([string] $answerManifest.manifest_hash)) {
Add-Failure "Answer-only route pack manifest must include manifest_hash."
}
}
}
catch {
Add-Failure ("Route pack deterministic check failed: {0}" -f $_.Exception.Message)
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Route pack deterministic checks passed."
}
}

if ($StandaloneRoutePackValidation) {
Push-Location $RepoRoot
try {
Test-RoutePackDeterminism
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
