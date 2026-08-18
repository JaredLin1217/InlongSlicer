Set-StrictMode -Version Latest

function Test-ContextIntelligenceIntegrity {
    param([switch]$RunPractice)

    $requiredPaths = @(
        ".agents/docs/agents/context-intelligence.yaml",
        "schemas/agents-context-intelligence.schema.json",
        "schemas/agents-context-evidence.schema.json",
        "scripts/agent-toml.ps1",
        "scripts/resolve-agent-context.ps1",
        "scripts/test-context-intelligence.ps1",
        "tests/context-intelligence/cases.json",
        "tests/context-intelligence/fixtures/codex-config.valid.toml",
        "tests/context-intelligence/fixtures/codex-config.invalid.toml"
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
    $canonical = Get-Content -LiteralPath $canonicalPath -Raw

    $requiredMarkers = @(
        "schema: agents-context-intelligence/v1",
        "agents-context-evidence/v1",
        ".agents/runtime/context-intelligence/",
        "heuristic_limit: discovery only",
        "dirty working tree",
        "TOML",
        "declared local",
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

    $tomlHelperPath = Get-RepoPath "scripts/agent-toml.ps1"
    $tomlFixtureRelativePath = "tests/context-intelligence/fixtures/codex-config.valid.toml"
    $tomlFixturePath = Get-RepoPath $tomlFixtureRelativePath
    $invalidTomlFixturePath = Get-RepoPath "tests/context-intelligence/fixtures/codex-config.invalid.toml"
    try {
        . $tomlHelperPath
        $invalidToml = Test-AgentTomlFile -Path $invalidTomlFixturePath
        if (-not [bool]$invalidToml.available -or [bool]$invalidToml.success) {
            Add-Failure "Invalid TOML was not rejected by parser $($invalidToml.parser)."
        }
    }
    catch {
        Add-Failure "TOML rejection smoke failed: $($_.Exception.Message)"
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

    try {
        $tomlEvidence = (& $resolverPath -Task "validate Codex TOML context" -ChangedPath $tomlFixtureRelativePath -Format Json) | ConvertFrom-Json
        $tomlRelevant = @($tomlEvidence.relevant_files | Where-Object path -eq $tomlFixtureRelativePath)
        $forbiddenGaps = @("unsupported:$tomlFixtureRelativePath", "parse_error:$tomlFixtureRelativePath", "degraded:parser-unavailable:$tomlFixtureRelativePath")
        if ($tomlRelevant.Count -ne 1 -or
            [string]$tomlRelevant[0].provenance -notmatch "parser=(python-toml|powershell-convertfrom-toml)" -or
            @($tomlEvidence.gaps | Where-Object { $forbiddenGaps -contains [string]$_ }).Count -gt 0) {
            Add-Failure "Context resolver did not produce complete TOML evidence."
        }
    }
    catch {
        Add-Failure "TOML context resolver smoke failed: $($_.Exception.Message)"
    }

    if ($RunPractice) {
        $practicePath = Get-RepoPath "scripts/test-context-intelligence.ps1"
        & $practicePath -Quiet
        if ($LASTEXITCODE -ne 0) {
            Add-Failure "Context intelligence practice suite failed."
        }
    }
}
