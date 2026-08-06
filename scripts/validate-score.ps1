function Get-IntendedRepoFiles {
$files = @(
& git -c core.quotepath=false -C $RepoRoot ls-files
& git -c core.quotepath=false -C $RepoRoot ls-files --others --exclude-standard
) | Where-Object { $_ } | Sort-Object -Unique
return @($files | Where-Object { Test-Path -LiteralPath (Get-RepoPath $_) -PathType Leaf })
}

function Get-RepoFilesSize {
param([string[]] $Paths)
$total = 0
foreach ($path in $Paths) {
$fullPath = Get-RepoPath $path
if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
$total += (Get-Item -LiteralPath $fullPath).Length
}
}
return $total
}

function Test-ScoreMarkers {
param(
[string] $Text,
[string[]] $Markers
)
foreach ($marker in $Markers) {
if ($Text -notmatch [regex]::Escape($marker)) {
return $false
}
}
return $true
}

function Write-AgentQualityScore {
$runtimePath = Get-RepoPath ".agents/docs/agents/ai-runtime.yaml"
$foundationPath = Get-RepoPath ".agents/docs/agents/openai-foundations.yaml"
$knowledgePath = Get-RepoPath ".agents/docs/agents/knowledge-footprint.yaml"
$contextPath = Get-RepoPath ".agents/docs/agents/context-compact.yaml"
$contextIntelligencePath = Get-RepoPath ".agents/docs/agents/context-intelligence.yaml"
$runtimeEvidenceSchemaPath = Get-RepoPath "schemas/agents-runtime-evidence.schema.json"
$verifyPath = Get-RepoPath ".agents/docs/agents/verify.yaml"
$deployPath = Get-RepoPath ".agents/docs/agents/deploy.yaml"
$runtimeText = Get-Content -LiteralPath $runtimePath -Raw
$foundationText = Get-Content -LiteralPath $foundationPath -Raw
$knowledgeText = Get-Content -LiteralPath $knowledgePath -Raw
$contextText = Get-Content -LiteralPath $contextPath -Raw
$contextIntelligenceText = Get-Content -LiteralPath $contextIntelligencePath -Raw
$runtimeEvidenceSchemaText = Get-Content -LiteralPath $runtimeEvidenceSchemaPath -Raw
$verifyText = Get-Content -LiteralPath $verifyPath -Raw
$deployText = Get-Content -LiteralPath $deployPath -Raw
$runtimeBytes = (Get-Item -LiteralPath $runtimePath).Length
$validateBytes = (Get-Item -LiteralPath (Get-RepoPath "scripts/validate.ps1")).Length
$intendedBytes = Get-RepoFilesSize -Paths (Get-IntendedRepoFiles)
$repoLimitMatch = [regex]::Match($verifyText, "(?m)^\s*tracked_repo_kib:\s*(\d+)\s*$")
$repoLimitBytes = if ($repoLimitMatch.Success) { [int]$repoLimitMatch.Groups[1].Value * 1024 } else { 896 * 1024 }

$routesReady = Test-ScoreMarkers -Text $runtimeText -Markers @(
"enterprise_dispatch:", "workflow_artifact:", "context_compact:", "context_intelligence:", "collaborator_window:",
"core_system:", "runtime_execution:", "provider_adapter:", "foundation_creation:",
"route_pack:", "knowledge_footprint:", "expand_only", "canonical YAML wins"
)
$officialSourcesReady = Test-ScoreMarkers -Text $foundationText -Markers @(
"schema: agents-openai-foundations/v2", "official_docs_first", "structured_outputs:",
"conversation_state:", "agents_sdk:", "codex_instructions:", "codex_skills:",
"codex_memories:", "subagents:", "prompt_caching:", "predicted_outputs:",
"compaction:", "token_counting:", "evaluation:"
)
$sourceFreshnessReady = (
([regex]::Matches($foundationText, "(?m)^\s{4}refresh_interval_days:\s*90\s*$")).Count -eq 12 -and
([regex]::Matches($foundationText, '(?m)^\s{4}next_review_due:\s*"\d{4}-\d{2}-\d{2}"\s*$')).Count -eq 12
)
$accuracyReady = Test-ScoreMarkers -Text $foundationText -Markers @(
"structured_outputs_first", "one_state_owner", "evidence_before_claim", "dataset_first", "guardrail_rule"
)
$memoryReady = (
(Test-ScoreMarkers -Text $knowledgeText -Markers @(
"schema: agents-knowledge-footprint/v3", "index_first: true", "max_detail_entries: 3",
"max_detail_bytes: 8192", "source_commit", "content_hash", "checked_at", "update_trigger", "supersedes", "boundary", "Next Review Due", "current evidence"
)) -and
(Test-Path -LiteralPath (Get-RepoPath ".agents/docs/memory/index.md") -PathType Leaf) -and
(Test-Path -LiteralPath (Get-RepoPath ".agents/docs/project-memory.md") -PathType Leaf)
)
$tokenReady = (
$runtimeBytes -le 4096 -and
(Test-ScoreMarkers -Text $contextText -Markers @(
"progressive_disclosure_rule", "stable_prefix_rule", "tool_output_rule", "measurement_rule",
"detail_file_default_max: 3", "context_evidence_ref", "recovery_pointers"
)) -and
(Test-ScoreMarkers -Text $contextIntelligenceText -Markers @(
"schema: agents-context-intelligence/v1", "max_files: 3", "budget_bytes: 8192",
"provenance", "confidence", "freshness", "verification recommendation", ".agents/runtime/context-intelligence/"
)) -and
(Test-Path -LiteralPath (Get-RepoPath "scripts/resolve-agent-context.ps1") -PathType Leaf)
)
$verificationReady = (
$validateBytes -le 100000 -and
(Test-Path -LiteralPath (Get-RepoPath "scripts/validate-changes.ps1") -PathType Leaf) -and
(Test-ScoreMarkers -Text $verifyText -Markers @("changed_path_set", "validate-changes.ps1", "ContextMode Auto", "context_intelligence_practice", "change_aware"))
)
$releaseReady = (
$Full -and
(Test-Path -LiteralPath (Get-RepoPath "scripts/export-release-package.ps1") -PathType Leaf) -and
$runtimeEvidenceSchemaText -match "agents-runtime-evidence/v4" -and
$runtimeEvidenceSchemaText -match '"context_intelligence"' -and
$deployText -match "do_not_deploy" -and
$deployText -match "validation_levels"
)

$checks = @(
[pscustomobject]@{ Name = "rule_and_route_fit"; Passed = $routesReady; Evidence = "compact expand-only route index and canonical priority" },
[pscustomobject]@{ Name = "official_source_freshness"; Passed = ($officialSourcesReady -and $sourceFreshnessReady); Evidence = "12 official capability sources with 90-day review metadata" },
[pscustomobject]@{ Name = "accuracy_guardrails"; Passed = $accuracyReady; Evidence = "schema-first output, one state owner, evidence, guardrails, and evals" },
[pscustomobject]@{ Name = "durable_memory"; Passed = $memoryReady; Evidence = "index-first, freshness-bound, evidence-verified tracked memory" },
[pscustomobject]@{ Name = "token_economy"; Passed = $tokenReady; Evidence = "route-only reads, three-entry cap, stable prefix, and measured context" },
[pscustomobject]@{ Name = "verification_economy"; Passed = $verificationReady; Evidence = "change-aware entry point and $validateBytes-byte validation orchestrator" },
[pscustomobject]@{ Name = "repo_growth_control"; Passed = ($intendedBytes -le $repoLimitBytes); Evidence = "$intendedBytes intended bytes against $repoLimitBytes-byte gate" },
[pscustomobject]@{ Name = "deploy_portability"; Passed = $releaseReady; Evidence = "full release and export gates included" }
)

$items = @($checks | ForEach-Object {
$passed = $_.Passed -and $Failures.Count -eq 0
[pscustomobject]@{
Name = $_.Name
Status = if ($passed) { "PASS" } else { "WARN" }
Score = if ($passed) { 100.0 } else { 80.0 }
Evidence = $_.Evidence
}
})
$overall = [Math]::Round([decimal](($items | Measure-Object -Property Score -Average).Average), 1)
Write-Host ""
Write-Host "Individual validation:"
foreach ($item in $items) {
Write-Host ("[{0}] {1}: {2:0.0}/100 - {3}" -f $item.Status, $item.Name, $item.Score, $item.Evidence)
}
Write-Host ("Individual target: {0} (>= 100.0)" -f $(if (@($items | Where-Object { $_.Score -lt 100 }).Count -eq 0) { "PASS" } else { "WARN" }))
Write-Host ("Overall: {0:0.0}/100" -f $overall)
Write-Host "Evaluation: static quality score is bounded by the captured evidence tier; it is not a hard-isolation claim."
}
