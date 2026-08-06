param(
[string] $OutputPath,
[string] $Channel = "release",
[switch] $Quiet
)
$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
function Convert-ToRepoRelative {
param([string] $Path)
$normalized = ($Path -replace "\\", "/").Trim()
while ($normalized.StartsWith("./")) {
$normalized = $normalized.Substring(2)
}
return $normalized.TrimStart("/")
}
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
function Test-BlockedReleasePath {
param([string] $Path)
$rel = (Convert-ToRepoRelative $Path).ToLowerInvariant()
$exact = @(
".codex/config.toml",
".codex/environments/environment.toml",
".codex/environments/environment.template.toml",
".agents/runtime/collaborators.jsonl",
".agents/runtime/agent-ledger.jsonl",
"docs/agent-status.md",
".agents/docs/agent-status.md"
)
$prefix = @(
".git/",
".codex/environments/",
".agents/runtime/workflows/",
".agents/runtime/",
".workflow/",
"docs/agent-events/",
".agents/docs/agent-events/",
"docs/tmp-approval-",
".agents/docs/tmp-approval-",
"docs/hard-isolation-evidence/",
".agents/docs/hard-isolation-evidence/",
"docs/runtime-multi-agent-validation/",
".agents/docs/runtime-multi-agent-validation/"
)
if ($exact -contains $rel) {
return $true
}
foreach ($blockedPrefix in $prefix) {
if ($rel.StartsWith($blockedPrefix)) {
return $true
}
}
return $false
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
$projectId = Get-ProjectId
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
$OutputPath = [System.IO.Path]::Combine([System.IO.Path]::GetTempPath(), "codex-agent-status", $projectId, "release-package")
}
$outputFull = [System.IO.Path]::GetFullPath($OutputPath)
$repoFull = [System.IO.Path]::GetFullPath($RepoRoot)
$repoWithSeparator = $repoFull
if (-not $repoWithSeparator.EndsWith([System.IO.Path]::DirectorySeparatorChar.ToString())) {
$repoWithSeparator += [System.IO.Path]::DirectorySeparatorChar
}
if ($outputFull.StartsWith($repoWithSeparator, [System.StringComparison]::OrdinalIgnoreCase)) {
throw "Release output path must be outside the repo: $outputFull"
}
if (Test-Path -LiteralPath $outputFull) {
Remove-Item -LiteralPath $outputFull -Recurse -Force
}
New-Item -ItemType Directory -Path $outputFull -Force | Out-Null
$version = Get-WorkflowVersion
$packageRoot = Join-Path $outputFull ("jared-ai-team-v{0}" -f $version)
New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
$commit = "unknown"
$commitOutput = & git -C $RepoRoot rev-parse --short=12 HEAD 2>$null
if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($commitOutput)) {
$commit = ($commitOutput | Select-Object -First 1).Trim()
}
$tracked = & git -c core.quotepath=false -C $RepoRoot ls-files
if ($LASTEXITCODE -ne 0) {
throw "git ls-files failed."
}
$untracked = & git -c core.quotepath=false -C $RepoRoot ls-files --others --exclude-standard
if ($LASTEXITCODE -ne 0) {
throw "git ls-files --others failed."
}
$files = @($tracked) + @($untracked) |
Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
ForEach-Object { Convert-ToRepoRelative $_ } |
Sort-Object -Unique
$blocklistPolicy = @(
".agents/runtime/**",
".agents/runtime/context-intelligence/**",
".workflow/**",
".codex/config.toml",
".codex/environments/environment.toml",
".codex/environments/environment.template.toml",
".git/**",
".agents/runtime/agent-ledger.jsonl",
"live thread ids",
"API keys",
"provider sessions"
)
$blockedExcluded = @($files | Where-Object { Test-BlockedReleasePath $_ } | Sort-Object -Unique)
$entries = @()
foreach ($rel in $files) {
if (Test-BlockedReleasePath $rel) {
continue
}
$source = Join-Path $RepoRoot ($rel -replace "/", [System.IO.Path]::DirectorySeparatorChar)
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
continue
}
$destination = Join-Path $packageRoot ($rel -replace "/", [System.IO.Path]::DirectorySeparatorChar)
$destinationParent = Split-Path -Parent $destination
if (-not (Test-Path -LiteralPath $destinationParent)) {
New-Item -ItemType Directory -Path $destinationParent -Force | Out-Null
}
Copy-Item -LiteralPath $source -Destination $destination -Force
$item = Get-Item -LiteralPath $destination
$entries += [pscustomobject]@{
path = $rel
bytes = $item.Length
sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
}
}
$hashInput = ($entries | ForEach-Object { "{0}|{1}|{2}" -f $_.path, $_.bytes, $_.sha256 }) -join "`n"
$manifest = [pscustomobject]@{
schema = "agents-release-manifest/v1"
version = $version
channel = $Channel
commit = $commit
created_at_utc = (Get-Date).ToUniversalTime().ToString("o")
file_count = $entries.Count
package_hash = Get-StringSha256 $hashInput
blocklist_result = [pscustomobject]@{
checked = $true
policy = $blocklistPolicy
blocked_paths_excluded = $blockedExcluded
}
files = $entries
}
$manifestPath = Join-Path $packageRoot "release-manifest.json"
$json = $manifest | ConvertTo-Json -Depth 8
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($manifestPath, $json, $utf8NoBom)
if (-not $Quiet) {
Write-Host ("Release package: {0}" -f $packageRoot)
Write-Host ("Manifest: {0}" -f $manifestPath)
}
