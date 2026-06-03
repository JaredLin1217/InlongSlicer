[CmdletBinding()]
param(
[string]$OutputPath = "docs/github-updates.md",
[int]$MaxCommits = 12,
[int]$MaxFilesPerCommit = 8,
[string]$SourceRef = "HEAD",
[string]$BranchName = ""
)
$ErrorActionPreference = "Stop"
function Invoke-Git {
param(
[Parameter(Mandatory = $true)]
[string[]]$Arguments
)
$output = & git @Arguments 2>&1
if ($LASTEXITCODE -ne 0) {
throw ("git {0} failed: {1}" -f ($Arguments -join " "), ($output -join [Environment]::NewLine))
}
return $output
}
function ConvertTo-AsciiText {
param([AllowNull()][string]$Value)
if ($null -eq $Value) {
return ""
}
$builder = [System.Text.StringBuilder]::new()
foreach ($char in $Value.ToCharArray()) {
$code = [int][char]$char
if ($code -ge 32 -and $code -le 126) {
[void]$builder.Append($char)
}
elseif ($code -eq 9) {
[void]$builder.Append(" ")
}
else {
[void]$builder.Append(("\u{{{0:X4}}}" -f $code))
}
}
return $builder.ToString()
}
function Format-CodeSpan {
param([AllowNull()][string]$Value)
$safe = (ConvertTo-AsciiText -Value $Value).Replace('`', "'")
return ('`' + $safe + '`')
}
function ConvertTo-PublicSubject {
param([AllowNull()][string]$Value)
$subject = ConvertTo-AsciiText -Value $Value
return ($subject -replace '\bv([0-9]+\.[0-9]+(\.[0-9]+)?)\b', '$1')
}
function Get-FirstLineOrDefault {
param(
[string[]]$Lines,
[string]$Default
)
foreach ($line in $Lines) {
if (-not [string]::IsNullOrWhiteSpace($line)) {
return $line.Trim()
}
}
return $Default
}
function Get-CurrentWorkflowMetadata {
param([string]$RepoRoot)
$path = Join-Path $RepoRoot ".agents/docs/agents/version.yaml"
$metadata = @{
version = "unknown"
channel = "unknown"
positioning = ""
}
if (-not (Test-Path -LiteralPath $path)) {
return $metadata
}
foreach ($line in Get-Content -LiteralPath $path) {
if ($line -match '^\s+version:\s+"?([^"]+)"?\s*$') {
$metadata.version = $Matches[1]
}
elseif ($line -match '^\s+channel:\s+"?([^"]+)"?\s*$') {
$metadata.channel = $Matches[1]
}
elseif ($line -match '^\s+positioning:\s+"?([^"]+)"?\s*$') {
$metadata.positioning = $Matches[1]
}
}
return $metadata
}
if ($MaxCommits -lt 1) {
throw "MaxCommits must be at least 1."
}
if ($MaxFilesPerCommit -lt 1) {
throw "MaxFilesPerCommit must be at least 1."
}
$repoRoot = Get-FirstLineOrDefault -Lines @(Invoke-Git -Arguments @("rev-parse", "--show-toplevel")) -Default ""
if ([string]::IsNullOrWhiteSpace($repoRoot)) {
throw "Unable to resolve the repository root."
}
Set-Location -LiteralPath $repoRoot
$sourceFull = Get-FirstLineOrDefault -Lines @(Invoke-Git -Arguments @("rev-parse", $SourceRef)) -Default ""
if ([string]::IsNullOrWhiteSpace($sourceFull)) {
throw "Unable to resolve SourceRef '$SourceRef'."
}
$sourceShort = Get-FirstLineOrDefault -Lines @(Invoke-Git -Arguments @("rev-parse", "--short", $sourceFull)) -Default $sourceFull
$sourceDate = Get-FirstLineOrDefault -Lines @(Invoke-Git -Arguments @("show", "-s", "--format=%cI", $sourceFull)) -Default "unknown"
if ([string]::IsNullOrWhiteSpace($BranchName)) {
if (-not [string]::IsNullOrWhiteSpace($env:GITHUB_REF_NAME)) {
$BranchName = $env:GITHUB_REF_NAME
}
else {
$BranchName = Get-FirstLineOrDefault -Lines @(Invoke-Git -Arguments @("branch", "--show-current")) -Default "detached"
}
}
if ([System.IO.Path]::IsPathRooted($OutputPath)) {
$outputFullPath = $OutputPath
}
else {
$outputFullPath = Join-Path $repoRoot $OutputPath
}
$outputDirectory = Split-Path -Parent $outputFullPath
if (-not [string]::IsNullOrWhiteSpace($outputDirectory)) {
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}
$separator = [string][char]31
$commitRows = @(Invoke-Git -Arguments @(
"log",
$sourceFull,
"-n",
[string]$MaxCommits,
"--date=short",
"--pretty=format:%H%x1f%h%x1f%ad%x1f%an%x1f%s",
"--no-merges"
))
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add("# GitHub Updates")
$lines.Add("")
$lines.Add("This file is generated from git history after public branch pushes.")
$lines.Add(("Do not hand-edit routine entries; change {0} instead." -f (Format-CodeSpan -Value "scripts/update-github-updates.ps1")))
$lines.Add("")
$lines.Add(("- Source branch: {0}" -f (Format-CodeSpan -Value $BranchName)))
$lines.Add(("- Source commit analyzed: {0}" -f (Format-CodeSpan -Value $sourceShort)))
$lines.Add(("- Source commit date: {0}" -f (Format-CodeSpan -Value $sourceDate)))
$lines.Add(("- Commit window: latest {0} non-merge commits" -f $MaxCommits))
$lines.Add("")
$workflowMetadata = Get-CurrentWorkflowMetadata -RepoRoot $repoRoot
$lines.Add("## Current Project Version")
$lines.Add("")
$lines.Add(("- Version: {0}" -f (Format-CodeSpan -Value $workflowMetadata.version)))
$lines.Add(("- Channel: {0}" -f (Format-CodeSpan -Value $workflowMetadata.channel)))
if (-not [string]::IsNullOrWhiteSpace($workflowMetadata.positioning)) {
$lines.Add(("- Positioning: {0}" -f (ConvertTo-AsciiText -Value $workflowMetadata.positioning)))
}
$lines.Add("")
$lines.Add("## Recent Commits")
$lines.Add("")
foreach ($row in $commitRows) {
if ([string]::IsNullOrWhiteSpace($row)) {
continue
}
$parts = $row -split [System.Text.RegularExpressions.Regex]::Escape($separator), 5
if ($parts.Count -lt 5) {
continue
}
$hash = $parts[0]
$shortHash = $parts[1]
$date = $parts[2]
$author = $parts[3]
$subject = ConvertTo-PublicSubject -Value $parts[4]
if ([string]::IsNullOrWhiteSpace($subject)) {
$subject = "No commit subject recorded."
}
$shortStatLines = @(Invoke-Git -Arguments @("show", "--shortstat", "--format=", $hash) |
Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
$shortStat = "No file changes recorded."
if ($shortStatLines.Count -gt 0) {
$shortStat = ConvertTo-AsciiText -Value (($shortStatLines | Select-Object -Last 1).Trim())
}
$files = @(Invoke-Git -Arguments @("diff-tree", "--root", "--no-commit-id", "--name-only", "-r", $hash) |
Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
Select-Object -First $MaxFilesPerCommit)
$lines.Add(("### {0} - {1}" -f (ConvertTo-AsciiText -Value $date), (Format-CodeSpan -Value $shortHash)))
$lines.Add("")
$lines.Add($subject)
$lines.Add("")
$lines.Add(("- Author: {0}" -f (Format-CodeSpan -Value $author)))
$lines.Add(("- Shortstat: {0}" -f (Format-CodeSpan -Value $shortStat)))
if ($files.Count -gt 0) {
$lines.Add("- Files:")
foreach ($file in $files) {
$lines.Add(("  - {0}" -f (Format-CodeSpan -Value $file)))
}
}
$lines.Add("")
}
while (($lines.Count -gt 0) -and ($lines[$lines.Count - 1] -eq "")) {
$lines.RemoveAt($lines.Count - 1)
}
$content = ($lines -join "`n") + "`n"
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText($outputFullPath, $content, $utf8NoBom)
Write-Host ("Updated {0} from {1}." -f $OutputPath, $sourceShort)
