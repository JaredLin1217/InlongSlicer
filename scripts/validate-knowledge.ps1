function Test-KnowledgeMemoryIntegrity {
    $startFailureCount = $Failures.Count
    $requiredFiles = @(
        ".agents/docs/agents/knowledge-footprint.yaml",
        ".agents/docs/templates/agents/agents/knowledge-footprint.yaml",
        "schemas/agents-knowledge-footprint.schema.json",
        ".agents/docs/project-memory.md",
        ".agents/docs/memory/index.md",
        ".agents/docs/memory-entry.template.md",
        ".agents/docs/templates/agents/project-memory.md",
        ".agents/docs/templates/agents/memory-index.md",
        ".agents/docs/templates/agents/memory-entry.template.md"
    )
    foreach ($path in $requiredFiles) {
        if (-not (Test-Path -LiteralPath (Get-RepoPath $path) -PathType Leaf)) {
            Add-Failure ("Knowledge memory file is missing: {0}" -f $path)
        }
    }
    if ($Failures.Count -ne $startFailureCount) {
        return
    }

    $canonical = Get-Content -LiteralPath (Get-RepoPath ".agents/docs/agents/knowledge-footprint.yaml") -Raw
    foreach ($marker in @(
        "schema: agents-knowledge-footprint/v3",
        "durable_memory:",
        "index_first: true",
        "max_detail_entries: 3",
        "max_detail_bytes: 8192",
        "default_review_days: 90",
        "source_commit",
        "content_hash",
        "conflict_rule:",
        "cannot directly drive"
    )) {
        if (-not $canonical.Contains($marker)) {
            Add-Failure ("Knowledge footprint marker is missing: {0}" -f $marker)
        }
    }

    $pairs = @(
        @(".agents/docs/agents/knowledge-footprint.yaml", ".agents/docs/templates/agents/agents/knowledge-footprint.yaml"),
        @(".agents/docs/memory-entry.template.md", ".agents/docs/templates/agents/memory-entry.template.md")
    )
    foreach ($pair in $pairs) {
        $leftHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Get-RepoPath $pair[0])).Hash
        $rightHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Get-RepoPath $pair[1])).Hash
        if ($leftHash -ne $rightHash) {
            Add-Failure ("Knowledge memory exact pair drift: {0} != {1}" -f $pair[0], $pair[1])
        }
    }

    $expectedHeader = "| ID | Date | Title | Trigger | Keywords | Summary | Entry | Status | Confidence | Source Commit | Content Hash | Checked At | Last Verified | Next Review Due | Update Trigger | Supersedes | Boundary | Source Refs |"
    $indexPath = Get-RepoPath ".agents/docs/memory/index.md"
    $indexLines = @(Get-Content -LiteralPath $indexPath)
    if ($indexLines -notcontains $expectedHeader) {
        Add-Failure "Memory index must use the canonical 18-column header."
    }
    $entryLines = @($indexLines | Where-Object { $_ -match '^\|\s*M[0-9]{3,}\s*\|' })
    $seenIds = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    $memoryRoot = [System.IO.Path]::GetFullPath((Get-RepoPath "docs/memory"))
    $today = (Get-Date).ToUniversalTime().Date
    foreach ($line in $entryLines) {
        $cells = @($line.Trim().Trim('|').Split('|') | ForEach-Object { $_.Trim() })
        if ($cells.Count -ne 18) {
            Add-Failure ("Memory index row must have 18 columns: {0}" -f $line)
            continue
        }
        $id = $cells[0]
        if (-not $seenIds.Add($id)) {
            Add-Failure ("Memory index contains duplicate id: {0}" -f $id)
        }
        if ($cells[7] -notin @("active", "stale", "retired")) {
            Add-Failure ("Memory index status is invalid for {0}: {1}" -f $id, $cells[7])
        }
        if ($cells[8] -notin @("high", "medium", "low")) {
            Add-Failure ("Memory index confidence is invalid for {0}: {1}" -f $id, $cells[8])
        }
        if ($cells[9] -notmatch '^[0-9a-f]{40}$') {
            Add-Failure ("Memory index source commit is invalid for {0}." -f $id)
        }
        if ($cells[10] -notmatch '^[0-9a-f]{64}$') {
            Add-Failure ("Memory index content hash is invalid for {0}." -f $id)
        }
        $checkedAt = [datetimeoffset]::MinValue
        if (-not [datetimeoffset]::TryParse($cells[11], [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::AssumeUniversal, [ref] $checkedAt)) {
            Add-Failure ("Memory index checked time is invalid for {0}." -f $id)
        }
        $lastVerified = [datetime]::MinValue
        $nextReview = [datetime]::MinValue
        $lastOk = [datetime]::TryParseExact($cells[12], "yyyy-MM-dd", [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::AssumeUniversal, [ref] $lastVerified)
        $nextOk = [datetime]::TryParseExact($cells[13], "yyyy-MM-dd", [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::AssumeUniversal, [ref] $nextReview)
        if (-not $lastOk -or -not $nextOk -or $nextReview.Date -lt $lastVerified.Date) {
            Add-Failure ("Memory index freshness dates are invalid for {0}." -f $id)
        }
        elseif ($cells[7] -eq "active" -and $nextReview.Date -lt $today) {
            Add-Failure ("Active memory entry is overdue and must be reviewed or marked stale: {0}" -f $id)
        }
        foreach ($fieldIndex in @(14, 15, 16, 17)) {
            if ([string]::IsNullOrWhiteSpace($cells[$fieldIndex])) {
                Add-Failure ("Memory index provenance field {0} is missing for {1}." -f $fieldIndex, $id)
            }
        }
        if ([string]::IsNullOrWhiteSpace($cells[17])) {
            Add-Failure ("Memory index source refs are missing for {0}." -f $id)
        }

        $entry = $cells[6].Trim('`') -replace '/', [System.IO.Path]::DirectorySeparatorChar
        $entryPath = [System.IO.Path]::GetFullPath((Join-Path $memoryRoot $entry))
        $memoryPrefix = $memoryRoot.TrimEnd([System.IO.Path]::DirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
        if (-not $entryPath.StartsWith($memoryPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            Add-Failure ("Memory index entry escapes docs/memory for {0}: {1}" -f $id, $cells[6])
            continue
        }
        if (-not (Test-Path -LiteralPath $entryPath -PathType Leaf)) {
            Add-Failure ("Memory index entry file is missing for {0}: {1}" -f $id, $cells[6])
            continue
        }
        $detail = Get-Content -LiteralPath $entryPath -Raw
        foreach ($marker in @(
            "ID: $id",
            "Status: $($cells[7])",
            "Confidence: $($cells[8])",
            "Source Commit: $($cells[9])",
            "Content Hash: $($cells[10])",
            "Checked At: $($cells[11])",
            "Last Verified: $($cells[12])",
            "Next Review Due: $($cells[13])",
            "Update Trigger: $($cells[14])",
            "Supersedes: $($cells[15])",
            "Boundary: $($cells[16])",
            "Source Refs:",
            "## Trigger",
            "## Evidence",
            "## Reuse when"
        )) {
            if (-not $detail.Contains($marker)) {
                Add-Failure ("Memory detail marker is missing for {0}: {1}" -f $id, $marker)
            }
        }
    }

    $starter = Get-Content -LiteralPath (Get-RepoPath ".agents/docs/templates/agents/memory-index.md") -Raw
    if ($starter -notmatch [regex]::Escape($expectedHeader) -or $starter -match '(?m)^\|\s*M[0-9]{3,}\s*\|') {
        Add-Failure "Target memory index must keep the canonical header and contain no provider memory rows."
    }
    if ($Failures.Count -eq $startFailureCount) {
        Add-Pass "Knowledge footprint and durable memory integrity checks passed."
    }
}
