function Test-SizeGates {
$startFailureCount = $Failures.Count
$verifyContent = Get-Content -LiteralPath (Get-RepoPath ".agents/docs/agents/verify.yaml") -Raw
function Get-SizeGateLimit {
param([string] $Name)
$match = [regex]::Match($verifyContent, ("(?m)^\s*{0}:\s*(\d+)\s*$" -f [regex]::Escape($Name)))
if (-not $match.Success) {
Add-Failure ("Size gate config is missing or non-numeric: {0}" -f $Name)
return $null
}
return [int] $match.Groups[1].Value
}
$agentsLimit = Get-SizeGateLimit "AGENTS.md"
$skillLimit = Get-SizeGateLimit "project_skill"
$canonicalYamlLimit = Get-SizeGateLimit "canonical_agents_yaml"
$scriptLimit = Get-SizeGateLimit "script_ps1"
$validateScriptAfterSplitLimit = Get-SizeGateLimit "validate_script_ps1_after_split"
$repoLimitKiB = Get-SizeGateLimit "tracked_repo_kib"
if ($null -in @($agentsLimit, $skillLimit, $canonicalYamlLimit, $scriptLimit, $validateScriptAfterSplitLimit, $repoLimitKiB)) {
return
}
$agentsSize = (Get-Item -LiteralPath (Get-RepoPath "AGENTS.md")).Length
if ($agentsSize -gt $agentsLimit) {
Add-Failure ("AGENTS.md exceeds {0} bytes: {1}" -f $agentsLimit, $agentsSize)
}
$skillSize = (Get-Item -LiteralPath (Get-RepoPath ".agents/skills/project-isolation-workflow/SKILL.md")).Length
if ($skillSize -gt $skillLimit) {
Add-Failure ("Project skill exceeds {0} bytes: {1}" -f $skillLimit, $skillSize)
}
$maxYamlSize = 0
foreach ($file in Get-ChildItem -LiteralPath (Get-RepoPath "docs/agents") -Filter "*.yaml") {
if ($file.Length -gt $maxYamlSize) {
$maxYamlSize = $file.Length
}
if ($file.Length -gt $canonicalYamlLimit) {
Add-Failure ("Canonical Agents YAML exceeds {0} bytes: {1} ({2} bytes)" -f $canonicalYamlLimit, $file.FullName, $file.Length)
}
}
$maxScriptSize = 0
foreach ($file in Get-ChildItem -LiteralPath (Get-RepoPath "scripts") -Filter "*.ps1") {
if ($file.Length -gt $maxScriptSize) {
$maxScriptSize = $file.Length
}
if ($file.Length -gt $scriptLimit) {
Add-Failure ("PowerShell script exceeds {0} bytes: {1} ({2} bytes)" -f $scriptLimit, $file.FullName, $file.Length)
}
}
$validateScriptSize = (Get-Item -LiteralPath (Get-RepoPath "scripts/validate.ps1")).Length
if ($validateScriptSize -gt $validateScriptAfterSplitLimit) {
Add-Failure ("scripts/validate.ps1 exceeds split target {0} bytes: {1}" -f $validateScriptAfterSplitLimit, $validateScriptSize)
}
$trackedTotal = Get-RepoFilesSize -Paths @(& git -C $RepoRoot ls-files)
$intendedTotal = Get-RepoFilesSize -Paths (Get-IntendedRepoFiles)
$limit = $repoLimitKiB * 1024
if ($trackedTotal -gt $limit) {
Add-Failure ("Tracked repo size exceeds {0} KiB: {1} bytes" -f $repoLimitKiB, $trackedTotal)
}
if ($intendedTotal -gt $limit) {
Add-Failure ("Intended repo size exceeds {0} KiB: {1} bytes" -f $repoLimitKiB, $intendedTotal)
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass ("Size gates passed: AGENTS.md {0} bytes; project skill {1} bytes; max yaml {2} bytes; max ps1 {3} bytes; validate.ps1 {4} bytes; tracked repo {5} bytes; intended repo {6} bytes." -f $agentsSize, $skillSize, $maxYamlSize, $maxScriptSize, $validateScriptSize, $trackedTotal, $intendedTotal)
}
}
