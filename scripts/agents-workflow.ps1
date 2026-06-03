param(
[ValidateSet("New", "Verify", "Collect", "SimulateDispatch", "NormalizeReport")]
[string] $Action = "Verify",
[string] $WorkflowId = "workflow-smoke",
[string] $Root,
[ValidateRange(0, 6)]
[int] $Level = 0,
[switch] $Quiet
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$LiveRoot = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot ".agents/runtime/workflows"))
$AliasRoot = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot ".workflow"))
$AllowedStatuses = @("drafted", "approved", "active", "waiting_approval", "collecting", "completed", "blocked", "stopped")
$PacketTypes = @("department_leader_assignment", "worker_packet", "verification_packet", "escalation_packet")
$GateActions = @("write", "deploy", "external_read", "external_write", "destructive_action", "model_tier_upgrade")
function Write-WorkflowResult {
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
function Assert-WorkflowId {
if ($WorkflowId -notmatch '^[a-z0-9._-]+$') {
throw "Invalid workflow_id. Use lowercase letters, numbers, dot, underscore, or dash only."
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
param(
[string] $Path,
[string] $RootPath
)
$normalizedPath = [System.IO.Path]::GetFullPath($Path).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
$normalizedRoot = [System.IO.Path]::GetFullPath($RootPath).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
return ($normalizedPath -eq $normalizedRoot) -or $normalizedPath.StartsWith($normalizedRoot + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)
}
function Assert-WorkflowPathAllowed {
param([string] $Path)
$fullPath = Get-FullPath $Path
foreach ($blocked in @(".git", ".codex")) {
$blockedPath = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $blocked))
if (Test-InsideRoot -Path $fullPath -RootPath $blockedPath) {
throw "Workflow artifact path is blocked: $blocked"
}
}
if ((Test-InsideRoot -Path $fullPath -RootPath $LiveRoot) -or (Test-InsideRoot -Path $fullPath -RootPath $AliasRoot)) {
return $fullPath
}
throw "Workflow artifact path must stay under .agents/runtime/workflows/ or .workflow/."
}
function Get-WorkflowRoot {
Assert-WorkflowId
if (-not [string]::IsNullOrWhiteSpace($Root)) {
return Assert-WorkflowPathAllowed -Path $Root
}
return Assert-WorkflowPathAllowed -Path (Join-Path $LiveRoot $WorkflowId)
}
function Read-JsonObject {
param([string] $Path)
try {
return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}
catch {
throw "Invalid JSON: $Path"
}
}
function Get-PropertyValue {
param(
[object] $Object,
[string] $Name
)
$property = $Object.PSObject.Properties[$Name]
if ($null -eq $property) {
return $null
}
return $property.Value
}
function Get-ArrayValue {
param([object] $Value)
if ($null -eq $Value) {
return ,@()
}
if ($Value -is [System.Array]) {
return ,@($Value)
}
return ,@($Value)
}
function Test-ApprovalCoverage {
param([object] $State)
$issues = New-Object System.Collections.Generic.List[string]
$actions = Get-ArrayValue (Get-PropertyValue -Object $State -Name "actions_requested")
$approvals = Get-ArrayValue (Get-PropertyValue -Object $State -Name "approval_gates")
$escalations = Get-ArrayValue (Get-PropertyValue -Object $State -Name "escalations")
foreach ($action in $actions) {
$actionName = [string] $action
if ($GateActions -contains $actionName) {
if ($approvals.Count -eq 0 -and $escalations.Count -eq 0) {
$issues.Add("Gated action lacks approval_gate or escalation_record: $actionName") | Out-Null
}
}
}
return @($issues)
}
function Invoke-NewWorkflow {
$workflowRoot = Get-WorkflowRoot
New-Item -ItemType Directory -Force -Path $workflowRoot | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $workflowRoot "packets") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $workflowRoot "results") | Out-Null
$state = [ordered]@{
workflow_id = $WorkflowId
version = Get-WorkflowVersion
objective = "supervised workflow artifact"
owner = "controller"
status = "drafted"
created_at = (Get-Date).ToUniversalTime().ToString("o")
artifact_root = $workflowRoot
approval_policy = "approval_gate or escalation_record required for write, deploy, external access, destructive action, or model_tier_upgrade"
actions_requested = @()
approval_gates = @()
escalations = @()
isolation = "GM not used | GS not used | XR none | XW none"
}
$state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $workflowRoot "state.json") -Encoding UTF8
"# Workflow Plan`n`n- Owner: controller`n- State: drafted`n" | Set-Content -LiteralPath (Join-Path $workflowRoot "plan.md") -Encoding UTF8
"# Orchestration`n`nController assigns department leaders. Leaders merge worker packets before collection.`n" | Set-Content -LiteralPath (Join-Path $workflowRoot "orchestration.md") -Encoding UTF8
Write-WorkflowResult ([ordered]@{
ok = $true
action = "New"
workflow_id = $WorkflowId
root = $workflowRoot
})
}
function Invoke-VerifyWorkflow {
$workflowRoot = Get-WorkflowRoot
$issues = New-Object System.Collections.Generic.List[string]
foreach ($name in @("state.json", "plan.md", "orchestration.md")) {
if (-not (Test-Path -LiteralPath (Join-Path $workflowRoot $name) -PathType Leaf)) {
$issues.Add("Missing required workflow file: $name") | Out-Null
}
}
$packetCount = 0
$state = $null
$statePath = Join-Path $workflowRoot "state.json"
if (Test-Path -LiteralPath $statePath -PathType Leaf) {
$state = Read-JsonObject -Path $statePath
foreach ($field in @("workflow_id", "version", "objective", "owner", "status", "created_at", "artifact_root", "approval_policy", "isolation")) {
if ([string]::IsNullOrWhiteSpace([string] (Get-PropertyValue -Object $state -Name $field))) {
$issues.Add("state.json missing required field: $field") | Out-Null
}
}
$status = [string] (Get-PropertyValue -Object $state -Name "status")
if ($AllowedStatuses -notcontains $status) {
$issues.Add("state.json has invalid status: $status") | Out-Null
}
foreach ($issue in Test-ApprovalCoverage -State $state) {
$issues.Add($issue) | Out-Null
}
}
$packetRoot = Join-Path $workflowRoot "packets"
if (Test-Path -LiteralPath $packetRoot -PathType Container) {
foreach ($packetFile in Get-ChildItem -LiteralPath $packetRoot -Filter "*.json" -File) {
$packetCount++
$packet = Read-JsonObject -Path $packetFile.FullName
foreach ($field in @("packet_id", "workflow_id", "type", "owner", "target", "objective", "scope", "authority", "model_tier", "expected_output")) {
if ([string]::IsNullOrWhiteSpace([string] (Get-PropertyValue -Object $packet -Name $field))) {
$issues.Add("$($packetFile.Name) missing packet field: $field") | Out-Null
}
}
$type = [string] (Get-PropertyValue -Object $packet -Name "type")
if ($PacketTypes -notcontains $type) {
$issues.Add("$($packetFile.Name) has invalid packet type: $type") | Out-Null
}
$owner = [string] (Get-PropertyValue -Object $packet -Name "owner")
$target = [string] (Get-PropertyValue -Object $packet -Name "target")
if ($owner -eq "controller" -and $target -match "_worker$" -and $type -ne "escalation_packet") {
$issues.Add("$($packetFile.Name) violates controller-to-worker routing without escalation_packet") | Out-Null
}
}
}
$ok = $issues.Count -eq 0
Write-WorkflowResult ([ordered]@{
ok = $ok
action = "Verify"
workflow_id = $WorkflowId
root = $workflowRoot
packets = $packetCount
issues = @($issues)
})
if (-not $ok) {
exit 1
}
}
function Get-NormalizedReport {
param([object] $InputObject)
return [ordered]@{
workflow_id = [string] (Get-PropertyValue -Object $InputObject -Name "workflow_id")
owner = [string] (Get-PropertyValue -Object $InputObject -Name "owner")
status = [string] (Get-PropertyValue -Object $InputObject -Name "status")
summary = [string] (Get-PropertyValue -Object $InputObject -Name "summary")
verification = Get-ArrayValue (Get-PropertyValue -Object $InputObject -Name "verification")
risks = Get-ArrayValue (Get-PropertyValue -Object $InputObject -Name "risks")
escalations = Get-ArrayValue (Get-PropertyValue -Object $InputObject -Name "escalations")
isolation = [string] (Get-PropertyValue -Object $InputObject -Name "isolation")
}
}
function Invoke-CollectWorkflow {
$workflowRoot = Get-WorkflowRoot
$verification = & $PSCommandPath -Action Verify -WorkflowId $WorkflowId -Root $workflowRoot -Quiet 2>&1
if ($LASTEXITCODE -ne 0) {
foreach ($line in $verification) {
if (-not $Quiet) {
Write-Host $line
}
}
exit 1
}
$results = @()
$resultRoot = Join-Path $workflowRoot "results"
if (Test-Path -LiteralPath $resultRoot -PathType Container) {
foreach ($file in Get-ChildItem -LiteralPath $resultRoot -Filter "*.json" -File) {
$result = Read-JsonObject -Path $file.FullName
if ([string] (Get-PropertyValue -Object $result -Name "type") -eq "raw_worker_chatter") {
continue
}
$results += Get-NormalizedReport -InputObject $result
}
}
$escalations = @()
foreach ($report in $results) {
foreach ($escalation in @($report.escalations)) {
$escalations += $escalation
}
}
$collection = [ordered]@{
workflow_id = $WorkflowId
status = "collecting"
packets = @(Get-ChildItem -LiteralPath (Join-Path $workflowRoot "packets") -Filter "*.json" -File -ErrorAction SilentlyContinue).Count
results = $results.Count
department_reports = @($results | Where-Object { $_.owner -match "_lead$" }).Count
escalations = $escalations.Count
missing = 0
invalid = 0
blocked = @($results | Where-Object { $_.status -eq "blocked" }).Count
final_report_path = (Join-Path $workflowRoot "final-report.md")
}
$collection | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $workflowRoot "collection-report.json") -Encoding UTF8
$lines = @(
"# Workflow Final Report",
"",
"- Workflow: $WorkflowId",
"- Results: $($collection.results)",
"- Department reports: $($collection.department_reports)",
"- Escalations: $($collection.escalations)",
"- Raw worker chatter: excluded"
)
$lines | Set-Content -LiteralPath $collection.final_report_path -Encoding UTF8
Write-WorkflowResult $collection
}
function Invoke-SimulateDispatch {
$levels = @(
[ordered]@{ level = "level_1"; kind = "pass"; outcome = "department_report"; pass = $true },
[ordered]@{ level = "level_2"; kind = "pass"; outcome = "merged department_report"; pass = $true },
[ordered]@{ level = "level_3"; kind = "pass"; outcome = "controller integrates leader reports only"; pass = $true },
[ordered]@{ level = "level_4"; kind = "guardrail"; outcome = "blocked direct worker routing with escalation_record"; pass = $true },
[ordered]@{ level = "level_5"; kind = "reserved"; outcome = "not executed without explicit definition"; pass = $true },
[ordered]@{ level = "level_6"; kind = "guardrail"; outcome = "blocked insufficient model tier or authority; approval_gate or escalation_record required"; pass = $true }
)
$selectedLevels = if ($Level -gt 0) {
@($levels | Where-Object { $_.level -eq ("level_{0}" -f $Level) })
}
else {
@($levels)
}
Write-WorkflowResult ([ordered]@{
ok = $true
action = "SimulateDispatch"
workflow_id = $WorkflowId
levels = $selectedLevels
requested = @($selectedLevels).Count
spawned = 0
completed = 0
closed = 0
missing = 0
duplicate = 0
invalid_format = 0
failed = 0
risk = 0
})
}
function Invoke-NormalizeReport {
$workflowRoot = Get-WorkflowRoot
$statePath = Join-Path $workflowRoot "state.json"
if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) {
throw "Cannot normalize report without state.json."
}
$state = Read-JsonObject -Path $statePath
Write-WorkflowResult (Get-NormalizedReport -InputObject ([pscustomobject]@{
workflow_id = [string] (Get-PropertyValue -Object $state -Name "workflow_id")
owner = [string] (Get-PropertyValue -Object $state -Name "owner")
status = [string] (Get-PropertyValue -Object $state -Name "status")
summary = [string] (Get-PropertyValue -Object $state -Name "objective")
verification = @()
risks = @()
escalations = Get-ArrayValue (Get-PropertyValue -Object $state -Name "escalations")
isolation = [string] (Get-PropertyValue -Object $state -Name "isolation")
}))
}
switch ($Action) {
"New" { Invoke-NewWorkflow }
"Verify" { Invoke-VerifyWorkflow }
"Collect" { Invoke-CollectWorkflow }
"SimulateDispatch" { Invoke-SimulateDispatch }
"NormalizeReport" { Invoke-NormalizeReport }
}
$global:LASTEXITCODE = 0
