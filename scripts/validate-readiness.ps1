function Test-ReadinessLadderEvidence {
$startFailureCount = $Failures.Count
$checks = @(
@{
Level = "P0"
Evidence = @(
@(".agents/docs/agents/version.yaml", "P0:"),
@("scripts/validate.ps1", "Test-SchemaContracts"),
@("scripts/validate.ps1", "Test-RuntimeBoundaries"),
@("scripts/validate.ps1", "forbidden local repair")
)
},
@{
Level = "P1"
Evidence = @(
@(".agents/docs/agents/version.yaml", "P1:"),
@("scripts/deploy-agents-workflow.ps1", "DryRun"),
@("scripts/deploy-agents-workflow.ps1", "Get-TargetLayout"),
@("scripts/deploy-agents-workflow.ps1", "Refusing to write into the provider/source repo"),
@("scripts/deploy-agents-workflow.ps1", "requires -Upgrade"),
@("scripts/deploy-agents-workflow.ps1", "Test-DeployPathAllowed"),
@("scripts/deploy-agents-workflow.ps1", "Test-DeployedFileSet")
)
},
@{
Level = "P2"
Evidence = @(
@(".agents/docs/agents/version.yaml", "P2:"),
@(".agents/docs/agents/version.yaml", "root_principle"),
@("scripts/validate.ps1", "Test-TemplateSourceNeutrality"),
@("scripts/validate.ps1", "Test-CanonicalSourceUniqueness"),
@("scripts/validate.ps1", "Test-TemplateCoverage")
)
},
@{
Level = "P3"
Evidence = @(
@(".agents/docs/agents/version.yaml", "P3:"),
@(".agents/docs/agents/deploy.yaml", "deployment_worker"),
@(".agents/docs/agents/workflows.yaml", "enterprise_dispatch_runtime"),
@(".agents/docs/agents/workflows.yaml", "workflow_artifact_runtime"),
@(".agents/docs/agents/workflows.yaml", "context_compact_runtime"),
@(".agents/docs/agents/workflows.yaml", "collaborator_window_runtime"),
@(".agents/docs/agents/workflow-artifacts.yaml", "artifact_root_rule"),
@(".agents/docs/agents/context-compact.yaml", "summary_contract"),
@(".agents/docs/agents/collaborators.yaml", "lifecycle"),
@(".agents/docs/agents/collaborators.yaml", "leader_mapping"),
@(".agents/docs/agents/dispatch.yaml", "department_report"),
@(".agents/docs/agents/workflows.yaml", "ownership:"),
@(".agents/docs/agents/workflows.yaml", "ledger_missing_or_cleared"),
@(".agents/docs/agents/workflows.yaml", "before:"),
@(".agents/docs/agents/workflows.yaml", "during:"),
@(".agents/docs/agents/workflows.yaml", "after:"),
@("scripts/agents-workflow.ps1", "SimulateDispatch"),
@("scripts/validate.ps1", "Test-EnterpriseDispatchIntegrity"),
@("scripts/validate.ps1", "Test-WorkflowArtifactIntegrity"),
@("scripts/validate.ps1", "Test-ContextCompactIntegrity"),
@("scripts/validate.ps1", "Test-CollaboratorWindowIntegrity"),
@("scripts/validate.ps1", "Test-MultiAgentWorkflowIntegrity"),
@("scripts/validate.ps1", "Test-AgentCleanupHelperIntegrity")
)
},
@{
Level = "P4"
Evidence = @(
@(".agents/docs/agents/version.yaml", "P4:"),
@(".agents/docs/agents/verify.yaml", "release_deploy_push_audit"),
@(".agents/docs/agents/verify.yaml", "runtime_multi_agent"),
@(".agents/docs/agents/verify.yaml", "hard_isolation"),
@(".agents/docs/agents/verify.yaml", "workflow_artifact"),
@(".agents/docs/agents/verify.yaml", "context_compact"),
@(".agents/docs/agents/verify.yaml", "collaborator_window"),
@(".agents/docs/agents/verify.yaml", "core_system"),
@(".agents/docs/agents/verify.yaml", "runtime_execution"),
@(".agents/docs/agents/verify.yaml", "provider_adapter"),
@(".agents/docs/agents/verify.yaml", "route_pack"),
@(".agents/docs/agents/verify.yaml", "knowledge_footprint"),
@(".agents/docs/agents/verify.yaml", "foundation_creation"),
@("scripts/export-release-package.ps1", "release-manifest.json"),
@("scripts/validate-release-evidence.ps1", "Test-ReleasePackageExport"),
@("scripts/validate-release-evidence.ps1", "Test-RuntimeReleaseEvidence"),
@("scripts/validate-size-gates.ps1", "Test-SizeGates"),
@("scripts/validate-evidence-templates.ps1", "Test-EvidenceTemplateSchemaCoverage"),
@("scripts/validate.ps1", "Test-CIWorkflowStability"),
@("scripts/validate.ps1", "Full release audit gates passed")
)
},
@{
Level = "P5"
Evidence = @(
@(".agents/docs/agents/version.yaml", "P5:"),
@(".agents/docs/agents/version.yaml", "precision-efficiency"),
@(".agents/docs/agents/version.yaml", "core_contract_rule"),
@(".agents/docs/agents/version.yaml", "foundation_creation_rule"),
@(".agents/docs/agents/version.yaml", "workflow_artifact"),
@(".agents/docs/agents/version.yaml", "context_compact"),
@(".agents/docs/agents/version.yaml", "rollback"),
@(".agents/docs/agents/openai-foundations.yaml", "structured_outputs"),
@(".agents/docs/agents/openai-foundations.yaml", "progressive_disclosure"),
@(".agents/docs/agents/openai-foundations.yaml", "one_state_owner"),
@(".agents/docs/agents/openai-foundations.yaml", "codex_memories"),
@(".agents/docs/agents/openai-foundations.yaml", "compaction"),
@(".agents/docs/agents/openai-foundations.yaml", "token_counting"),
@("scripts/validate-foundation.ps1", "Test-FoundationCreationIntegrity"),
@(".agents/docs/agents/verify.yaml", "change_aware:"),
@(".agents/docs/agents/verify.yaml", "knowledge_footprint:"),
@("scripts/validate-changes.ps1", "Select-AutoProfile"),
@("scripts/validate-knowledge.ps1", "Test-KnowledgeMemoryIntegrity"),
@(".agents/docs/agents/deploy.yaml", ".workflow/"),
@(".agents/docs/agents/workflows.yaml", "compact_output"),
@(".agents/docs/agents/context-compact.yaml", "raw transcript"),
@("scripts/validate-size-gates.ps1", "Test-SizeGates"),
@("scripts/deploy-agents-workflow.ps1", "target git rollback scope"),
@("scripts/deploy-agents-workflow.ps1", "Update-TargetStateClassification"),
@("scripts/deploy-agents-workflow.ps1", "Protected dirty/local target state observed"),
@("scripts/deploy-agents-workflow.ps1", "Target-owned historical Agents files outside deployed file set")
)
}
)
foreach ($check in $checks) {
foreach ($item in $check.Evidence) {
$path = $item[0]
$marker = $item[1]
$fullPath = Get-RepoPath $path
if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
Add-Failure ("{0} readiness evidence file is missing: {1}" -f $check.Level, $path)
continue
}
$content = Get-Content -LiteralPath $fullPath -Raw
if (-not $content.Contains($marker)) {
Add-Failure ("{0} readiness evidence marker is missing in {1}: {2}" -f $check.Level, $path, $marker)
}
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "P0-P5 readiness evidence checks passed."
}
}
