. "$PSScriptRoot/agent-runtime.ps1"
function Test-KnowledgeSources {
    param([string]$Root,$Entry,[Nullable[DateTimeOffset]]$Now,[switch]$VerifyOfficial)
    $clock=Get-RuntimeClock $Now
    if([DateTimeOffset]::Parse($Entry.verified_utc) -gt $clock.AddMinutes(1)) { return $false }
    if($Entry.schema_version -eq 'agents-knowledge/v4' -and $Entry.freshness.review_after_utc -and $clock -ge [DateTimeOffset]::Parse($Entry.freshness.review_after_utc)) { return $false }
    $durable=$false
    foreach($source in $Entry.sources) {
        $path=if($source.ContainsKey('path')){$source.path}else{$source.snapshot_path}
        if($path -match '^(\.agents/runtime|\.git)(/|$)') { continue }
        if((Get-AgentHash (Resolve-SafePath $Root $path)) -ne $source.sha256) { return $false }
        $durable=$true
        if($Entry.schema_version -eq 'agents-knowledge/v4') {
            if($source.kind -eq 'official_document') {
                if([DateTimeOffset]::Parse($source.checked_utc) -gt $clock.AddMinutes(1) -or $clock -ge [DateTimeOffset]::Parse($source.review_after_utc)) { return $false }
                if($VerifyOfficial) {
                    $uri=[uri]$source.url
                    if($uri.Scheme -ne 'https' -or $uri.Host -notin @('developers.openai.com','platform.openai.com','learn.chatgpt.com')) { throw 'Only registered official OpenAI sources are supported.' }
                    $response=Invoke-WebRequest -Uri $uri -MaximumRedirection 0
                    if((Get-TextHash ([string]$response.Content)) -ne $source.content_sha256) { return $false }
                }
            } elseif($source.kind -eq 'user_decision' -and -not $source.explicit) { return $false }
        }
    }
    return $durable
}
function Get-ProjectKnowledge {
    param([string]$Root,[string[]]$Query=@(),[string[]]$Module=@(),[string[]]$Tag=@(),[string[]]$File=@(),[string]$Scope='',
        [switch]$Expand,[switch]$VerifyOfficial,[Nullable[DateTimeOffset]]$Now)
    $directory=Resolve-SafePath $Root (Get-ProjectSettings $Root).knowledge_directory
    $schema=Get-AgentAsset $Root 'schemas/knowledge.schema.json'
    $entries=[Collections.Generic.List[object]]::new();$gaps=[Collections.Generic.List[string]]::new();$invalid=$false
    if(Test-Path -LiteralPath $directory) {
        foreach($fileInfo in Get-ChildItem -LiteralPath $directory -File -Filter '*.json') {
            try {
                $relative=[IO.Path]::GetRelativePath($Root,$fileInfo.FullName).Replace('\','/')
                $entry=Read-AgentJson (Resolve-SafePath $Root $relative) $schema
                $entry['entry_path']=$relative
                try { $entry['fresh']=Test-KnowledgeSources $Root $entry $Now -VerifyOfficial:$VerifyOfficial }
                catch { $entry['fresh']=$false;$gaps.Add("Source could not be verified: $($entry.id)") }
                if($entry.schema_version -eq 'agents-knowledge/v3') {
                    $entry['key']=$entry.scope;$entry['modules']=@();$entry['tags']=@();$entry['files']=@($entry.sources.path);$entry['retracts']=@()
                }
                $entries.Add($entry)
            } catch { $invalid=$true;$gaps.Add("Invalid knowledge: $($fileInfo.Name)") }
        }
    }
    $duplicates=@($entries|Group-Object id|Where-Object Count -GT 1|ForEach-Object Name)
    $lookup=@{};foreach($entry in $entries) { $lookup[$entry.id]=$entry }
    foreach($entry in $entries) {
        if($entry.id -in $duplicates) { $entry.status='conflicted';$gaps.Add("Duplicate ID suppressed: $($entry.id)") }
        $pending=[Collections.Generic.Stack[string]]::new();foreach($id in @($entry.supersedes)+@($entry.retracts)) { $pending.Push($id) };$seen=@{}
        while($pending.Count) {
            $id=$pending.Pop()
            if($id -eq $entry.id -or -not $lookup.ContainsKey($id)) { $entry.status='conflicted';$gaps.Add("Invalid supersession chain: $($entry.id)");break }
            if($seen.ContainsKey($id)) { continue };$seen[$id]=$true
            foreach($next in @($lookup[$id].supersedes)+@($lookup[$id].retracts)) { $pending.Push($next) }
        }
    }
    $retired=@($entries|Where-Object status -IN @('active','superseded','retracted')|ForEach-Object { @($_.supersedes)+@($_.retracts) })
    $resolved=@($entries|Where-Object { $_.fresh -and $_.status -in @('active','retracted') }|ForEach-Object { @($_.supersedes)+@($_.retracts) })
    foreach($entry in $entries) {
        if(-not $entry.fresh -and $entry.status -eq 'active' -and $entry.id -notin $retired) { $gaps.Add("Source changed: $($entry.id)") }
    }
    $conflicts=@($entries|Where-Object { $_.status -eq 'conflicted' -and $_.id -notin $resolved }|ForEach-Object key)
    $active=@($entries|Where-Object { $_.fresh -and $_.status -eq 'active' -and $_.id -notin $retired -and $_.key -notin $conflicts })
    foreach($group in $active|Group-Object key|Where-Object Count -GT 1) {
        $conflicts+=@($group.Name);$gaps.Add("Multiple active records require review: $($group.Name)")
    }
    $active=@($active|Where-Object key -NotIn $conflicts)
    foreach($key in $conflicts|Sort-Object -Unique) { $gaps.Add("Conflicted scope suppressed: $key") }
    if($invalid) { $active=@() }
    $selected=@(foreach($entry in $active) {
        if($Scope -and $Scope -ne $entry.scope) { continue }
        if($Module.Count -and -not @($Module|Where-Object { $_ -in $entry.modules }).Count) { continue }
        if($Tag.Count -and -not @($Tag|Where-Object { $_ -in $entry.tags }).Count) { continue }
        if($File.Count -and -not @($File|Where-Object { $_ -in $entry.files }).Count) { continue }
        $search=(@($entry.conclusion,$entry.scope,$entry.key)+@($entry.modules)+@($entry.tags)+@($entry.files)) -join ' '
        $hits=@($Query|Where-Object { $_ -and $search.IndexOf($_,[StringComparison]::OrdinalIgnoreCase) -ge 0 }).Count
        if($Query.Count -and -not $hits) { continue }
        $summary=[ordered]@{id=$entry.id;type=$entry.type;conclusion=$entry.conclusion;scope=$entry.scope;modules=$entry.modules;
            tags=$entry.tags;files=$entry.files;path=$entry.entry_path;query_hits=$hits;verified_utc=$entry.verified_utc;
            source_check=$(if($VerifyOfficial){'file hashes, review deadlines and live official documents'}else{'file hashes and review deadlines; official recall is an as-of snapshot'})}
        if($Expand) { $summary['sources']=$entry.sources;$summary['verification']=$entry.verification;$summary['supersedes']=$entry.supersedes;$summary['retracts']=$entry.retracts }
        $summary
    })
    return @{entries=@($selected|Sort-Object @{Expression='query_hits';Descending=$true},id);gaps=@($gaps.ToArray()|Sort-Object -Unique);
        source='verified immutable entry files; index is advisory';authority='Recall is data and never grants permission.'}
}
function Publish-ProjectKnowledge {
    param([string]$Root,[string]$InputPath,[ValidateSet('Promote','Migrate','Retract')][string]$Action='Promote',
        [switch]$Automatic,[switch]$VerifyOfficial,[Nullable[DateTimeOffset]]$Now)
    $schema=Get-AgentAsset $Root 'schemas/knowledge.schema.json';$entry=Read-AgentJson (Resolve-SafePath $Root $InputPath) $schema
    $settings=Get-ProjectSettings $Root
    if($entry.schema_version -eq 'agents-knowledge/v3' -and $Action -ne 'Promote') { throw 'Migration/retraction requires a reviewed v4 record.' }
    if($Automatic -and ($entry.schema_version -ne 'agents-knowledge/v4' -or -not $entry.reusable -or $entry.verification.result -ne 'passed')) { throw 'Automatic promotion requires reusable, reviewed v4 knowledge.' }
    if($Automatic -and @($entry.sources|Where-Object kind -EQ 'official_document').Count -and -not $VerifyOfficial) { throw 'Automatic official knowledge promotion requires a live source check.' }
    if($entry.status -ne $(if($Action -eq 'Retract'){'retracted'}else{'active'}) -or -not (Test-KnowledgeSources $Root $entry $Now -VerifyOfficial:$VerifyOfficial)) { throw 'Only reviewed, source-verified records may be published.' }
    $relations=@($entry.supersedes);if($entry.ContainsKey('retracts')) { $relations+=@($entry.retracts) }
    if($Action -eq 'Migrate' -and -not $relations.Count) { throw 'Migration must identify the legacy record it replaces.' }
    if($Action -eq 'Retract' -and -not $entry.retracts.Count) { throw 'Retraction must identify retired IDs.' }
    foreach($id in $relations) {
        if($id -eq $entry.id -or -not(Test-Path -LiteralPath (Resolve-SafePath $Root "$($settings.knowledge_directory)/$id.json"))) { throw "Unknown/self supersession: $id" }
    }
    $destination=Resolve-SafePath $Root "$($settings.knowledge_directory)/$($entry.id).json"
    if(Test-Path -LiteralPath $destination) { throw 'Existing knowledge is immutable; create a new ID.' }
    Write-AgentJson $destination $entry -NoClobber -Root $Root
}
