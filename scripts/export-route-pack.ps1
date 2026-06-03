param(
[string] $RouteId = "core_system",
[string] $OutputPath,
[switch] $Quiet
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
function Get-ProjectId {
$leaf = Split-Path -Leaf $RepoRoot
$id = ($leaf.ToLowerInvariant() -replace "[^a-z0-9]+", "-").Trim("-")
if ([string]::IsNullOrWhiteSpace($id)) {
return "agents"
}
return $id
}
function Get-WorkflowVersion {
$versionPath = Join-Path $RepoRoot ".agents/docs/agents/version.yaml"
$inWorkflow = $false
foreach ($line in Get-Content -LiteralPath $versionPath) {
if ($line -match "^workflow:\s*$") {
$inWorkflow = $true
continue
}
if ($inWorkflow -and $line -match "^\S") {
$inWorkflow = $false
}
if ($inWorkflow -and $line -match "^\s+version:\s*[""']?([^""']+)[""']?\s*$") {
return $Matches[1].Trim().Trim('"').Trim("'")
}
}
throw "Unable to read workflow.version from .agents/docs/agents/version.yaml."
}
function Get-StringSha256 {
param([string] $Value)
$sha = [System.Security.Cryptography.SHA256]::Create()
try {
$bytes = [System.Text.Encoding]::UTF8.GetBytes($Value)
return ([System.BitConverter]::ToString($sha.ComputeHash($bytes)) -replace "-", "").ToLowerInvariant()
}
finally {
$sha.Dispose()
}
}
function Convert-ToRepoRelative {
param([string] $Path)
return (($Path -replace "\\", "/").Trim()).TrimStart("/")
}
function Get-RouteFiles {
$runtimePath = Join-Path $RepoRoot ".agents/docs/agents/ai-runtime.yaml"
$routePattern = "^\s*" + [regex]::Escape($RouteId) + ":\s*\{\s*f:\s*\[(.*?)\]"
foreach ($line in Get-Content -LiteralPath $runtimePath) {
if ($line -match $routePattern) {
$files = @()
foreach ($match in [regex]::Matches($Matches[1], '\"([^\"]+)\"')) {
$value = [string] $match.Groups[1].Value
if ($value -match '^(docs|schemas|scripts|\.agents)/') {
$files += (Convert-ToRepoRelative $value)
}
}
if ($files.Count -eq 0) {
if ($RouteId -eq "answer_only") {
return @()
}
throw "Route has no concrete repo files: $RouteId"
}
return @($files | Sort-Object -Unique)
}
}
throw "Route not found in .agents/docs/agents/ai-runtime.yaml: $RouteId"
}
$version = Get-WorkflowVersion
$files = @(Get-RouteFiles)
$entries = @()
foreach ($rel in $files) {
$full = Join-Path $RepoRoot ($rel -replace "/", [System.IO.Path]::DirectorySeparatorChar)
if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
throw "Route file missing: $rel"
}
$item = Get-Item -LiteralPath $full
$entries += [ordered]@{
path = $rel
bytes = $item.Length
sha256 = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant()
}
}
$schemaInput = ($entries | ForEach-Object { $_.path }) -join "`n"
$fileSetInput = ($entries | ForEach-Object { "{0}|{1}|{2}" -f $_.path, $_.bytes, $_.sha256 }) -join "`n"
$toolSurface = "read_only_manifest_export"
$cacheKeyFields = @("route_id", "version", "required_file_hashes")
if ($entries.Count -eq 0) {
$toolSurface = "no_file_read"
$cacheKeyFields = @("route_id", "version", "tool_surface")
}
$schemaHash = Get-StringSha256 $schemaInput
$manifestSeed = (@($RouteId, $version, $toolSurface, $schemaHash, $fileSetInput) + $cacheKeyFields) -join "`n"
$manifest = [ordered]@{
schema = "agents-route-pack-manifest/v1"
route_id = $RouteId
version = $version
tool_surface = $toolSurface
cache_key_fields = $cacheKeyFields
schema_hash = $schemaHash
required_files = $entries
manifest_hash = Get-StringSha256 $manifestSeed
}
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
$OutputPath = [System.IO.Path]::Combine([System.IO.Path]::GetTempPath(), "codex-agent-status", (Get-ProjectId), "route-packs", ("{0}.json" -f $RouteId))
}
$outputFull = [System.IO.Path]::GetFullPath($OutputPath)
$outputParent = Split-Path -Parent $outputFull
if (-not (Test-Path -LiteralPath $outputParent -PathType Container)) {
New-Item -ItemType Directory -Path $outputParent -Force | Out-Null
}
$json = $manifest | ConvertTo-Json -Depth 12
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($outputFull, $json, $utf8NoBom)
if (-not $Quiet) {
Write-Host ("Route pack: {0}" -f $outputFull)
Write-Host ("Hash: {0}" -f $manifest.manifest_hash)
}
