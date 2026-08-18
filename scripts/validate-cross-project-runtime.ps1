function Test-CrossProjectRuntimeResilienceIntegrity {
$startFailureCount = $Failures.Count
$markerChecks = @(
@(".agents/docs/agents/deploy.yaml", @("root-layout", "dot-agents-layout", "pre_dirty_snapshot", "post_dirty_snapshot", "changed_by_deploy", "unexpected_changed_files", "cleanup_capability", "dirty_snapshot_guard", "scripts/agents-cleanup.ps1", "%TEMP%/codex-agent-status/<project-id>-<repo-path-hash>/<run-id>/")),
@("scripts/deploy-agents-workflow.ps1", @("LayoutProfile", "Get-TargetDirtySnapshot", "Test-AgentsRoutePathConsistency", "Assert-NoUnexpectedTargetChanges", "cleanup_capability", "run_id", "Get-StatusProjectKey")),
@("scripts/validate.ps1", @("TempRoot", "Get-ValidationTempRoot", "Get-RepoPathHash", "ValidationRunId")),
@("scripts/agents-runtime.ps1", @("resume_pointer", "event_summary", "verification_refs", "deployment_evidence", "AddDeploymentEvidence")),
@("scripts/export-route-pack.ps1", @("Get-ProjectKey", "Get-RunId", "codex-agent-status", "manifest_hash")),
@(".agents/docs/agents/runtime-execution.yaml", @("cross_window_recovery", "deployment_evidence", "resume_pointer", "verification_refs")),
@(".agents/docs/agents/workflows.yaml", @("runtime.quiet_cleanup", "scripts/agents-cleanup.ps1")),
@(".agents/docs/agents/verify.yaml", @("scripts/agents-cleanup.ps1", "cleanup"))
)
foreach ($check in $markerChecks) {
$path = [string] $check[0]
$contentPath = Get-RepoPath $path
if (-not (Test-Path -LiteralPath $contentPath -PathType Leaf)) {
Add-Failure ("Cross-project marker file missing: {0}" -f $path)
continue
}
$content = Get-Content -LiteralPath $contentPath -Raw
foreach ($marker in @($check[1])) {
if (-not $content.Contains([string] $marker)) {
Add-Failure ("Cross-project marker missing from {0}: {1}" -f $path, $marker)
}
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Cross-project ok."
}
}
