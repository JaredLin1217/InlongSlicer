function Test-LegacyResidue {
$startFailureCount = $Failures.Count
$roots = @(
"README.md",
"docs/agents",
".agents/docs/project-structure.md",
"docs/templates/agents",
"docs/runbooks",
"schemas",
"scripts",
".agents/skills",
"tests/agents-governance-fixtures"
)
$excluded = @(
"CHANGELOG.md",
"docs/github-updates.md",
".agents/docs/templates/agents/github-updates.md"
)
$files = @()
foreach ($root in $roots) {
$fullRoot = Get-RepoPath $root
if (-not (Test-Path -LiteralPath $fullRoot)) {
continue
}
if (Test-Path -LiteralPath $fullRoot -PathType Leaf) {
$files += Get-Item -LiteralPath $fullRoot
}
else {
$files += Get-ChildItem -LiteralPath $fullRoot -Recurse -File | Where-Object { @(".md", ".yaml", ".yml", ".json", ".ps1", ".toml", ".txt").Contains($_.Extension.ToLowerInvariant()) }
}
}
$files = @($files | Sort-Object FullName -Unique | Where-Object {
$rel = ($_.FullName.Substring($RepoRoot.Path.Length).TrimStart("\") -replace "\\", "/")
$excluded -notcontains $rel
})
$bannedPatterns = @(
"v2 compat" + "ible",
"v2-" + "compatible",
"legacy " + "v2",
"optional " + "overlay",
"compatible " + "overlay",
"within " + "v2",
"target_" + "legacy_agents",
"routed-" + "legacy",
"workflow compat" + "ibility",
"legacy target " + "docs",
"v1_" + "preservation",
"compatibility_" + "rule",
"2\." + "2\.0",
"2\." + "3\.0",
"Target" + "Legacy",
"legacy" + "Candidates",
"\[" + "LEGACY\]",
"target-owned " + "legacy",
"legacy " + "Agents",
"M" + "CP",
"m" + "cp",
"Dev" + "Space",
"dev" + "space",
".agents/docs/agents/" + "m" + "cp",
"agents-" + "m" + "cp",
"optional " + "capability registry"
)
foreach ($file in $files) {
foreach ($pattern in $bannedPatterns) {
$matches = Select-String -LiteralPath $file.FullName -Pattern $pattern -AllMatches
foreach ($match in $matches) {
$relative = ($file.FullName.Substring($RepoRoot.Path.Length).TrimStart("\") -replace "\\", "/")
Add-Failure ("Retired positioning residue remains in {0}:{1}: {2}" -f $relative, $match.LineNumber, $match.Line.Trim())
}
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Legacy residue scan passed."
}
}
