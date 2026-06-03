param(
[ValidateSet("NewRun", "AddStep", "AddApproval", "AddResult", "AddEscalation", "Collect", "Verify", "Cleanup")]
[string] $Action = "Verify",
[string] $RunId = "runtime-smoke",
[string] $Objective = "runtime execution smoke",
[string] $Authority = "read_only",
[string] $Step = "read_only",
[string] $Result = "completed",
[string] $Summary = "",
[string] $RuntimeRoot,
[switch] $Quiet
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$DefaultRuntimeRoot = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot ".agents/runtime/executions"))
$AllowedStatuses = @("planned", "approved", "running", "waiting_approval", "completed", "failed", "blocked", "cleaned")
$GatedActions = @("write", "deploy", "external_access", "external_read", "external_write", "destructive_action", "model_tier_upgrade")
function Write-RuntimeResult {
param([object] $Value)
if (-not $Quiet) {
$Value | ConvertTo-Json -Depth 12
}
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
function Assert-RunId {
if ($RunId -notmatch '^[a-z0-9._-]+$') {
throw "Invalid run_id. Use lowercase letters, numbers, dot, underscore, or dash only."
}
}
function Get-FullPath {
param([string] $Path)
if ([System.IO.Path]::IsPathRooted($Path)) {
return [System.IO.Path]::GetFullPath($Path)
}
return [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $Path))
}
function Test-InsideRoot {
param([string] $Path, [string] $RootPath)
$normalizedPath = [System.IO.Path]::GetFullPath($Path).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
$normalizedRoot = [System.IO.Path]::GetFullPath($RootPath).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
return ($normalizedPath -eq $normalizedRoot) -or $normalizedPath.StartsWith($normalizedRoot + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)
}
function Get-ProjectId {
$leaf = Split-Path -Leaf $RepoRoot
$id = ($leaf.ToLowerInvariant() -replace "[^a-z0-9]+", "-").Trim("-")
if ([string]::IsNullOrWhiteSpace($id)) {
return "agents"
}
return $id
}
function Get-TempStatusRoot {
return [System.IO.Path]::GetFullPath((Join-Path ([System.IO.Path]::GetTempPath()) (Join-Path "codex-agent-status" (Get-ProjectId))))
}
function Assert-RuntimeRootAllowed {
param([string] $Path)
$fullPath = Get-FullPath $Path
foreach ($blocked in @(".git", ".codex")) {
$blockedPath = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $blocked))
if (Test-InsideRoot -Path $fullPath -RootPath $blockedPath) {
throw "Runtime execution path is blocked: $blocked"
}
}
if ((Test-InsideRoot -Path $fullPath -RootPath $DefaultRuntimeRoot) -or (Test-InsideRoot -Path $fullPath -RootPath (Get-TempStatusRoot))) {
return $fullPath
}
throw "Runtime execution root must stay under .agents/runtime/executions/ or approved temp status space."
}
function Get-RunRoot {
Assert-RunId
$root = if ([string]::IsNullOrWhiteSpace($RuntimeRoot)) {
$DefaultRuntimeRoot
}
else {
$RuntimeRoot
}
$allowedRoot = Assert-RuntimeRootAllowed -Path $root
return [System.IO.Path]::GetFullPath((Join-Path $allowedRoot $RunId))
}
function Get-RunPath {
return Join-Path (Get-RunRoot) "run.json"
}
function Read-Run {
$path = Get-RunPath
if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
throw "Missing runtime execution run.json for run_id: $RunId"
}
try {
return Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
}
catch {
throw "Invalid runtime execution JSON: $path"
}
}
function Write-JsonNoBom {
param([string] $Path, [object] $Value)
$json = $Value | ConvertTo-Json -Depth 16
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($Path, $json, $utf8NoBom)
}
function Get-PropertyValue {
param([object] $Object, [string] $Name)
$property = $Object.PSObject.Properties[$Name]
if ($null -eq $property) {
return $null
}
return $property.Value
}
function Set-PropertyValue {
param([object] $Object, [string] $Name, [object] $Value)
$property = $Object.PSObject.Properties[$Name]
if ($null -eq $property) {
$Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value
}
else {
$property.Value = $Value
}
}
function Get-ArrayValue {
param([object] $Value)
if ($null -eq $Value) {
return @()
}
if ($Value -is [System.Array]) {
return $Value
}
return ,$Value
}
function Append-RunArray {
param([object] $Run, [string] $Name, [object] $Item)
$items = @(Get-ArrayValue (Get-PropertyValue -Object $Run -Name $Name))
$items += $Item
Set-PropertyValue -Object $Run -Name $Name -Value $items
Set-PropertyValue -Object $Run -Name "updated_at" -Value ((Get-Date).ToUniversalTime().ToString("o"))
}
function Write-Run {
param([object] $Run)
$runRoot = Get-RunRoot
if (-not (Test-Path -LiteralPath $runRoot -PathType Container)) {
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
}
Write-JsonNoBom -Path (Join-Path $runRoot "run.json") -Value $Run
}
function Get-StepCoverageIssues {
param([object] $Run)
$issues = New-Object System.Collections.Generic.List[string]
$steps = @(Get-ArrayValue (Get-PropertyValue -Object $Run -Name "steps"))
$approvals = @(Get-ArrayValue (Get-PropertyValue -Object $Run -Name "approvals"))
$escalations = @(Get-ArrayValue (Get-PropertyValue -Object $Run -Name "escalations"))
foreach ($stepRecord in $steps) {
$action = [string] (Get-PropertyValue -Object $stepRecord -Name "action")
if ($GatedActions -contains $action) {
$coveredByApproval = @($approvals | Where-Object { [string] (Get-PropertyValue -Object $_ -Name "action") -eq $action }).Count -gt 0
$coveredByEscalation = @($escalations | Where-Object { [string] (Get-PropertyValue -Object $_ -Name "action") -eq $action }).Count -gt 0
if (-not $coveredByApproval -and -not $coveredByEscalation) {
$issues.Add("Gated action lacks approval or escalation evidence: $action") | Out-Null
}
}
}
return @($issues)
}
function Invoke-NewRun {
$runRoot = Get-RunRoot
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
$now = (Get-Date).ToUniversalTime().ToString("o")
$run = [ordered]@{
schema = "agents-runtime-execution-run/v1"
run_id = $RunId
version = Get-WorkflowVersion
status = "planned"
objective = $Objective
scope = "repo-local runtime evidence"
authority = $Authority
owner = "controller"
model_tier = "code_standard"
created_at = $now
updated_at = $now
steps = @()
approvals = @()
tool_evidence = @()
results = @()
escalations = @()
cleanup_evidence = @()
isolation = "GM not used | GS not used | XR none | XW none"
}
Write-Run -Run $run
Write-RuntimeResult ([ordered]@{ ok = $true; action = "NewRun"; run_id = $RunId; root = $runRoot })
}
function Invoke-AddStep {
$run = Read-Run
$status = if ($GatedActions -contains $Step) { "waiting_approval" } else { "running" }
Append-RunArray -Run $run -Name "steps" -Item ([ordered]@{
step_id = "step-{0:0000}" -f (@(Get-ArrayValue (Get-PropertyValue -Object $run -Name "steps")).Count + 1)
action = $Step
authority = $Authority
status = $status
created_at = (Get-Date).ToUniversalTime().ToString("o")
evidence = "tool evidence required for non-read-only actions"
})
Set-PropertyValue -Object $run -Name "status" -Value $status
Write-Run -Run $run
Write-RuntimeResult ([ordered]@{ ok = $true; action = "AddStep"; run_id = $RunId; step = $Step; status = $status })
}
function Invoke-AddApproval {
$run = Read-Run
Append-RunArray -Run $run -Name "approvals" -Item ([ordered]@{
approval_id = "approval-{0:0000}" -f (@(Get-ArrayValue (Get-PropertyValue -Object $run -Name "approvals")).Count + 1)
action = $Step
authority = $Authority
approved_by = "controller"
approved_at = (Get-Date).ToUniversalTime().ToString("o")
})
Set-PropertyValue -Object $run -Name "status" -Value "approved"
Write-Run -Run $run
Write-RuntimeResult ([ordered]@{ ok = $true; action = "AddApproval"; run_id = $RunId; approved_action = $Step })
}
function Invoke-AddResult {
$run = Read-Run
Append-RunArray -Run $run -Name "results" -Item ([ordered]@{
result_id = "result-{0:0000}" -f (@(Get-ArrayValue (Get-PropertyValue -Object $run -Name "results")).Count + 1)
status = $Result
summary = $Summary
created_at = (Get-Date).ToUniversalTime().ToString("o")
})
Set-PropertyValue -Object $run -Name "status" -Value $Result
Write-Run -Run $run
Write-RuntimeResult ([ordered]@{ ok = $true; action = "AddResult"; run_id = $RunId; status = $Result })
}
function Invoke-AddEscalation {
$run = Read-Run
Append-RunArray -Run $run -Name "escalations" -Item ([ordered]@{
escalation_id = "escalation-{0:0000}" -f (@(Get-ArrayValue (Get-PropertyValue -Object $run -Name "escalations")).Count + 1)
action = $Step
reason = if ([string]::IsNullOrWhiteSpace($Summary)) { "approval or authority required" } else { $Summary }
created_at = (Get-Date).ToUniversalTime().ToString("o")
})
Set-PropertyValue -Object $run -Name "status" -Value "blocked"
Write-Run -Run $run
Write-RuntimeResult ([ordered]@{ ok = $true; action = "AddEscalation"; run_id = $RunId; escalated_action = $Step })
}
function Invoke-Collect {
$run = Read-Run
$summaryObject = [ordered]@{
schema = "agents-runtime-execution-summary/v1"
run_id = $RunId
version = [string] (Get-PropertyValue -Object $run -Name "version")
status = [string] (Get-PropertyValue -Object $run -Name "status")
steps = @(Get-ArrayValue (Get-PropertyValue -Object $run -Name "steps")).Count
approvals = @(Get-ArrayValue (Get-PropertyValue -Object $run -Name "approvals")).Count
results = @(Get-ArrayValue (Get-PropertyValue -Object $run -Name "results")).Count
escalations = @(Get-ArrayValue (Get-PropertyValue -Object $run -Name "escalations")).Count
cleanup_evidence = @(Get-ArrayValue (Get-PropertyValue -Object $run -Name "cleanup_evidence")).Count
collected_at = (Get-Date).ToUniversalTime().ToString("o")
}
Write-JsonNoBom -Path (Join-Path (Get-RunRoot) "summary.json") -Value $summaryObject
Write-RuntimeResult $summaryObject
}
function Invoke-Verify {
$run = Read-Run
$issues = New-Object System.Collections.Generic.List[string]
foreach ($field in @("schema", "run_id", "version", "status", "objective", "scope", "authority", "owner", "model_tier", "created_at", "updated_at", "isolation")) {
if ([string]::IsNullOrWhiteSpace([string] (Get-PropertyValue -Object $run -Name $field))) {
$issues.Add("run.json missing required field: $field") | Out-Null
}
}
$status = [string] (Get-PropertyValue -Object $run -Name "status")
if ($AllowedStatuses -notcontains $status) {
$issues.Add("Invalid run status: $status") | Out-Null
}
foreach ($issue in Get-StepCoverageIssues -Run $run) {
$issues.Add($issue) | Out-Null
}
if ($status -eq "cleaned") {
$cleanup = @(Get-ArrayValue (Get-PropertyValue -Object $run -Name "cleanup_evidence"))
if ($cleanup.Count -eq 0) {
$issues.Add("Cleaned status requires cleanup_evidence.") | Out-Null
}
elseif (@($cleanup | Where-Object { [bool] (Get-PropertyValue -Object $_ -Name "verified") }).Count -eq 0) {
$issues.Add("Cleanup evidence must be verified before claiming cleaned.") | Out-Null
}
}
$ok = $issues.Count -eq 0
Write-RuntimeResult ([ordered]@{ ok = $ok; action = "Verify"; run_id = $RunId; issues = @($issues) })
if (-not $ok) {
exit 1
}
}
function Invoke-Cleanup {
$run = Read-Run
Append-RunArray -Run $run -Name "cleanup_evidence" -Item ([ordered]@{
cleanup_id = "cleanup-{0:0000}" -f (@(Get-ArrayValue (Get-PropertyValue -Object $run -Name "cleanup_evidence")).Count + 1)
operation = "runtime closeout evidence recorded"
verified = $true
created_at = (Get-Date).ToUniversalTime().ToString("o")
claim = "closed or archived only when runtime evidence supports it; hard delete is not claimed"
})
Set-PropertyValue -Object $run -Name "status" -Value "cleaned"
Write-Run -Run $run
Write-RuntimeResult ([ordered]@{ ok = $true; action = "Cleanup"; run_id = $RunId; status = "cleaned" })
}
switch ($Action) {
"NewRun" { Invoke-NewRun }
"AddStep" { Invoke-AddStep }
"AddApproval" { Invoke-AddApproval }
"AddResult" { Invoke-AddResult }
"AddEscalation" { Invoke-AddEscalation }
"Collect" { Invoke-Collect }
"Verify" { Invoke-Verify }
"Cleanup" { Invoke-Cleanup }
}
$global:LASTEXITCODE = 0
