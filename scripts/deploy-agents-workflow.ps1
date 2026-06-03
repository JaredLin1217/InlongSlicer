param(
[string] $TargetPath,
[ValidateSet("core_bootstrap", "full_workflow", "template_provider_mode")]
[string] $Mode = "core_bootstrap",
[switch] $DryRun,
[switch] $Upgrade,
[switch] $CreateTarget,
[switch] $SelfTest,
[switch] $Quiet
)
$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$DeployManifestPath = Join-Path $RepoRoot ".agents/docs/agents/deploy.yaml"
$VersionSourceRelative = ".agents/docs/agents/version.yaml"
$VersionSourcePath = Join-Path $RepoRoot $VersionSourceRelative
$GitignoreFragmentPath = Join-Path $RepoRoot ".agents/docs/templates/agents/gitignore.fragment"
$DeployedFiles = New-Object System.Collections.Generic.List[string]
$PlannedWrites = New-Object System.Collections.Generic.List[string]
$ProtectedExisting = New-Object System.Collections.Generic.List[string]
$UnchangedExisting = New-Object System.Collections.Generic.List[string]
$TargetHistoricalAgents = New-Object System.Collections.Generic.List[string]
$ProtectedDirty = New-Object System.Collections.Generic.List[string]
$TargetLocalEnvironment = New-Object System.Collections.Generic.List[string]
function Test-SameDirectory {
param(
[string] $Left,
[string] $Right
)
$leftFull = [System.IO.Path]::GetFullPath($Left).TrimEnd("\", "/")
$rightFull = [System.IO.Path]::GetFullPath($Right).TrimEnd("\", "/")
return [string]::Equals($leftFull, $rightFull, [System.StringComparison]::OrdinalIgnoreCase)
}
function Test-PathInsideRoot {
param(
[string] $Path,
[string] $Root
)
$pathFull = [System.IO.Path]::GetFullPath($Path).TrimEnd("\", "/")
$rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd("\", "/")
return ($pathFull.Equals($rootFull, [System.StringComparison]::OrdinalIgnoreCase) -or
$pathFull.StartsWith($rootFull + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase) -or
$pathFull.StartsWith($rootFull + [System.IO.Path]::AltDirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase))
}
function Assert-RelativeDeployPath {
param([string] $RelativePath)
$normalized = Normalize-RepoPath $RelativePath
if ([string]::IsNullOrWhiteSpace($normalized)) {
throw "Deploy path must not be empty."
}
if ([System.IO.Path]::IsPathRooted($RelativePath) -or $normalized -match '(^|/)\.\.(/|$)') {
throw "Deploy path must be a safe repo-relative path: $RelativePath"
}
}
function Assert-InsideRoot {
param(
[string] $Path,
[string] $Root,
[string] $Label
)
if (-not (Test-PathInsideRoot -Path $Path -Root $Root)) {
throw "$Label escapes its allowed root: $Path"
}
}
function Write-Step {
param(
[string] $Status,
[string] $Message
)
if (-not $Quiet) {
Write-Output ("[{0}] {1}" -f $Status, $Message)
}
}
function Get-AgentsWorkflowVersion {
param([string] $Path)
if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
throw "Workflow version source is missing: $Path"
}
$version = $null
$channel = $null
$insideWorkflow = $false
foreach ($line in Get-Content -LiteralPath $Path) {
if ($line -match "^workflow:\s*$") {
$insideWorkflow = $true
continue
}
if ($insideWorkflow -and $line -match "^[A-Za-z0-9_]+:") {
break
}
if (-not $insideWorkflow) {
continue
}
if ($line -match '^\s+version:\s+"?([^"]+)"?\s*$') {
$version = $Matches[1]
continue
}
if ($line -match '^\s+channel:\s+"?([^"]+)"?\s*$') {
$channel = $Matches[1]
continue
}
}
if ([string]::IsNullOrWhiteSpace($version)) {
throw "Workflow version is missing in $VersionSourceRelative."
}
if ([string]::IsNullOrWhiteSpace($channel)) {
throw "Workflow channel is missing in $VersionSourceRelative."
}
return [pscustomobject]@{
Version = $version
Channel = $channel
Source = $VersionSourceRelative
}
}
function Format-AgentsWorkflowVersion {
param([object] $WorkflowVersion)
return ("{0} ({1})" -f $WorkflowVersion.Version, $WorkflowVersion.Channel)
}
function Get-SourceSpecificLiterals {
$literals = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($literal in @($RepoRoot.Path, (Split-Path -Leaf $RepoRoot.Path))) {
if (-not [string]::IsNullOrWhiteSpace($literal) -and $literal -ne "Agents") {
[void] $literals.Add($literal)
}
}
$remoteUrl = (& git -C $RepoRoot remote get-url origin 2>$null)
if (-not [string]::IsNullOrWhiteSpace($remoteUrl)) {
[void] $literals.Add($remoteUrl)
if ($remoteUrl -match "[:/]([^/]+)/([^/]+?)(\.git)?$") {
[void] $literals.Add($Matches[1])
[void] $literals.Add(("{0}/{1}" -f $Matches[1], $Matches[2]))
[void] $literals.Add(("{0}.git" -f $Matches[2]))
}
}
return @($literals)
}
function Assert-DeployWriteAllowed {
param(
[string] $Path,
[string] $RelativePath,
[string] $Content
)
$state = Get-DeployWriteState -Path $Path -Content $Content
if ($state -eq "create") {
return $true
}
if ($state -eq "current") {
$UnchangedExisting.Add($RelativePath) | Out-Null
return $false
}
$ProtectedExisting.Add($RelativePath) | Out-Null
if (-not $Upgrade) {
throw "Existing target file requires -Upgrade after dry-run: $RelativePath"
}
return $true
}
function Get-DeployWriteState {
param(
[string] $Path,
[string] $Content
)
if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
return "create"
}
$existing = Get-Content -LiteralPath $Path -Raw
if ($existing -eq $Content) {
return "current"
}
return "upgrade_required"
}
function Normalize-RepoPath {
param([string] $Path)
return $Path.Replace("\", "/").Trim("/")
}
function Join-TargetPath {
param(
[string] $Root,
[string] $RelativePath
)
Assert-RelativeDeployPath -RelativePath $RelativePath
$parts = (Normalize-RepoPath $RelativePath).Split("/", [System.StringSplitOptions]::RemoveEmptyEntries)
$path = $Root
foreach ($part in $parts) {
$path = Join-Path $path $part
}
Assert-InsideRoot -Path $path -Root $Root -Label "Target path"
return $path
}
function Get-RelativeTargetPath {
param(
[string] $Root,
[string] $Path
)
Assert-InsideRoot -Path $Path -Root $Root -Label "Target classification path"
$rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd("\", "/")
$pathFull = [System.IO.Path]::GetFullPath($Path)
return Normalize-RepoPath ($pathFull.Substring($rootFull.Length).TrimStart("\", "/"))
}
function Add-UniqueListItem {
param(
[object] $List,
[string] $Value
)
if (-not $List.Contains($Value)) {
$List.Add($Value) | Out-Null
}
}
function Get-SafeEnvironmentName {
param([string] $Root)
$leaf = Split-Path -Leaf $Root
if ([string]::IsNullOrWhiteSpace($leaf)) {
$leaf = "project"
}
$safe = ($leaf -replace '[\\/:*?"<>|]+', "-").Trim().Trim(".")
if ([string]::IsNullOrWhiteSpace($safe)) {
return "project"
}
return $safe
}
function ConvertTo-TomlBasicString {
param([string] $Value)
if ($null -eq $Value) {
return ""
}
$backslash = [string][char] 92
$escaped = $Value.Replace($backslash, ($backslash + $backslash))
return $escaped.Replace('"', '\"')
}
function Get-TargetEnvironmentFiles {
param([string] $Root)
$envDir = Join-TargetPath -Root $Root -RelativePath ".codex/environments"
if (-not (Test-Path -LiteralPath $envDir -PathType Container)) {
return @()
}
return @(Get-ChildItem -LiteralPath $envDir -Filter "*.toml" -File -Force | Sort-Object Name)
}
function Initialize-TargetLocalEnvironment {
param(
[string] $Root,
[switch] $PlanOnly
)
$existing = @(Get-TargetEnvironmentFiles -Root $Root)
if ($existing.Count -gt 0) {
foreach ($file in $existing) {
$relative = Get-RelativeTargetPath -Root $Root -Path $file.FullName
Add-UniqueListItem -List $TargetLocalEnvironment -Value ("preserve: {0}" -f $relative)
Add-UniqueListItem -List $ProtectedDirty -Value $relative
}
return
}
$name = Get-SafeEnvironmentName -Root $Root
$relative = Normalize-RepoPath (".codex/environments/{0}.toml" -f $name)
if ($PlanOnly) {
Add-UniqueListItem -List $TargetLocalEnvironment -Value ("planned: {0}" -f $relative)
return
}
$path = Join-TargetPath -Root $Root -RelativePath $relative
$dir = Split-Path -Parent $path
if (-not (Test-Path -LiteralPath $dir -PathType Container)) {
New-Item -ItemType Directory -Path $dir | Out-Null
}
$projectName = ConvertTo-TomlBasicString (Split-Path -Leaf $Root)
$content = (@(
"# Target-local Codex environment. Generated only when no environment config exists.",
"version = 1",
("name = ""{0}""" -f $projectName),
"",
"[setup]",
"script = '''",
'cd "$env:CODEX_WORKTREE_PATH"',
"git status -sb",
"'''"
) -join [System.Environment]::NewLine) + [System.Environment]::NewLine
Set-Content -LiteralPath $path -Value $content -NoNewline -Encoding utf8
Add-UniqueListItem -List $TargetLocalEnvironment -Value ("created: {0}" -f $relative)
Add-UniqueListItem -List $ProtectedDirty -Value $relative
}
function Test-DeployPathAllowed {
param([string] $RelativePath)
$normalized = Normalize-RepoPath $RelativePath
Assert-RelativeDeployPath -RelativePath $normalized
$blockedPrefixes = @(
".git/",
".codex/",
".agents/runtime/workflows/",
".agents/runtime/executions/",
".agents/runtime/knowledge/",
".agents/runtime/route-packs/",
".agents/runtime/tool-evidence/",
".agents/runtime/deployments/",
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
$blockedFiles = @(
"docs/agent-status.md",
".agents/docs/agent-status.md",
".agents/runtime/collaborators.jsonl"
)
foreach ($prefix in $blockedPrefixes) {
if ($normalized.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
throw "Blocked target path selected for deployment: $normalized"
}
}
if ($blockedFiles -contains $normalized) {
throw "Blocked target path selected for deployment: $normalized"
}
}
function Test-DeploymentTextFile {
param([System.IO.FileInfo] $File)
$textExtensions = @(".md", ".yaml", ".yml", ".json", ".toml", ".ps1", ".txt", ".gitignore")
if ($File.Name -eq ".gitignore") {
return $true
}
return ($textExtensions -contains $File.Extension.ToLowerInvariant())
}
function Update-TargetStateClassification {
param(
[string] $Root,
[string] $Layout
)
$deployed = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
foreach ($file in ($DeployedFiles | Sort-Object -Unique)) {
[void] $deployed.Add((Normalize-RepoPath $file))
}
$historicalCandidates = @(
"docs/agents",
".agents/docs/agents",
"docs/runbooks",
".agents/docs/runbooks",
"docs/memory",
".agents/docs/memory",
"docs/decisions",
".agents/docs/decisions",
".agents/docs/project-memory.md",
".agents/.agents/docs/project-memory.md",
".agents/docs/project-structure.md",
".agents/.agents/docs/project-structure.md",
"docs/agent-status.md",
".agents/docs/agent-status.md",
"docs/agent-events",
".agents/docs/agent-events",
"docs/hard-isolation-evidence",
".agents/docs/hard-isolation-evidence",
"docs/runtime-multi-agent-validation",
".agents/docs/runtime-multi-agent-validation"
)
foreach ($relative in $historicalCandidates) {
$candidate = Join-TargetPath -Root $Root -RelativePath $relative
if (-not (Test-Path -LiteralPath $candidate)) {
continue
}
$items = if (Test-Path -LiteralPath $candidate -PathType Leaf) {
@(Get-Item -LiteralPath $candidate)
}
else {
@(Get-ChildItem -LiteralPath $candidate -Recurse -Force -File)
}
foreach ($item in $items) {
$itemRelative = Get-RelativeTargetPath -Root $Root -Path $item.FullName
if (-not $deployed.Contains($itemRelative)) {
$TargetHistoricalAgents.Add($itemRelative) | Out-Null
}
}
}
foreach ($file in (Get-TargetEnvironmentFiles -Root $Root)) {
$relative = Get-RelativeTargetPath -Root $Root -Path $file.FullName
Add-UniqueListItem -List $ProtectedDirty -Value $relative
}
foreach ($relative in @(".codex/config.toml", ".agents/runtime/agent-ledger.jsonl", ".agents/runtime/compact-events.jsonl", ".agents/runtime/collaborators.jsonl", ".agents/runtime/workflows/example/state.json", ".workflow/example/state.json", ".git/HEAD")) {
$candidate = Join-TargetPath -Root $Root -RelativePath $relative
if (Test-Path -LiteralPath $candidate) {
Add-UniqueListItem -List $ProtectedDirty -Value $relative
}
}
}
function Test-DeployedFileSet {
param(
[string] $Root,
[switch] $PlanOnly
)
$sourceLiterals = Get-SourceSpecificLiterals
foreach ($relative in ($DeployedFiles | Sort-Object -Unique)) {
$normalized = Normalize-RepoPath $relative
Test-DeployPathAllowed -RelativePath $normalized
if ($PlanOnly) {
continue
}
$path = Join-TargetPath -Root $Root -RelativePath $normalized
if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
throw "Deployed file set validation found a missing file: $normalized"
}
$item = Get-Item -LiteralPath $path
if (-not (Test-DeploymentTextFile -File $item)) {
continue
}
$lines = @(Get-Content -LiteralPath $path)
for ($i = 0; $i -lt $lines.Count; $i++) {
if ($lines[$i] -match "\s+$") {
throw "Deployed file set validation found trailing whitespace in $normalized line $($i + 1)."
}
}
foreach ($literal in $sourceLiterals) {
$match = Select-String -LiteralPath $path -Pattern $literal -SimpleMatch -Quiet -ErrorAction SilentlyContinue
if ($match) {
throw "Deployed file set validation found source-specific literal in $normalized`: $literal"
}
}
if ($normalized.EndsWith("/SKILL.md", [System.StringComparison]::OrdinalIgnoreCase)) {
$head = @(Get-Content -LiteralPath $path -TotalCount 10)
if ($head[0] -ne "---" -or -not ($head | Where-Object { $_ -match "^name:\s+.+" }) -or -not ($head | Where-Object { $_ -match "^description:\s+.+" })) {
throw "Deployed skill metadata is incomplete: $normalized"
}
}
if ($normalized.EndsWith("/openai.yaml", [System.StringComparison]::OrdinalIgnoreCase)) {
$content = @(Get-Content -LiteralPath $path)
if (-not ($content | Where-Object { $_ -match "^\s*default_prompt:" })) {
throw "Deployed agent metadata is missing default_prompt: $normalized"
}
}
}
}
function Get-TargetRelativePath {
param(
[string] $ProviderPath,
[string] $Layout
)
$path = Normalize-RepoPath $ProviderPath
if ($Layout -eq "dot_agents_docs") {
if ($path -eq "AGENTS.md") {
return $path
}
if ($path -like ".agents/skills/*") {
return $path
}
if ($path -like ".agents/docs/agents/*") {
return $path -replace "^.agents/docs/agents/", ".agents/.agents/docs/agents/"
}
if ($path -like ".agents/docs/runbooks/*") {
return $path -replace "^.agents/docs/runbooks/", ".agents/.agents/docs/runbooks/"
}
if ($path -like ".agents/docs/templates/agents/*") {
return $path -replace "^.agents/docs/templates/agents/", ".agents/.agents/docs/templates/agents/"
}
if ($path -eq ".agents/docs/project-memory.md") {
return ".agents/.agents/docs/project-memory.md"
}
if ($path -eq ".agents/docs/memory/index.md") {
return ".agents/.agents/docs/memory/index.md"
}
if ($path -eq ".agents/docs/memory/entries/README.md") {
return ".agents/.agents/docs/memory/entries/README.md"
}
if ($path -eq ".agents/docs/project-structure.md") {
return ".agents/.agents/docs/project-structure.md"
}
if ($path -like "docs/*.md") {
return $path -replace "^docs/", ".agents/docs/"
}
}
return $path
}
function Rewrite-ContentForLayout {
param(
[string] $Content,
[string] $Layout
)
if ($Layout -ne "dot_agents_docs") {
return $Content
}
$rewritten = $Content
$rewritten = $rewritten.Replace(".agents/docs/agents/", ".agents/.agents/docs/agents/")
$rewritten = $rewritten.Replace(".agents/docs/runbooks/", ".agents/.agents/docs/runbooks/")
$rewritten = $rewritten.Replace(".agents/docs/templates/agents/", ".agents/.agents/docs/templates/agents/")
$rewritten = $rewritten.Replace(".agents/docs/project-memory.md", ".agents/.agents/docs/project-memory.md")
$rewritten = $rewritten.Replace(".agents/docs/memory/index.md", ".agents/.agents/docs/memory/index.md")
$rewritten = $rewritten.Replace(".agents/docs/memory/entries/README.md", ".agents/.agents/docs/memory/entries/README.md")
$rewritten = $rewritten.Replace(".agents/docs/project-structure.md", ".agents/.agents/docs/project-structure.md")
return $rewritten
}
function Get-TargetLayout {
param([string] $Root)
$agentsPath = Join-Path $Root "AGENTS.md"
$rootDocsPath = Join-Path $Root "docs/agents"
$dotAgentsDocsPath = Join-Path $Root ".agents/docs/agents"
$dotAgentsSkillsPath = Join-Path $Root ".agents/skills"
$hasRootDocs = Test-Path -LiteralPath $rootDocsPath -PathType Container
$hasDotAgentsDocs = Test-Path -LiteralPath $dotAgentsDocsPath -PathType Container
$hasDotAgentsSkills = Test-Path -LiteralPath $dotAgentsSkillsPath -PathType Container
if (Test-Path -LiteralPath $agentsPath -PathType Leaf) {
$agentsContent = Get-Content -LiteralPath $agentsPath -Raw
if ($agentsContent -match "\.agents/docs/agents") {
return "dot_agents_docs"
}
if ($agentsContent -match "docs/agents") {
return "root_docs"
}
}
if ($hasRootDocs -and $hasDotAgentsDocs) {
throw "Target has both docs/agents and .agents/docs/agents without an AGENTS.md route. Resolve the layout or provide an AGENTS.md route before deployment."
}
if ($hasDotAgentsDocs -or $hasDotAgentsSkills) {
return "dot_agents_docs"
}
if ($hasRootDocs) {
return "root_docs"
}
return "root_docs"
}
function Get-ModeGroups {
param([string] $SelectedMode)
if ($SelectedMode -eq "core_bootstrap") {
return @("core_bootstrap")
}
if ($SelectedMode -eq "full_workflow") {
return @("core_bootstrap", "full_workflow_additions")
}
return @("core_bootstrap", "full_workflow_additions", "template_provider_additions")
}
function Get-DeployEntries {
param([string[]] $Groups)
$entries = New-Object System.Collections.Generic.List[object]
$currentGroup = $null
$currentFrom = $null
$insideDeployable = $false
function Add-DeployBundle {
param(
[string] $Group,
[string] $Directory,
[string[]] $Filters
)
foreach ($filter in $Filters) {
$bundleFiles = Get-ChildItem -LiteralPath (Join-Path $RepoRoot $Directory) -Filter $filter -File | Sort-Object FullName
foreach ($file in $bundleFiles) {
$relative = Normalize-RepoPath ($file.FullName.Substring($RepoRoot.Path.Length).TrimStart("\", "/"))
$entries.Add([pscustomobject]@{
Group = $Group
From = $relative
To = $relative
}) | Out-Null
}
}
}
foreach ($line in Get-Content -LiteralPath $DeployManifestPath) {
if ($line -match "^deployable_by_mode:") {
$insideDeployable = $true
continue
}
if ($insideDeployable -and $line -match "^[a-zA-Z_]+:") {
break
}
if (-not $insideDeployable) {
continue
}
if ($line -match "^  ([a-zA-Z0-9_]+):") {
$currentGroup = $Matches[1]
$currentFrom = $null
continue
}
if ($line -match '^\s+- from: "([^"]+)"') {
$currentFrom = $Matches[1]
continue
}
if (($Groups -contains $currentGroup) -and $line -match '^\s+schema_bundle:') {
Add-DeployBundle -Group $currentGroup -Directory "schemas" -Filters @("README.md", "*.schema.json")
$currentFrom = $null
continue
}
if (($Groups -contains $currentGroup) -and $line -match '^\s+script_bundle:') {
Add-DeployBundle -Group $currentGroup -Directory "scripts" -Filters @("README.md", "*.ps1")
$currentFrom = $null
continue
}
if ($line -match '^\s+to: "([^"]+)"') {
if ($Groups -contains $currentGroup) {
$entries.Add([pscustomobject]@{
Group = $currentGroup
From = $currentFrom
To = $Matches[1]
}) | Out-Null
}
$currentFrom = $null
}
}
if ($Groups -contains "template_provider_additions") {
$templateFiles = Get-ChildItem -LiteralPath (Join-Path $RepoRoot "docs/templates/agents") -Recurse -File
foreach ($file in $templateFiles) {
$relative = Normalize-RepoPath ($file.FullName.Substring($RepoRoot.Path.Length).TrimStart("\", "/"))
$entries.Add([pscustomobject]@{
Group = "template_provider_additions"
From = $relative
To = $relative
}) | Out-Null
}
}
return $entries.ToArray()
}
function Confirm-SourceAllowed {
param([string] $SourcePath)
$normalized = Normalize-RepoPath $SourcePath
Assert-RelativeDeployPath -RelativePath $normalized
$blockedPrefixes = @(
".git/",
".agents/runtime/workflows/",
".agents/runtime/executions/",
".agents/runtime/knowledge/",
".agents/runtime/route-packs/",
".agents/runtime/tool-evidence/",
".agents/runtime/deployments/",
".agents/runtime/",
".workflow/",
".codex/",
"docs/agent-events/",
"docs/tmp-approval-",
"docs/hard-isolation-evidence/",
"docs/runtime-multi-agent-validation/"
)
$blockedFiles = @(
"docs/agent-status.md",
".agents/runtime/collaborators.jsonl"
)
foreach ($prefix in $blockedPrefixes) {
if ($normalized.StartsWith($prefix)) {
throw "Blocked source path selected for deployment: $normalized"
}
}
if ($blockedFiles -contains $normalized) {
throw "Blocked source path selected for deployment: $normalized"
}
}
function Append-GitignoreFragment {
param(
[string] $Root,
[switch] $PlanOnly
)
$targetGitignore = Join-Path $Root ".gitignore"
$fragment = @(Get-Content -LiteralPath $GitignoreFragmentPath)
$existing = @()
if (Test-Path -LiteralPath $targetGitignore -PathType Leaf) {
$existing = @(Get-Content -LiteralPath $targetGitignore)
}
$missing = @($fragment | Where-Object { $_.Trim().Length -gt 0 -and ($existing -notcontains $_) })
if ($missing.Count -eq 0) {
$DeployedFiles.Add(".gitignore") | Out-Null
$UnchangedExisting.Add(".gitignore") | Out-Null
return
}
$PlannedWrites.Add(".gitignore append/adapt") | Out-Null
$DeployedFiles.Add(".gitignore") | Out-Null
if ($PlanOnly) {
return
}
if (-not (Test-Path -LiteralPath $targetGitignore -PathType Leaf)) {
Set-Content -LiteralPath $targetGitignore -Value $fragment -Encoding utf8
return
}
$appendLines = New-Object System.Collections.Generic.List[string]
if ($existing.Count -gt 0 -and $existing[$existing.Count - 1].Trim().Length -gt 0) {
$appendLines.Add("") | Out-Null
}
foreach ($line in $missing) {
$appendLines.Add($line) | Out-Null
}
Add-Content -LiteralPath $targetGitignore -Value $appendLines
}
function Copy-DeployEntry {
param(
[object] $Entry,
[string] $Root,
[string] $Layout,
[switch] $PlanOnly
)
if ($Entry.To -like "*append/adapt*") {
Append-GitignoreFragment -Root $Root -PlanOnly:$PlanOnly
return
}
Confirm-SourceAllowed -SourcePath $Entry.From
$sourcePath = Join-Path $RepoRoot $Entry.From
Assert-InsideRoot -Path $sourcePath -Root $RepoRoot.Path -Label "Source path"
if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
throw "Deploy source is missing: $($Entry.From)"
}
$targetRelative = Get-TargetRelativePath -ProviderPath $Entry.To -Layout $Layout
$targetFull = Join-TargetPath -Root $Root -RelativePath $targetRelative
$DeployedFiles.Add($targetRelative) | Out-Null
$content = Get-Content -LiteralPath $sourcePath -Raw
$content = Rewrite-ContentForLayout -Content $content -Layout $Layout
$state = Get-DeployWriteState -Path $targetFull -Content $content
if ($state -eq "create" -or ($state -eq "upgrade_required" -and $Upgrade)) {
$PlannedWrites.Add($targetRelative) | Out-Null
}
elseif ($state -eq "current") {
$UnchangedExisting.Add($targetRelative) | Out-Null
}
else {
$ProtectedExisting.Add($targetRelative) | Out-Null
}
if ($PlanOnly) {
return
}
$shouldWrite = Assert-DeployWriteAllowed -Path $targetFull -RelativePath $targetRelative -Content $content
if (-not $shouldWrite) {
return
}
$targetDir = Split-Path -Parent $targetFull
if (-not (Test-Path -LiteralPath $targetDir -PathType Container)) {
New-Item -ItemType Directory -Path $targetDir | Out-Null
}
Set-Content -LiteralPath $targetFull -Value $content -NoNewline -Encoding utf8
}
function Write-DeploymentReport {
param(
[string] $Root,
[string] $Layout,
[switch] $PlanOnly
)
$reportRelative = if ($Layout -eq "dot_agents_docs") {
".agents/docs/agents-workflow-deployment.md"
}
else {
"docs/agents-workflow-deployment.md"
}
$deployedVersionRelative = if ($Layout -eq "dot_agents_docs") {
".agents/.agents/docs/agents/version.yaml"
}
else {
".agents/docs/agents/version.yaml"
}
$deployedValidationState = if ($PlanOnly) { "planned" } else { "checked" }
$reportPath = Join-TargetPath -Root $Root -RelativePath $reportRelative
$DeployedFiles.Add($reportRelative) | Out-Null
if ($PlanOnly) {
if (Test-Path -LiteralPath $reportPath -PathType Leaf) {
$UnchangedExisting.Add($reportRelative) | Out-Null
}
else {
$PlannedWrites.Add($reportRelative) | Out-Null
}
return
}
$reportLines = @(
"# Agents Workflow Deployment",
"",
("Mode: {0}" -f $Mode),
("Upgrade: {0}" -f [bool] $Upgrade),
("Layout: {0}" -f $Layout),
("Dry run: {0}" -f [bool] $PlanOnly),
("Workflow version: {0}" -f $WorkflowVersion.Version),
("Workflow channel: {0}" -f $WorkflowVersion.Channel),
("Version source: {0}" -f $WorkflowVersion.Source),
("Deployed version file: {0}" -f $deployedVersionRelative),
"",
"## Deployed File Set",
""
)
foreach ($file in ($DeployedFiles | Sort-Object -Unique)) {
$reportLines += ("- {0}" -f $file)
}
$feedbackRelative = if ($Layout -eq "dot_agents_docs") {
".agents/docs/deployment-feedback.template.md"
}
else {
"docs/deployment-feedback.template.md"
}
$feedbackAction = if ($Mode -eq "core_bootstrap") {
"Use a target-owned tracker for deployment feedback, or upgrade to full_workflow before filling the feedback template."
}
else {
"Record target-specific feedback in $feedbackRelative or a target-owned tracker; do not store target deployment history in the provider repo."
}
$reportLines += @(
"",
"## Version Alignment",
"",
("- Provider version source: {0}" -f $WorkflowVersion.Source),
("- Deployed version file: {0}" -f $deployedVersionRelative),
("- Workflow version: {0}" -f $WorkflowVersion.Version),
("- Workflow channel: {0}" -f $WorkflowVersion.Channel),
"",
"## What Changed",
"",
"Planned or completed create/update:"
)
if ($PlannedWrites.Count -gt 0) {
foreach ($file in ($PlannedWrites | Sort-Object -Unique)) {
$reportLines += ("- {0}" -f $file)
}
}
else {
$reportLines += "- none observed"
}
$reportLines += @(
"",
"Already current in target:"
)
if ($UnchangedExisting.Count -gt 0) {
foreach ($file in ($UnchangedExisting | Sort-Object -Unique)) {
$reportLines += ("- {0}" -f $file)
}
}
else {
$reportLines += "- none observed"
}
$reportLines += @(
"",
"Requires -Upgrade before write:"
)
if ($ProtectedExisting.Count -gt 0) {
foreach ($file in ($ProtectedExisting | Sort-Object -Unique)) {
$reportLines += ("- {0}" -f $file)
}
}
else {
$reportLines += "- none observed"
}
$reportLines += @(
"",
"Target-local Codex environment bootstrap:"
)
if ($TargetLocalEnvironment.Count -gt 0) {
foreach ($file in ($TargetLocalEnvironment | Sort-Object -Unique)) {
$reportLines += ("- {0}" -f $file)
}
}
else {
$reportLines += "- none observed"
}
$reportLines += @(
"",
"## What Was Intentionally Not Touched",
"",
"- Target app/source files outside deployed_file_set.",
"- Runtime/local Codex config, agent status, ledger, evidence records, and Git metadata.",
"- Existing .codex/environments/*.toml files; target-local bootstrap creates a project-named file only when none exists.",
"- Target-owned historical Agents files outside deployed_file_set.",
"",
"## Target Owner Next Actions",
"",
"- Review this deployment report and target git status.",
"- Decide whether to commit or revert the deployed_file_set in the target repo according to target policy.",
"- Run a target handoff check: AGENTS route, selected project skill path, runbook links, protected runtime/local paths, and target git status summary.",
("- {0}" -f $feedbackAction)
)
$reportLines += @(
"",
"## Target State Classification",
"",
"Protected dirty/local state:"
)
if ($ProtectedDirty.Count -gt 0) {
foreach ($file in ($ProtectedDirty | Sort-Object -Unique)) {
$reportLines += ("- {0}" -f $file)
}
}
else {
$reportLines += "- none observed"
}
$reportLines += @(
"",
"Target-owned historical Agents files outside deployed file set:"
)
if ($TargetHistoricalAgents.Count -gt 0) {
foreach ($file in ($TargetHistoricalAgents | Sort-Object -Unique)) {
$reportLines += ("- {0}" -f $file)
}
}
else {
$reportLines += "- none observed"
}
$reportLines += @(
"",
"## Validation Summary",
"",
"- Deployment path blocklist: enforced.",
("- Deployed file set validation: {0}." -f $deployedValidationState),
"- Target-owned runtime/local state: classified outside deployed file set.",
"- Target-local Codex environment bootstrap: checked outside deployed file set.",
"",
"## Notes",
"",
"- Runtime, local Codex config, Git metadata, status, event, and filled evidence files are not deployed.",
"- Provider environment templates are never deployed; target environments stay local.",
"- Target-owned historical Agents files are reported separately and remain target-owned."
)
$reportContent = ($reportLines -join [System.Environment]::NewLine) + [System.Environment]::NewLine
$state = Get-DeployWriteState -Path $reportPath -Content $reportContent
if ($state -eq "create" -or $state -eq "upgrade_required") {
$PlannedWrites.Add($reportRelative) | Out-Null
}
elseif ($state -eq "current") {
$UnchangedExisting.Add($reportRelative) | Out-Null
}
else {
$ProtectedExisting.Add($reportRelative) | Out-Null
}
if ($state -eq "current") {
return
}
$reportDir = Split-Path -Parent $reportPath
if (-not (Test-Path -LiteralPath $reportDir -PathType Container)) {
New-Item -ItemType Directory -Path $reportDir | Out-Null
}
Set-Content -LiteralPath $reportPath -Value $reportContent -NoNewline -Encoding utf8
}
function Write-DeploymentCloseoutSummary {
param(
[string] $Layout
)
$feedbackRelative = if ($Layout -eq "dot_agents_docs") {
".agents/docs/deployment-feedback.template.md"
}
else {
"docs/deployment-feedback.template.md"
}
$feedbackAction = if ($Mode -eq "core_bootstrap") {
"Use a target-owned tracker for feedback, or deploy full_workflow before filling the feedback template."
}
else {
"Record feedback in $feedbackRelative or a target-owned tracker."
}
Write-Step "INFO" "Deployment closeout summary:"
Write-Step "INFO" "What changed:"
if ($PlannedWrites.Count -gt 0) {
foreach ($file in ($PlannedWrites | Sort-Object -Unique)) {
Write-Step "WRITE" $file
}
}
else {
Write-Step "WRITE" "none observed"
}
Write-Step "INFO" "What was intentionally not touched:"
Write-Step "SKIP" "Target app/source files outside deployed_file_set."
Write-Step "SKIP" "Runtime/local Codex config, agent status, ledger, evidence records, and Git metadata."
Write-Step "SKIP" "Existing .codex/environments/*.toml files are preserved; project-named environment is created only when absent."
Write-Step "SKIP" "Target-owned historical Agents files outside deployed_file_set."
Write-Step "INFO" "Target-local Codex environment bootstrap:"
if ($TargetLocalEnvironment.Count -gt 0) {
foreach ($file in ($TargetLocalEnvironment | Sort-Object -Unique)) {
Write-Step "ENV" $file
}
}
else {
Write-Step "ENV" "none observed"
}
Write-Step "INFO" "Target owner next actions:"
Write-Step "NEXT" "Review deployment report and target git status."
Write-Step "NEXT" "Run target handoff check: AGENTS route, project skill path, runbook links, protected runtime/local paths, and git status summary."
Write-Step "NEXT" $feedbackAction
}
function Reset-Directory {
param(
[string] $Path,
[string] $AllowedRoot
)
if (Test-Path -LiteralPath $Path) {
Assert-InsideRoot -Path $Path -Root $AllowedRoot -Label "Self-test cleanup path"
Remove-Item -LiteralPath $Path -Recurse -Force
}
New-Item -ItemType Directory -Path $Path | Out-Null
}
function Assert-SelfTestFile {
param(
[string] $Root,
[string] $RelativePath
)
$path = Join-TargetPath -Root $Root -RelativePath $RelativePath
if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
throw "Deployment self-test expected file is missing: $RelativePath"
}
}
function Assert-SelfTestMissing {
param(
[string] $Root,
[string] $RelativePath
)
$path = Join-TargetPath -Root $Root -RelativePath $RelativePath
if (Test-Path -LiteralPath $path) {
throw "Deployment self-test expected path to be absent: $RelativePath"
}
}
function Assert-SelfTestContent {
param(
[string] $Path,
[string] $Expected
)
$content = Get-Content -LiteralPath $Path -Raw
if ($content.TrimEnd() -ne $Expected) {
throw "Deployment self-test target-owned file changed: $Path"
}
}
function Assert-SelfTestContains {
param(
[string] $Path,
[string] $Expected
)
$content = Get-Content -LiteralPath $Path -Raw
if (-not $content.Contains($Expected)) {
throw "Deployment self-test expected content is missing from: $Path"
}
}
function Assert-SelfTestLineCount {
param(
[string] $Path,
[string] $Expected,
[int] $Count
)
$matches = @(Get-Content -LiteralPath $Path | Where-Object { $_ -eq $Expected })
if ($matches.Count -ne $Count) {
throw "Deployment self-test expected line count $Count for '$Expected' in: $Path"
}
}
function Assert-SelfTestTextContains {
param(
[string] $Text,
[string] $Expected
)
if (-not $Text.Contains($Expected)) {
throw "Deployment self-test expected output is missing: $Expected"
}
}
function Assert-SelfTestBlockedDeployPath {
param([string] $RelativePath)
$blocked = $false
try {
Test-DeployPathAllowed -RelativePath $RelativePath
}
catch {
$blocked = $true
}
if (-not $blocked) {
throw "Deployment self-test expected blocked deployed path rejection: $RelativePath"
}
}
function Assert-NoSourceLiteral {
param([string] $Root)
$sourceLiterals = Get-SourceSpecificLiterals
$files = Get-ChildItem -LiteralPath $Root -Recurse -File |
Where-Object { $_.Extension.ToLowerInvariant() -in @(".md", ".yaml", ".yml", ".json", ".toml", ".ps1") }
foreach ($literal in $sourceLiterals) {
foreach ($file in $files) {
$match = Select-String -LiteralPath $file.FullName -Pattern $literal -SimpleMatch -Quiet -ErrorAction SilentlyContinue
if ($match) {
throw "Deployment self-test found source-specific literal in target file: $($file.FullName)"
}
}
}
}
function Invoke-SelfTestGit {
param(
[string] $Root,
[string[]] $Arguments
)
$output = & git -C $Root @Arguments 2>&1
if ($LASTEXITCODE -ne 0) {
throw "Deployment self-test git command failed in ${Root}: git $($Arguments -join ' '): $($output -join ' ')"
}
return @($output)
}
function Invoke-ChildDeployment {
param([hashtable] $CommandArgs)
try {
& $PSCommandPath @CommandArgs
if (-not $?) {
throw "Child deployment command returned a failed status."
}
}
catch {
throw $_
}
}
function Invoke-ChildDeploymentOutput {
param([hashtable] $CommandArgs)
try {
$output = & $PSCommandPath @CommandArgs 2>&1
return ($output -join [System.Environment]::NewLine)
}
catch {
throw $_
}
}
function Get-SafeStatusProjectId {
param([string] $SourceRoot)
$leaf = Split-Path -Leaf $SourceRoot
$safe = ($leaf.ToLowerInvariant() -replace "[^a-z0-9._-]+", "-").Trim("-._")
if ([string]::IsNullOrWhiteSpace($safe)) {
return "agents-workflow"
}
return $safe
}
function Invoke-DeploymentSelfTest {
$projectId = Get-SafeStatusProjectId -SourceRoot $RepoRoot.Path
$statusRoot = Join-Path ([System.IO.Path]::GetTempPath()) "codex-agent-status"
$projectRoot = Join-Path $statusRoot $projectId
$selfTestRoot = Join-Path $projectRoot ("deploy-selftest-{0}" -f $PID)
if (-not (Test-Path -LiteralPath $statusRoot -PathType Container)) {
New-Item -ItemType Directory -Path $statusRoot | Out-Null
}
if (-not (Test-Path -LiteralPath $projectRoot -PathType Container)) {
New-Item -ItemType Directory -Path $projectRoot | Out-Null
}
Assert-InsideRoot -Path $selfTestRoot -Root $projectRoot -Label "Self-test root"
Reset-Directory -Path $selfTestRoot -AllowedRoot $projectRoot
foreach ($blockedPath in @(
".git/HEAD",
".codex/config.toml",
".codex/environments/environment.toml",
".codex/environments/environment.template.toml",
".codex/environments/project.toml",
".agents/runtime/agent-ledger.jsonl",
".agents/runtime/compact-events.jsonl",
".agents/runtime/collaborators.jsonl",
".agents/runtime/workflows/example/state.json",
".workflow/example/state.json",
"docs/agent-status.md",
".agents/docs/agent-status.md",
"docs/agent-events/event.jsonl",
".agents/docs/agent-events/event.jsonl",
"docs/tmp-approval-example/report.md",
".agents/docs/tmp-approval-example/report.md",
"docs/hard-isolation-evidence/example.md",
".agents/docs/hard-isolation-evidence/example.md",
"docs/runtime-multi-agent-validation/example.md",
".agents/docs/runtime-multi-agent-validation/example.md"
)) {
Assert-SelfTestBlockedDeployPath -RelativePath $blockedPath
}
$rootTarget = Join-Path $selfTestRoot "root-docs"
Invoke-ChildDeployment -CommandArgs @{ TargetPath = $rootTarget; Mode = "full_workflow"; CreateTarget = $true; Quiet = $true }
Assert-SelfTestFile -Root $rootTarget -RelativePath "AGENTS.md"
Assert-SelfTestFile -Root $rootTarget -RelativePath ".agents/docs/agents/ai-runtime.yaml"
Assert-SelfTestFile -Root $rootTarget -RelativePath ".agents/docs/agents/workflows.yaml"
Assert-SelfTestFile -Root $rootTarget -RelativePath ".agents/docs/agents/workflow-artifacts.yaml"
Assert-SelfTestFile -Root $rootTarget -RelativePath ".agents/docs/agents/context-compact.yaml"
Assert-SelfTestFile -Root $rootTarget -RelativePath ".agents/docs/agents/collaborators.yaml"
Assert-SelfTestFile -Root $rootTarget -RelativePath "docs/deployment-feedback.template.md"
Assert-SelfTestFile -Root $rootTarget -RelativePath "docs/agents-workflow-deployment.md"
Assert-SelfTestFile -Root $rootTarget -RelativePath ".codex/environments/root-docs.toml"
Assert-SelfTestContains -Path (Join-Path $rootTarget "docs/agents-workflow-deployment.md") -Expected "- .agents/docs/agents/workflows.yaml"
Assert-SelfTestContains -Path (Join-Path $rootTarget "docs/agents-workflow-deployment.md") -Expected "## What Changed"
Assert-SelfTestContains -Path (Join-Path $rootTarget "docs/agents-workflow-deployment.md") -Expected "## What Was Intentionally Not Touched"
Assert-SelfTestContains -Path (Join-Path $rootTarget "docs/agents-workflow-deployment.md") -Expected "## Target Owner Next Actions"
Assert-SelfTestContains -Path (Join-Path $rootTarget "docs/agents-workflow-deployment.md") -Expected "Target-local Codex environment bootstrap:"
Assert-SelfTestContains -Path (Join-Path $rootTarget "docs/agents-workflow-deployment.md") -Expected "- created: .codex/environments/root-docs.toml"
Assert-SelfTestContains -Path (Join-Path $rootTarget "docs/agents-workflow-deployment.md") -Expected "target handoff check"
Assert-SelfTestContains -Path (Join-Path $rootTarget "docs/agents-workflow-deployment.md") -Expected "## Validation Summary"
Assert-SelfTestContains -Path (Join-Path $rootTarget "docs/agents-workflow-deployment.md") -Expected "- Deployment path blocklist: enforced."
Assert-SelfTestContains -Path (Join-Path $rootTarget "docs/agents-workflow-deployment.md") -Expected ("Workflow version: {0}" -f $WorkflowVersion.Version)
Assert-SelfTestContains -Path (Join-Path $rootTarget "docs/agents-workflow-deployment.md") -Expected ("Workflow channel: {0}" -f $WorkflowVersion.Channel)
Assert-SelfTestContains -Path (Join-Path $rootTarget "docs/agents-workflow-deployment.md") -Expected "Version source: .agents/docs/agents/version.yaml"
Assert-SelfTestContains -Path (Join-Path $rootTarget "docs/agents-workflow-deployment.md") -Expected "- Deployed version file: .agents/docs/agents/version.yaml"
Assert-NoSourceLiteral -Root $rootTarget
Invoke-ChildDeployment -CommandArgs @{ TargetPath = $rootTarget; Mode = "full_workflow"; Quiet = $true }
$currentPlan = Invoke-ChildDeploymentOutput -CommandArgs @{ TargetPath = $rootTarget; Mode = "full_workflow"; DryRun = $true }
Assert-SelfTestTextContains -Text $currentPlan -Expected "Existing target files already current:"
Assert-SelfTestTextContains -Text $currentPlan -Expected ("Workflow version: {0} ({1})" -f $WorkflowVersion.Version, $WorkflowVersion.Channel)
Assert-SelfTestTextContains -Text $currentPlan -Expected "[CURRENT] AGENTS.md"
Assert-SelfTestTextContains -Text $currentPlan -Expected "Target-local Codex environment bootstrap:"
Assert-SelfTestTextContains -Text $currentPlan -Expected "[ENV] preserve: .codex/environments/root-docs.toml"
$templateProviderTarget = Join-Path $selfTestRoot "template-provider"
Invoke-ChildDeployment -CommandArgs @{ TargetPath = $templateProviderTarget; Mode = "template_provider_mode"; CreateTarget = $true; Quiet = $true }
Assert-SelfTestFile -Root $templateProviderTarget -RelativePath ".agents/docs/templates/agents/AGENTS.md"
Assert-SelfTestFile -Root $templateProviderTarget -RelativePath ".agents/docs/templates/agents/agents/deploy.yaml"
Assert-SelfTestFile -Root $templateProviderTarget -RelativePath ".agents/docs/templates/agents/agents/workflow-artifacts.yaml"
Assert-SelfTestFile -Root $templateProviderTarget -RelativePath ".agents/docs/templates/agents/agents/context-compact.yaml"
Assert-SelfTestFile -Root $templateProviderTarget -RelativePath ".agents/docs/templates/agents/agents/collaborators.yaml"
Assert-SelfTestFile -Root $templateProviderTarget -RelativePath ".agents/docs/templates/agents/deployment-feedback.template.md"
Assert-NoSourceLiteral -Root $templateProviderTarget
$dotTarget = Join-Path $selfTestRoot "dot-agents-docs"
New-Item -ItemType Directory -Path $dotTarget | Out-Null
New-Item -ItemType Directory -Path (Join-Path $dotTarget ".agents/skills") -Force | Out-Null
Set-Content -LiteralPath (Join-Path $dotTarget "AGENTS.md") -Value "Route to .agents/docs/agents." -Encoding utf8
Invoke-ChildDeployment -CommandArgs @{ TargetPath = $dotTarget; Mode = "core_bootstrap"; Upgrade = $true; Quiet = $true }
Assert-SelfTestFile -Root $dotTarget -RelativePath "AGENTS.md"
Assert-SelfTestFile -Root $dotTarget -RelativePath ".agents/.agents/docs/agents/ai-runtime.yaml"
Assert-SelfTestFile -Root $dotTarget -RelativePath ".agents/.agents/docs/agents/workflows.yaml"
Assert-SelfTestFile -Root $dotTarget -RelativePath ".agents/.agents/docs/agents/workflow-artifacts.yaml"
Assert-SelfTestFile -Root $dotTarget -RelativePath ".agents/.agents/docs/agents/context-compact.yaml"
Assert-SelfTestFile -Root $dotTarget -RelativePath ".agents/.agents/docs/agents/collaborators.yaml"
Assert-SelfTestFile -Root $dotTarget -RelativePath ".agents/docs/agents-workflow-deployment.md"
Invoke-ChildDeployment -CommandArgs @{ TargetPath = $dotTarget; Mode = "full_workflow"; Upgrade = $true; Quiet = $true }
Assert-SelfTestFile -Root $dotTarget -RelativePath ".agents/.agents/docs/project-memory.md"
Assert-SelfTestFile -Root $dotTarget -RelativePath ".agents/.agents/docs/memory/index.md"
Assert-SelfTestFile -Root $dotTarget -RelativePath ".agents/.agents/docs/project-structure.md"
Assert-SelfTestFile -Root $dotTarget -RelativePath ".agents/.agents/docs/runbooks/session-handoff.md"
Assert-SelfTestFile -Root $dotTarget -RelativePath ".agents/docs/agent-status.template.md"
Assert-SelfTestFile -Root $dotTarget -RelativePath ".agents/docs/deployment-feedback.template.md"
Invoke-ChildDeployment -CommandArgs @{ TargetPath = $dotTarget; Mode = "template_provider_mode"; Upgrade = $true; Quiet = $true }
Assert-SelfTestFile -Root $dotTarget -RelativePath ".agents/.agents/docs/templates/agents/AGENTS.md"
Assert-SelfTestFile -Root $dotTarget -RelativePath ".agents/.agents/docs/templates/agents/agents/deploy.yaml"
Assert-SelfTestFile -Root $dotTarget -RelativePath ".agents/.agents/docs/templates/agents/agents/workflow-artifacts.yaml"
Assert-SelfTestFile -Root $dotTarget -RelativePath ".agents/.agents/docs/templates/agents/agents/context-compact.yaml"
Assert-SelfTestFile -Root $dotTarget -RelativePath ".agents/.agents/docs/templates/agents/agents/collaborators.yaml"
Assert-SelfTestContains -Path (Join-Path $dotTarget ".agents/docs/agents-workflow-deployment.md") -Expected "- .agents/.agents/docs/templates/agents/agents/deploy.yaml"
Assert-SelfTestContains -Path (Join-Path $dotTarget ".agents/docs/agents-workflow-deployment.md") -Expected "- Deployed version file: .agents/.agents/docs/agents/version.yaml"
Assert-NoSourceLiteral -Root $dotTarget
$protectedTarget = Join-Path $selfTestRoot "protected-existing"
New-Item -ItemType Directory -Path $protectedTarget | Out-Null
Set-Content -LiteralPath (Join-Path $protectedTarget "AGENTS.md") -Value "target-owned agents" -Encoding utf8
$protectedPlan = Invoke-ChildDeploymentOutput -CommandArgs @{ TargetPath = $protectedTarget; Mode = "core_bootstrap"; DryRun = $true }
Assert-SelfTestTextContains -Text $protectedPlan -Expected "Existing target files requiring -Upgrade:"
Assert-SelfTestTextContains -Text $protectedPlan -Expected "[EXISTING] AGENTS.md"
Assert-SelfTestContent -Path (Join-Path $protectedTarget "AGENTS.md") -Expected "target-owned agents"
$protectedWriteBlocked = $false
try {
Invoke-ChildDeployment -CommandArgs @{ TargetPath = $protectedTarget; Mode = "core_bootstrap"; Quiet = $true }
}
catch {
$protectedWriteBlocked = $true
}
if (-not $protectedWriteBlocked) {
throw "Deployment self-test expected existing target file to require -Upgrade."
}
Assert-SelfTestContent -Path (Join-Path $protectedTarget "AGENTS.md") -Expected "target-owned agents"
Invoke-ChildDeployment -CommandArgs @{ TargetPath = $protectedTarget; Mode = "core_bootstrap"; Upgrade = $true; Quiet = $true }
Assert-SelfTestFile -Root $protectedTarget -RelativePath ".agents/docs/agents/workflows.yaml"
$dryRunTarget = Join-Path $selfTestRoot "dry-run"
New-Item -ItemType Directory -Path $dryRunTarget | Out-Null
Invoke-ChildDeployment -CommandArgs @{ TargetPath = $dryRunTarget; Mode = "core_bootstrap"; DryRun = $true; Quiet = $true }
Assert-SelfTestMissing -Root $dryRunTarget -RelativePath "AGENTS.md"
Assert-SelfTestMissing -Root $dryRunTarget -RelativePath ".agents/docs/agents/workflows.yaml"
Assert-SelfTestMissing -Root $dryRunTarget -RelativePath ".codex/environments/dry-run.toml"
$foreignTarget = Join-Path $selfTestRoot "git-backed-foreign-project"
New-Item -ItemType Directory -Path (Join-Path $foreignTarget "src") -Force | Out-Null
Set-Content -LiteralPath (Join-Path $foreignTarget "README.md") -Value "# Foreign Project" -Encoding utf8
Set-Content -LiteralPath (Join-Path $foreignTarget "src/app.txt") -Value "target app code" -Encoding utf8
Set-Content -LiteralPath (Join-Path $foreignTarget ".gitignore") -Value "bin/" -Encoding utf8
Invoke-SelfTestGit -Root $foreignTarget -Arguments @("init") | Out-Null
Invoke-SelfTestGit -Root $foreignTarget -Arguments @("config", "core.autocrlf", "false") | Out-Null
Invoke-SelfTestGit -Root $foreignTarget -Arguments @("add", "README.md", "src/app.txt", ".gitignore") | Out-Null
Invoke-SelfTestGit -Root $foreignTarget -Arguments @("-c", "user.name=Agents Self Test", "-c", "user.email=agents-selftest@example.invalid", "commit", "-m", "Initial foreign project") | Out-Null
Invoke-ChildDeployment -CommandArgs @{ TargetPath = $foreignTarget; Mode = "core_bootstrap"; Quiet = $true }
Assert-SelfTestContent -Path (Join-Path $foreignTarget "README.md") -Expected "# Foreign Project"
Assert-SelfTestContent -Path (Join-Path $foreignTarget "src/app.txt") -Expected "target app code"
$appStatus = Invoke-SelfTestGit -Root $foreignTarget -Arguments @("status", "--porcelain", "--", "README.md", "src/app.txt")
if (@($appStatus).Count -gt 0) {
throw "Deployment self-test expected foreign project app files to stay clean: $($appStatus -join '; ')"
}
Assert-SelfTestContains -Path (Join-Path $foreignTarget ".gitignore") -Expected "bin/"
Assert-SelfTestContains -Path (Join-Path $foreignTarget ".gitignore") -Expected ".agents/runtime/"
Assert-SelfTestContains -Path (Join-Path $foreignTarget ".gitignore") -Expected ".agents/runtime/collaborators.jsonl"
Assert-SelfTestContains -Path (Join-Path $foreignTarget ".gitignore") -Expected ".agents/runtime/workflows/"
Assert-SelfTestContains -Path (Join-Path $foreignTarget ".gitignore") -Expected ".workflow/"
Assert-SelfTestContains -Path (Join-Path $foreignTarget ".gitignore") -Expected ".codex/config.toml"
Assert-SelfTestContains -Path (Join-Path $foreignTarget ".gitignore") -Expected ".codex/environments/environment.toml"
Assert-SelfTestContains -Path (Join-Path $foreignTarget ".gitignore") -Expected ".codex/environments/*.toml"
Assert-SelfTestFile -Root $foreignTarget -RelativePath ".codex/environments/git-backed-foreign-project.toml"
$envStatus = Invoke-SelfTestGit -Root $foreignTarget -Arguments @("status", "--porcelain", "--", ".codex/environments/git-backed-foreign-project.toml")
if (@($envStatus).Count -gt 0) {
throw "Deployment self-test expected target-local environment to stay ignored: $($envStatus -join '; ')"
}
Invoke-SelfTestGit -Root $foreignTarget -Arguments @("add", "--", "AGENTS.md", "docs/agents", "docs/runbooks", ".agents/skills", "docs/agents-workflow-deployment.md", ".gitignore") | Out-Null
Invoke-SelfTestGit -Root $foreignTarget -Arguments @("-c", "user.name=Agents Self Test", "-c", "user.email=agents-selftest@example.invalid", "commit", "-m", "Deploy agents workflow") | Out-Null
$rollbackScope = Invoke-SelfTestGit -Root $foreignTarget -Arguments @("diff", "--name-only", "HEAD~1..HEAD", "--")
$rollbackScopeText = ($rollbackScope -join [Environment]::NewLine)
Assert-SelfTestTextContains -Text $rollbackScopeText -Expected "AGENTS.md"
Assert-SelfTestTextContains -Text $rollbackScopeText -Expected ".agents/docs/agents/workflows.yaml"
foreach ($appPath in @("README.md", "src/app.txt")) {
if ($rollbackScope -contains $appPath) {
throw "Deployment self-test expected target git rollback scope to exclude app file: $appPath"
}
}
$partialGitignoreTarget = Join-Path $selfTestRoot "partial-gitignore"
New-Item -ItemType Directory -Path $partialGitignoreTarget | Out-Null
Set-Content -LiteralPath (Join-Path $partialGitignoreTarget ".gitignore") -Value ".agents/runtime/" -Encoding utf8
Invoke-ChildDeployment -CommandArgs @{ TargetPath = $partialGitignoreTarget; Mode = "core_bootstrap"; Quiet = $true }
Assert-SelfTestContains -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".agents/runtime/"
Assert-SelfTestContains -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".agents/runtime/collaborators.jsonl"
Assert-SelfTestContains -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".agents/runtime/workflows/"
Assert-SelfTestContains -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".workflow/"
Assert-SelfTestContains -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".codex/config.toml"
Assert-SelfTestContains -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".codex/environments/environment.toml"
Assert-SelfTestContains -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".codex/environments/*.toml"
Assert-SelfTestLineCount -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".agents/runtime/" -Count 1
Assert-SelfTestLineCount -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".agents/runtime/collaborators.jsonl" -Count 1
Assert-SelfTestLineCount -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".agents/runtime/workflows/" -Count 1
Assert-SelfTestLineCount -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".workflow/" -Count 1
Assert-SelfTestLineCount -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".codex/config.toml" -Count 1
Assert-SelfTestLineCount -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".codex/environments/*.toml" -Count 1
Invoke-ChildDeployment -CommandArgs @{ TargetPath = $partialGitignoreTarget; Mode = "core_bootstrap"; Quiet = $true }
Assert-SelfTestLineCount -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".agents/runtime/" -Count 1
Assert-SelfTestLineCount -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".agents/runtime/collaborators.jsonl" -Count 1
Assert-SelfTestLineCount -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".agents/runtime/workflows/" -Count 1
Assert-SelfTestLineCount -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".workflow/" -Count 1
Assert-SelfTestLineCount -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".codex/config.toml" -Count 1
Assert-SelfTestLineCount -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".codex/environments/environment.toml" -Count 1
Assert-SelfTestLineCount -Path (Join-Path $partialGitignoreTarget ".gitignore") -Expected ".codex/environments/*.toml" -Count 1
$missingTarget = Join-Path $selfTestRoot "missing-target"
foreach ($args in @(
@{ TargetPath = $missingTarget; Mode = "core_bootstrap"; DryRun = $true; Quiet = $true },
@{ TargetPath = $missingTarget; Mode = "core_bootstrap"; Quiet = $true }
)) {
$missingBlocked = $false
try {
Invoke-ChildDeployment -CommandArgs $args
}
catch {
$missingBlocked = $true
}
if (-not $missingBlocked) {
throw "Deployment self-test expected missing target to require explicit creation."
}
}
$ownedTarget = Join-Path $selfTestRoot "target-owned-state"
New-Item -ItemType Directory -Path (Join-Path $ownedTarget ".codex") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $ownedTarget ".codex/environments") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $ownedTarget ".agents/runtime") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $ownedTarget ".agents/runtime/workflows/example") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $ownedTarget ".workflow/example") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $ownedTarget ".git") -Force | Out-Null
Set-Content -LiteralPath (Join-Path $ownedTarget ".codex/config.toml") -Value "local = true" -Encoding utf8
Set-Content -LiteralPath (Join-Path $ownedTarget ".codex/environments/environment.toml") -Value "runtime = true" -Encoding utf8
Set-Content -LiteralPath (Join-Path $ownedTarget ".agents/runtime/agent-ledger.jsonl") -Value '{"local":true}' -Encoding utf8
Set-Content -LiteralPath (Join-Path $ownedTarget ".agents/runtime/compact-events.jsonl") -Value '{"compact":true}' -Encoding utf8
Set-Content -LiteralPath (Join-Path $ownedTarget ".agents/runtime/collaborators.jsonl") -Value '{"collaborator":true}' -Encoding utf8
Set-Content -LiteralPath (Join-Path $ownedTarget ".agents/runtime/workflows/example/state.json") -Value '{"workflow":true}' -Encoding utf8
Set-Content -LiteralPath (Join-Path $ownedTarget ".workflow/example/state.json") -Value '{"alias":true}' -Encoding utf8
Set-Content -LiteralPath (Join-Path $ownedTarget ".git/HEAD") -Value "ref: refs/heads/main" -Encoding utf8
Set-Content -LiteralPath (Join-Path $ownedTarget ".gitignore") -Value "target-local/" -Encoding utf8
Invoke-ChildDeployment -CommandArgs @{ TargetPath = $ownedTarget; Mode = "core_bootstrap"; Quiet = $true }
Assert-SelfTestContent -Path (Join-Path $ownedTarget ".codex/config.toml") -Expected "local = true"
Assert-SelfTestContent -Path (Join-Path $ownedTarget ".codex/environments/environment.toml") -Expected "runtime = true"
Assert-SelfTestMissing -Root $ownedTarget -RelativePath ".codex/environments/target-owned-state.toml"
Assert-SelfTestContent -Path (Join-Path $ownedTarget ".agents/runtime/agent-ledger.jsonl") -Expected '{"local":true}'
Assert-SelfTestContent -Path (Join-Path $ownedTarget ".agents/runtime/compact-events.jsonl") -Expected '{"compact":true}'
Assert-SelfTestContent -Path (Join-Path $ownedTarget ".agents/runtime/collaborators.jsonl") -Expected '{"collaborator":true}'
Assert-SelfTestContent -Path (Join-Path $ownedTarget ".agents/runtime/workflows/example/state.json") -Expected '{"workflow":true}'
Assert-SelfTestContent -Path (Join-Path $ownedTarget ".workflow/example/state.json") -Expected '{"alias":true}'
Assert-SelfTestContent -Path (Join-Path $ownedTarget ".git/HEAD") -Expected "ref: refs/heads/main"
Assert-SelfTestContains -Path (Join-Path $ownedTarget ".gitignore") -Expected "target-local/"
Assert-SelfTestContains -Path (Join-Path $ownedTarget ".gitignore") -Expected ".agents/runtime/"
Assert-SelfTestContains -Path (Join-Path $ownedTarget ".gitignore") -Expected ".agents/runtime/collaborators.jsonl"
Assert-SelfTestContains -Path (Join-Path $ownedTarget ".gitignore") -Expected ".agents/runtime/workflows/"
Assert-SelfTestContains -Path (Join-Path $ownedTarget ".gitignore") -Expected ".workflow/"
Assert-SelfTestContains -Path (Join-Path $ownedTarget ".gitignore") -Expected ".codex/config.toml"
Assert-SelfTestContains -Path (Join-Path $ownedTarget ".gitignore") -Expected ".codex/environments/environment.toml"
Assert-SelfTestContains -Path (Join-Path $ownedTarget ".gitignore") -Expected ".codex/environments/*.toml"
Assert-SelfTestContains -Path (Join-Path $ownedTarget "docs/agents-workflow-deployment.md") -Expected "Target-local Codex environment bootstrap:"
Assert-SelfTestContains -Path (Join-Path $ownedTarget "docs/agents-workflow-deployment.md") -Expected "Protected dirty/local state:"
$ownedPlan = Invoke-ChildDeploymentOutput -CommandArgs @{ TargetPath = $ownedTarget; Mode = "core_bootstrap"; DryRun = $true }
Assert-SelfTestTextContains -Text $ownedPlan -Expected "Protected dirty/local target state observed:"
Assert-SelfTestTextContains -Text $ownedPlan -Expected "[PROTECTED] .codex/config.toml"
Assert-SelfTestTextContains -Text $ownedPlan -Expected "[PROTECTED] .codex/environments/environment.toml"
Assert-SelfTestTextContains -Text $ownedPlan -Expected "[ENV] preserve: .codex/environments/environment.toml"
Assert-SelfTestTextContains -Text $ownedPlan -Expected "[PROTECTED] .agents/runtime/agent-ledger.jsonl"
Assert-SelfTestTextContains -Text $ownedPlan -Expected "[PROTECTED] .agents/runtime/compact-events.jsonl"
Assert-SelfTestTextContains -Text $ownedPlan -Expected "[PROTECTED] .agents/runtime/collaborators.jsonl"
Assert-SelfTestTextContains -Text $ownedPlan -Expected "[PROTECTED] .agents/runtime/workflows/example/state.json"
Assert-SelfTestTextContains -Text $ownedPlan -Expected "[PROTECTED] .workflow/example/state.json"
Assert-SelfTestTextContains -Text $ownedPlan -Expected "[PROTECTED] .git/HEAD"
$routedHistoricalTarget = Join-Path $selfTestRoot "routed-historical"
New-Item -ItemType Directory -Path (Join-Path $routedHistoricalTarget "docs/agents") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $routedHistoricalTarget ".agents/docs/agents") -Force | Out-Null
Set-Content -LiteralPath (Join-Path $routedHistoricalTarget "AGENTS.md") -Value "Route to .agents/docs/agents." -Encoding utf8
Set-Content -LiteralPath (Join-Path $routedHistoricalTarget ".agents/docs/agents/historical.md") -Value "historical root docs" -Encoding utf8
Invoke-ChildDeployment -CommandArgs @{ TargetPath = $routedHistoricalTarget; Mode = "core_bootstrap"; Upgrade = $true; Quiet = $true }
Assert-SelfTestFile -Root $routedHistoricalTarget -RelativePath ".agents/.agents/docs/agents/workflows.yaml"
Assert-SelfTestContent -Path (Join-Path $routedHistoricalTarget ".agents/docs/agents/historical.md") -Expected "historical root docs"
Assert-SelfTestContains -Path (Join-Path $routedHistoricalTarget ".agents/docs/agents-workflow-deployment.md") -Expected "- .agents/docs/agents/historical.md"
$historicalPlan = Invoke-ChildDeploymentOutput -CommandArgs @{ TargetPath = $routedHistoricalTarget; Mode = "core_bootstrap"; DryRun = $true; Upgrade = $true }
Assert-SelfTestTextContains -Text $historicalPlan -Expected "Target-owned historical Agents files outside deployed file set:"
Assert-SelfTestTextContains -Text $historicalPlan -Expected "[HISTORICAL] .agents/docs/agents/historical.md"
$ambiguousTarget = Join-Path $selfTestRoot "ambiguous-layout"
New-Item -ItemType Directory -Path (Join-Path $ambiguousTarget "docs/agents") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $ambiguousTarget ".agents/docs/agents") -Force | Out-Null
$ambiguousBlocked = $false
try {
Invoke-ChildDeployment -CommandArgs @{ TargetPath = $ambiguousTarget; Mode = "core_bootstrap"; DryRun = $true; Quiet = $true }
}
catch {
$ambiguousBlocked = $true
}
if (-not $ambiguousBlocked) {
throw "Deployment self-test expected ambiguous target layout to require an AGENTS.md route."
}
Invoke-ChildDeployment -CommandArgs @{ TargetPath = $RepoRoot.Path; Mode = "core_bootstrap"; DryRun = $true; Quiet = $true }
$sourceWriteBlocked = $false
try {
Invoke-ChildDeployment -CommandArgs @{ TargetPath = $RepoRoot.Path; Mode = "core_bootstrap"; Quiet = $true }
}
catch {
$sourceWriteBlocked = $true
}
if (-not $sourceWriteBlocked) {
throw "Deployment self-test expected provider/source write refusal."
}
Write-Step "PASS" "Deployment self-test completed."
}
$WorkflowVersion = Get-AgentsWorkflowVersion -Path $VersionSourcePath
if ($SelfTest) {
Invoke-DeploymentSelfTest
exit 0
}
if ([string]::IsNullOrWhiteSpace($TargetPath)) {
throw "TargetPath is required unless -SelfTest is used."
}
$resolvedTarget = Resolve-Path -LiteralPath $TargetPath -ErrorAction SilentlyContinue
if (-not $resolvedTarget) {
if ($DryRun) {
throw "Target path must exist for dry-run inspection: $TargetPath"
}
if (-not $CreateTarget) {
throw "Target path does not exist. Create it explicitly first or rerun with -CreateTarget after confirming the exact path: $TargetPath"
}
New-Item -ItemType Directory -Path $TargetPath | Out-Null
$resolvedTarget = Resolve-Path -LiteralPath $TargetPath
}
if (-not (Test-Path -LiteralPath $resolvedTarget -PathType Container)) {
throw "Target path is not a directory: $TargetPath"
}
$targetRoot = $resolvedTarget.Path
if ((Test-PathInsideRoot -Path $targetRoot -Root $RepoRoot.Path) -and (-not $DryRun)) {
throw "Refusing to write into the provider/source repo or one of its child directories. Use -DryRun for provider self-checks or provide an external target path."
}
$layout = Get-TargetLayout -Root $targetRoot
$groups = Get-ModeGroups -SelectedMode $Mode
$entries = Get-DeployEntries -Groups $groups
Write-Step "INFO" ("Source repo: {0}" -f $RepoRoot)
Write-Step "INFO" ("Target repo: {0}" -f $targetRoot)
Write-Step "INFO" ("Mode: {0}" -f $Mode)
Write-Step "INFO" ("Workflow version: {0}" -f (Format-AgentsWorkflowVersion -WorkflowVersion $WorkflowVersion))
Write-Step "INFO" ("Version source: {0}" -f $WorkflowVersion.Source)
Write-Step "INFO" ("Upgrade: {0}" -f [bool] $Upgrade)
Write-Step "INFO" ("Layout: {0}" -f $layout)
Write-Step "INFO" ("Dry run: {0}" -f [bool] $DryRun)
foreach ($entry in $entries) {
Copy-DeployEntry -Entry $entry -Root $targetRoot -Layout $layout -PlanOnly:$DryRun
}
Initialize-TargetLocalEnvironment -Root $targetRoot -PlanOnly:$DryRun
Update-TargetStateClassification -Root $targetRoot -Layout $layout
Write-DeploymentReport -Root $targetRoot -Layout $layout -PlanOnly:$DryRun
Test-DeployedFileSet -Root $targetRoot -PlanOnly:$DryRun
Write-Step "INFO" "Deployed file set:"
foreach ($file in ($DeployedFiles | Sort-Object -Unique)) {
Write-Step "FILE" $file
}
if ($ProtectedExisting.Count -gt 0) {
Write-Step "INFO" "Existing target files requiring -Upgrade:"
foreach ($file in ($ProtectedExisting | Sort-Object -Unique)) {
Write-Step "EXISTING" $file
}
}
if ($PlannedWrites.Count -gt 0) {
Write-Step "INFO" "Target files planned for create/update:"
foreach ($file in ($PlannedWrites | Sort-Object -Unique)) {
Write-Step "WRITE" $file
}
}
if ($UnchangedExisting.Count -gt 0) {
Write-Step "INFO" "Existing target files already current:"
foreach ($file in ($UnchangedExisting | Sort-Object -Unique)) {
Write-Step "CURRENT" $file
}
}
if ($ProtectedDirty.Count -gt 0) {
Write-Step "INFO" "Protected dirty/local target state observed:"
foreach ($file in ($ProtectedDirty | Sort-Object -Unique)) {
Write-Step "PROTECTED" $file
}
}
if ($TargetHistoricalAgents.Count -gt 0) {
Write-Step "INFO" "Target-owned historical Agents files outside deployed file set:"
foreach ($file in ($TargetHistoricalAgents | Sort-Object -Unique)) {
Write-Step "HISTORICAL" $file
}
}
Write-DeploymentCloseoutSummary -Layout $layout
if ($DryRun) {
Write-Step "PASS" "Dry-run deployment plan completed without writing target files."
}
else {
Write-Step "PASS" "Agents workflow deployment completed."
}
