[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Task,

    [string[]]$ChangedPath = @(),

    [ValidateRange(1, 3)]
    [int]$MaxFiles = 3,

    [ValidateRange(512, 8192)]
    [int]$BudgetBytes = 8192,

    [ValidateSet("Compact", "Json")]
    [string]$Format = "Compact",

    [Parameter(DontShow = $true)]
    [string]$RepositoryRoot,

    [Parameter(DontShow = $true)]
    [ValidateSet("none", "stale", "degraded", "unsupported", "parse_error", "conflict", "dirty")]
    [string]$Simulation = "none"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$tomlHelperPath = Join-Path $PSScriptRoot "agent-toml.ps1"
$tomlHelperAvailable = Test-Path -LiteralPath $tomlHelperPath -PathType Leaf
if ($tomlHelperAvailable) {
    . $tomlHelperPath
}

function ConvertTo-AgentPath {
    param([string]$PathValue)

    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        return ""
    }

    $value = $PathValue.Replace("\", "/").Trim()
    while ($value.StartsWith("./", [System.StringComparison]::Ordinal)) {
        $value = $value.Substring(2)
    }
    return $value.TrimStart("/")
}

function Invoke-AgentGit {
    param(
        [string]$Root,
        [string[]]$Arguments
    )

    $output = @(& git -C $Root -c core.quotepath=false @Arguments 2>$null)
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Arguments -join ' ') failed with exit code $LASTEXITCODE."
    }
    return @($output | ForEach-Object { [string]$_ })
}

function Get-AgentRoute {
    param(
        [string]$TaskText,
        [string[]]$Paths
    )

    $taskValue = $TaskText.ToLowerInvariant()
    $pathValue = (@($Paths) -join " ").ToLowerInvariant()

    if ($taskValue -match '\b(deploy|deployment|release|rollback|roll back|package export)\b') {
        return "deploy_or_release"
    }
    if ($taskValue -match '\b(commit|push|tag|checkpoint|branch)\b') {
        return "git_checkpoint"
    }
    if ($taskValue -match '\b(memory|knowledge|durable knowledge|supersede)\b' -or $pathValue -match 'knowledge-footprint|docs/memory/') {
        return "knowledge_footprint"
    }
    if ($taskValue -match '\b(resume|recovery|recover|handoff|compact|long task)\b' -or $pathValue -match 'context-compact') {
        return "context_compact"
    }
    if ($taskValue -match '\b(version upgrade|upgrade version|core system)\b' -or $pathValue -match '.agents/docs/agents/version\.yaml') {
        return "core_system"
    }
    if ($taskValue -match '\b(context|impact|affected|dependency|minimal context|validation script|verify changes)\b' -or $pathValue -match 'resolve-agent-context|validate-changes') {
        return "context_intelligence"
    }
    if ($taskValue -match '\b(runtime|lifecycle|execution)\b' -or $pathValue -match 'runtime-execution|agents-runtime') {
        return "runtime_execution"
    }
    if ($taskValue -match '\b(answer only|explain only|no repository state)\b') {
        return "answer_only"
    }
    if ($pathValue -match '(^|\s)agents\.md|\.agents/skills/|.agents/docs/agents/(policy|workflows|verify)\.yaml') {
        return "policy_pack_edit"
    }
    if (@($Paths).Count -gt 0) {
        return "scoped_edit"
    }
    return "answer_only"
}

function Get-RouteFiles {
    param([string]$Route)

    $routes = @{
        answer_only          = @()
        scoped_edit          = @(".agents/docs/agents/verify.yaml")
        policy_pack_edit     = @("AGENTS.md", ".agents/docs/agents/verify.yaml")
        git_checkpoint       = @(".agents/docs/agents/verify.yaml")
        deploy_or_release    = @(".agents/docs/agents/deploy.yaml", ".agents/docs/agents/verify.yaml")
        context_compact      = @(".agents/docs/agents/context-compact.yaml", ".agents/docs/agents/schemas.yaml", ".agents/docs/agents/verify.yaml")
        context_intelligence = @(".agents/docs/agents/context-intelligence.yaml", ".agents/docs/agents/context-compact.yaml", ".agents/docs/agents/schemas.yaml", ".agents/docs/agents/verify.yaml")
        knowledge_footprint  = @(".agents/docs/agents/knowledge-footprint.yaml", ".agents/docs/agents/context-compact.yaml", ".agents/docs/agents/verify.yaml")
        runtime_execution    = @(".agents/docs/agents/runtime-execution.yaml", ".agents/docs/agents/verify.yaml")
        core_system          = @(".agents/docs/agents/core-system.yaml", ".agents/docs/agents/version.yaml", ".agents/docs/agents/verify.yaml")
    }

    if ($routes.ContainsKey($Route)) {
        return @($routes[$Route])
    }
    return @(".agents/docs/agents/ai-runtime.yaml", ".agents/docs/agents/verify.yaml")
}

function Get-AgentLanguage {
    param([string]$PathValue)

    $extension = [System.IO.Path]::GetExtension($PathValue).ToLowerInvariant()
    switch ($extension) {
        ".ps1" { return "powershell" }
        ".psm1" { return "powershell" }
        ".json" { return "json" }
        ".toml" { return "toml" }
        ".yaml" { return "yaml" }
        ".yml" { return "yaml" }
        ".md" { return "markdown" }
        ".py" { return "python" }
        ".js" { return "javascript" }
        ".jsx" { return "javascript" }
        ".ts" { return "typescript" }
        ".tsx" { return "typescript" }
        ".c" { return "c_cpp" }
        ".cc" { return "c_cpp" }
        ".cpp" { return "c_cpp" }
        ".cxx" { return "c_cpp" }
        ".h" { return "c_cpp" }
        ".hpp" { return "c_cpp" }
        default { return "unsupported" }
    }
}

function Get-TextDependencies {
    param(
        [string]$Text,
        [string]$CurrentPath,
        [hashtable]$TrackedPaths,
        [hashtable]$BaseNames
    )

    $found = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $searchText = $Text.Replace("\", "/")
    $pathPattern = '(?i)(?<![A-Za-z0-9_.-])(?:\.{0,2}/)?(?:[A-Za-z0-9_.-]+/)+[A-Za-z0-9_.-]+\.(?:ps1|psm1|yaml|yml|json|toml|md|py|js|jsx|ts|tsx|c|cc|cpp|cxx|h|hpp)'
    foreach ($match in [regex]::Matches($searchText, $pathPattern)) {
        $candidate = ConvertTo-AgentPath $match.Value
        while ($candidate.StartsWith("../", [System.StringComparison]::Ordinal)) {
            $candidate = $candidate.Substring(3)
        }
        if ($TrackedPaths.ContainsKey($candidate)) {
            [void]$found.Add($candidate)
        }
    }

    $includePattern = '(?im)^\s*(?:#include\s*[<"]|from\s+|import\s+)["<]?([A-Za-z0-9_./-]+)'
    foreach ($match in [regex]::Matches($searchText, $includePattern)) {
        $leaf = [System.IO.Path]::GetFileName($match.Groups[1].Value)
        if ($BaseNames.ContainsKey($leaf)) {
            foreach ($candidate in @($BaseNames[$leaf])) {
                [void]$found.Add([string]$candidate)
            }
        }
    }

    [void]$found.Remove($CurrentPath)
    return @($found | Sort-Object)
}

function Get-RelevantLine {
    param(
        [string]$FullPath,
        [string[]]$Tokens
    )

    try {
        $lines = [System.IO.File]::ReadAllLines($FullPath)
        foreach ($token in @($Tokens)) {
            for ($index = 0; $index -lt $lines.Count; $index++) {
                if ($lines[$index].IndexOf($token, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                    return ($index + 1)
                }
            }
        }
    }
    catch {
        return 1
    }
    return 1
}

function Get-StringDigest {
    param([string]$Value)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Value)
        return ([System.BitConverter]::ToString($sha.ComputeHash($bytes))).Replace("-", "").ToLowerInvariant()
    }
    finally {
        $sha.Dispose()
    }
}

$repoRoot = $RepositoryRoot
if ([string]::IsNullOrWhiteSpace($repoRoot)) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
}
$repoRoot = [System.IO.Path]::GetFullPath($repoRoot)
$repoPrefix = $repoRoot.TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar

$gaps = [System.Collections.Generic.List[string]]::new()
$rawChangedPaths = @($ChangedPath)
$normalizedChangedPaths = @()
foreach ($rawPath in $rawChangedPaths) {
    if ([string]::IsNullOrWhiteSpace($rawPath)) {
        continue
    }
    try {
        $candidate = [string]$rawPath
        $fullCandidate = if ([System.IO.Path]::IsPathRooted($candidate)) {
            [System.IO.Path]::GetFullPath($candidate)
        }
        else {
            [System.IO.Path]::GetFullPath((Join-Path $repoRoot $candidate))
        }
        if ($fullCandidate -ne $repoRoot -and -not $fullCandidate.StartsWith($repoPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            [void]$gaps.Add("conflict:external-path:$rawPath")
            continue
        }
        $candidate = $fullCandidate.Substring($repoRoot.Length).TrimStart("\", "/")
        if (-not [string]::IsNullOrWhiteSpace($candidate)) {
            $normalizedChangedPaths += ConvertTo-AgentPath $candidate
        }
    }
    catch {
        [void]$gaps.Add("conflict:invalid-path:$rawPath")
    }
}
$normalizedChangedPaths = @($normalizedChangedPaths | Sort-Object -Unique)

$trackedFiles = @()
$sourceCommit = "unavailable"
$workingTreeStatus = @()
$gitAvailable = $true
try {
    $trackedFiles = @(
        @(Invoke-AgentGit -Root $repoRoot -Arguments @("ls-files")) +
        @(Invoke-AgentGit -Root $repoRoot -Arguments @("ls-files", "--others", "--exclude-standard"))
    ) | Sort-Object -Unique
    $sourceCommitLines = @(Invoke-AgentGit -Root $repoRoot -Arguments @("rev-parse", "HEAD"))
    if ($sourceCommitLines.Count -gt 0) {
        $sourceCommit = $sourceCommitLines[0].Trim().ToLowerInvariant()
    }
    $workingTreeStatus = @(Invoke-AgentGit -Root $repoRoot -Arguments @("status", "--porcelain=v1", "--untracked-files=all"))
}
catch {
    $gitAvailable = $false
    [void]$gaps.Add("degraded:git-unavailable")
}

$supportedExtensions = @(
    ".ps1", ".psm1", ".json", ".toml", ".yaml", ".yml", ".md", ".py", ".js", ".jsx", ".ts", ".tsx",
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"
)
$ignoredPrefixes = @(".agents/runtime/", ".git/", "node_modules/", "vendor/", "build/", "dist/", "out/")
$trackedMap = @{}
$baseNameMap = @{}
foreach ($file in @($trackedFiles)) {
    $pathValue = ConvertTo-AgentPath $file
    $ignore = $false
    foreach ($prefix in $ignoredPrefixes) {
        if ($pathValue.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            $ignore = $true
            break
        }
    }
    if ($ignore) {
        continue
    }
    $extension = [System.IO.Path]::GetExtension($pathValue).ToLowerInvariant()
    if ($supportedExtensions -notcontains $extension) {
        continue
    }
    $trackedMap[$pathValue] = "git-inventory"
    $leaf = [System.IO.Path]::GetFileName($pathValue)
    if (-not $baseNameMap.ContainsKey($leaf)) {
        $baseNameMap[$leaf] = @()
    }
    $baseNameMap[$leaf] = @($baseNameMap[$leaf]) + $pathValue
}

foreach ($pathValue in $normalizedChangedPaths) {
    $extension = [System.IO.Path]::GetExtension($pathValue).ToLowerInvariant()
    if ($supportedExtensions -notcontains $extension -or $trackedMap.ContainsKey($pathValue)) {
        continue
    }
    $ignore = $false
    foreach ($prefix in $ignoredPrefixes) {
        if ($pathValue.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            $ignore = $true
            break
        }
    }
    $fullPath = Join-Path $repoRoot ($pathValue.Replace("/", [System.IO.Path]::DirectorySeparatorChar))
    if ($ignore -or -not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        continue
    }
    $trackedMap[$pathValue] = "declared-local"
    $leaf = [System.IO.Path]::GetFileName($pathValue)
    if (-not $baseNameMap.ContainsKey($leaf)) {
        $baseNameMap[$leaf] = @()
    }
    $baseNameMap[$leaf] = @($baseNameMap[$leaf]) + $pathValue
}

$runtimeRoot = Join-Path $repoRoot ".agents/runtime/context-intelligence"
$indexPath = Join-Path $runtimeRoot "index.json"
$oldIndexByPath = @{}
if (Test-Path -LiteralPath $indexPath -PathType Leaf) {
    try {
        $oldIndex = Get-Content -LiteralPath $indexPath -Raw | ConvertFrom-Json
        if ([string]$oldIndex.schema_version -eq "agents-context-index/v2") {
            foreach ($entry in @($oldIndex.files)) {
                $oldIndexByPath[[string]$entry.path] = $entry
            }
        }
        else {
            [void]$gaps.Add("stale:index-version")
        }
    }
    catch {
        [void]$gaps.Add("stale:index-unreadable")
    }
}

$indexEntries = [System.Collections.Generic.List[object]]::new()
$entryByPath = @{}
$reusedPaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($pathValue in @($trackedMap.Keys | Sort-Object)) {
    $fullPath = Join-Path $repoRoot ($pathValue.Replace("/", [System.IO.Path]::DirectorySeparatorChar))
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        continue
    }

    $fileInfo = Get-Item -LiteralPath $fullPath
    if ($fileInfo.Length -gt 1048576) {
        continue
    }
    $hash = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $oldEntry = $null
    if ($oldIndexByPath.ContainsKey($pathValue)) {
        $oldEntry = $oldIndexByPath[$pathValue]
    }

    $inventorySource = [string]$trackedMap[$pathValue]
    if ($null -ne $oldEntry -and [string]$oldEntry.sha256 -eq $hash -and
        [string]$oldEntry.inventory_source -eq $inventorySource -and [bool]$oldEntry.parser_available) {
        $entry = [ordered]@{
            path             = $pathValue
            sha256           = $hash
            size_bytes       = [int64]$fileInfo.Length
            language         = [string]$oldEntry.language
            inventory_source = $inventorySource
            parser           = [string]$oldEntry.parser
            parser_available = [bool]$oldEntry.parser_available
            dependency_paths = @($oldEntry.dependency_paths)
            parse_errors     = @($oldEntry.parse_errors)
        }
        [void]$reusedPaths.Add($pathValue)
    }
    else {
        $language = Get-AgentLanguage $pathValue
        $parseErrors = [System.Collections.Generic.List[string]]::new()
        $parser = "bounded-reference-scan"
        $parserAvailable = $true
        $text = ""
        try {
            $text = Get-Content -LiteralPath $fullPath -Raw
            if ($language -eq "powershell") {
                $parser = "powershell-ast"
                $tokens = $null
                $errors = $null
                [void][System.Management.Automation.Language.Parser]::ParseFile($fullPath, [ref]$tokens, [ref]$errors)
                foreach ($parseError in @($errors)) {
                    [void]$parseErrors.Add([string]$parseError.Message)
                }
            }
            elseif ($language -eq "json") {
                $parser = "powershell-json"
                [void]($text | ConvertFrom-Json)
            }
            elseif ($language -eq "toml") {
                if (-not $tomlHelperAvailable) {
                    $parser = "unavailable"
                    $parserAvailable = $false
                    [void]$parseErrors.Add("TOML validator helper is unavailable.")
                }
                else {
                    $tomlResult = Test-AgentTomlFile -Path $fullPath
                    $parser = [string]$tomlResult.parser
                    $parserAvailable = [bool]$tomlResult.available
                    foreach ($parseError in @($tomlResult.errors)) {
                        [void]$parseErrors.Add([string]$parseError)
                    }
                }
            }
        }
        catch {
            [void]$parseErrors.Add($_.Exception.Message)
        }

        $dependencies = @()
        if (-not [string]::IsNullOrEmpty($text)) {
            $dependencies = @(Get-TextDependencies -Text $text -CurrentPath $pathValue -TrackedPaths $trackedMap -BaseNames $baseNameMap)
        }
        $entry = [ordered]@{
            path             = $pathValue
            sha256           = $hash
            size_bytes       = [int64]$fileInfo.Length
            language         = $language
            inventory_source = $inventorySource
            parser           = $parser
            parser_available = $parserAvailable
            dependency_paths = @($dependencies)
            parse_errors     = @($parseErrors)
        }
    }
    $entryObject = [pscustomobject]$entry
    [void]$indexEntries.Add($entryObject)
    $entryByPath[$pathValue] = $entryObject
}

$statusFingerprint = Get-StringDigest ((@($workingTreeStatus) -join "`n"))
$indexState = if ($reusedPaths.Count -gt 0) { "incremental" } else { "rebuilt" }
$generatedUtc = [DateTime]::UtcNow.ToString("o")
$indexObject = [ordered]@{
    schema_version          = "agents-context-index/v2"
    source_commit           = $sourceCommit
    working_tree_fingerprint = $statusFingerprint
    generated_utc           = $generatedUtc
    files                   = @($indexEntries)
}
try {
    New-Item -ItemType Directory -Path $runtimeRoot -Force | Out-Null
    $indexObject | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $indexPath -Encoding UTF8
}
catch {
    [void]$gaps.Add("degraded:index-write-failed")
}

if ($workingTreeStatus.Count -gt 0) {
    [void]$gaps.Add("dirty:working-tree")
}
foreach ($pathValue in $normalizedChangedPaths) {
    $extension = [System.IO.Path]::GetExtension($pathValue).ToLowerInvariant()
    if ($supportedExtensions -notcontains $extension -and $pathValue -ne "AGENTS.md") {
        [void]$gaps.Add("unsupported:$pathValue")
    }
    if (-not $trackedMap.ContainsKey($pathValue) -and -not (Test-Path -LiteralPath (Join-Path $repoRoot $pathValue))) {
        [void]$gaps.Add("conflict:missing-path:$pathValue")
    }
    if ($entryByPath.ContainsKey($pathValue)) {
        $changedEntry = $entryByPath[$pathValue]
        if (-not [bool]$changedEntry.parser_available) {
            [void]$gaps.Add("degraded:parser-unavailable:$pathValue")
        }
        if (@($changedEntry.parse_errors).Count -gt 0) {
            [void]$gaps.Add("parse_error:$pathValue")
        }
    }
}

$route = Get-AgentRoute -TaskText $Task -Paths $normalizedChangedPaths
$routeFiles = @(Get-RouteFiles $route)
$candidateScores = @{}
$candidateReasons = @{}
function Add-AgentCandidate {
    param(
        [string]$PathValue,
        [int]$Score,
        [string]$Reason
    )
    if ([string]::IsNullOrWhiteSpace($PathValue) -or -not $entryByPath.ContainsKey($PathValue)) {
        return
    }
    if (-not $candidateScores.ContainsKey($PathValue)) {
        $candidateScores[$PathValue] = 0
        $candidateReasons[$PathValue] = [System.Collections.Generic.List[string]]::new()
    }
    $candidateScores[$PathValue] = [int]$candidateScores[$PathValue] + $Score
    if (-not $candidateReasons[$PathValue].Contains($Reason)) {
        [void]$candidateReasons[$PathValue].Add($Reason)
    }
}

for ($index = 0; $index -lt $routeFiles.Count; $index++) {
    Add-AgentCandidate -PathValue $routeFiles[$index] -Score (110 - ($index * 5)) -Reason "canonical route dependency"
}
foreach ($pathValue in $normalizedChangedPaths) {
    Add-AgentCandidate -PathValue $pathValue -Score 160 -Reason "declared changed path"
    if ($entryByPath.ContainsKey($pathValue)) {
        foreach ($dependency in @($entryByPath[$pathValue].dependency_paths)) {
            Add-AgentCandidate -PathValue ([string]$dependency) -Score 90 -Reason "dependency of changed path"
        }
    }
}

$taskTokens = @($Task.ToLowerInvariant() -split '[^a-z0-9_.-]+' | Where-Object { $_.Length -ge 3 } | Sort-Object -Unique)
foreach ($pathValue in @($entryByPath.Keys)) {
    $pathLower = $pathValue.ToLowerInvariant()
    foreach ($token in $taskTokens) {
        if ($pathLower.Contains($token)) {
            Add-AgentCandidate -PathValue $pathValue -Score 18 -Reason "task token match"
            break
        }
    }
}

if ($route -ne "answer_only" -and $candidateScores.Count -eq 0) {
    Add-AgentCandidate -PathValue ".agents/docs/agents/ai-runtime.yaml" -Score 100 -Reason "route authority fallback"
}

$rankedCandidates = @(
    foreach ($pathValue in $candidateScores.Keys) {
        [pscustomobject]@{
            path  = $pathValue
            score = [int]$candidateScores[$pathValue]
        }
    }
) | Sort-Object @{ Expression = "score"; Descending = $true }, @{ Expression = "path"; Descending = $false }

$relevantFiles = [System.Collections.Generic.List[object]]::new()
$selectedBytes = 0
foreach ($candidate in @($rankedCandidates)) {
    if ($relevantFiles.Count -ge $MaxFiles) {
        break
    }
    $entry = $entryByPath[[string]$candidate.path]
    $contextBytes = [Math]::Min([int64]$entry.size_bytes, 1536) + 256
    if (($selectedBytes + $contextBytes) -gt $BudgetBytes) {
        continue
    }
    $fullPath = Join-Path $repoRoot ([string]$entry.path).Replace("/", [System.IO.Path]::DirectorySeparatorChar)
    $line = Get-RelevantLine -FullPath $fullPath -Tokens $taskTokens
    $freshness = if ($reusedPaths.Contains([string]$entry.path)) { "unchanged" } else { "checked" }
    $confidence = if (@($entry.parse_errors).Count -eq 0) { "high" } else { "low" }
    if (@($entry.parse_errors).Count -gt 0) {
        [void]$gaps.Add("parse_error:$($entry.path)")
    }
    $inventoryDescription = if ([string]$entry.inventory_source -eq "declared-local") { "declared local path" } else { "Git inventory" }
    [void]$relevantFiles.Add([pscustomobject][ordered]@{
        path          = [string]$entry.path
        line          = [int]$line
        reason        = (@($candidateReasons[[string]$entry.path]) -join "; ")
        sha256        = [string]$entry.sha256
        freshness     = $freshness
        provenance    = "$inventoryDescription; parser=$($entry.parser)"
        confidence    = $confidence
        context_bytes = [int64]$contextBytes
    })
    $selectedBytes += $contextBytes
}

$dependencyPaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($file in @($relevantFiles)) {
    if ($entryByPath.ContainsKey([string]$file.path)) {
        foreach ($dependency in @($entryByPath[[string]$file.path].dependency_paths)) {
            [void]$dependencyPaths.Add([string]$dependency)
        }
    }
}
foreach ($routeFile in $routeFiles) {
    if (-not (@($relevantFiles.path) -contains $routeFile) -and $entryByPath.ContainsKey($routeFile)) {
        [void]$dependencyPaths.Add($routeFile)
    }
}
$dependencyPathList = @($dependencyPaths | Sort-Object | Select-Object -First 8)

$affectedTests = [System.Collections.Generic.List[string]]::new()
switch ($route) {
    "deploy_or_release" {
        [void]$affectedTests.Add("scripts/deploy-agents-workflow.ps1 -SelfTest -Quiet")
        [void]$affectedTests.Add("scripts/validate-release-evidence.ps1")
    }
    "runtime_execution" { [void]$affectedTests.Add("scripts/validate-runtime-execution.ps1") }
    "knowledge_footprint" { [void]$affectedTests.Add("scripts/validate-knowledge.ps1") }
    "context_intelligence" { [void]$affectedTests.Add("scripts/test-context-intelligence.ps1 -Quiet") }
}
$baseProfile = "Fast"
if ($route -in @("policy_pack_edit", "context_compact", "context_intelligence", "knowledge_footprint", "runtime_execution", "core_system")) {
    $baseProfile = "Policy"
}
if ($route -in @("git_checkpoint", "deploy_or_release")) {
    $baseProfile = "Full"
}

if ($Simulation -ne "none") {
    $simulatedGap = switch ($Simulation) {
        "stale" { "stale:simulated-index" }
        "degraded" { "degraded:simulated-parser" }
        "unsupported" { "unsupported:simulated-language" }
        "parse_error" { "parse_error:simulated-source" }
        "conflict" { "conflict:simulated-evidence" }
        "dirty" { "dirty:simulated-working-tree" }
    }
    [void]$gaps.Add($simulatedGap)
}
$gapList = @($gaps | Sort-Object -Unique)
$profile = $baseProfile
if ($gapList.Count -gt 0) {
    $profile = "Full"
}

switch ($profile) {
    "Fast" { [void]$affectedTests.Add("scripts/validate-changes.ps1 -Profile Fast") }
    "Policy" { [void]$affectedTests.Add("scripts/validate-changes.ps1 -Profile Policy") }
    "Full" { [void]$affectedTests.Add("scripts/validate-changes.ps1 -Profile Full -ContextMode Required -Score") }
}
$affectedTestList = @($affectedTests | Sort-Object -Unique)

$allHashes = @($indexEntries | ForEach-Object { "{0}:{1}" -f $_.path, $_.sha256 }) -join "`n"
$overallConfidence = "high"
if ($gapList.Count -gt 0) {
    $overallConfidence = "low"
}
elseif ($relevantFiles.Count -eq 0 -and $route -ne "answer_only") {
    $overallConfidence = "medium"
}

$evidence = [ordered]@{
    schema_version              = "agents-context-evidence/v1"
    route                       = $route
    relevant_files              = @($relevantFiles)
    dependency_paths            = @($dependencyPathList)
    affected_tests              = @($affectedTestList)
    freshness                   = [ordered]@{
        checked_utc              = $generatedUtc
        source_commit            = $sourceCommit
        working_tree_fingerprint = $statusFingerprint
        working_tree_state       = if ($workingTreeStatus.Count -eq 0) { "clean" } else { "dirty" }
        index_state              = $indexState
        content_digest           = Get-StringDigest $allHashes
    }
    provenance                  = @(
        [ordered]@{ method = "repository inventory"; authority = "primary"; detail = "Git plus declared local paths and state" },
        [ordered]@{ method = "structured parsing"; authority = "primary"; detail = "PowerShell AST, JSON, and TOML" },
        [ordered]@{ method = "bounded reference scan"; authority = "discovery-only"; detail = "cannot authorize edits or skipped verification" }
    )
    confidence                  = $overallConfidence
    gaps                        = @($gapList)
    verification_recommendation = [ordered]@{
        profile        = $profile
        reason         = if ($gapList.Count -gt 0) { "Evidence gap requires expanded context and full validation." } else { "Route and impact evidence support the selected minimum profile." }
        expand_context = [bool]($gapList.Count -gt 0)
        commands       = @($affectedTestList)
    }
    budget                      = [ordered]@{
        max_files      = $MaxFiles
        budget_bytes   = $BudgetBytes
        selected_files = $relevantFiles.Count
        selected_bytes = [int64]$selectedBytes
    }
    authority                   = [ordered]@{
        heuristic_limit = "Discovery only; never sufficient to modify content or skip verification."
        failure_policy  = "Parsing failure, conflict, unsupported input, stale evidence, or dirty state expands context and validation."
        runtime_index   = ".agents/runtime/context-intelligence/index.json"
    }
}

if ($Format -eq "Json") {
    $evidence | ConvertTo-Json -Depth 12 -Compress
    exit 0
}

$compactLines = [System.Collections.Generic.List[string]]::new()
[void]$compactLines.Add("route=$route profile=$profile confidence=$overallConfidence")
foreach ($file in @($relevantFiles)) {
    [void]$compactLines.Add("$($file.path):$($file.line) [$($file.reason)]")
}
if ($gapList.Count -gt 0) {
    [void]$compactLines.Add("gaps=$($gapList -join ',')")
}
[void]$compactLines.Add("evidence=.agents/runtime/context-intelligence/index.json bytes=$selectedBytes/$BudgetBytes")
$compactLines -join [Environment]::NewLine
