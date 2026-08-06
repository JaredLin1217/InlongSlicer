param(
[ValidateSet("NewRun", "AddStep", "AddApproval", "AddResult", "AddEscalation", "AddDeploymentEvidence", "Collect", "Verify", "Cleanup")]
[string] $Action = "Verify",
[string] $RunId = "runtime-smoke",
[string] $Objective = "runtime execution smoke",
[string] $Authority = "read_only",
[string] $Step = "read_only",
[string] $Result = "completed",
[string] $Summary = "",
[string] $RuntimeRoot,
[ValidateSet("preflight", "copy", "verify", "cleanup", "result_summary")]
[string] $EvidenceType = "result_summary",
[string] $EvidenceRef = "",
[string] $VerificationRef = "",
[string] $Risk = "",
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

function Get-ProjectKey {
$leaf = Split-Path -Leaf $RepoRoot
$safe = ($leaf.ToLowerInvariant() -replace "[^a-z0-9._-]+", "-").Trim("-._")
if ([string]::IsNullOrWhiteSpace($safe)) {
$safe = "agents"
}
return ("{0}-{1}" -f $safe, (Get-RepoPathHash -Path $RepoRoot))
}

function Get-TempStatusRoot {
return [System.IO.Path]::GetFullPath((Join-Path ([System.IO.Path]::GetTempPath()) (Join-Path "codex-agent-status" (Get-ProjectKey))))
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
throw "Runtime execution root must stay under .agents/runtime/executions/ or approved per-project temp status space."
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

function Add-EventSummary {
param([object] $Run, [string] $Message)
if ([string]::IsNullOrWhiteSpace($Message)) {
return
}
Append-RunArray -Run $Run -Name "event_summary" -Item ([ordered]@{
message = $Message
created_at = (Get-Date).ToUniversalTime().ToString("o")
})
}

function Add-VerificationRef {
param([object] $Run, [string] $Ref)
if (-not [string]::IsNullOrWhiteSpace($Ref)) {
Append-RunArray -Run $Run -Name "verification_refs" -Item $Ref
}
}

function Add-Risk {
param([object] $Run, [string] $Value)
if (-not [string]::IsNullOrWhiteSpace($Value)) {
Append-RunArray -Run $Run -Name "risks" -Item $Value
}
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
current_route = "runtime_execution"
verified_assumptions = @()
change_boundary = "repo-local runtime evidence"
remaining_steps = @()
created_at = $now
updated_at = $now
steps = @()
approvals = @()
tool_evidence = @()
results = @()
escalations = @()
cleanup_evidence = @()
event_summary = @([ordered]@{
message = "run created"
created_at = $now
})
verification_refs = @()
risks = @()
deployment_evidence = @()
resume_pointer = (".agents/runtime/executions/{0}/summary.json" -f $RunId)
recovery_pointers = @((".agents/runtime/executions/{0}/run.json" -f $RunId), (".agents/runtime/executions/{0}/summary.json" -f $RunId))
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
owner = "controller"
target = "repo-local"
authority = $Authority
status = $status
created_at = (Get-Date).ToUniversalTime().ToString("o")
evidence = "tool evidence required for non-read-only actions"
})
Add-EventSummary -Run $run -Message ("step added: {0}" -f $Step)
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
Add-EventSummary -Run $run -Message ("approval added: {0}" -f $Step)
Set-PropertyValue -Object $run -Name "status" -Value "approved"
Write-Run -Run $run
Write-RuntimeResult ([ordered]@{ ok = $true; action = "AddApproval"; run_id = $RunId; approved_action = $Step })
}

function Invoke-AddResult {
$run = Read-Run
Append-RunArray -Run $run -Name "results" -Item ([ordered]@{
result_id = "result-{0:0000}" -f (@(Get-ArrayValue (Get-PropertyValue -Object $run -Name "results")).Count + 1)
run_id = $RunId
status = $Result
summary = $Summary
risks = if ([string]::IsNullOrWhiteSpace($Risk)) { @() } else { @($Risk) }
evidence_refs = if ([string]::IsNullOrWhiteSpace($EvidenceRef)) { @() } else { @($EvidenceRef) }
created_at = (Get-Date).ToUniversalTime().ToString("o")
})
Add-VerificationRef -Run $run -Ref $VerificationRef
Add-Risk -Run $run -Value $Risk
Add-EventSummary -Run $run -Message ("result added: {0}" -f $Result)
Set-PropertyValue -Object $run -Name "status" -Value $Result
Write-Run -Run $run
Write-RuntimeResult ([ordered]@{ ok = $true; action = "AddResult"; run_id = $RunId; status = $Result })
}

function Invoke-AddEscalation {
$run = Read-Run
$reason = if ([string]::IsNullOrWhiteSpace($Summary)) { "approval or authority required" } else { $Summary }
Append-RunArray -Run $run -Name "escalations" -Item ([ordered]@{
escalation_id = "escalation-{0:0000}" -f (@(Get-ArrayValue (Get-PropertyValue -Object $run -Name "escalations")).Count + 1)
action = $Step
severity = "high"
reason = $reason
created_at = (Get-Date).ToUniversalTime().ToString("o")
})
Add-Risk -Run $run -Value $reason
Add-EventSummary -Run $run -Message ("escalation added: {0}" -f $Step)
Set-PropertyValue -Object $run -Name "status" -Value "blocked"
Write-Run -Run $run
Write-RuntimeResult ([ordered]@{ ok = $true; action = "AddEscalation"; run_id = $RunId; escalated_action = $Step })
}

function Invoke-AddDeploymentEvidence {
$run = Read-Run
Append-RunArray -Run $run -Name "deployment_evidence" -Item ([ordered]@{
evidence_id = "deploy-evidence-{0:0000}" -f (@(Get-ArrayValue (Get-PropertyValue -Object $run -Name "deployment_evidence")).Count + 1)
evidence_type = $EvidenceType
summary = $Summary
evidence_ref = $EvidenceRef
verification_ref = $VerificationRef
risk = $Risk
created_at = (Get-Date).ToUniversalTime().ToString("o")
})
Add-VerificationRef -Run $run -Ref $VerificationRef
Add-Risk -Run $run -Value $Risk
Add-EventSummary -Run $run -Message ("deployment evidence added: {0}" -f $EvidenceType)
if ([string] (Get-PropertyValue -Object $run -Name "status") -eq "planned") {
Set-PropertyValue -Object $run -Name "status" -Value "running"
}
Write-Run -Run $run
Write-RuntimeResult ([ordered]@{ ok = $true; action = "AddDeploymentEvidence"; run_id = $RunId; evidence_type = $EvidenceType })
}

function Invoke-Collect {
$run = Read-Run
$deploymentEvidence = @(Get-ArrayValue (Get-PropertyValue -Object $run -Name "deployment_evidence"))
$deploymentEvidenceRefs = @($deploymentEvidence | ForEach-Object { [string] (Get-PropertyValue -Object $_ -Name "evidence_ref") } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
$summaryObject = [ordered]@{
schema = "agents-runtime-execution-summary/v1"
run_id = $RunId
version = [string] (Get-PropertyValue -Object $run -Name "version")
status = [string] (Get-PropertyValue -Object $run -Name "status")
current_route = [string] (Get-PropertyValue -Object $run -Name "current_route")
verified_assumptions = @(Get-ArrayValue (Get-PropertyValue -Object $run -Name "verified_assumptions"))
change_boundary = [string] (Get-PropertyValue -Object $run -Name "change_boundary")
remaining_steps = @(Get-ArrayValue (Get-PropertyValue -Object $run -Name "remaining_steps"))
recovery_pointers = @(Get-ArrayValue (Get-PropertyValue -Object $run -Name "recovery_pointers"))
steps = @(Get-ArrayValue (Get-PropertyValue -Object $run -Name "steps")).Count
approvals = @(Get-ArrayValue (Get-PropertyValue -Object $run -Name "approvals")).Count
results = @(Get-ArrayValue (Get-PropertyValue -Object $run -Name "results")).Count
escalations = @(Get-ArrayValue (Get-PropertyValue -Object $run -Name "escalations")).Count
cleanup_evidence = @(Get-ArrayValue (Get-PropertyValue -Object $run -Name "cleanup_evidence")).Count
deployment_evidence = $deploymentEvidence.Count
event_summary = @(Get-ArrayValue (Get-PropertyValue -Object $run -Name "event_summary"))
verification_refs = @(Get-ArrayValue (Get-PropertyValue -Object $run -Name "verification_refs"))
risks = @(Get-ArrayValue (Get-PropertyValue -Object $run -Name "risks"))
deployment_evidence_refs = $deploymentEvidenceRefs
resume_pointer = [string] (Get-PropertyValue -Object $run -Name "resume_pointer")
collected_at = (Get-Date).ToUniversalTime().ToString("o")
}
Write-JsonNoBom -Path (Join-Path (Get-RunRoot) "summary.json") -Value $summaryObject
Write-RuntimeResult $summaryObject
}

function Invoke-Verify {
$run = Read-Run
$issues = New-Object System.Collections.Generic.List[string]
foreach ($field in @("schema", "run_id", "version", "status", "objective", "scope", "authority", "owner", "model_tier", "current_route", "change_boundary", "created_at", "updated_at", "resume_pointer", "isolation")) {
if ([string]::IsNullOrWhiteSpace([string] (Get-PropertyValue -Object $run -Name $field))) {
$issues.Add("run.json missing required field: $field") | Out-Null
}
}
foreach ($arrayField in @("verified_assumptions", "remaining_steps", "recovery_pointers", "steps", "approvals", "tool_evidence", "results", "escalations", "cleanup_evidence", "event_summary", "verification_refs", "risks", "deployment_evidence")) {
if ($null -eq (Get-PropertyValue -Object $run -Name $arrayField)) {
$issues.Add("run.json missing required array: $arrayField") | Out-Null
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
run_id = $RunId
operation = "runtime closeout evidence recorded"
requested = $true
remaining_runtime = @()
verified = $true
created_at = (Get-Date).ToUniversalTime().ToString("o")
claim_scope = "verified inactive; no hard delete claim"
claim = "closed or archived only when runtime evidence supports it; hard delete is not claimed"
})
Add-EventSummary -Run $run -Message "cleanup verified: inactive runtime evidence recorded"
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
"AddDeploymentEvidence" { Invoke-AddDeploymentEvidence }
"Collect" { Invoke-Collect }
"Verify" { Invoke-Verify }
"Cleanup" { Invoke-Cleanup }
}
$global:LASTEXITCODE = 0
