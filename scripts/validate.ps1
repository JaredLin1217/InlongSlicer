param(
[switch] $Quiet,
[switch] $Full,
[switch] $Score,
[string] $TempRoot
)
$ErrorActionPreference = "Stop"
$ScriptPath = $MyInvocation.MyCommand.Path
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$Failures = New-Object System.Collections.Generic.List[string]
$Warnings = New-Object System.Collections.Generic.List[string]
function Write-Check {
param(
[string] $Status,
[string] $Message
)
if (-not $Quiet) {
Write-Host ("[{0}] {1}" -f $Status, $Message)
}
}
function Add-Failure {
param([string] $Message)
$Failures.Add($Message) | Out-Null
Write-Check "FAIL" $Message
}
function Add-Warning {
param([string] $Message)
$Warnings.Add($Message) | Out-Null
Write-Check "WARN" $Message
}
function Add-Pass {
param([string] $Message)
Write-Check "PASS" $Message
}
function Get-RepoPath {
param([string] $Path)
return Join-Path $RepoRoot $Path
}
$FoundationValidationHelper = Join-Path $PSScriptRoot "validate-foundation.ps1"
if (Test-Path -LiteralPath $FoundationValidationHelper -PathType Leaf) {
. $FoundationValidationHelper
}
else {
function Test-FoundationCreationIntegrity {
Add-Failure "Foundation validation helper missing: scripts/validate-foundation.ps1"
}
}
$ResidueValidationHelper = Join-Path $PSScriptRoot "validate-residue.ps1"
if (Test-Path -LiteralPath $ResidueValidationHelper -PathType Leaf) {
. $ResidueValidationHelper
}
else {
function Test-LegacyResidue {
Add-Failure "Residue validation helper missing: scripts/validate-residue.ps1"
}
}
$EvidenceTemplateValidationHelper = Join-Path $PSScriptRoot "validate-evidence-templates.ps1"
if (Test-Path -LiteralPath $EvidenceTemplateValidationHelper -PathType Leaf) {
. $EvidenceTemplateValidationHelper
}
else {
function Test-EvidenceTemplateSchemaCoverage {
Add-Failure "Evidence template validation helper missing: scripts/validate-evidence-templates.ps1"
}
}
$ScoreValidationHelper = Join-Path $PSScriptRoot "validate-score.ps1"
if (Test-Path -LiteralPath $ScoreValidationHelper -PathType Leaf) {
. $ScoreValidationHelper
}
else {
function Get-IntendedRepoFiles { return @() }
function Get-RepoFilesSize { param([string[]] $Paths) return 0 }
function Write-AgentQualityScore { Add-Failure "Score validation helper missing: scripts/validate-score.ps1" }
}
$SizeGateValidationHelper = Join-Path $PSScriptRoot "validate-size-gates.ps1"
if (Test-Path -LiteralPath $SizeGateValidationHelper -PathType Leaf) {
. $SizeGateValidationHelper
}
else {
function Test-SizeGates {
Add-Failure "Size gate validation helper missing: scripts/validate-size-gates.ps1"
}
}
$ReleaseEvidenceValidationHelper = Join-Path $PSScriptRoot "validate-release-evidence.ps1"
if (Test-Path -LiteralPath $ReleaseEvidenceValidationHelper -PathType Leaf) {
. $ReleaseEvidenceValidationHelper
}
else {
function Test-ReleasePackageExport {
Add-Failure "Release evidence validation helper missing: scripts/validate-release-evidence.ps1"
}
function Test-RuntimeReleaseEvidence {
Add-Failure "Release evidence validation helper missing: scripts/validate-release-evidence.ps1"
}
}
$RequiredFilesValidationHelper = Join-Path $PSScriptRoot "validate-required-files.ps1"
if (Test-Path -LiteralPath $RequiredFilesValidationHelper -PathType Leaf) {
. $RequiredFilesValidationHelper
}
else {
function Test-RequiredFiles {
Add-Failure "Required files validation helper missing: scripts/validate-required-files.ps1"
}
}
$RoutePackValidationHelper = Join-Path $PSScriptRoot "validate-route-pack.ps1"
if (Test-Path -LiteralPath $RoutePackValidationHelper -PathType Leaf) {
. $RoutePackValidationHelper
}
else {
function Test-RoutePackDeterminism {
Add-Failure "Route pack validation helper missing: scripts/validate-route-pack.ps1"
}
}
$RuntimeExecutionValidationHelper = Join-Path $PSScriptRoot "validate-runtime-execution.ps1"
if (Test-Path -LiteralPath $RuntimeExecutionValidationHelper -PathType Leaf) {
. $RuntimeExecutionValidationHelper
}
else {
function Test-RuntimeExecutionSmoke {
Add-Failure "Runtime execution validation helper missing: scripts/validate-runtime-execution.ps1"
}
}
$ReadinessValidationHelper = Join-Path $PSScriptRoot "validate-readiness.ps1"
if (Test-Path -LiteralPath $ReadinessValidationHelper -PathType Leaf) {
. $ReadinessValidationHelper
}
else {
function Test-ReadinessLadderEvidence {
Add-Failure "Readiness validation helper missing: scripts/validate-readiness.ps1"
}
}
$CrossProjectRuntimeValidationHelper = Join-Path $PSScriptRoot "validate-cross-project-runtime.ps1"
if (Test-Path -LiteralPath $CrossProjectRuntimeValidationHelper -PathType Leaf) {
. $CrossProjectRuntimeValidationHelper
}
else {
function Test-CrossProjectRuntimeResilienceIntegrity {
Add-Failure "Cross-project runtime validation helper missing: scripts/validate-cross-project-runtime.ps1"
}
}
$KnowledgeValidationHelper = Join-Path $PSScriptRoot "validate-knowledge.ps1"
if (Test-Path -LiteralPath $KnowledgeValidationHelper -PathType Leaf) {
. $KnowledgeValidationHelper
}
else {
function Test-KnowledgeMemoryIntegrity {
Add-Failure "Knowledge validation helper missing: scripts/validate-knowledge.ps1"
}
}
$ContextIntelligenceValidationHelper = Join-Path $PSScriptRoot "validate-context-intelligence.ps1"
if (Test-Path -LiteralPath $ContextIntelligenceValidationHelper -PathType Leaf) {
. $ContextIntelligenceValidationHelper
}
else {
function Test-ContextIntelligenceIntegrity {
param([switch] $RunPractice)
Add-Failure "Context intelligence validation helper missing: scripts/validate-context-intelligence.ps1"
}
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
$ValidationRunId = ("validate-{0}-{1}" -f ((Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssZ")), ([guid]::NewGuid().ToString("N").Substring(0, 8)))
function Get-ValidationTempRoot {
param([string] $Purpose)
$base = if ([string]::IsNullOrWhiteSpace($TempRoot)) {
[System.IO.Path]::Combine([System.IO.Path]::GetTempPath(), "codex-agent-status", (Get-ValidationProjectKey))
}
else {
[System.IO.Path]::GetFullPath($TempRoot)
}
return [System.IO.Path]::Combine($base, $ValidationRunId, $Purpose)
}
function Get-CanonicalWorkflowVersion {
$values = Get-LightweightYamlPathValues -File (Get-Item -LiteralPath (Get-RepoPath ".agents/docs/agents/version.yaml"))
return [string] $values["workflow.version"]
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
function Get-TextFiles {
param([string[]] $Roots)
$extensions = @(".md", ".yaml", ".yml", ".json", ".toml", ".ps1", ".txt")
foreach ($root in $Roots) {
$fullRoot = Get-RepoPath $root
if (-not (Test-Path -LiteralPath $fullRoot)) {
continue
}
if (Test-Path -LiteralPath $fullRoot -PathType Leaf) {
$item = Get-Item -LiteralPath $fullRoot
if ($extensions -contains $item.Extension.ToLowerInvariant()) {
$item
}
continue
}
Get-ChildItem -LiteralPath $fullRoot -Recurse -Force -File |
Where-Object { $extensions -contains $_.Extension.ToLowerInvariant() }
}
}
function Test-LightweightYaml {
param([System.IO.FileInfo] $File)
$lines = Get-Content -LiteralPath $File.FullName
$lineNumber = 0
foreach ($line in $lines) {
$lineNumber++
if ($line.Trim().Length -eq 0 -or $line.TrimStart().StartsWith("#")) {
continue
}
if ($line -match "`t") {
Add-Failure ("{0}:{1} contains a tab indentation character." -f $File.FullName, $lineNumber)
continue
}
$indent = $line.Length - $line.TrimStart(" ").Length
if (($indent % 2) -ne 0) {
Add-Failure ("{0}:{1} uses odd indentation; policy YAML should use two-space levels." -f $File.FullName, $lineNumber)
continue
}
$trimmed = $line.Trim()
$isListItem = $trimmed -match "^-($|\s+.+)"
$isKeyValue = $trimmed -match '^("[^"]+"|''[^'']+''|[^:\[\]\{\},]+):\s*.*$'
if (-not ($isListItem -or $isKeyValue)) {
Add-Failure ("{0}:{1} is not a recognized policy YAML line." -f $File.FullName, $lineNumber)
continue
}
$doubleQuoteCount = ([regex]::Matches($trimmed, '(?<!\\)"')).Count
if (($doubleQuoteCount % 2) -ne 0) {
Add-Failure ("{0}:{1} has an unbalanced double quote." -f $File.FullName, $lineNumber)
}
$openSquare = ([regex]::Matches($trimmed, "\[")).Count
$closeSquare = ([regex]::Matches($trimmed, "\]")).Count
if ($openSquare -ne $closeSquare) {
Add-Failure ("{0}:{1} has unbalanced square brackets." -f $File.FullName, $lineNumber)
}
}
}
function Get-LightweightYamlTopLevel {
param([System.IO.FileInfo] $File)
$document = @{}
foreach ($line in Get-Content -LiteralPath $File.FullName) {
if ($line.Trim().Length -eq 0 -or $line.TrimStart().StartsWith("#")) {
continue
}
$indent = $line.Length - $line.TrimStart(" ").Length
if ($indent -ne 0) {
continue
}
$trimmed = $line.Trim()
if ($trimmed -match '^("[^"]+"|''[^'']+''|[^:\[\]\{\},]+):\s*(.*)$') {
$key = $Matches[1].Trim().Trim('"').Trim("'")
$value = $Matches[2].Trim()
if ($value.Length -ge 2) {
if (($value.StartsWith('"') -and $value.EndsWith('"')) -or ($value.StartsWith("'") -and $value.EndsWith("'"))) {
$value = $value.Substring(1, $value.Length - 2)
}
}
$document[$key] = $value
}
}
return $document
}
function Get-LightweightYamlPathValues {
param([System.IO.FileInfo] $File)
$paths = @{}
$stack = @{}
foreach ($line in Get-Content -LiteralPath $File.FullName) {
if ($line.Trim().Length -eq 0 -or $line.TrimStart().StartsWith("#")) {
continue
}
$indent = $line.Length - $line.TrimStart(" ").Length
$level = [int] ($indent / 2)
$trimmed = $line.Trim()
if ($trimmed -match '^- ') {
continue
}
if ($trimmed -match '^("[^"]+"|''[^'']+''|[^:\[\]\{\},]+):\s*(.*)$') {
$key = $Matches[1].Trim().Trim('"').Trim("'")
$value = $Matches[2].Trim()
foreach ($existingLevel in @($stack.Keys)) {
if ([int] $existingLevel -ge $level) {
$stack.Remove($existingLevel)
}
}
$stack[$level] = $key
$orderedKeys = @()
foreach ($stackLevel in ($stack.Keys | Sort-Object { [int] $_ })) {
$orderedKeys += $stack[$stackLevel]
}
$path = $orderedKeys -join "."
if ($value.Length -ge 2) {
if (($value.StartsWith('"') -and $value.EndsWith('"')) -or ($value.StartsWith("'") -and $value.EndsWith("'"))) {
$value = $value.Substring(1, $value.Length - 2)
}
}
$paths[$path] = $value
}
}
return $paths
}
function Get-JsonSchemaConst {
param(
[object] $SchemaDocument,
[string] $PropertyName
)
$property = $SchemaDocument.properties.PSObject.Properties[$PropertyName]
if (-not $property) {
return $null
}
$const = $property.Value.PSObject.Properties["const"]
if (-not $const) {
return $null
}
return [string] $const.Value
}
function Get-SchemaContractIssues {
param(
[string] $YamlPath,
[string] $SchemaPath,
[string] $YamlLabel,
[string] $SchemaLabel
)
$issues = New-Object System.Collections.Generic.List[string]
if (-not (Test-Path -LiteralPath $YamlPath -PathType Leaf)) {
$issues.Add(("Schema contract target is missing: {0}" -f $YamlLabel)) | Out-Null
return $issues
}
if (-not (Test-Path -LiteralPath $SchemaPath -PathType Leaf)) {
$issues.Add(("Schema contract file is missing: {0}" -f $SchemaLabel)) | Out-Null
return $issues
}
try {
$schemaDocument = Get-Content -LiteralPath $SchemaPath -Raw | ConvertFrom-Json
}
catch {
$issues.Add(("Schema contract is not valid JSON: {0}" -f $SchemaLabel)) | Out-Null
return $issues
}
$yamlDocument = Get-LightweightYamlTopLevel -File (Get-Item -LiteralPath $YamlPath)
foreach ($required in @($schemaDocument.required)) {
if (-not $yamlDocument.ContainsKey([string] $required)) {
$issues.Add(("{0} is missing required top-level key from {1}: {2}" -f $YamlLabel, $SchemaLabel, $required)) | Out-Null
}
}
$yamlPathValues = Get-LightweightYamlPathValues -File (Get-Item -LiteralPath $YamlPath)
$requiredPathsProperty = $schemaDocument.PSObject.Properties["x-required-paths"]
if ($requiredPathsProperty) {
foreach ($requiredPath in @($requiredPathsProperty.Value)) {
if (-not $yamlPathValues.ContainsKey([string] $requiredPath)) {
$issues.Add(("{0} is missing required nested path from {1}: {2}" -f $YamlLabel, $SchemaLabel, $requiredPath)) | Out-Null
}
}
}
$requiredValuesProperty = $schemaDocument.PSObject.Properties["x-required-values"]
if ($requiredValuesProperty) {
foreach ($requiredValue in @($requiredValuesProperty.Value)) {
$path = [string] $requiredValue.path
$expected = [string] $requiredValue.value
if (-not $yamlPathValues.ContainsKey($path)) {
$issues.Add(("{0} is missing required value path from {1}: {2}" -f $YamlLabel, $SchemaLabel, $path)) | Out-Null
}
elseif ([string] $yamlPathValues[$path] -ne $expected) {
$issues.Add(("{0} value mismatch at {1}. Expected {2}; found {3}." -f $YamlLabel, $path, $expected, $yamlPathValues[$path])) | Out-Null
}
}
}
$requiredContainsProperty = $schemaDocument.PSObject.Properties["x-required-contains"]
if ($requiredContainsProperty) {
foreach ($requiredContains in @($requiredContainsProperty.Value)) {
$path = [string] $requiredContains.path
if (-not $yamlPathValues.ContainsKey($path)) {
$issues.Add(("{0} is missing required contains path from {1}: {2}" -f $YamlLabel, $SchemaLabel, $path)) | Out-Null
continue
}
$actual = [string] $yamlPathValues[$path]
foreach ($needle in @($requiredContains.contains)) {
if (-not $actual.Contains([string] $needle)) {
$issues.Add(("{0} value at {1} must contain {2}." -f $YamlLabel, $path, $needle)) | Out-Null
}
}
}
}
$expectedSchema = Get-JsonSchemaConst -SchemaDocument $schemaDocument -PropertyName "schema"
if ($expectedSchema) {
if (-not $yamlDocument.ContainsKey("schema")) {
$issues.Add(("{0} is missing schema version required by {1}." -f $YamlLabel, $SchemaLabel)) | Out-Null
}
elseif ($yamlDocument["schema"] -ne $expectedSchema) {
$issues.Add(("{0} schema version mismatch. Expected {1}; found {2}." -f $YamlLabel, $expectedSchema, $yamlDocument["schema"])) | Out-Null
}
}
return $issues
}
function Test-SchemaContracts {
$contracts = @(
@{ Yaml = ".agents/docs/agents/ai-runtime.yaml"; Schema = "schemas/agents-ai-runtime.schema.json" },
@{ Yaml = ".agents/docs/agents/workflows.yaml"; Schema = "schemas/agents-workflows.schema.json" },
@{ Yaml = ".agents/docs/agents/verify.yaml"; Schema = "schemas/agents-verify.schema.json" },
@{ Yaml = ".agents/docs/agents/policy.yaml"; Schema = "schemas/agents-policy.schema.json" },
@{ Yaml = ".agents/docs/agents/schemas.yaml"; Schema = "schemas/agents-schemas.schema.json" },
@{ Yaml = ".agents/docs/agents/deploy.yaml"; Schema = "schemas/agents-deploy.schema.json" },
@{ Yaml = ".agents/docs/agents/openai-foundations.yaml"; Schema = "schemas/agents-openai-foundations.schema.json" },
@{ Yaml = ".agents/docs/agents/version.yaml"; Schema = "schemas/agents-version.schema.json" },
@{ Yaml = ".agents/docs/agents/org.yaml"; Schema = "schemas/agents-org.schema.json" },
@{ Yaml = ".agents/docs/agents/model-policy.yaml"; Schema = "schemas/agents-model-policy.schema.json" },
@{ Yaml = ".agents/docs/agents/dispatch.yaml"; Schema = "schemas/agents-dispatch.schema.json" },
@{ Yaml = ".agents/docs/agents/workflow-artifacts.yaml"; Schema = "schemas/agents-workflow-artifacts.schema.json" },
@{ Yaml = ".agents/docs/agents/collaborators.yaml"; Schema = "schemas/agents-collaborators.schema.json" },
@{ Yaml = ".agents/docs/agents/context-compact.yaml"; Schema = "schemas/agents-context-compact.schema.json" },
@{ Yaml = ".agents/docs/agents/core-system.yaml"; Schema = "schemas/agents-core-system.schema.json" },
@{ Yaml = ".agents/docs/agents/runtime-execution.yaml"; Schema = "schemas/agents-runtime-execution.schema.json" },
@{ Yaml = ".agents/docs/agents/provider-adapters.yaml"; Schema = "schemas/agents-provider-adapters.schema.json" },
@{ Yaml = ".agents/docs/agents/route-packs.yaml"; Schema = "schemas/agents-route-packs.schema.json" },
@{ Yaml = ".agents/docs/agents/knowledge-footprint.yaml"; Schema = "schemas/agents-knowledge-footprint.schema.json" },
@{ Yaml = ".agents/docs/agents/context-intelligence.yaml"; Schema = "schemas/agents-context-intelligence.schema.json" }
)
foreach ($contract in $contracts) {
$yamlPath = Get-RepoPath $contract.Yaml
$schemaPath = Get-RepoPath $contract.Schema
$issues = Get-SchemaContractIssues -YamlPath $yamlPath -SchemaPath $schemaPath -YamlLabel $contract.Yaml -SchemaLabel $contract.Schema
foreach ($issue in $issues) {
Add-Failure $issue
}
}
}
function Test-PublicReadmeVersionAlignment {
$versionPath = Get-RepoPath ".agents/docs/agents/version.yaml"
$readmePath = Get-RepoPath "README.md"
if (-not (Test-Path -LiteralPath $versionPath -PathType Leaf)) {
Add-Failure "Canonical version file is missing: .agents/docs/agents/version.yaml"
return
}
if (-not (Test-Path -LiteralPath $readmePath -PathType Leaf)) {
Add-Failure "Public README is missing."
return
}
$values = Get-LightweightYamlPathValues -File (Get-Item -LiteralPath $versionPath)
foreach ($path in @("workflow.version", "workflow.channel")) {
if (-not $values.ContainsKey($path)) {
Add-Failure ("Canonical version file is missing required path: {0}" -f $path)
return
}
}
$expected = "Current Agents workflow version: ``{0}`` (``{1}``)." -f $values["workflow.version"], $values["workflow.channel"]
$readmeLines = Get-Content -LiteralPath $readmePath
if ($readmeLines -notcontains $expected) {
Add-Failure ("README.md workflow version mismatch. Expected line: {0}" -f $expected)
}
}
function Test-ValidationFixtures {
$casePath = Get-RepoPath "tests/agents-governance-fixtures/schema-contracts/cases.json"
if (-not (Test-Path -LiteralPath $casePath -PathType Leaf)) {
Add-Failure "Validation fixture cases are missing."
return
}
try {
$cases = Get-Content -LiteralPath $casePath -Raw | ConvertFrom-Json
}
catch {
Add-Failure "Validation fixture case manifest is not valid JSON."
return
}
foreach ($case in $cases) {
$yamlLabel = Join-Path "tests/agents-governance-fixtures/schema-contracts" ([string] $case.yaml)
$schemaLabel = [string] $case.schema
$yamlPath = Get-RepoPath $yamlLabel
$schemaPath = Get-RepoPath $schemaLabel
$issues = @(Get-SchemaContractIssues -YamlPath $yamlPath -SchemaPath $schemaPath -YamlLabel $yamlLabel -SchemaLabel $schemaLabel)
if ($case.expected -eq "pass") {
if ($issues.Count -gt 0) {
Add-Failure ("Fixture expected pass but failed: {0}" -f $case.name)
foreach ($issue in $issues) {
Add-Failure ("Fixture detail: {0}" -f $issue)
}
}
}
elseif ($case.expected -eq "fail") {
if ($issues.Count -eq 0) {
Add-Failure ("Fixture expected fail but passed: {0}" -f $case.name)
}
}
else {
Add-Failure ("Fixture has unsupported expected value: {0}" -f $case.name)
}
}
}
function Test-PatternScan {
param(
[string] $Name,
[string] $Pattern,
[System.IO.FileInfo[]] $Files
)
$matches = @()
foreach ($file in $Files) {
if ($file.FullName -eq $ScriptPath) {
continue
}
$result = Select-String -LiteralPath $file.FullName -Pattern $Pattern -AllMatches -ErrorAction SilentlyContinue
if ($result) {
$matches += $result
}
}
if ($matches.Count -gt 0) {
foreach ($match in $matches) {
Add-Failure ("{0}: {1}:{2}: {3}" -f $Name, $match.Path, $match.LineNumber, $match.Line.Trim())
}
}
}
function Test-RuntimeBoundaries {
$ignoredRuntimePaths = @(
".agents/runtime/agent-ledger.jsonl",
".agents/runtime/collaborators.jsonl",
".codex/config.toml",
".codex/environments/environment.toml",
".codex/environments/example-project.toml",
".codex/environments/environment.template.toml",
"docs/agent-status.md",
"docs/agent-events/example.jsonl",
".agents/docs/agent-status.md",
".agents/docs/agent-events/example.jsonl",
"docs/tmp-approval-example/report.md",
".agents/docs/tmp-approval-example/report.md",
"docs/hard-isolation-evidence/example.md",
".agents/docs/hard-isolation-evidence/example.md",
"docs/runtime-multi-agent-validation/example.md",
".agents/docs/runtime-multi-agent-validation/example.md",
".agents/runtime/workflows/example/state.json",
".workflow/example/state.json",
".agents/runtime/executions/example/run.json",
".agents/runtime/knowledge/example.json",
".agents/runtime/route-packs/example.json",
".agents/runtime/tool-evidence/example.json",
".agents/runtime/deployments/example.json"
)
foreach ($path in $ignoredRuntimePaths) {
& git -C $RepoRoot check-ignore -q --no-index -- $path
if ($LASTEXITCODE -ne 0) {
Add-Failure ("Runtime/local path is not ignored: {0}" -f $path)
}
}
$gitignoreFragmentPath = Get-RepoPath ".agents/docs/templates/agents/gitignore.fragment"
if (Test-Path -LiteralPath $gitignoreFragmentPath -PathType Leaf) {
$fragmentEntries = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
foreach ($line in Get-Content -LiteralPath $gitignoreFragmentPath) {
$entry = $line.Trim()
if ($entry.Length -gt 0 -and -not $entry.StartsWith("#")) {
[void] $fragmentEntries.Add($entry)
}
}
$requiredFragmentEntries = @(
".agents/runtime/",
".agents/runtime/collaborators.jsonl",
".codex/config.toml",
".codex/environments/environment.toml",
".codex/environments/*.toml",
"docs/agent-status.md",
"docs/agent-events/",
".agents/docs/agent-status.md",
".agents/docs/agent-events/",
"docs/tmp-approval-*/",
".agents/docs/tmp-approval-*/",
"docs/hard-isolation-evidence/",
".agents/docs/hard-isolation-evidence/",
"docs/runtime-multi-agent-validation/",
".agents/docs/runtime-multi-agent-validation/",
".agents/runtime/workflows/",
".workflow/",
".agents/runtime/executions/",
".agents/runtime/knowledge/",
".agents/runtime/route-packs/",
".agents/runtime/tool-evidence/",
".agents/runtime/deployments/"
)
foreach ($entry in $requiredFragmentEntries) {
if (-not $fragmentEntries.Contains($entry)) {
Add-Failure ("Gitignore fragment is missing runtime/local entry: {0}" -f $entry)
}
}
}
else {
Add-Failure "Gitignore fragment is missing: .agents/docs/templates/agents/gitignore.fragment"
}
$trackedRuntime = & git -c core.quotepath=false -C $RepoRoot ls-files -- `
".agents/runtime" `
".agents/runtime/collaborators.jsonl" `
".codex/config.toml" `
".codex/environments/environment.toml" `
"docs/agent-status.md" `
"docs/agent-events" `
".agents/docs/agent-status.md" `
".agents/docs/agent-events" `
"docs/tmp-approval-example" `
".agents/docs/tmp-approval-example" `
"docs/hard-isolation-evidence" `
".agents/docs/hard-isolation-evidence" `
"docs/runtime-multi-agent-validation" `
".agents/docs/runtime-multi-agent-validation" `
".agents/runtime/workflows" `
".workflow" `
".agents/runtime/executions" `
".agents/runtime/knowledge" `
".agents/runtime/route-packs" `
".agents/runtime/tool-evidence" `
".agents/runtime/deployments"
if ($trackedRuntime) {
foreach ($path in $trackedRuntime) {
Add-Failure ("Runtime/local path is tracked: {0}" -f $path)
}
}
}
function Test-GitDiffCheck {
$startFailureCount = $Failures.Count
$previousErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
$output = & git -C $RepoRoot diff --check 2>&1
$exitCode = $LASTEXITCODE
}
finally {
$ErrorActionPreference = $previousErrorActionPreference
}
if ($exitCode -ne 0) {
foreach ($line in $output) {
Add-Failure ("Git diff check failed: {0}" -f $line)
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Git diff hygiene checks passed."
}
}
function Test-LineEndings {
$startFailureCount = $Failures.Count
$textFiles = @(Get-TextFiles -Roots @(
"AGENTS.md",
".agents/skills",
"docs",
"schemas",
"scripts",
"tests",
"artifacts",
".github/workflows"
))
foreach ($file in $textFiles) {
$bytes = [System.IO.File]::ReadAllBytes($file.FullName)
for ($i = 0; $i -lt ($bytes.Length - 1); $i++) {
if ($bytes[$i] -eq 13 -and $bytes[$i + 1] -eq 10) {
Add-Failure ("Text file uses CRLF line endings; expected LF: {0}" -f $file.FullName)
break
}
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Line-ending readiness checks passed."
}
}
function Test-CanonicalSourceUniqueness {
$startFailureCount = $Failures.Count
$canonicalByHash = @{}
foreach ($path in @(Get-IntendedRepoFiles | Where-Object { $_ -notlike ".agents/docs/templates/agents/*" })) {
$normalized = $path.Replace("\", "/")
$fullPath = Get-RepoPath $normalized
if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
continue
}
$hash = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash
if (-not $canonicalByHash.ContainsKey($hash)) {
$canonicalByHash[$hash] = $normalized
}
}
foreach ($path in @(Get-IntendedRepoFiles | Where-Object { $_ -like ".agents/docs/templates/agents/*" })) {
$normalized = $path.Replace("\", "/")
$fullPath = Get-RepoPath $normalized
if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
continue
}
$hash = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash
if ($canonicalByHash.ContainsKey($hash)) {
Add-Failure ("Target-specific starter duplicates canonical source: {0} == {1}" -f $normalized, $canonicalByHash[$hash])
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Canonical source uniqueness checks passed."
}
}
function Test-TemplateCoverage {
$startFailureCount = $Failures.Count
$allowed = New-Object 'System.Collections.Generic.HashSet[string]'
$allowedItems = @(
".agents/docs/templates/agents/README.md",
".agents/docs/templates/agents/gitignore.fragment",
".agents/docs/templates/agents/project-memory.md",
".agents/docs/templates/agents/memory-index.md"
)
foreach ($item in $allowedItems) {
[void] $allowed.Add($item)
}
$templateFiles = Get-IntendedRepoFiles | Where-Object { $_ -like ".agents/docs/templates/agents/*" }
foreach ($path in $templateFiles) {
$normalized = $path.Replace("\", "/")
if (-not $allowed.Contains($normalized)) {
Add-Failure ("Template bundle file is not an approved target-specific starter: {0}" -f $normalized)
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Target-specific starter coverage checks passed."
}
}
function Test-TemplateSourceNeutrality {
$startFailureCount = $Failures.Count
$literals = Get-SourceSpecificLiterals
$templateFiles = @(Get-TextFiles -Roots @("docs/templates/agents"))
foreach ($literal in $literals) {
foreach ($file in $templateFiles) {
$match = Select-String -LiteralPath $file.FullName -Pattern $literal -SimpleMatch -Quiet -ErrorAction SilentlyContinue
if ($match) {
Add-Failure ("Template source-neutrality check found provider literal in {0}: {1}" -f $file.FullName, $literal)
}
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Template source-neutrality checks passed."
}
}
function Test-DeployManifestIntegrity {
$startFailureCount = $Failures.Count
$deployPaths = @(".agents/docs/agents/deploy.yaml")
$deploymentScriptContent = Get-Content -LiteralPath (Get-RepoPath "scripts/deploy-agents-workflow.ps1") -Raw
$scriptModes = @()
$modeSetMatch = [regex]::Match($deploymentScriptContent, '\[ValidateSet\(([^)]*)\)\]\s*\r?\n\s*\[string\]\s*\$Mode')
if ($modeSetMatch.Success) {
$scriptModes = @([regex]::Matches($modeSetMatch.Groups[1].Value, '"([^"]+)"') | ForEach-Object { $_.Groups[1].Value })
}
else {
Add-Failure "Deployment script mode ValidateSet is missing."
}
if (-not ($deploymentScriptContent.Contains("Add-DeployBundle") -and $deploymentScriptContent.Contains("schema_bundle") -and $deploymentScriptContent.Contains("script_bundle"))) {
Add-Failure "Deployment script must support schema_bundle and script_bundle deployment entries."
}
$requiredBlocklist = @(
@{ Manifest = ".agents/runtime/"; Script = ".agents/runtime/" },
@{ Manifest = ".agents/runtime/compact-events.jsonl"; Script = ".agents/runtime/compact-events.jsonl" },
@{ Manifest = ".agents/runtime/collaborators.jsonl"; Script = ".agents/runtime/collaborators.jsonl" },
@{ Manifest = ".agents/runtime/workflows/"; Script = ".agents/runtime/workflows/" },
@{ Manifest = ".agents/runtime/executions/"; Script = ".agents/runtime/executions/" },
@{ Manifest = ".agents/runtime/knowledge/"; Script = ".agents/runtime/knowledge/" },
@{ Manifest = ".agents/runtime/route-packs/"; Script = ".agents/runtime/route-packs/" },
@{ Manifest = ".agents/runtime/tool-evidence/"; Script = ".agents/runtime/tool-evidence/" },
@{ Manifest = ".agents/runtime/deployments/"; Script = ".agents/runtime/deployments/" },
@{ Manifest = ".workflow/"; Script = ".workflow/" },
@{ Manifest = "docs/agent-status.md"; Script = "docs/agent-status.md" },
@{ Manifest = "docs/agent-events/"; Script = "docs/agent-events/" },
@{ Manifest = ".agents/docs/agent-status.md"; Script = ".agents/docs/agent-status.md" },
@{ Manifest = ".agents/docs/agent-events/"; Script = ".agents/docs/agent-events/" },
@{ Manifest = "docs/tmp-approval-*/"; Script = "docs/tmp-approval-" },
@{ Manifest = ".agents/docs/tmp-approval-*/"; Script = ".agents/docs/tmp-approval-" },
@{ Manifest = "docs/hard-isolation-evidence/"; Script = "docs/hard-isolation-evidence/" },
@{ Manifest = ".agents/docs/hard-isolation-evidence/"; Script = ".agents/docs/hard-isolation-evidence/" },
@{ Manifest = "docs/runtime-multi-agent-validation/"; Script = "docs/runtime-multi-agent-validation/" },
@{ Manifest = ".agents/docs/runtime-multi-agent-validation/"; Script = ".agents/docs/runtime-multi-agent-validation/" },
@{ Manifest = ".codex/config.toml"; Script = ".codex/config.toml" },
@{ Manifest = ".codex/environments/environment.toml"; Script = ".codex/environments/environment.toml" },
@{ Manifest = ".codex/environments/*.toml"; Script = ".codex/environments/" },
@{ Manifest = ".codex/environments/environment.template.toml"; Script = ".codex/environments/environment.template.toml" },
@{ Manifest = ".git/"; Script = ".git/" }
)
foreach ($path in $deployPaths) {
$fullPath = Get-RepoPath $path
$content = Get-Content -LiteralPath $fullPath
if (-not ($content | Where-Object { $_ -match "^steps:" })) {
Add-Failure ("Deploy manifest has no top-level steps: {0}" -f $path)
}
if ($content | Where-Object { $_ -match "^  steps:" }) {
Add-Failure ("Deploy manifest contains nested steps: {0}" -f $path)
}
if (-not ($content | Where-Object { $_ -match '^\s+schema_bundle:' })) {
Add-Failure ("Deploy manifest must include schema_bundle for schema contract deployment: {0}" -f $path)
}
if (-not ($content | Where-Object { $_ -match '^\s+script_bundle:' })) {
Add-Failure ("Deploy manifest must include script_bundle for validation/deployment entry points: {0}" -f $path)
}
$fromPaths = @()
$toPaths = @()
$deployableGroups = @()
$declaredModes = @()
$modeNames = @()
$modeGroupRefs = @()
$section = $null
foreach ($line in $content) {
if ($line -match "^deployment_modes:") {
$section = "deployment_modes"
continue
}
if ($line -match "^mode_composition:") {
$section = "mode_composition"
continue
}
if ($line -match "^deployable_by_mode:") {
$section = "deployable_by_mode"
continue
}
if ($line -match "^[a-zA-Z_]+:") {
$section = $null
}
if ($section -eq "deployment_modes" -and $line -match '^\s{2}([a-zA-Z0-9_]+):') {
$declaredModes += $Matches[1]
}
elseif ($section -eq "mode_composition" -and $line -match '^\s{2}([a-zA-Z0-9_]+):\s*\[(.+)\]') {
$modeNames += $Matches[1]
$modeGroupRefs += @($Matches[2] -split "," | ForEach-Object { $_.Trim().Trim('"') } | Where-Object { $_.Length -gt 0 })
}
elseif ($section -eq "deployable_by_mode" -and $line -match '^\s{2}([a-zA-Z0-9_]+):') {
$deployableGroups += $Matches[1]
}
if ($line -match '^\s+- from: "([^"]+)"') {
$fromPaths += $Matches[1]
}
elseif ($line -match '^\s+to: "([^"]+)"') {
$toPaths += $Matches[1]
}
}
foreach ($from in $fromPaths) {
if (-not (Test-Path -LiteralPath (Get-RepoPath $from) -PathType Leaf)) {
Add-Failure ("Deploy source path is missing in {0}: {1}" -f $path, $from)
}
}
$duplicateFrom = $fromPaths | Group-Object | Where-Object { $_.Count -gt 1 }
foreach ($group in $duplicateFrom) {
Add-Failure ("Deploy source path is duplicated in {0}: {1}" -f $path, $group.Name)
}
$nonAppendToPaths = $toPaths | Where-Object { $_ -notlike "*append/adapt*" }
$duplicateTo = $nonAppendToPaths | Group-Object | Where-Object { $_.Count -gt 1 }
foreach ($group in $duplicateTo) {
Add-Failure ("Deploy destination path is duplicated in {0}: {1}" -f $path, $group.Name)
}
foreach ($group in ($modeGroupRefs | Select-Object -Unique)) {
if ($deployableGroups -notcontains $group) {
Add-Failure ("Deploy mode_composition references missing deployable_by_mode group in {0}: {1}" -f $path, $group)
}
if (-not $deploymentScriptContent.Contains(('"{0}"' -f $group))) {
Add-Failure ("Deployment script mode groups are not synchronized with deploy manifest group: {0}" -f $group)
}
}
foreach ($group in ($deployableGroups | Select-Object -Unique)) {
if ($modeGroupRefs -notcontains $group) {
Add-Failure ("Deploy deployable_by_mode group is not reachable from mode_composition in {0}: {1}" -f $path, $group)
}
}
foreach ($mode in ($declaredModes | Select-Object -Unique)) {
if ($modeNames -notcontains $mode) {
Add-Failure ("Deploy deployment_modes entry is missing mode_composition in {0}: {1}" -f $path, $mode)
}
if ($scriptModes -notcontains $mode) {
Add-Failure ("Deployment script ValidateSet is missing deploy manifest mode: {0}" -f $mode)
}
}
foreach ($mode in ($modeNames | Select-Object -Unique)) {
if ($declaredModes -notcontains $mode) {
Add-Failure ("Deploy mode_composition entry is missing deployment_modes entry in {0}: {1}" -f $path, $mode)
}
if (-not $deploymentScriptContent.Contains(('"{0}"' -f $mode))) {
Add-Failure ("Deployment script mode names are not synchronized with deploy manifest mode: {0}" -f $mode)
}
}
foreach ($mode in ($scriptModes | Select-Object -Unique)) {
if ($declaredModes -notcontains $mode) {
Add-Failure ("Deployment script ValidateSet has a mode missing from deploy manifest: {0}" -f $mode)
}
}
foreach ($block in $requiredBlocklist) {
$required = $block.Manifest
if (-not ($content | Where-Object { $_ -match [regex]::Escape($required) })) {
Add-Failure ("Deploy blocklist is missing required path in {0}: {1}" -f $path, $required)
}
if (-not $deploymentScriptContent.Contains($block.Script)) {
Add-Failure ("Deployment script blocklist is not synchronized with deploy manifest path: {0}" -f $required)
}
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Deploy manifest integrity checks passed."
}
}
function Test-AiRuntimeCompactness {
$startFailureCount = $Failures.Count
$verifyProfiles = New-Object 'System.Collections.Generic.HashSet[string]'
$verifyPath = Get-RepoPath ".agents/docs/agents/verify.yaml"
if (Test-Path -LiteralPath $verifyPath -PathType Leaf) {
$inProfiles = $false
foreach ($line in Get-Content -LiteralPath $verifyPath) {
if ($line -match '^profiles:\s*$') {
$inProfiles = $true
continue
}
if ($inProfiles -and $line -match '^\S') {
break
}
if ($inProfiles -and $line -match '^  ([A-Za-z0-9_]+):') {
[void] $verifyProfiles.Add($Matches[1])
}
}
}
else {
Add-Failure "AI runtime route profile check requires .agents/docs/agents/verify.yaml."
}
$profileExemptions = @("none", "named_state")
$paths = @(".agents/docs/agents/ai-runtime.yaml")
$requiredNeedles = @(
"expand_only: true",
"answer_only",
"scoped_edit",
"policy_pack_edit",
"git_checkpoint",
"deploy_or_release",
"multi_agent",
"enterprise_dispatch",
"workflow_artifact",
"context_compact",
"context_intelligence",
"collaborator_window",
"core_system",
"runtime_execution",
"provider_adapter",
"route_pack",
"knowledge_footprint",
"foundation_creation",
".agents/docs/agents/org.yaml",
".agents/docs/agents/model-policy.yaml",
".agents/docs/agents/dispatch.yaml",
".agents/docs/agents/workflow-artifacts.yaml",
".agents/docs/agents/context-compact.yaml",
".agents/docs/agents/context-intelligence.yaml",
".agents/docs/agents/collaborators.yaml",
".agents/docs/agents/core-system.yaml",
".agents/docs/agents/runtime-execution.yaml",
".agents/docs/agents/provider-adapters.yaml",
".agents/docs/agents/route-packs.yaml",
".agents/docs/agents/knowledge-footprint.yaml",
".agents/docs/agents/openai-foundations.yaml",
"hard_isolation",
"Do not load .agents/docs/templates/agents/**",
"Never stage, deploy, or copy runtime_local"
)
foreach ($path in $paths) {
$fullPath = Get-RepoPath $path
if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
Add-Failure ("AI runtime compact route is missing: {0}" -f $path)
continue
}
$item = Get-Item -LiteralPath $fullPath
if ($item.Length -gt 4096) {
Add-Failure ("AI runtime compact route exceeds 4096 bytes: {0} ({1} bytes)" -f $path, $item.Length)
}
$content = Get-Content -LiteralPath $fullPath -Raw
foreach ($needle in $requiredNeedles) {
if (-not $content.Contains($needle)) {
Add-Failure ("AI runtime compact route missing {0}: {1}" -f $needle, $path)
}
}
if ($content -notmatch 'multi_agent:\s*\{\s*f:\s*\[[^\]]*".agents/docs/agents/workflows\.yaml"[^\]]*".agents/docs/agents/schemas\.yaml"[^\]]*".agents/docs/agents/verify\.yaml"[^\]]*\]') {
Add-Failure ("AI runtime multi-agent route must load workflows, schemas, and verify: {0}" -f $path)
}
if ($content -notmatch 'enterprise_dispatch:\s*\{\s*f:\s*\[[^\]]*".agents/docs/agents/org\.yaml"[^\]]*".agents/docs/agents/model-policy\.yaml"[^\]]*".agents/docs/agents/dispatch\.yaml"[^\]]*".agents/docs/agents/verify\.yaml"[^\]]*\]' -or $content -match 'enterprise_dispatch:\s*\{[^\r\n]*(workflows|schemas)\.yaml') {
Add-Failure ("AI runtime enterprise dispatch route must load only org, model-policy, dispatch, and verify: {0}" -f $path)
}
if ($content -notmatch 'workflow_artifact:\s*\{\s*f:\s*\[[^\]]*".agents/docs/agents/workflow-artifacts\.yaml"[^\]]*".agents/docs/agents/schemas\.yaml"[^\]]*".agents/docs/agents/verify\.yaml"[^\]]*\]' -or $content -match 'workflow_artifact:\s*\{[^\r\n]*(org|model-policy|dispatch|workflows|deploy)\.yaml') {
Add-Failure ("AI runtime workflow artifact route must load only workflow-artifacts, schemas, and verify: {0}" -f $path)
}
if ($content -notmatch 'context_compact:\s*\{\s*f:\s*\[[^\]]*".agents/docs/agents/context-compact\.yaml"[^\]]*".agents/docs/agents/schemas\.yaml"[^\]]*".agents/docs/agents/verify\.yaml"[^\]]*\]' -or $content -match 'context_compact:\s*\{[^\r\n]*(org|model-policy|dispatch|workflows|deploy|workflow-artifacts)\.yaml') {
Add-Failure ("AI runtime context compact route must load only context-compact, schemas, and verify: {0}" -f $path)
}
if ($content -notmatch 'context_intelligence:\s*\{\s*f:\s*\[[^\]]*".agents/docs/agents/context-intelligence\.yaml"[^\]]*".agents/docs/agents/context-compact\.yaml"[^\]]*".agents/docs/agents/schemas\.yaml"[^\]]*".agents/docs/agents/verify\.yaml"[^\]]*\]' -or $content -match 'context_intelligence:\s*\{[^\r\n]*(org|model-policy|dispatch|workflows|deploy|workflow-artifacts)\.yaml') {
Add-Failure ("AI runtime context intelligence route must load only context-intelligence, context-compact, schemas, and verify: {0}" -f $path)
}
if ($content -notmatch 'collaborator_window:\s*\{\s*f:\s*\[[^\]]*".agents/docs/agents/collaborators\.yaml"[^\]]*".agents/docs/agents/verify\.yaml"[^\]]*\]' -or $content -match 'collaborator_window:\s*\{[^\r\n]*(org|model-policy|dispatch|workflows|deploy|schemas|workflow-artifacts|context-compact)\.yaml') {
Add-Failure ("AI runtime collaborator window route must load only collaborators and verify: {0}" -f $path)
}
if ($content -notmatch 'core_system:\s*\{\s*f:\s*\[[^\]]*".agents/docs/agents/core-system\.yaml"[^\]]*".agents/docs/agents/verify\.yaml"[^\]]*\]' -or $content -match 'core_system:\s*\{[^\r\n]*(workflows|schemas|deploy|org|dispatch|context-compact)\.yaml') {
Add-Failure ("AI runtime core system route must load only core-system and verify: {0}" -f $path)
}
if ($content -notmatch 'runtime_execution:\s*\{\s*f:\s*\[[^\]]*".agents/docs/agents/runtime-execution\.yaml"[^\]]*".agents/docs/agents/verify\.yaml"[^\]]*\]') {
Add-Failure ("AI runtime execution route must load runtime-execution and verify: {0}" -f $path)
}
if ($content -notmatch 'provider_adapter:\s*\{\s*f:\s*\[[^\]]*".agents/docs/agents/provider-adapters\.yaml"[^\]]*".agents/docs/agents/model-policy\.yaml"[^\]]*".agents/docs/agents/verify\.yaml"[^\]]*\]') {
Add-Failure ("AI runtime provider adapter route must load provider-adapters, model-policy, and verify: {0}" -f $path)
}
if ($content -notmatch 'provider_adapter:\s*\{[^\r\n]*v:\s*provider_adapter\b') {
Add-Failure ("AI runtime provider adapter route must use provider_adapter verify profile: {0}" -f $path)
}
if ($content -notmatch 'route_pack:\s*\{\s*f:\s*\[[^\]]*".agents/docs/agents/route-packs\.yaml"[^\]]*".agents/docs/agents/ai-runtime\.yaml"[^\]]*\]') {
Add-Failure ("AI runtime route pack route must load route-packs and ai-runtime: {0}" -f $path)
}
if ($content -notmatch 'knowledge_footprint:\s*\{\s*f:\s*\[[^\]]*".agents/docs/agents/knowledge-footprint\.yaml"[^\]]*".agents/docs/agents/context-compact\.yaml"[^\]]*".agents/docs/agents/verify\.yaml"[^\]]*\]') {
Add-Failure ("AI runtime knowledge footprint route must load knowledge-footprint, context-compact, and verify: {0}" -f $path)
}
if ($content -notmatch 'foundation_creation:\s*\{\s*f:\s*\[[^\]]*".agents/docs/agents/openai-foundations\.yaml"[^\]]*".agents/docs/agents/schemas\.yaml"[^\]]*".agents/docs/agents/verify\.yaml"[^\]]*\]' -or $content -match 'foundation_creation:\s*\{[^\r\n]*(org|dispatch|workflows|deploy|runtime-execution|collaborators)\.yaml') {
Add-Failure ("AI runtime foundation creation route must load only openai-foundations, schemas, and verify: {0}" -f $path)
}
$routeMatches = [regex]::Matches($content, '(?m)^\s+([A-Za-z0-9_]+):\s*\{[^\r\n]*\bv:\s*([A-Za-z0-9_]+|none)\s*\}')
foreach ($match in $routeMatches) {
$routeId = [string] $match.Groups[1].Value
$profile = [string] $match.Groups[2].Value
if ($profileExemptions -contains $profile) {
continue
}
if (-not $verifyProfiles.Contains($profile)) {
Add-Failure ("AI runtime route {0} references missing verify profile: {1}" -f $routeId, $profile)
}
}
}
foreach ($path in @("AGENTS.md", ".agents/skills/project-isolation-workflow/SKILL.md")) {
$fullPath = Get-RepoPath $path
if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
Add-Failure ("AI runtime reference file is missing: {0}" -f $path)
continue
}
$content = Get-Content -LiteralPath $fullPath -Raw
if (-not $content.Contains(".agents/docs/agents/ai-runtime.yaml")) {
Add-Failure ("AI runtime compact route is not referenced by {0}" -f $path)
}
}
foreach ($path in @(".agents/docs/agents/ai-runtime.yaml", ".agents/docs/agents/verify.yaml", ".agents/docs/agents/provider-adapters.yaml")) {
$fullPath = Get-RepoPath $path
$content = Get-Content -LiteralPath $fullPath -Raw
if ($content -match '\bprovider_adapters\b') {
Add-Failure ("Legacy provider naming token provider_adapters is present in {0}" -f $path)
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "AI runtime compact route checks passed."
}
}
function Test-EnterpriseDispatchIntegrity {
$startFailureCount = $Failures.Count
$orgPath = Get-RepoPath ".agents/docs/agents/org.yaml"
$modelPath = Get-RepoPath ".agents/docs/agents/model-policy.yaml"
$dispatchPath = Get-RepoPath ".agents/docs/agents/dispatch.yaml"
$workflowsPath = Get-RepoPath ".agents/docs/agents/workflows.yaml"
if (-not ((Test-Path -LiteralPath $orgPath -PathType Leaf) -and (Test-Path -LiteralPath $modelPath -PathType Leaf) -and (Test-Path -LiteralPath $dispatchPath -PathType Leaf) -and (Test-Path -LiteralPath $workflowsPath -PathType Leaf))) {
Add-Failure "Enterprise dispatch canonical files are incomplete."
return
}
$org = Get-LightweightYamlPathValues -File (Get-Item -LiteralPath $orgPath)
$model = Get-LightweightYamlPathValues -File (Get-Item -LiteralPath $modelPath)
$dispatch = Get-LightweightYamlPathValues -File (Get-Item -LiteralPath $dispatchPath)
$workflows = Get-LightweightYamlPathValues -File (Get-Item -LiteralPath $workflowsPath)
$tiers = @("low_fast", "quick_code", "code_standard", "senior_review", "principal")
$departments = [ordered]@{
executive_office = "executive_lead"
pmo = "pmo_lead"
architecture = "architecture_lead"
engineering = "engineering_lead"
devops = "devops_lead"
qa = "qa_lead"
security = "security_lead"
documentation = "documentation_lead"
provider_management = "provider_management_lead"
}
foreach ($department in $departments.Keys) {
$leader = $departments[$department]
foreach ($suffix in @("leader_role", "allowed_worker_roles", "report_target", "default_responsibility")) {
$path = "departments.{0}.{1}" -f $department, $suffix
if (-not $org.ContainsKey($path)) {
Add-Failure ("Enterprise org missing department field: {0}" -f $path)
}
}
if ($org.ContainsKey("departments.$department.leader_role") -and $org["departments.$department.leader_role"] -ne $leader) {
Add-Failure ("Enterprise org leader mismatch for {0}." -f $department)
}
if ($org.ContainsKey("departments.$department.report_target") -and $org["departments.$department.report_target"] -ne "controller") {
Add-Failure ("Enterprise org report target must be controller: {0}" -f $department)
}
if (-not $org.ContainsKey("leader_registry.$leader.department")) {
Add-Failure ("Enterprise org missing leader registry entry: {0}" -f $leader)
}
elseif ($org["leader_registry.$leader.department"] -ne $department) {
Add-Failure ("Enterprise org leader registry department mismatch: {0}" -f $leader)
}
$leaderTierPath = "leader_registry.$leader.default_model_tier"
if (-not $org.ContainsKey($leaderTierPath)) {
Add-Failure ("Enterprise org missing leader default tier: {0}" -f $leader)
}
elseif ($tiers -notcontains $org[$leaderTierPath]) {
Add-Failure ("Enterprise org references unknown leader default tier: {0}" -f $org[$leaderTierPath])
}
$bindingPath = "department_bindings.$department"
if (-not $model.ContainsKey($bindingPath)) {
Add-Failure ("Model policy missing department binding: {0}" -f $department)
}
elseif ($tiers -notcontains $model[$bindingPath]) {
Add-Failure ("Model policy department binding references unknown tier: {0}" -f $department)
}
}
foreach ($tier in $tiers) {
foreach ($suffix in @("capability", "risk_limit")) {
$path = "tiers.{0}.{1}" -f $tier, $suffix
if (-not $model.ContainsKey($path)) {
Add-Failure ("Model policy missing tier field: {0}" -f $path)
}
}
if (-not $model.ContainsKey("model_mapping.$tier")) {
Add-Failure ("Model policy missing replaceable model mapping for tier: {0}" -f $tier)
}
}
if (-not $model.ContainsKey("model_mapping.replaceable") -or $model["model_mapping.replaceable"] -ne "true") {
Add-Failure "Model policy must mark model_mapping.replaceable as true."
}
foreach ($path in @("risk_rules.high_risk_min_tier", "risk_rules.release_min_tier", "risk_rules.deploy_write_min_tier", "risk_rules.code_write_min_tier", "risk_rules.read_only_batch_default")) {
if (-not $model.ContainsKey($path)) {
Add-Failure ("Model policy missing risk tier path: {0}" -f $path)
}
elseif ($tiers -notcontains $model[$path]) {
Add-Failure ("Model policy risk rule references unknown tier at {0}: {1}" -f $path, $model[$path])
}
}
foreach ($path in @("risk_rules.fallback_order", "risk_rules.model_missing_behavior")) {
if (-not $model.ContainsKey($path)) {
Add-Failure ("Model policy missing deterministic fallback path: {0}" -f $path)
}
}
if ($model.ContainsKey("risk_rules.fallback_order")) {
$fallbackOrder = $model["risk_rules.fallback_order"]
$lastIndex = -1
foreach ($tier in $tiers) {
$index = $fallbackOrder.IndexOf($tier)
if ($index -lt 0) {
Add-Failure ("Model policy fallback_order missing tier: {0}" -f $tier)
}
elseif ($index -le $lastIndex) {
Add-Failure "Model policy fallback_order must preserve low-to-high tier order."
}
$lastIndex = $index
}
}
if ($model.ContainsKey("risk_rules.model_missing_behavior") -and (-not $model["risk_rules.model_missing_behavior"].Contains("never downgrade") -or -not $model["risk_rules.model_missing_behavior"].Contains("escalation_record"))) {
Add-Failure "Model policy model_missing_behavior must forbid downgrade and require escalation_record."
}
if (-not $dispatch.ContainsKey("protocol.name") -or $dispatch["protocol.name"] -ne "enterprise_dispatch") {
Add-Failure "Dispatch protocol name must be enterprise_dispatch."
}
if (-not $dispatch.ContainsKey("protocol.canonical_source_rule") -or -not $dispatch["protocol.canonical_source_rule"].Contains("enterprise dispatch semantics") -or -not $dispatch["protocol.canonical_source_rule"].Contains("workflows.yaml")) {
Add-Failure "Dispatch protocol must name dispatch.yaml as the enterprise dispatch source of truth."
}
if (-not $dispatch.ContainsKey("controller_assignment.target") -or $dispatch["controller_assignment.target"] -ne "department leader") {
Add-Failure "Controller assignment target must be department leader."
}
foreach ($field in @("department", "leader_role", "model_tier", "report_back")) {
$path = "controller_assignment.required_fields.$field"
if (-not $dispatch.ContainsKey($path)) {
Add-Failure ("Dispatch controller assignment missing required field: {0}" -f $field)
}
}
if ($dispatch.ContainsKey("controller_assignment.required_fields.report_back") -and $dispatch["controller_assignment.required_fields.report_back"] -ne "department_report") {
Add-Failure "Dispatch controller assignment must report back with department_report."
}
if (-not $dispatch.ContainsKey("worker_report_policy.direct_to_controller") -or -not $dispatch["worker_report_policy.direct_to_controller"].Contains("forbidden") -or -not $dispatch["worker_report_policy.direct_to_controller"].Contains("escalation_record")) {
Add-Failure "Dispatch worker direct-to-controller policy must require escalation_record."
}
if (-not $dispatch.ContainsKey("validation.worker_bypass_rule") -or -not $dispatch["validation.worker_bypass_rule"].Contains("escalation_record")) {
Add-Failure "Dispatch validation worker bypass rule must require escalation_record."
}
$levelKinds = @{
level_1 = "pass"
level_2 = "pass"
level_3 = "pass"
level_4 = "guardrail"
level_5 = "reserved"
level_6 = "guardrail"
}
foreach ($level in $levelKinds.Keys) {
foreach ($suffix in @("kind", "assignment", "model_tier", "pass_when")) {
$path = "runtime_test_matrix.{0}.{1}" -f $level, $suffix
if (-not $dispatch.ContainsKey($path)) {
Add-Failure ("Dispatch runtime test matrix missing path: {0}" -f $path)
}
}
$kindPath = "runtime_test_matrix.{0}.kind" -f $level
if ($dispatch.ContainsKey($kindPath) -and $dispatch[$kindPath] -ne $levelKinds[$level]) {
Add-Failure ("Dispatch runtime test matrix kind mismatch at {0}." -f $kindPath)
}
$tierPath = "runtime_test_matrix.{0}.model_tier" -f $level
if ($dispatch.ContainsKey($tierPath) -and ($tiers -notcontains $dispatch[$tierPath])) {
Add-Failure ("Dispatch runtime test matrix unknown model tier at {0}: {1}" -f $tierPath, $dispatch[$tierPath])
}
}
foreach ($level in @("level_4", "level_6")) {
$path = "runtime_test_matrix.{0}.pass_when" -f $level
if (-not $dispatch.ContainsKey($path) -or -not $dispatch[$path].Contains("blocked") -or -not $dispatch[$path].Contains("escalation_record") -or -not $dispatch[$path].Contains("fails")) {
Add-Failure ("Dispatch {0} guardrail pass_when must require block/escalation/fail behavior." -f $level)
}
}
if ($dispatch.ContainsKey("runtime_test_matrix.level_6.pass_when") -and (-not $dispatch["runtime_test_matrix.level_6.pass_when"].Contains("senior_review") -or -not $dispatch["runtime_test_matrix.level_6.pass_when"].Contains("silent acceptance fails"))) {
Add-Failure "Dispatch level_6 pass_when must reject silent acceptance."
}
foreach ($path in @("escalation_record.canonical_value_rule", "validation.escalation_value_rule")) {
if (-not $dispatch.ContainsKey($path)) {
Add-Failure ("Dispatch escalation canonical value rule is missing: {0}" -f $path)
}
}
if ($dispatch.ContainsKey("escalation_record.canonical_value_rule") -and (-not $dispatch["escalation_record.canonical_value_rule"].Contains("department id") -or -not $dispatch["escalation_record.canonical_value_rule"].Contains("model tiers"))) {
Add-Failure "Dispatch escalation record must require canonical department ids and model tiers."
}
if ($dispatch.ContainsKey("validation.escalation_value_rule") -and (-not $dispatch["validation.escalation_value_rule"].Contains("canonical department ids") -or -not $dispatch["validation.escalation_value_rule"].Contains("model tiers"))) {
Add-Failure "Dispatch validation must check escalation canonical department ids and model tiers."
}
foreach ($countName in @("requested", "spawned", "completed", "closed", "missing", "duplicate", "invalid_format", "failed", "risk")) {
$path = "runtime_report_contract.required_counts.{0}" -f $countName
if (-not $dispatch.ContainsKey($path)) {
Add-Failure ("Dispatch runtime report contract missing count: {0}" -f $countName)
}
}
foreach ($path in @("runtime_report_contract.output_format", "runtime_report_contract.response_envelope_rule", "runtime_report_contract.count_authority_rule", "runtime_report_contract.positive_rule", "runtime_report_contract.guardrail_rule", "runtime_report_contract.cleanup_rule", "runtime_report_contract.isolation_rule", "validation.runtime_test_rule", "validation.runtime_report_rule", "validation.count_authority_rule", "validation.isolation_normalization_rule", "validation.department_report_field_rule", "validation.authority_enforcement_rule", "validation.source_of_truth_rule")) {
if (-not $dispatch.ContainsKey($path)) {
Add-Failure ("Dispatch runtime report or validation rule is missing: {0}" -f $path)
}
}
if ($dispatch.ContainsKey("runtime_report_contract.output_format") -and $dispatch["runtime_report_contract.output_format"] -ne "single compact JSON object") {
Add-Failure "Dispatch runtime report output_format must be a single compact JSON object."
}
if ($dispatch.ContainsKey("runtime_report_contract.response_envelope_rule") -and (-not $dispatch["runtime_report_contract.response_envelope_rule"].Contains('strip $$') -or -not $dispatch["runtime_report_contract.response_envelope_rule"].Contains("valid JSON"))) {
Add-Failure "Dispatch runtime report contract must normalize the required visible prefix before machine parsing."
}
if ($dispatch.ContainsKey("runtime_report_contract.response_envelope_rule") -and $dispatch["runtime_report_contract.response_envelope_rule"].Contains("stable fields")) {
Add-Failure "Dispatch response envelope must not permit ambiguous stable-field parsing."
}
if ($dispatch.ContainsKey("runtime_report_contract.count_authority_rule") -and (-not $dispatch["runtime_report_contract.count_authority_rule"].Contains("Controller-reconciled") -or -not $dispatch["runtime_report_contract.count_authority_rule"].Contains("authoritative") -or -not $dispatch["runtime_report_contract.count_authority_rule"].Contains("employee"))) {
Add-Failure "Dispatch runtime report contract must make controller-reconciled counts authoritative."
}
if ($dispatch.ContainsKey("runtime_report_contract.positive_rule") -and (-not $dispatch["runtime_report_contract.positive_rule"].Contains("Level 1-3") -or -not $dispatch["runtime_report_contract.positive_rule"].Contains("department_report") -or -not $dispatch["runtime_report_contract.positive_rule"].Contains("no raw worker chatter"))) {
Add-Failure "Dispatch positive runtime rule must require Level 1-3 department reports without raw worker chatter."
}
if ($dispatch.ContainsKey("runtime_report_contract.guardrail_rule") -and (-not $dispatch["runtime_report_contract.guardrail_rule"].Contains("Level 4") -or -not $dispatch["runtime_report_contract.guardrail_rule"].Contains("level_6") -or -not $dispatch["runtime_report_contract.guardrail_rule"].Contains("escalation_record"))) {
Add-Failure "Dispatch guardrail runtime rule must cover Level 4, level_6, and escalation_record."
}
if ($dispatch.ContainsKey("runtime_report_contract.cleanup_rule") -and (-not $dispatch["runtime_report_contract.cleanup_rule"].Contains("protocol smoke success") -or -not $dispatch["runtime_report_contract.cleanup_rule"].Contains("runtime cleanup success"))) {
Add-Failure "Dispatch cleanup rule must split protocol smoke success from runtime cleanup success."
}
if ($dispatch.ContainsKey("runtime_report_contract.isolation_rule") -and (-not $dispatch["runtime_report_contract.isolation_rule"].Contains(".agents/skills/**") -or -not $dispatch["runtime_report_contract.isolation_rule"].Contains("not GS"))) {
Add-Failure "Dispatch isolation rule must normalize project-local skill usage as not GS."
}
if ($dispatch.ContainsKey("validation.count_authority_rule") -and (-not $dispatch["validation.count_authority_rule"].Contains("controller reconciliation") -or -not $dispatch["validation.count_authority_rule"].Contains("employee self-counts"))) {
Add-Failure "Dispatch validation must reject employee self-counts as lifecycle proof."
}
if (-not $workflows.ContainsKey("enterprise_dispatch_runtime.source_of_truth") -or -not $workflows["enterprise_dispatch_runtime.source_of_truth"].Contains(".agents/docs/agents/dispatch.yaml")) {
Add-Failure "Workflows enterprise dispatch summary must point to .agents/docs/agents/dispatch.yaml."
}
if (-not $workflows.ContainsKey("enterprise_dispatch_runtime.department_report") -or -not $workflows["enterprise_dispatch_runtime.department_report"].Contains("objective_result")) {
Add-Failure "Workflows enterprise dispatch department_report summary must use objective_result."
}
if ($workflows.ContainsKey("enterprise_dispatch_runtime.department_report") -and $workflows["enterprise_dispatch_runtime.department_report"].Contains("worker_count, result")) {
Add-Failure "Workflows enterprise dispatch department_report summary must not use result alias."
}
$routeChecks = @(
@(".agents/docs/agents/workflows.yaml", "enterprise_dispatch_runtime"),
@(".agents/docs/agents/schemas.yaml", "enterprise_assignment"),
@(".agents/docs/agents/schemas.yaml", "department_report"),
@(".agents/docs/agents/schemas.yaml", "model_tier"),
@(".agents/docs/agents/schemas.yaml", "escalation_record"),
@(".agents/docs/agents/schemas.yaml", "project_local_skill_rule"),
@(".agents/docs/agents/verify.yaml", "enterprise_dispatch"),
@(".agents/docs/agents/deploy.yaml", ".agents/docs/agents/org.yaml"),
@(".agents/docs/agents/deploy.yaml", ".agents/docs/agents/model-policy.yaml"),
@(".agents/docs/agents/deploy.yaml", ".agents/docs/agents/dispatch.yaml"),
@(".agents/docs/agents/ai-runtime.yaml", "enterprise_dispatch")
)
foreach ($check in $routeChecks) {
$content = Get-Content -LiteralPath (Get-RepoPath $check[0]) -Raw
if (-not $content.Contains($check[1])) {
Add-Failure ("Enterprise dispatch marker is missing in {0}: {1}" -f $check[0], $check[1])
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Enterprise dispatch integrity checks passed."
}
}
function Test-WorkflowArtifactIntegrity {
$startFailureCount = $Failures.Count
$requiredFiles = @(
".agents/docs/agents/workflow-artifacts.yaml",
"schemas/agents-workflow-artifacts.schema.json",
"scripts/agents-workflow.ps1"
)
$allRequiredFilesExist = $true
foreach ($path in $requiredFiles) {
if (-not (Test-Path -LiteralPath (Get-RepoPath $path) -PathType Leaf)) {
Add-Failure ("Workflow artifact required file is missing: {0}" -f $path)
$allRequiredFilesExist = $false
}
}
if (-not $allRequiredFilesExist) {
return
}
$canonicalFile = Get-Item -LiteralPath (Get-RepoPath ".agents/docs/agents/workflow-artifacts.yaml")
$workflow = Get-LightweightYamlPathValues -File $canonicalFile
function Assert-WorkflowPath {
param([string] $Path)
if (-not $workflow.ContainsKey($Path)) {
Add-Failure ("Workflow artifact canonical is missing path: {0}" -f $Path)
}
}
function Assert-WorkflowPathContains {
param(
[string] $Path,
[string[]] $Needles
)
if (-not $workflow.ContainsKey($Path)) {
Add-Failure ("Workflow artifact canonical is missing path: {0}" -f $Path)
return
}
$value = [string] $workflow[$Path]
foreach ($needle in $Needles) {
if (-not $value.Contains($needle)) {
Add-Failure ("Workflow artifact path {0} is missing marker: {1}" -f $Path, $needle)
}
}
}
foreach ($path in @(
"storage.live_root",
"storage.import_alias",
"storage.git_rule",
"states.allowed",
"workflow_instance.required_fields",
"workflow_instance.artifact_root_rule",
"packets.types",
"packets.required_fields",
"packets.routing_rule",
"approval_gates.required_for",
"security.path_guard",
"collection.inputs",
"collection.controller_rule",
"collection.required_counts",
"simulation.levels",
"simulation.guardrail",
"helper.actions",
"helper.validate_entry"
)) {
Assert-WorkflowPath $path
}
Assert-WorkflowPathContains "storage.live_root" @(".agents/runtime/workflows/")
Assert-WorkflowPathContains "storage.import_alias" @(".workflow/")
Assert-WorkflowPathContains "states.allowed" @("drafted", "approved", "active", "waiting_approval", "collecting", "completed", "blocked", "stopped")
Assert-WorkflowPathContains "packets.types" @("department_leader_assignment", "worker_packet", "verification_packet", "escalation_packet")
Assert-WorkflowPathContains "packets.required_fields" @("owner", "target", "model_tier")
Assert-WorkflowPathContains "approval_gates.required_for" @("write", "deploy", "external_read", "external_write", "destructive_action", "model_tier_upgrade")
Assert-WorkflowPathContains "collection.controller_rule" @("raw worker chatter")
Assert-WorkflowPathContains "simulation.levels" @("level_1", "level_2", "level_3", "level_4", "level_5", "level_6")
Assert-WorkflowPathContains "simulation.guardrail" @("blocked", "escalated")
$scriptContent = Get-Content -LiteralPath (Get-RepoPath "scripts/agents-workflow.ps1") -Raw
foreach ($marker in @("New", "Verify", "Collect", "SimulateDispatch", "NormalizeReport", "raw_worker_chatter", "approval_gate", "Level")) {
if (-not $scriptContent.Contains($marker)) {
Add-Failure ("Workflow artifact helper script is missing marker: {0}" -f $marker)
}
}
foreach ($level in 1..6) {
$previousErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
$output = & (Get-RepoPath "scripts/agents-workflow.ps1") -Action SimulateDispatch -WorkflowId ("validate-l{0}" -f $level) -Level $level -Quiet 2>&1
$exitCode = if ($null -eq $LASTEXITCODE) { 0 } else { $LASTEXITCODE }
}
finally {
$ErrorActionPreference = $previousErrorActionPreference
}
if ($exitCode -ne 0) {
Add-Failure ("Workflow artifact simulation Level {0} failed." -f $level)
foreach ($line in $output) {
Add-Failure ("Workflow artifact simulation detail: {0}" -f $line)
}
}
elseif (@($output).Count -gt 0) {
Add-Failure ("Workflow artifact simulation Level {0} quiet mode produced output." -f $level)
foreach ($line in $output) {
Add-Failure ("Workflow artifact simulation quiet output: {0}" -f $line)
}
}
}
$routeChecks = @(
@(".agents/docs/agents/ai-runtime.yaml", "workflow_artifact"),
@(".agents/docs/agents/workflows.yaml", "workflow_artifact_runtime"),
@(".agents/docs/agents/schemas.yaml", "workflow_packet"),
@(".agents/docs/agents/verify.yaml", "workflow_artifact"),
@(".agents/docs/agents/deploy.yaml", ".agents/docs/agents/workflow-artifacts.yaml")
)
foreach ($check in $routeChecks) {
$content = Get-Content -LiteralPath (Get-RepoPath $check[0]) -Raw
if (-not $content.Contains($check[1])) {
Add-Failure ("Workflow artifact marker is missing in {0}: {1}" -f $check[0], $check[1])
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Workflow artifact integrity checks passed."
}
}
function Test-ContextCompactIntegrity {
$startFailureCount = $Failures.Count
$requiredFiles = @(
".agents/docs/agents/context-compact.yaml",
"schemas/agents-context-compact.schema.json"
)
$allRequiredFilesExist = $true
foreach ($path in $requiredFiles) {
if (-not (Test-Path -LiteralPath (Get-RepoPath $path) -PathType Leaf)) {
Add-Failure ("Context compact required file is missing: {0}" -f $path)
$allRequiredFilesExist = $false
}
}
if (-not $allRequiredFilesExist) {
return
}
$canonicalFile = Get-Item -LiteralPath (Get-RepoPath ".agents/docs/agents/context-compact.yaml")
$compact = Get-LightweightYamlPathValues -File $canonicalFile
function Assert-CompactPath {
param([string] $Path)
if (-not $compact.ContainsKey($Path)) {
Add-Failure ("Context compact canonical is missing path: {0}" -f $Path)
}
}
function Assert-CompactPathContains {
param(
[string] $Path,
[string[]] $Needles
)
if (-not $compact.ContainsKey($Path)) {
Add-Failure ("Context compact canonical is missing path: {0}" -f $Path)
return
}
$value = [string] $compact[$Path]
foreach ($needle in $Needles) {
if (-not $value.Contains($needle)) {
Add-Failure ("Context compact path {0} is missing marker: {1}" -f $Path, $needle)
}
}
}
foreach ($path in @(
"trigger_when.include",
"trigger_when.route_signal",
"summary_contract.required_fields",
"summary_contract.freshness_rule",
"summary_contract.minimality_rule",
"auto_compact.before",
"auto_compact.after",
"auto_compact.reject_when",
"runtime_events.optional_path",
"runtime_events.git_rule",
"runtime_events.event_fields",
"runtime_events.trigger_values",
"subagent_rule",
"approval_rule",
"verification.profile"
)) {
Assert-CompactPath $path
}
Assert-CompactPathContains "summary_contract.required_fields" @("latest_user_request", "changed_files", "verification_state", "open_risks", "external_access", "subagents_open", "next_step", "isolation")
Assert-CompactPathContains "summary_contract.minimality_rule" @("raw transcript", "raw worker chatter")
Assert-CompactPathContains "auto_compact.reject_when" @("missing latest_user_request", "unclosed subagents", "raw transcript", "external access omitted")
Assert-CompactPathContains "runtime_events.optional_path" @(".agents/runtime/compact-events.jsonl")
Assert-CompactPathContains "runtime_events.git_rule" @("never stage", "deploy", "release")
Assert-CompactPathContains "subagent_rule" @("requested", "spawned", "completed", "closed")
Assert-CompactPathContains "approval_rule" @("approval gate", "escalation record")
Assert-CompactPathContains "verification.profile" @("context_compact")
$routeChecks = @(
@(".agents/docs/agents/ai-runtime.yaml", "context_compact"),
@(".agents/docs/agents/workflows.yaml", "context_compact_runtime"),
@(".agents/docs/agents/schemas.yaml", "context_compact_summary"),
@(".agents/docs/agents/verify.yaml", "context_compact"),
@(".agents/docs/agents/deploy.yaml", ".agents/docs/agents/context-compact.yaml"),
@(".agents/docs/agents/version.yaml", "context_compact")
)
foreach ($check in $routeChecks) {
$content = Get-Content -LiteralPath (Get-RepoPath $check[0]) -Raw
if (-not $content.Contains($check[1])) {
Add-Failure ("Context compact marker is missing in {0}: {1}" -f $check[0], $check[1])
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Context compact integrity checks passed."
}
}
function Test-CollaboratorWindowIntegrity {
$startFailureCount = $Failures.Count
$requiredFiles = @(
".agents/docs/agents/collaborators.yaml",
"schemas/agents-collaborators.schema.json"
)
$allRequiredFilesExist = $true
foreach ($path in $requiredFiles) {
if (-not (Test-Path -LiteralPath (Get-RepoPath $path) -PathType Leaf)) {
Add-Failure ("Collaborator window required file is missing: {0}" -f $path)
$allRequiredFilesExist = $false
}
}
if (-not $allRequiredFilesExist) {
return
}
$canonicalFile = Get-Item -LiteralPath (Get-RepoPath ".agents/docs/agents/collaborators.yaml")
$collab = Get-LightweightYamlPathValues -File $canonicalFile
$org = Get-LightweightYamlPathValues -File (Get-Item -LiteralPath (Get-RepoPath ".agents/docs/agents/org.yaml"))
$model = Get-LightweightYamlPathValues -File (Get-Item -LiteralPath (Get-RepoPath ".agents/docs/agents/model-policy.yaml"))
function Assert-CollabPath {
param([string] $Path)
if (-not $collab.ContainsKey($Path)) {
Add-Failure ("Collaborator canonical is missing path: {0}" -f $Path)
}
}
function Assert-CollabPathContains {
param(
[string] $Path,
[string[]] $Needles
)
if (-not $collab.ContainsKey($Path)) {
Add-Failure ("Collaborator canonical is missing path: {0}" -f $Path)
return
}
$value = [string] $collab[$Path]
foreach ($needle in $Needles) {
if (-not $value.Contains($needle)) {
Add-Failure ("Collaborator path {0} is missing marker: {1}" -f $Path, $needle)
}
}
}
foreach ($path in @(
"trigger_when.include",
"trigger_when.route_signal",
"storage.runtime_registry",
"storage.temp_registry",
"storage.git_rule",
"storage.thread_id_rule",
"capability.discover_before_use",
"capability.unavailable_behavior",
"capability.delete_claim_rule",
"types.default",
"types.allowed",
"types.blocked_without_override",
"lifecycle.states",
"lifecycle.close_rule",
"lifecycle.cleanup_boundary",
"leader_mapping.greeting_or_docs.department",
"leader_mapping.greeting_or_docs.leader_role",
"leader_mapping.greeting_or_docs.default_model_tier",
"commands.create_named_collaborator",
"commands.rename_named_collaborator",
"commands.dismiss_named_collaborator",
"assignment.required_fields",
"assignment.target_rule",
"assignment.worker_window_rule",
"reports.allowed_outputs",
"reports.integration_rule",
"reports.collaborator_report_fields",
"validation.profile",
"validation.leader_reference_rule",
"validation.model_tier_rule",
"validation.runtime_boundary_rule",
"validation.close_claim_rule"
)) {
Assert-CollabPath $path
}
Assert-CollabPathContains "storage.runtime_registry" @(".agents/runtime/collaborators.jsonl")
Assert-CollabPathContains "storage.git_rule" @("Never stage", "deploy", "release")
Assert-CollabPathContains "storage.thread_id_rule" @("thread ids", "runtime evidence", "templates", "release packages")
Assert-CollabPathContains "capability.discover_before_use" @("Discover thread tools")
Assert-CollabPathContains "types.allowed" @("department_leader_window")
Assert-CollabPathContains "types.blocked_without_override" @("worker_window")
Assert-CollabPathContains "lifecycle.states" @("created", "active", "renamed", "reporting", "archived", "closed", "orphaned")
Assert-CollabPathContains "lifecycle.cleanup_boundary" @("thread archive or close", "subagent sidebar/history cleanup")
Assert-CollabPathContains "assignment.required_fields" @("department", "leader_role", "thread_id", "model_tier")
Assert-CollabPathContains "assignment.worker_window_rule" @("blocked", "explicit override", "escalation_record")
Assert-CollabPathContains "reports.allowed_outputs" @("department_report", "collaborator_report", "escalation_record")
Assert-CollabPathContains "reports.integration_rule" @("raw worker chatter")
Assert-CollabPathContains "validation.runtime_boundary_rule" @("thread ids", "deploy", "release")
if ($collab.ContainsKey("types.default") -and $collab["types.default"] -ne "department_leader_window") {
Add-Failure "Collaborator default type must be department_leader_window."
}
if ($collab.ContainsKey("validation.profile") -and $collab["validation.profile"] -ne "collaborator_window") {
Add-Failure "Collaborator validation profile must be collaborator_window."
}
$mappings = [ordered]@{
greeting_or_docs = @("documentation", "documentation_lead", "low_fast")
validation_or_testing = @("qa", "qa_lead", "code_standard")
deploy_or_release = @("devops", "devops_lead", "senior_review")
architecture_or_design = @("architecture", "architecture_lead", "principal")
model_or_provider = @("provider_management", "provider_management_lead", "senior_review")
cross_department = @("pmo", "pmo_lead", "senior_review")
}
foreach ($mapping in $mappings.Keys) {
$expected = $mappings[$mapping]
$deptPath = "leader_mapping.$mapping.department"
$leaderPath = "leader_mapping.$mapping.leader_role"
$tierPath = "leader_mapping.$mapping.default_model_tier"
if (-not $collab.ContainsKey($deptPath) -or $collab[$deptPath] -ne $expected[0]) {
Add-Failure ("Collaborator mapping department mismatch: {0}" -f $mapping)
}
if (-not $collab.ContainsKey($leaderPath) -or $collab[$leaderPath] -ne $expected[1]) {
Add-Failure ("Collaborator mapping leader mismatch: {0}" -f $mapping)
}
if (-not $collab.ContainsKey($tierPath) -or $collab[$tierPath] -ne $expected[2]) {
Add-Failure ("Collaborator mapping model tier mismatch: {0}" -f $mapping)
}
if (-not $org.ContainsKey(("departments.{0}.leader_role" -f $expected[0])) -or $org[("departments.{0}.leader_role" -f $expected[0])] -ne $expected[1]) {
Add-Failure ("Collaborator mapping references invalid department leader: {0}" -f $mapping)
}
if (-not $org.ContainsKey(("leader_registry.{0}.department" -f $expected[1]))) {
Add-Failure ("Collaborator mapping references missing leader registry entry: {0}" -f $expected[1])
}
if (-not $model.ContainsKey(("tiers.{0}.capability" -f $expected[2]))) {
Add-Failure ("Collaborator mapping references missing model tier: {0}" -f $expected[2])
}
}
$routeChecks = @(
@(".agents/docs/agents/ai-runtime.yaml", "collaborator_window"),
@(".agents/docs/agents/ai-runtime.yaml", ".agents/docs/agents/collaborators.yaml"),
@(".agents/docs/agents/org.yaml", "collaborator_windows"),
@(".agents/docs/agents/org.yaml", "department_leader_window"),
@(".agents/docs/agents/dispatch.yaml", "collaborator_target_rule"),
@(".agents/docs/agents/dispatch.yaml", "collaborator_window_target"),
@(".agents/docs/agents/dispatch.yaml", "collaborator_rule"),
@(".agents/docs/agents/workflows.yaml", "collaborator_window_runtime"),
@(".agents/docs/agents/workflows.yaml", "close_rule"),
@(".agents/docs/agents/workflows.yaml", "runtime_rule"),
@(".agents/docs/agents/deploy.yaml", ".agents/docs/agents/collaborators.yaml"),
@(".agents/docs/agents/deploy.yaml", ".agents/runtime/collaborators.jsonl"),
@(".agents/docs/agents/deploy.yaml", "live thread ids"),
@(".agents/docs/agents/schemas.yaml", "collaborator_record"),
@(".agents/docs/agents/schemas.yaml", "collaborator_assignment"),
@(".agents/docs/agents/schemas.yaml", "collaborator_report"),
@(".agents/docs/agents/schemas.yaml", "thread_operation_record"),
@(".agents/docs/agents/verify.yaml", "collaborator_window"),
@(".agents/docs/agents/version.yaml", "collaborator_window")
)
foreach ($check in $routeChecks) {
$content = Get-Content -LiteralPath (Get-RepoPath $check[0]) -Raw
if (-not $content.Contains($check[1])) {
Add-Failure ("Collaborator marker is missing in {0}: {1}" -f $check[0], $check[1])
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Collaborator window integrity checks passed."
}
}
function Test-DeploymentScriptSafety {
$startFailureCount = $Failures.Count
$path = "scripts/deploy-agents-workflow.ps1"
$fullPath = Get-RepoPath $path
if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
Add-Failure "Deployment entry point is missing: scripts/deploy-agents-workflow.ps1"
return
}
$content = Get-Content -LiteralPath $fullPath -Raw
$requiredMarkers = @(
"Assert-RelativeDeployPath",
"Assert-InsideRoot",
"Test-PathInsideRoot",
"Confirm-SourceAllowed",
"Assert-DeployWriteAllowed",
"requires -Upgrade",
"Get-SourceSpecificLiterals",
"Assert-NoSourceLiteral",
"Assert-SelfTestMissing",
"Assert-SelfTestContent",
"Assert-SelfTestContains",
"Assert-SelfTestTextContains",
"Assert-SelfTestLineCount",
"Assert-SelfTestBlockedDeployPath",
"CreateTarget",
"SelfTest",
"Validation Summary",
"Deployment closeout summary",
"What Was Intentionally Not Touched",
"Target Owner Next Actions",
"Target files planned for create/update",
"Existing target files already current",
"Refusing to write into the provider/source repo",
"Initialize-TargetLocalEnvironment",
"Get-SafeEnvironmentName",
"Target-local Codex environment bootstrap"
)
foreach ($marker in $requiredMarkers) {
if (-not $content.Contains($marker)) {
Add-Failure ("Deployment script is missing safety marker: {0}" -f $marker)
}
}
if ($content -match '\$projectId\s*=\s*["''][^"'']+["'']') {
Add-Failure "Deployment self-test temp namespace must be derived from the source root, not hard-coded."
}
if (-not $content.Contains("Get-SafeStatusProjectId")) {
Add-Failure "Deployment script is missing source-neutral self-test status namespace derivation."
}
$selfTestScriptMarkers = @(
"root-docs",
"template-provider",
"dot-agents-docs",
"forced-dot-layout",
"dry-run",
"protected-existing",
"git-backed-foreign-project",
"partial-gitignore",
"missing-target",
"target-owned-state",
"mixed-route",
"routed-historical",
"ambiguous-layout"
)
foreach ($marker in $selfTestScriptMarkers) {
if (-not $content.Contains(('"{0}"' -f $marker))) {
Add-Failure ("Deployment self-test scenario is missing from script: {0}" -f $marker)
}
}
$forbiddenPatterns = @(
"#requires",
"RunAsAdministrator",
"Start-Process",
"Verb RunAs",
"sudo ",
"chmod ",
"chown ",
"icacls",
"takeown",
"Set-Acl",
"Get-Acl",
"attrib ",
"git reset --hard",
"git checkout --"
)
foreach ($pattern in $forbiddenPatterns) {
if ($content -match [regex]::Escape($pattern)) {
Add-Failure ("Deployment script contains forbidden local repair or destructive pattern: {0}" -f $pattern)
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Deployment script safety checks passed."
}
}
function Test-DeploymentSelfTest {
$startFailureCount = $Failures.Count
$previousErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
$output = & (Get-RepoPath "scripts/deploy-agents-workflow.ps1") -SelfTest -Quiet 2>&1
$exitCode = $LASTEXITCODE
}
finally {
$ErrorActionPreference = $previousErrorActionPreference
}
if ($exitCode -ne 0) {
Add-Failure "Deployment self-test failed."
foreach ($line in $output) {
Add-Failure ("Deployment self-test detail: {0}" -f $line)
}
}
elseif (@($output).Count -gt 0) {
Add-Failure "Deployment self-test quiet mode produced output."
foreach ($line in $output) {
Add-Failure ("Deployment self-test quiet output: {0}" -f $line)
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Deployment self-test passed."
}
}
function Test-ChangeValidationSelfTest {
$startFailureCount = $Failures.Count
$previousErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
$output = & (Get-RepoPath "scripts/validate-changes.ps1") -SelfTest -Quiet 2>&1
$exitCode = $LASTEXITCODE
}
finally {
$ErrorActionPreference = $previousErrorActionPreference
}
if ($exitCode -ne 0) {
Add-Failure "Change-aware validation self-test failed."
foreach ($line in $output) {
Add-Failure ("Change-aware validation self-test detail: {0}" -f $line)
}
}
elseif (@($output).Count -gt 0) {
Add-Failure "Change-aware validation self-test quiet mode produced output."
foreach ($line in $output) {
Add-Failure ("Change-aware validation self-test quiet output: {0}" -f $line)
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Change-aware validation self-test passed."
}
}
function Test-MultiAgentWorkflowIntegrity {
$startFailureCount = $Failures.Count
$workflowPaths = @(".agents/docs/agents/workflows.yaml")
$workflowMarkers = @(
"multi_agent_runtime:",
"batch_ack:",
"do_not_trigger_for:",
"lifecycle_checks:",
"before:",
"during:",
"after:",
"ownership:",
"hard_fail:",
"ledger_missing_or_cleared",
"report_dedupe:",
"scoring_batches:",
"convergence:",
"delegated_deployment:",
"deployment_worker",
"deployed_file_set",
"standing cleanup authorization",
"Standing closeout cleanup",
"residue-zero proof",
"state_*.sqlite",
"session_index.jsonl",
"thread_spawn_edges",
"cleanup_helper:",
"scripts/agents-cleanup.ps1",
"session-index zero",
"unread-state zero",
"not sidebar nicknames",
"delayed zero verification"
)
foreach ($path in $workflowPaths) {
$content = Get-Content -LiteralPath (Get-RepoPath $path) -Raw
foreach ($marker in $workflowMarkers) {
if (-not $content.Contains($marker)) {
Add-Failure ("Multi-agent workflow marker is missing in {0}: {1}" -f $path, $marker)
}
}
}
$schemaPaths = @(".agents/docs/agents/schemas.yaml")
$schemaMarkers = @(
"assignment:",
"ownership_matrix:",
"employee_final_report:",
"agent_status_snapshot:",
"agent_event:",
"agent_ledger_event:",
"canonical_fields:",
"controller_lease:",
"runtime_multi_agent_validation:",
"Ownership matrix status",
"never sidebar nicknames",
"history_cleanup_evidence:",
"cleanup_helper",
"runtime_ids_resolved_from",
"script_result",
"scripts/agents-cleanup.ps1"
)
foreach ($path in $schemaPaths) {
$content = Get-Content -LiteralPath (Get-RepoPath $path) -Raw
foreach ($marker in $schemaMarkers) {
if (-not $content.Contains($marker)) {
Add-Failure ("Multi-agent schema marker is missing in {0}: {1}" -f $path, $marker)
}
}
}
$runbookPaths = @(".agents/docs/runbooks/multi-agent-workflow.md")
$runbookMarkers = @(
"thread_spawn_edges",
"session_index.jsonl",
".codex-global-state",
"delayed zero",
"runtime ids, never sidebar",
"scripts/agents-cleanup.ps1",
"clean roster"
)
foreach ($path in $runbookPaths) {
$content = Get-Content -LiteralPath (Get-RepoPath $path) -Raw
foreach ($marker in $runbookMarkers) {
if (-not $content.Contains($marker)) {
Add-Failure ("Multi-agent runbook marker is missing in {0}: {1}" -f $path, $marker)
}
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Multi-agent workflow integrity checks passed."
}
}
function Test-AgentCleanupHelperIntegrity {
$startFailureCount = $Failures.Count
$path = Get-RepoPath "scripts/agents-cleanup.ps1"
if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
Add-Failure "Agent cleanup helper is missing: scripts/agents-cleanup.ps1"
return
}
$content = Get-Content -LiteralPath $path -Raw
$markers = @(
"RuntimeIds",
"ParentThreadId",
"RepoCwd",
"CodexHome",
"session_index.jsonl",
".codex-global-state",
"archived_sessions",
"thread_spawn_edges",
"thread_dynamic_tools",
"never sidebar nicknames",
"no-backup",
"DelaySeconds",
"subagent",
"-Force",
"Cleanup is destructive"
)
foreach ($marker in $markers) {
if (-not $content.Contains($marker)) {
Add-Failure ("Agent cleanup helper marker is missing: {0}" -f $marker)
}
}
try {
$output = & $path -Action Verify -Quiet 2>&1
if (@($output).Count -gt 0) {
Add-Failure "Agent cleanup helper quiet Verify produced output."
foreach ($line in @($output)) {
Add-Failure ("Agent cleanup helper quiet output: {0}" -f $line)
}
}
}
catch {
Add-Failure ("Agent cleanup helper Verify failed: {0}" -f $_.Exception.Message)
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Agent cleanup helper integrity checks passed."
}
}
function Test-CIWorkflowStability {
$startFailureCount = $Failures.Count
$path = ".github/workflows/checkpoint.yml"
$content = Get-Content -LiteralPath (Get-RepoPath $path) -Raw
if ($content -match "runs-on:\s*windows-latest") {
Add-Failure "Checkpoint workflow must pin a Windows runner instead of using windows-latest."
}
if ($content -notmatch "runs-on:\s*windows-2025-vs2026") {
Add-Failure "Checkpoint workflow is missing the pinned windows-2025-vs2026 runner."
}
if ($content -match "validate\.ps1\s+-Full") {
Add-Failure "Checkpoint workflow must use the default fast validation gate, not -Full."
}
if ($content -notmatch "run:\s*\.\\scripts\\validate\.ps1\s*(\r?\n|$)") {
Add-Failure "Checkpoint workflow is missing the default validate.ps1 command."
}
if ($content -match "git\s+diff\s+--check") {
Add-Failure "Checkpoint workflow must not duplicate git diff --check; full audits run that gate in scripts/validate.ps1 -Full."
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "CI workflow stability checks passed."
}
}
function Test-SkillMetadata {
$startFailureCount = $Failures.Count
$skillFiles = @(
".agents/skills/project-isolation-workflow/SKILL.md"
)
foreach ($path in $skillFiles) {
$fullPath = Get-RepoPath $path
$head = Get-Content -LiteralPath $fullPath -TotalCount 10
if ($head[0] -ne "---") {
Add-Failure ("Project skill is missing YAML front matter: {0}" -f $path)
}
if (-not ($head | Where-Object { $_ -match "^name:\s+.+" })) {
Add-Failure ("Project skill metadata is missing name: {0}" -f $path)
}
if (-not ($head | Where-Object { $_ -match "^description:\s+.+" })) {
Add-Failure ("Project skill metadata is missing description: {0}" -f $path)
}
}
foreach ($path in @(".agents/skills/project-isolation-workflow/agents/openai.yaml")) {
$content = Get-Content -LiteralPath (Get-RepoPath $path)
if (-not ($content | Where-Object { $_ -match "^\s*default_prompt:" })) {
Add-Failure ("Agent skill metadata is missing default_prompt: {0}" -f $path)
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Skill metadata checks passed."
}
}
function Test-CoreRuntimeSystemIntegrity {
$startFailureCount = $Failures.Count
$expectedWorkflowVersion = Get-CanonicalWorkflowVersion
$canonicalFiles = @(
".agents/docs/agents/core-system.yaml",
".agents/docs/agents/runtime-execution.yaml",
".agents/docs/agents/provider-adapters.yaml",
".agents/docs/agents/route-packs.yaml",
".agents/docs/agents/knowledge-footprint.yaml",
".agents/docs/agents/context-intelligence.yaml",
".agents/docs/agents/openai-foundations.yaml"
)
foreach ($path in $canonicalFiles) {
if (-not (Test-Path -LiteralPath (Get-RepoPath $path) -PathType Leaf)) {
Add-Failure ("Core runtime canonical file is missing: {0}" -f $path)
}
}
$markerChecks = @(
@(".agents/docs/agents/ai-runtime.yaml", @("core_system", "runtime_execution", "provider_adapter", "route_pack", "knowledge_footprint", "foundation_creation", "context_intelligence")),
@(".agents/docs/agents/workflows.yaml", @("core_system_runtime", "runtime_execution_runtime", "provider_adapter_runtime", "route_pack_runtime", "knowledge_footprint_runtime", "context_intelligence_runtime")),
@(".agents/docs/agents/deploy.yaml", @(".agents/docs/agents/core-system.yaml", ".agents/docs/agents/runtime-execution.yaml", ".agents/docs/agents/provider-adapters.yaml", ".agents/docs/agents/route-packs.yaml", ".agents/docs/agents/knowledge-footprint.yaml", ".agents/docs/agents/openai-foundations.yaml", ".agents/docs/agents/context-intelligence.yaml", ".agents/runtime/executions/", ".agents/runtime/tool-evidence/", ".agents/runtime/deployments/", ".agents/runtime/route-packs/", ".agents/runtime/knowledge/", ".agents/runtime/context-intelligence/")),
@(".agents/docs/agents/verify.yaml", @("core_system", "runtime_execution", "provider_adapter", "route_pack", "knowledge_footprint", "foundation_creation", "context_intelligence", "core_system_integrity", "runtime_execution_integrity", "provider_adapter_integrity", "route_pack_integrity", "knowledge_footprint_integrity", "foundation_creation_integrity", "context_intelligence_integrity", "route_pack_export", "runtime_helper", "context_intelligence_helper")),
@(".agents/docs/agents/route-packs.yaml", @("answer_only", "no_read_default", "no_file_read", "manifest_hash")),
@(".agents/docs/agents/version.yaml", @($expectedWorkflowVersion, "context-intelligence", "core_contract_rule", "runtime_execution_rule", "knowledge_footprint_rule", "foundation_creation_rule", "context_intelligence_rule")),
@(".agents/docs/agents/schemas.yaml", @("core_system", "runtime_execution", "provider_adapter", "route_pack", "knowledge_footprint", "foundation_creation", "context_intelligence", "context_evidence")),
@(".agents/docs/agents/collaborators.yaml", @("thread_operation_record", "execution_run_ref")),
@(".agents/docs/agents/context-compact.yaml", @("retained_facts", "dropped_details", "resume_pointer")),
@(".agents/docs/agents/dispatch.yaml", @("execution_run_ref")),
@(".agents/docs/agents/workflow-artifacts.yaml", @("runtime_execution"))
)
foreach ($check in $markerChecks) {
$path = [string] $check[0]
$contentPath = Get-RepoPath $path
if (-not (Test-Path -LiteralPath $contentPath -PathType Leaf)) {
Add-Failure ("Core runtime marker file is missing: {0}" -f $path)
continue
}
$content = Get-Content -LiteralPath $contentPath -Raw
foreach ($marker in @($check[1])) {
if (-not $content.Contains([string] $marker)) {
Add-Failure ("Core runtime marker missing from {0}: {1}" -f $path, $marker)
}
}
}
Test-RoutePackDeterminism
Test-RuntimeExecutionSmoke
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Core runtime system integrity checks passed."
}
}
function Test-FullAuditGates {
Test-GitDiffCheck
Test-LineEndings
Test-CanonicalSourceUniqueness
Test-DeployManifestIntegrity
Test-TemplateCoverage
Test-TemplateSourceNeutrality
Test-SkillMetadata
Test-DeploymentScriptSafety
Test-DeploymentSelfTest
Test-ChangeValidationSelfTest
Test-MultiAgentWorkflowIntegrity
Test-AgentCleanupHelperIntegrity
Test-WorkflowArtifactIntegrity
Test-ContextCompactIntegrity
Test-ContextIntelligenceIntegrity -RunPractice
Test-CollaboratorWindowIntegrity
Test-CoreRuntimeSystemIntegrity
Test-CrossProjectRuntimeResilienceIntegrity
Test-LegacyResidue
Test-FoundationCreationIntegrity
Test-EvidenceTemplateSchemaCoverage
Test-CIWorkflowStability
Test-ReadinessLadderEvidence
Test-SizeGates
Test-ReleasePackageExport
Test-RuntimeReleaseEvidence
}
Push-Location $RepoRoot
try {
Write-Check "INFO" ("Repo root: {0}" -f $RepoRoot)
$yamlFiles = Get-ChildItem -LiteralPath (Get-RepoPath ".agents/docs/agents/") -Filter "*.yaml" -File
foreach ($file in $yamlFiles) {
Test-LightweightYaml -File $file
}
if ($Failures.Count -eq 0) {
Add-Pass "Policy YAML files passed the lightweight syntax gate."
}
else {
Add-Warning "YAML gate found failures; later checks will still run."
}
$workflowFiles = @(Get-ChildItem -LiteralPath (Get-RepoPath ".github/workflows") -File -ErrorAction SilentlyContinue |
Where-Object { $_.Extension.ToLowerInvariant() -in @(".yaml", ".yml") })
foreach ($file in $workflowFiles) {
Test-LightweightYaml -File $file
}
if ($Failures.Count -eq 0) {
Add-Pass "Workflow YAML files passed the lightweight syntax gate."
}
Test-RequiredFiles
if ($Failures.Count -eq 0) {
Add-Pass "Required canonical files exist."
}
Test-AiRuntimeCompactness
Test-SchemaContracts
if ($Failures.Count -eq 0) {
Add-Pass "Canonical YAML files match initial schema contracts."
}
Test-EnterpriseDispatchIntegrity
Test-WorkflowArtifactIntegrity
Test-ContextCompactIntegrity
Test-ContextIntelligenceIntegrity
Test-CollaboratorWindowIntegrity
Test-CoreRuntimeSystemIntegrity
Test-FoundationCreationIntegrity
Test-KnowledgeMemoryIntegrity
Test-AgentCleanupHelperIntegrity
Test-CrossProjectRuntimeResilienceIntegrity
Test-LegacyResidue
Test-ValidationFixtures
if ($Failures.Count -eq 0) {
Add-Pass "Validation fixtures passed."
}
$startVersionFailureCount = $Failures.Count
Test-PublicReadmeVersionAlignment
if ($Failures.Count -eq $startVersionFailureCount) {
Add-Pass "Public README workflow version matches canonical version metadata."
}
$scanRoots = @(
"AGENTS.md",
".agents/skills",
"docs",
"schemas",
"scripts",
"tests",
"artifacts",
".github/workflows"
)
$textFiles = @(Get-TextFiles -Roots $scanRoots)
Test-PatternScan -Name "Placeholder" -Pattern "TO[D]O|\[TO[D]O\]|T[B]D|FI[X]ME|turn[0-9]+|filecite" -Files $textFiles
if ($Failures.Count -eq 0) {
Add-Pass "No placeholder markers found in durable text files."
}
Test-PatternScan -Name "English-only" -Pattern "\p{IsCJKUnifiedIdeographs}" -Files $textFiles
if ($Failures.Count -eq 0) {
Add-Pass "No CJK characters found in durable text files."
}
Test-RuntimeBoundaries
if ($Failures.Count -eq 0) {
Add-Pass "Runtime/local boundary checks passed."
}
if ($Full) {
Test-FullAuditGates
if ($Failures.Count -eq 0) {
Add-Pass "Full release audit gates passed."
}
}
}
finally {
Pop-Location
}
if ($Warnings.Count -gt 0 -and -not $Quiet) {
Write-Host ""
Write-Host ("Warnings: {0}" -f $Warnings.Count)
}
if ($Score -and -not $Quiet) {
Write-AgentQualityScore
}
if ($Failures.Count -gt 0) {
Write-Host ""
Write-Host ("Validation failed with {0} issue(s)." -f $Failures.Count)
exit 1
}
Write-Host ""
Write-Host "Validation passed."
