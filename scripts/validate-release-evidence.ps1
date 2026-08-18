function Test-ReleasePackageExport {
$startFailureCount = $Failures.Count
$validationRoot = Get-ValidationTempRoot -Purpose "release-export-validation"
$previousErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
$output = & (Get-RepoPath "scripts/export-release-package.ps1") -OutputPath $validationRoot -Quiet 2>&1
$exitCode = if ($null -eq $LASTEXITCODE) { 0 } else { $LASTEXITCODE }
}
finally {
$ErrorActionPreference = $previousErrorActionPreference
}
if ($exitCode -ne 0) {
Add-Failure "Release package export failed."
foreach ($line in $output) {
Add-Failure ("Release package export detail: {0}" -f $line)
}
return
}
if (@($output).Count -gt 0) {
Add-Failure "Release package export quiet mode produced output."
foreach ($line in $output) {
Add-Failure ("Release package export quiet output: {0}" -f $line)
}
}
$versionValues = Get-LightweightYamlPathValues -File (Get-Item -LiteralPath (Get-RepoPath ".agents/docs/agents/version.yaml"))
$version = [string] $versionValues["workflow.version"]
$packageRoot = Join-Path $validationRoot ("jared-ai-team-v{0}" -f $version)
$manifestPath = Join-Path $packageRoot "release-manifest.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
Add-Failure "Release package manifest is missing."
return
}
try {
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
}
catch {
Add-Failure "Release package manifest is not valid JSON."
return
}
if ([string] $manifest.version -ne $version) {
Add-Failure ("Release package manifest version mismatch. Expected {0}; found {1}." -f $version, $manifest.version)
}
if ([string]::IsNullOrWhiteSpace([string] $manifest.package_hash)) {
Add-Failure "Release package manifest package_hash is empty."
}
if ([int] $manifest.file_count -le 0) {
Add-Failure "Release package manifest file_count must be positive."
}
if ($null -eq $manifest.PSObject.Properties["blocklist_result"]) {
Add-Failure "Release package manifest blocklist_result is missing."
}
else {
if (-not [bool] $manifest.blocklist_result.checked) {
Add-Failure "Release package manifest blocklist_result.checked must be true."
}
$policy = @($manifest.blocklist_result.policy | ForEach-Object { [string] $_ })
foreach ($requiredPolicy in @(".agents/runtime/**", ".agents/runtime/context-intelligence/**", ".workflow/**", ".agents/runtime/agent-ledger.jsonl", ".codex/config.toml", "API keys", "provider sessions")) {
if ($policy -notcontains $requiredPolicy) {
Add-Failure ("Release package manifest blocklist policy is missing: {0}" -f $requiredPolicy)
}
}
}
$requiredFiles = @(
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
".agents/docs/agents/openai-foundations.yaml"
)
$manifestPaths = @($manifest.files | ForEach-Object { [string] $_.path })
foreach ($requiredFile in $requiredFiles) {
if ($manifestPaths -notcontains $requiredFile) {
Add-Failure ("Release package is missing required file: {0}" -f $requiredFile)
}
if (-not (Test-Path -LiteralPath (Join-Path $packageRoot ($requiredFile -replace "/", [System.IO.Path]::DirectorySeparatorChar)) -PathType Leaf)) {
Add-Failure ("Release package physical file is missing: {0}" -f $requiredFile)
}
}
$blockedExact = @(
".codex/config.toml",
".codex/environments/environment.toml",
".codex/environments/environment.template.toml",
".agents/runtime/collaborators.jsonl",
".agents/runtime/agent-ledger.jsonl",
"docs/agent-status.md",
".agents/docs/agent-status.md"
)
$blockedPrefix = @(
".git/",
".codex/environments/",
".agents/runtime/",
".agents/runtime/workflows/",
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
foreach ($path in $manifestPaths) {
$rel = $path.ToLowerInvariant()
if ($blockedExact -contains $rel) {
Add-Failure ("Release package manifest includes blocked local path: {0}" -f $path)
}
foreach ($prefix in $blockedPrefix) {
if ($rel.StartsWith($prefix)) {
Add-Failure ("Release package manifest includes blocked local path: {0}" -f $path)
}
}
}
$blockedPhysical = @(
".git",
".agents/runtime",
".agents/runtime/workflows",
".agents/runtime/executions",
".agents/runtime/knowledge",
".agents/runtime/route-packs",
".agents/runtime/context-intelligence",
".agents/runtime/tool-evidence",
".agents/runtime/deployments",
".workflow",
".codex/config.toml",
".codex/environments/environment.toml",
".codex/environments",
".agents/runtime/collaborators.jsonl",
".agents/runtime/agent-ledger.jsonl"
)
foreach ($blocked in $blockedPhysical) {
$fullPath = Join-Path $packageRoot ($blocked -replace "/", [System.IO.Path]::DirectorySeparatorChar)
if (Test-Path -LiteralPath $fullPath) {
Add-Failure ("Release package contains blocked local path: {0}" -f $blocked)
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Release package export checks passed."
}
}

function Test-ObjectField {
param(
[object] $Object,
[string] $Field,
[string] $Context
)
if ($null -eq $Object -or $null -eq $Object.PSObject.Properties[$Field]) {
Add-Failure ("{0} is missing required field: {1}" -f $Context, $Field)
return $false
}
return $true
}

function Test-RuntimeReleaseEvidence {
if ($env:AGENTS_RUNTIME_EVIDENCE_CAPTURE_ACTIVE -eq "1") {
Add-Pass "Runtime release evidence check deferred during evidence capture."
return
}
$startFailureCount = $Failures.Count
$schemaPath = Get-RepoPath "schemas/agents-runtime-evidence.schema.json"
$expectedVersion = Get-CanonicalWorkflowVersion
$evidencePath = Get-RepoPath ("docs/evidence/releases/v{0}-runtime-evidence.json" -f $expectedVersion)
foreach ($path in @($schemaPath, $evidencePath)) {
if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
Add-Failure ("Runtime release evidence file is missing: {0}" -f $path)
}
}
if ($Failures.Count -ne $startFailureCount) {
return
}
try {
$schema = Get-Content -LiteralPath $schemaPath -Raw | ConvertFrom-Json
$evidence = Get-Content -LiteralPath $evidencePath -Raw | ConvertFrom-Json
}
catch {
Add-Failure ("Runtime release evidence JSON parse failed: {0}" -f $_.Exception.Message)
return
}
$requiredFields = @(
"schema_version",
"workflow_version",
"repo_commit",
"source_commit",
"validated_content_digest",
"content_digest_excluded_paths",
"working_tree_status_at_capture",
"run_started_utc",
"run_finished_utc",
"duration_ms",
"commands",
"context_intelligence",
"claims",
"scope",
"xr_xw",
"evidence_tier",
"performance",
"result",
"release"
)
$schemaRequired = @($schema.required | ForEach-Object { [string] $_ })
foreach ($field in $requiredFields) {
if ($schemaRequired -notcontains $field) {
Add-Failure ("Runtime evidence schema is missing required field contract: {0}" -f $field)
}
[void] (Test-ObjectField -Object $evidence -Field $field -Context "Runtime evidence")
}
if ([string] $evidence.schema_version -ne "agents-runtime-evidence/v4") {
Add-Failure "Runtime evidence schema_version mismatch."
}
if ([string] $evidence.workflow_version -ne $expectedVersion) {
Add-Failure ("Runtime evidence workflow_version must be {0}." -f $expectedVersion)
}
if ([string]::IsNullOrWhiteSpace([string] $evidence.repo_commit)) {
Add-Failure "Runtime evidence repo_commit is empty."
}
if ([string]::IsNullOrWhiteSpace([string] $evidence.source_commit)) {
Add-Failure "Runtime evidence source_commit is empty."
}
if ([string] $evidence.release -ne ("v{0}" -f $expectedVersion)) {
Add-Failure ("Runtime evidence release must be v{0}." -f $expectedVersion)
}
try {
$started = [datetimeoffset]::Parse([string] $evidence.run_started_utc)
$finished = [datetimeoffset]::Parse([string] $evidence.run_finished_utc)
if ($finished -lt $started) {
Add-Failure "Runtime evidence finish time is earlier than start time."
}
}
catch {
Add-Failure "Runtime evidence timestamps must be ISO date-time values."
}
if ([int64] $evidence.duration_ms -lt 0) {
Add-Failure "Runtime evidence duration_ms must be non-negative."
}
$digest = $evidence.validated_content_digest
foreach ($field in @("algorithm", "value", "intended_file_count")) {
[void] (Test-ObjectField -Object $digest -Field $field -Context "Runtime evidence validated_content_digest")
}
if ([string] $digest.algorithm -ne "sha256") {
Add-Failure "Runtime evidence content digest algorithm must be sha256."
}
if ([string] $digest.value -notmatch "^[a-f0-9]{64}$") {
Add-Failure "Runtime evidence content digest must be a 64-character lowercase sha256 value."
}
if ([int] $digest.intended_file_count -le 0) {
Add-Failure "Runtime evidence content digest intended_file_count must be positive."
}
$excludedPaths = @($evidence.content_digest_excluded_paths | ForEach-Object { [string] $_ })
foreach ($requiredExcludedPath in @(".git/**", ".agents/runtime/**", ".workflow/**", ".codex/**", "docs/evidence/releases/*.json", "docs/evidence/raw/**")) {
if ($excludedPaths -notcontains $requiredExcludedPath) {
Add-Failure ("Runtime evidence content digest exclusions are missing: {0}" -f $requiredExcludedPath)
}
}
$tree = $evidence.working_tree_status_at_capture
foreach ($field in @("state", "porcelain", "entry_count")) {
[void] (Test-ObjectField -Object $tree -Field $field -Context "Runtime evidence working_tree_status_at_capture")
}
if (@("clean", "dirty") -notcontains [string] $tree.state) {
Add-Failure "Runtime evidence working tree state must be clean or dirty."
}
if ([int] $tree.entry_count -ne @($tree.porcelain).Count) {
Add-Failure "Runtime evidence working tree entry_count must match porcelain entries."
}
$treeText = (@($tree.porcelain) -join "`n")
if ($treeText -match "[A-Za-z]:\\") {
Add-Failure "Runtime evidence working tree status must not expose absolute paths."
}
$commands = @($evidence.commands)
$requiredCommandNames = @(
"deployment_self_test",
"current_project_dry_run",
"disposable_target_rollback_drill",
"runtime_lifecycle_smoke",
"release_package_export",
"route_pack_deterministic",
"context_intelligence_practice",
"full_validation"
)
if ($commands.Count -ne $requiredCommandNames.Count) {
Add-Failure ("Runtime evidence must contain exactly {0} practice commands." -f $requiredCommandNames.Count)
}
foreach ($name in $requiredCommandNames) {
if (@($commands | Where-Object { [string] $_.name -eq $name }).Count -ne 1) {
Add-Failure ("Runtime evidence must contain exactly one command: {0}" -f $name)
}
}
foreach ($command in $commands) {
foreach ($field in @("name", "command", "result", "exit_code", "duration_ms", "retry_count", "raw_output_ref")) {
[void] (Test-ObjectField -Object $command -Field $field -Context ("Runtime evidence command {0}" -f $command.name))
}
if ([string] $command.result -ne "passed") {
Add-Failure ("Runtime evidence command did not pass: {0}" -f $command.name)
}
if ([int] $command.exit_code -ne 0) {
Add-Failure ("Runtime evidence command exit_code must be 0: {0}" -f $command.name)
}
if ([int64] $command.duration_ms -lt 0) {
Add-Failure ("Runtime evidence command duration_ms must be non-negative: {0}" -f $command.name)
}
if ([int] $command.retry_count -lt 0) {
Add-Failure ("Runtime evidence command retry_count must be non-negative: {0}" -f $command.name)
}
$rawRef = [string] $command.raw_output_ref
if (-not $rawRef.StartsWith("%TEMP%/codex-agent-status/")) {
Add-Failure ("Runtime evidence raw output reference must stay in status scratch: {0}" -f $command.name)
}
if ($rawRef -match '^[A-Za-z]:\\' -or $rawRef.Contains("\")) {
Add-Failure ("Runtime evidence raw output reference must not expose an absolute or platform path: {0}" -f $command.name)
}
if ([string] $command.name -eq "full_validation") {
if ($null -eq $command.PSObject.Properties["overall_score"] -or [decimal] $command.overall_score -ne [decimal] 100.0) {
Add-Failure "Runtime evidence full_validation must record overall_score 100.0."
}
$fullCommand = [string] $command.command
foreach ($marker in @("validate-changes.ps1", "-Profile Full", "-ContextMode Required")) {
if (-not $fullCommand.Contains($marker)) {
Add-Failure ("Runtime evidence full_validation command is missing required context gate: {0}" -f $marker)
}
}
}
}
$context = $evidence.context_intelligence
foreach ($field in @("captured", "resolver_schema", "practice_report_schema", "runtime_report_ref", "result", "baseline", "candidate", "metrics", "thresholds", "fallback_tests", "scope")) {
[void] (Test-ObjectField -Object $context -Field $field -Context "Runtime evidence context_intelligence")
}
if (-not [bool] $context.captured) {
Add-Failure "Runtime evidence context intelligence practice must be captured."
}
if ([string] $context.resolver_schema -ne "agents-context-evidence/v1") {
Add-Failure "Runtime evidence context resolver schema must be agents-context-evidence/v1."
}
if ([string] $context.practice_report_schema -ne "agents-context-practice-report/v1") {
Add-Failure "Runtime evidence context practice report schema must be agents-context-practice-report/v1."
}
if ([string] $context.runtime_report_ref -ne ".agents/runtime/context-intelligence/practice-report.json") {
Add-Failure "Runtime evidence context practice report must remain an ignored runtime pointer."
}
if ([string] $context.result -ne "pass") {
Add-Failure "Runtime evidence context intelligence result must be pass."
}
foreach ($field in @("version", "source_commit", "method", "runs_per_task")) {
[void] (Test-ObjectField -Object $context.baseline -Field $field -Context "Runtime evidence context baseline")
}
if ([string] $context.baseline.version -ne "2.8.0") {
Add-Failure "Runtime evidence context baseline version must be 2.8.0."
}
if ([string] $context.baseline.source_commit -notmatch "^[a-f0-9]{40}$") {
Add-Failure "Runtime evidence context baseline source_commit must be a full Git hash."
}
if ([int] $context.baseline.runs_per_task -ne 3) {
Add-Failure "Runtime evidence context baseline must use three runs per task."
}
foreach ($field in @("version", "method", "runs_per_task")) {
[void] (Test-ObjectField -Object $context.candidate -Field $field -Context "Runtime evidence context candidate")
}
if ([string] $context.candidate.version -ne "2.9.1") {
Add-Failure "Runtime evidence context candidate version must be 2.9.1."
}
if ([int] $context.candidate.runs_per_task -ne 3) {
Add-Failure "Runtime evidence context candidate must use three runs per task."
}
$metrics = $context.metrics
$metricFields = @(
"task_accuracy",
"critical_impact_recall",
"impact_precision",
"baseline_context_median_bytes",
"candidate_context_median_bytes",
"context_median_reduction",
"baseline_tool_calls_median",
"candidate_tool_calls_median",
"tool_call_reduction",
"fallback_pass_rate"
)
foreach ($field in $metricFields) {
[void] (Test-ObjectField -Object $metrics -Field $field -Context "Runtime evidence context metrics")
}
if ([decimal] $metrics.task_accuracy -ne [decimal] 1) {
Add-Failure "Runtime evidence context task accuracy must be 100%."
}
if ([decimal] $metrics.critical_impact_recall -ne [decimal] 1) {
Add-Failure "Runtime evidence critical impact recall must be 100%."
}
if ([decimal] $metrics.impact_precision -lt [decimal] 0.85) {
Add-Failure "Runtime evidence impact precision must be at least 85%."
}
if ([decimal] $metrics.context_median_reduction -lt [decimal] 0.50) {
Add-Failure "Runtime evidence median context reduction must be at least 50%."
}
if ([decimal] $metrics.tool_call_reduction -lt [decimal] 0.30) {
Add-Failure "Runtime evidence median tool-call reduction must be at least 30%."
}
if ([decimal] $metrics.fallback_pass_rate -ne [decimal] 1) {
Add-Failure "Runtime evidence fallback pass rate must be 100%."
}
if ([int64] $metrics.candidate_context_median_bytes -ge [int64] $metrics.baseline_context_median_bytes) {
Add-Failure "Runtime evidence candidate context median must be lower than the v2.8 baseline."
}
if ([decimal] $metrics.candidate_tool_calls_median -ge [decimal] $metrics.baseline_tool_calls_median) {
Add-Failure "Runtime evidence candidate tool-call median must be lower than the v2.8 baseline."
}
foreach ($field in @("task_accuracy", "critical_impact_recall", "impact_precision", "context_median_reduction", "tool_call_reduction", "fallback_pass_rate", "deterministic")) {
if (-not (Test-ObjectField -Object $context.thresholds -Field $field -Context "Runtime evidence context thresholds")) {
continue
}
if (-not [bool] $context.thresholds.$field) {
Add-Failure ("Runtime evidence context threshold did not pass: {0}" -f $field)
}
}
$fallbackTests = @($context.fallback_tests)
$requiredFallbackStates = @("stale", "degraded", "unsupported", "parse_error", "conflict", "dirty")
if ($fallbackTests.Count -ne $requiredFallbackStates.Count) {
Add-Failure "Runtime evidence must include exactly six context fallback tests."
}
foreach ($state in $requiredFallbackStates) {
$matches = @($fallbackTests | Where-Object { [string] $_.state -eq $state })
if ($matches.Count -ne 1 -or [string] $matches[0].result -ne "pass") {
Add-Failure ("Runtime evidence context fallback must pass exactly once: {0}" -f $state)
}
}
foreach ($field in @("practice_level", "external_pilot", "codegraph_runtime")) {
[void] (Test-ObjectField -Object $context.scope -Field $field -Context "Runtime evidence context scope")
}
if ([string] $context.scope.practice_level -ne "T2 current-repo practice") {
Add-Failure "Runtime evidence context practice level must be T2 current-repo practice."
}
if ([string] $context.scope.external_pilot -ne "not executed") {
Add-Failure "Runtime evidence context scope must not claim an external pilot."
}
if ([string] $context.scope.codegraph_runtime -ne "not installed or used") {
Add-Failure "Runtime evidence must confirm that CodeGraph runtime was not installed or used."
}
foreach ($field in @("static_validation", "runtime_evidence", "practice_evidence", "context_intelligence", "evidence_tier", "hard_isolation", "external_pilot_boundary")) {
[void] (Test-ObjectField -Object $evidence.claims -Field $field -Context "Runtime evidence claims")
}
if ([string] $evidence.claims.evidence_tier -ne "T2 current-repo practice") {
Add-Failure "Runtime evidence claims must be limited to T2 current-repo practice."
}
if ([string] $evidence.claims.hard_isolation -notmatch "No hard isolation claim") {
Add-Failure "Runtime evidence must preserve the hard isolation boundary statement."
}
foreach ($field in @("target", "external_target_deployment", "external_pilot", "codegraph_runtime", "raw_output_storage", "practice", "disposable_target")) {
[void] (Test-ObjectField -Object $evidence.scope -Field $field -Context "Runtime evidence scope")
}
if ([bool] $evidence.scope.external_target_deployment) {
Add-Failure "Runtime evidence must not claim external target deployment."
}
if (-not [bool] $evidence.scope.practice) {
Add-Failure "Runtime evidence must be captured with the practice suite."
}
if ([string] $evidence.scope.external_pilot -ne "not executed") {
Add-Failure "Runtime evidence scope must not claim an external project pilot."
}
if ([string] $evidence.scope.codegraph_runtime -ne "not installed or used") {
Add-Failure "Runtime evidence scope must preserve the CodeGraph runtime boundary."
}
if ([string] $evidence.scope.raw_output_storage -ne "%TEMP%/codex-agent-status/<project-id>/<run-id>/") {
Add-Failure "Runtime evidence raw output storage must be the sanitized status scratch reference."
}
foreach ($field in @("external_reads", "external_writes")) {
[void] (Test-ObjectField -Object $evidence.xr_xw -Field $field -Context "Runtime evidence xr_xw")
}
if (@($evidence.xr_xw.external_reads).Count -ne 0) {
Add-Failure "Runtime evidence must not record external reads for this release."
}
if (@($evidence.xr_xw.external_writes).Count -eq 0) {
Add-Failure "Runtime evidence must record status scratch writes."
}
foreach ($field in @("current", "maximum_claimed", "scale")) {
[void] (Test-ObjectField -Object $evidence.evidence_tier -Field $field -Context "Runtime evidence tier")
}
if ([string] $evidence.evidence_tier.current -ne "T2 current-repo practice") {
Add-Failure "Runtime evidence tier current value must be T2 current-repo practice."
}
if ([string] $evidence.evidence_tier.maximum_claimed -ne "T2 current-repo practice") {
Add-Failure "Runtime evidence tier maximum_claimed value must be T2 current-repo practice."
}
$tierScale = @($evidence.evidence_tier.scale | ForEach-Object { [string] $_ })
foreach ($tier in @("T0 static", "T1 dry-run", "T2 current-repo practice", "T3 external pilot", "T4 enforced isolation")) {
if ($tierScale -notcontains $tier) {
Add-Failure ("Runtime evidence tier scale is missing: {0}" -f $tier)
}
}
foreach ($field in @("command_count", "total_duration_ms", "token_usage", "token_usage_reason")) {
[void] (Test-ObjectField -Object $evidence.performance -Field $field -Context "Runtime evidence performance")
}
if ([int] $evidence.performance.command_count -ne $commands.Count) {
Add-Failure "Runtime evidence performance command_count must match commands."
}
if ([int64] $evidence.performance.total_duration_ms -ne [int64] $evidence.duration_ms) {
Add-Failure "Runtime evidence performance total_duration_ms must match duration_ms."
}
if ([string] $evidence.performance.token_usage -ne "unavailable") {
Add-Failure "Runtime evidence token usage must be marked unavailable when metrics are absent."
}
if ([string]::IsNullOrWhiteSpace([string] $evidence.performance.token_usage_reason)) {
Add-Failure "Runtime evidence token usage reason is required."
}
if ([string] $evidence.result -ne "passed") {
Add-Failure "Runtime evidence result must be passed."
}
$repoRootMarker = if ($null -ne $RepoRoot.PSObject.Properties["Path"]) { [string] $RepoRoot.Path } else { [string] $RepoRoot }
$repoRootName = Split-Path -Path $repoRootMarker -Leaf
$serialized = $evidence | ConvertTo-Json -Depth 20 -Compress
if ($serialized -match "[A-Za-z]:\\") {
Add-Failure "Runtime evidence leaks a local absolute path marker."
}
$dynamicMarkers = New-Object System.Collections.Generic.List[string]
foreach ($marker in @(
    $repoRootMarker,
    $repoRootName,
    [string] (Split-Path -Path $repoRootMarker -Parent | Split-Path -Leaf),
    [string] $env:USERNAME,
    [string] $env:USERPROFILE
)) {
if (-not [string]::IsNullOrWhiteSpace($marker) -and $marker.Length -ge 3) {
[void] $dynamicMarkers.Add($marker)
}
}
foreach ($forbidden in ($dynamicMarkers | Select-Object -Unique)) {
if ($serialized.Contains($forbidden)) {
Add-Failure "Runtime evidence leaks a local source-specific path marker."
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Runtime release evidence checks passed."
}
}
