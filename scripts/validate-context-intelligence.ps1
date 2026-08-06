Set-StrictMode -Version Latest

function Test-ContextIntelligenceIntegrity {
    param([switch]$RunPractice)

    $requiredPaths = @(
        ".agents/docs/agents/context-intelligence.yaml",
        ".agents/docs/templates/agents/agents/context-intelligence.yaml",
        "schemas/agents-context-intelligence.schema.json",
        "schemas/agents-context-evidence.schema.json",
        "scripts/resolve-agent-context.ps1",
        "scripts/test-context-intelligence.ps1",
        "tests/context-intelligence/cases.json"
    )
    $missing = $false
    foreach ($relativePath in $requiredPaths) {
        $fullPath = Get-RepoPath $relativePath
        if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
            Add-Failure "Context intelligence required file is missing: $relativePath"
            $missing = $true
        }
    }
    if ($missing) {
        return
    }

    $canonicalPath = Get-RepoPath ".agents/docs/agents/context-intelligence.yaml"
    $mirrorPath = Get-RepoPath ".agents/docs/templates/agents/agents/context-intelligence.yaml"
    $canonical = Get-Content -LiteralPath $canonicalPath -Raw
    $mirror = Get-Content -LiteralPath $mirrorPath -Raw
    if ($canonical -cne $mirror) {
        Add-Failure "Context intelligence canonical/template mirror drifted."
    }

    $requiredMarkers = @(
        "schema: agents-context-intelligence/v1",
        "agents-context-evidence/v1",
        ".agents/runtime/context-intelligence/",
        "heuristic_limit: discovery only",
        "dirty working tree",
        "scripts/test-context-intelligence.ps1",
        "task_accuracy: 1.0",
        "critical_impact_recall: 1.0",
        "impact_precision_min: 0.85",
        "context_median_reduction_min: 0.50",
        "tool_call_reduction_min: 0.30",
        "fallback_pass_rate: 1.0",
        "No SQLite, tree-sitter, watcher, or standalone MCP service"
    )
    foreach ($marker in $requiredMarkers) {
        if (-not $canonical.Contains($marker)) {
            Add-Failure "Context intelligence contract is missing marker: $marker"
        }
    }

    foreach ($schemaRelativePath in @("schemas/agents-context-intelligence.schema.json", "schemas/agents-context-evidence.schema.json")) {
        try {
            [void](Get-Content -LiteralPath (Get-RepoPath $schemaRelativePath) -Raw | ConvertFrom-Json)
        }
        catch {
            Add-Failure "Context intelligence schema is invalid JSON: $schemaRelativePath"
        }
    }

    $resolverPath = Get-RepoPath "scripts/resolve-agent-context.ps1"
    try {
        $resolverOutput = & $resolverPath -Task "validate context intelligence impact" -ChangedPath ".agents/docs/agents/context-intelligence.yaml" -MaxFiles 3 -BudgetBytes 8192 -Format Json
        if ($LASTEXITCODE -ne 0) {
            throw "resolver exited with code $LASTEXITCODE"
        }
        $evidence = $resolverOutput | ConvertFrom-Json
        if ([string]$evidence.schema_version -ne "agents-context-evidence/v1") {
            Add-Failure "Context resolver emitted the wrong schema version."
        }
        if ([string]$evidence.route -ne "context_intelligence") {
            Add-Failure "Context resolver selected an unexpected route for its smoke task."
        }
        if (@($evidence.relevant_files).Count -gt 3) {
            Add-Failure "Context resolver exceeded the three-file default."
        }
        if ([int64]$evidence.budget.selected_bytes -gt 8192) {
            Add-Failure "Context resolver exceeded the 8192-byte budget."
        }
        if ([string]$evidence.authority.heuristic_limit -notmatch "never sufficient") {
            Add-Failure "Context resolver did not preserve the heuristic authority boundary."
        }
        foreach ($field in @("dependency_paths", "affected_tests", "freshness", "provenance", "confidence", "gaps", "verification_recommendation")) {
            if ($evidence.PSObject.Properties.Name -notcontains $field) {
                Add-Failure "Context resolver evidence is missing field: $field"
            }
        }
    }
    catch {
        Add-Failure "Context resolver smoke failed: $($_.Exception.Message)"
    }

    if ($RunPractice) {
        $practicePath = Get-RepoPath "scripts/test-context-intelligence.ps1"
        & $practicePath -Quiet
        if ($LASTEXITCODE -ne 0) {
            Add-Failure "Context intelligence practice suite failed."
        }
    }
}
