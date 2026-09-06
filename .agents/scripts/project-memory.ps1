#requires -Version 7.0
param([ValidateSet('Recall','Promote','Index')][string]$Action='Recall', [string]$Query='', [string]$InputPath, [string]$Root)
. "$PSScriptRoot/agent-core.ps1"
if (-not $Root) { $Root=Get-AgentRoot }
$settings=Get-ProjectSettings $Root
$directory=Resolve-SafePath $Root $settings.knowledge_directory
$schema=Get-AgentAsset $Root 'schemas/knowledge.schema.json'
function Test-KnowledgeSources($entry) {
    if([DateTimeOffset]::Parse($entry.verified_utc) -gt [DateTimeOffset]::UtcNow.AddMinutes(1)) { return $false }
    foreach($source in $entry.sources) {
        if ((Get-AgentHash (Resolve-SafePath $Root $source.path)) -ne $source.sha256) { return $false }
    }
    return $true
}
if($Action -eq 'Promote') {
    if(-not $InputPath) { throw 'Promotion requires an explicit reviewed entry.' }
    $entry=Read-AgentJson (Resolve-SafePath $Root $InputPath) $schema
    if($entry.status -ne 'active' -or -not (Test-KnowledgeSources $entry)) { throw 'Only active, source-verified entries may be promoted.' }
    $destination=Resolve-SafePath $Root "$($settings.knowledge_directory)/$($entry.id).json"
    if(Test-Path -LiteralPath $destination) { throw 'Existing knowledge is immutable; create a new ID with supersedes.' }
    foreach($id in $entry.supersedes) {
        if($id -eq $entry.id -or -not (Test-Path -LiteralPath (Resolve-SafePath $Root "$($settings.knowledge_directory)/$id.json"))) {
            throw "Unknown/self supersession: $id"
        }
    }
    Write-AgentJson $destination $entry -NoClobber
}
$entries=[Collections.Generic.List[object]]::new()
$gaps=[Collections.Generic.List[string]]::new()
$invalidKnowledge=$false
if(Test-Path -LiteralPath $directory) {
    foreach($file in Get-ChildItem -LiteralPath $directory -Filter '*.json' -File) {
        try {
            $relative=[IO.Path]::GetRelativePath($Root,$file.FullName).Replace('\','/')
            $entry=Read-AgentJson (Resolve-SafePath $Root $relative) $schema
            $entry['path']=$relative
            $entry['fresh']=Test-KnowledgeSources $entry
            $entries.Add($entry)
        } catch { $invalidKnowledge=$true; $gaps.Add("Invalid knowledge: $($file.Name)") }
    }
}
$duplicates=@($entries | Group-Object id | Where-Object Count -GT 1 | ForEach-Object Name)
$lookup=@{}; foreach($entry in $entries) { $lookup[$entry.id]=$entry }
foreach($entry in $entries) {
    if($entry.id -in $duplicates) { $entry.status='conflicted'; $gaps.Add("Duplicate ID suppressed: $($entry.id)") }
    $pending=[Collections.Generic.Stack[string]]::new(); foreach($id in $entry.supersedes) { $pending.Push($id) }; $seen=@{}
    while($pending.Count) {
        $id=$pending.Pop()
        if($id -eq $entry.id -or -not $lookup.ContainsKey($id)) {
            $entry.status='conflicted'; $gaps.Add("Invalid supersession chain: $($entry.id)"); break
        }
        if($seen.ContainsKey($id)) { continue }; $seen[$id]=$true
        foreach($next in $lookup[$id].supersedes) { $pending.Push($next) }
    }
}
$superseded=@($entries | Where-Object { $_.fresh -and $_.status -eq 'active' } | ForEach-Object { $_.supersedes })
# Retirement survives source drift; only fresh replacements resolve an explicit conflict.
$retired=@($entries | Where-Object { $_.status -in @('active','superseded') } | ForEach-Object { $_.supersedes })
foreach($entry in $entries) {
    if(-not $entry.fresh -and $entry.status -ne 'superseded' -and $entry.id -notin $retired) { $gaps.Add("Source changed: $($entry.id)") }
}
$active=@($entries | Where-Object { $_.fresh -and $_.status -eq 'active' -and $_.id -notin $retired })
# Explicit same-scope conflicts are suspended, not resolved by timestamp.
$conflicts=@($entries | Where-Object { $_.status -eq 'conflicted' -and $_.id -notin $superseded } | ForEach-Object scope)
$active=@($active | Where-Object { $_.scope -notin $conflicts })
foreach($scope in $conflicts) { $gaps.Add("Conflicted scope suppressed: $scope") }
# An unreadable record may contain a retirement or conflict affecting any other entry.
if($invalidKnowledge) { $active=@() }
$index=@($active | Sort-Object id | ForEach-Object { @{id=$_.id;type=$_.type;conclusion=$_.conclusion;scope=$_.scope;path=$_.path} })
if($Action -in @('Index','Promote')) {
    $indexPath=Resolve-SafePath $Root "$($settings.runtime_directory)/memory-index.json"
    Write-AgentJson $indexPath @{schema_version='agents-memory-index/v3';entries=$index;gaps=@($gaps.ToArray())}
}
$selected=if($Query) { @($index | Where-Object { ($_.conclusion+' '+$_.scope).IndexOf($Query,[StringComparison]::OrdinalIgnoreCase) -ge 0 }) } else { $index }
@{entries=@($selected);gaps=@($gaps.ToArray());source='verified entry files; cached index not trusted'}|ConvertTo-Json -Depth 12
