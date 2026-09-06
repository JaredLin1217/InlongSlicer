#requires -Version 7.0
param([string[]]$Path = @(), [string]$Query = '', [ValidateRange(512,1048576)][int]$BudgetBytes = 8192,
      [ValidateSet('Compact','Json')][string]$Format = 'Compact', [string]$Root)
. "$PSScriptRoot/agent-core.ps1"
if (-not $Root) { $Root = Get-AgentRoot }
$gaps = [Collections.Generic.List[string]]::new()
$candidates = [ordered]@{}
foreach ($p in $Path) { $candidates[$p.Replace('\','/')] = 'explicit path' }
if ($Query) {
    $rg = Get-Command rg -ErrorAction SilentlyContinue
    if ($rg) {
        $matches = @(& rg --files-with-matches --fixed-strings --glob '!.agents/runtime/**' --glob '!.git/**' -- $Query $Root 2>$null)
        if ($LASTEXITCODE -gt 1) { $gaps.Add('Text search failed; inspect paths directly.') }
        foreach ($m in $matches) {
            $p = [IO.Path]::GetRelativePath($Root, $m).Replace('\','/')
            if (-not $candidates.Contains($p)) { $candidates[$p] = 'text match; heuristic relevance' }
        }
    } else { $gaps.Add('rg unavailable; only explicit paths inspected.') }
}
$items = [Collections.Generic.List[object]]::new()
foreach ($p in $candidates.Keys) {
    try {
        $full = Resolve-SafePath $Root $p
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { $gaps.Add("Missing: $p"); continue }
        if ((Get-Item -LiteralPath $full).Length -gt 1048576) { $gaps.Add("Read directly (over 1 MiB): $p"); continue }
        $lines = @(Get-Content -LiteralPath $full)
        $line = 1
        if ($Query) {
            for ($i=0; $i -lt $lines.Count; $i++) { if ($lines[$i].Contains($Query)) { $line=$i+1; break } }
        }
        $basis = 'direct observation'
        $dependencies = @()
        switch ([IO.Path]::GetExtension($p)) {
            '.ps1' {
                $tokens=$null; $errors=$null
                $ast=[Management.Automation.Language.Parser]::ParseFile($full,[ref]$tokens,[ref]$errors)
                if ($errors.Count) { $gaps.Add("PowerShell parse failure: $p") }
                else {
                    $dependencies = @($ast.FindAll({param($n) $n -is [Management.Automation.Language.CommandAst]},$true) |
                        ForEach-Object { $_.GetCommandName() } | Where-Object { $_ -and $_ -like '*.ps1' } | Sort-Object -Unique)
                    $basis='structured AST; dynamic calls unresolved'
                }
            }
            '.json' { try { $null=Read-AgentJson $full; $basis='JSON structure observed' } catch { $gaps.Add("JSON parse failure: $p") } }
            '.md' { }
            default { $gaps.Add("Dependency parsing unsupported: $p") }
        }
        $item=[ordered]@{path=$p;line=$line;reason=$candidates[$p];provenance=$basis;
            relevance=$(if($candidates[$p] -eq 'explicit path'){'user-selected'}else{'heuristic'});
            sha256=(Get-AgentHash $full);checked_utc=[DateTime]::UtcNow.ToString('o');dependencies=$dependencies}
        $trial=@($items.ToArray())+@($item)
        if ([Text.Encoding]::UTF8.GetByteCount(($trial|ConvertTo-Json -Depth 8 -Compress)) -gt ($BudgetBytes-384)) {
            $gaps.Add('Initial budget reached; expand budget or read missing dependencies before decisions.'); break
        }
        $items.Add($item)
    } catch { $gaps.Add("Inspection failed: $p ($($_.Exception.Message))") }
}
$result=[ordered]@{schema_version='agents-context/v3';files=@($items.ToArray());gaps=@($gaps.ToArray());complete=$false;
    guidance='Pointers do not establish impact completeness or authorize skipping tests.'}
$output=$result|ConvertTo-Json -Depth 10 -Compress
while([Text.Encoding]::UTF8.GetByteCount($output) -gt $BudgetBytes) {
    if($result.files.Count) { $result.files=@($result.files|Select-Object -SkipLast 1) }
    else { $result.gaps=@('Budget exhausted; expand budget and inspect unresolved paths.'); break }
    $result.gaps=@('Results truncated; expand budget and inspect unresolved paths.')
    $output=$result|ConvertTo-Json -Depth 10 -Compress
}
if($Format -eq 'Json') { $result|ConvertTo-Json -Depth 10 -Compress } else {
    foreach($item in $result.files) { '{0}:{1} | {2} | {3}' -f $item.path,$item.line,$item.reason,$item.provenance }
    foreach($gap in $result.gaps) { "GAP: $gap" }
}
