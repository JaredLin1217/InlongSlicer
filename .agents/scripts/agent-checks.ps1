. "$PSScriptRoot/agent-core.ps1"
function Test-AgentSyntax([string]$Root,[string[]]$Files) {
    foreach($p in $Files | Where-Object { $_ -like '*.ps1' }) {
        $tokens=$null; $errors=$null
        $null=[Management.Automation.Language.Parser]::ParseFile((Resolve-SafePath $Root $p),[ref]$tokens,[ref]$errors)
        if($errors.Count) { throw "AST failed for $p : $($errors.Message -join '; ')" }
    }
}
function Test-AgentSchemas([string]$Root,[string[]]$Files) {
    $schemas=@{'agents-knowledge/v3'='knowledge';'agents-task/v3'='task-state';'agents-managed/v3'='managed';
        'agents-sources/v3'='sources';'agents-runtime-evidence/v5'='release-evidence';'agents-project/v3'='project';'agents-deployment/v3'='deployment'}
    foreach($p in $Files | Where-Object { $_ -like '*.json' }) {
        $data=Read-AgentJson (Resolve-SafePath $Root $p)
        if($data -is [Collections.IDictionary] -and $data.Contains('schema_version') -and $schemas.ContainsKey($data.schema_version)) {
            $schema=Get-AgentAsset $Root "schemas/$($schemas[$data.schema_version]).schema.json"
            $null=Read-AgentJson (Resolve-SafePath $Root $p) $schema
        }
    }
}
function Test-AgentOwnership([string]$Root,[string]$Scope) {
    if($Scope -eq 'Provider') {
        . "$PSScriptRoot/agent-deployment.ps1"
        foreach($layout in @('root-layout','dot-agents-layout')) { $null=@(Get-DeploymentEntries $Root $layout) }
        return
    }
    $manifest=Read-AgentJson (Resolve-SafePath $Root '.agents/managed.json') (Get-AgentAsset $Root 'schemas/managed.schema.json')
    $seen=@{}
    foreach($file in $manifest.files) {
        if($seen.ContainsKey($file.path)) { throw "Duplicate owned path: $($file.path)" }
        $seen[$file.path]=$true
        if((Get-AgentHash (Resolve-SafePath $Root $file.path)) -ne $file.sha256) { throw "Managed content changed: $($file.path)" }
    }
}
function Test-AgentKnowledge([string]$Root) {
    $script=Get-AgentAsset $Root 'scripts/project-memory.ps1'
    $report=(& $script -Action Recall -Root $Root)|ConvertFrom-Json -AsHashtable
    if($report.gaps.Count) { throw ($report.gaps -join '; ') }
}
function Test-AgentSources([string]$Root) {
    $data=Read-AgentJson (Get-AgentAsset $Root 'docs/agents/sources.json') (Get-AgentAsset $Root 'schemas/sources.schema.json')
    $seen=@{}
    foreach($entry in $data.sources) {
        if($seen.ContainsKey($entry.id)) { throw "Duplicate source: $($entry.id)" }; $seen[$entry.id]=$true
        $checked=[DateTime]::ParseExact($entry.checked,'yyyy-MM-dd',[Globalization.CultureInfo]::InvariantCulture)
        $due=[DateTime]::ParseExact($entry.next_review_due,'yyyy-MM-dd',[Globalization.CultureInfo]::InvariantCulture)
        if($due -ne $checked.AddDays($entry.refresh_interval_days) -or $checked -gt [DateTime]::UtcNow.Date) { throw "Invalid freshness dates: $($entry.id)" }
        if($due -lt [DateTime]::UtcNow.Date) { throw "Source requires review: $($entry.id)" }
    }
}
function Test-AgentSizes([string]$Root) {
    $production=@(Get-AgentFiles $Root | Where-Object { $_ -like 'scripts/*.ps1' -and $_ -notlike 'scripts/test-*' })
    $bytes=($production|ForEach-Object { (Get-Item -LiteralPath (Resolve-SafePath $Root $_)).Length }|Measure-Object -Sum).Sum
    $catalog=Read-AgentJson (Join-Path $Root 'docs/agents/deployment.json')
    $deployBytes=($catalog.files.source|Sort-Object -Unique|ForEach-Object {(Get-Item -LiteralPath (Resolve-SafePath $Root $_)).Length}|Measure-Object -Sum).Sum
    if((Get-Item -LiteralPath (Join-Path $Root 'AGENTS.md')).Length -gt 2048) { throw 'Root rules exceed 2 KiB.' }
    if((Get-Item -LiteralPath (Join-Path $Root 'scripts/validate.ps1')).Length -gt 15360) { throw 'Validator exceeds 15 KiB.' }
    "production_bytes=$bytes; deployable_bytes=$deployBytes"
}
function Test-AgentEvidence([string]$Root) {
    $path=(Read-AgentJson (Join-Path $Root 'agents.json')).release_evidence
    $full=Resolve-SafePath $Root $path
    if(-not(Test-Path -LiteralPath $full)) { return 'Release evidence absent; source checkpoint only, not release-ready.' }
    $entry=Read-AgentJson $full (Join-Path $Root 'schemas/release-evidence.schema.json')
    if($entry.excluded_paths.Count -ne 1 -or $entry.excluded_paths[0] -ne $path) { throw 'Unexpected evidence exclusions.' }
    $files=@(Get-AgentFiles $Root|Where-Object { $_ -ne $path })
    if($entry.validated_content_digest -ne (Get-SourceDigest $Root $files)) { return 'Evidence is stale; recapture after the next source checkpoint.' }
    $null=Invoke-AgentGit $Root @('merge-base','--is-ancestor',$entry.source_commit,'HEAD')
    $changes=@(Invoke-AgentGit $Root @('diff','--name-only',"$($entry.source_commit)..HEAD"))
    if(@($changes|Where-Object { $_ -ne $path }).Count) { throw 'Evidence commit range contains undeclared source changes.' }
    if($entry.result -ne 'passed' -or @($entry.commands|Where-Object result -NE 'passed').Count) { throw 'Release evidence contains failed checks.' }
    if(@($entry.commands|Group-Object id|Where-Object Count -GT 1).Count) { throw 'Duplicate evidence commands.' }
    'Evidence matches current source content.'
}
function Invoke-AgentChecks {
    param([string]$Root,[ValidateSet('Provider','Consumer')][string]$Scope,[ValidateSet('Changed','Checkpoint')][string]$Profile,[string[]]$Path=@())
    $started=[DateTime]::UtcNow
    $all=if($Scope -eq 'Provider'){ @(Get-AgentFiles $Root) }else{
        @((Read-AgentJson (Resolve-SafePath $Root '.agents/managed.json')).files.path)+@('.agents/managed.json')
    }
    if($Profile -eq 'Checkpoint') { $selected=$all }
    else {
        if(-not $Path.Count) {
            $Path=@(Invoke-AgentGit $Root @('diff','--name-only'))+@(Invoke-AgentGit $Root @('diff','--cached','--name-only'))+@(Invoke-AgentGit $Root @('ls-files','--others','--exclude-standard'))
        }
        $selected=@($all|Where-Object { $_ -in $Path })
    }
    $impact=if($Profile -eq 'Checkpoint'){$all}else{$Path}
    $receiptPaths=if($Profile -eq 'Checkpoint'){$all}else{@($selected)+@($all|Where-Object { $_ -match '^(?:\.agents/)?scripts/|^agents\.json$|^\.agents/managed\.json$' })}
    $inputDigest=Get-SourceDigest $Root $receiptPaths
    $checks=[Collections.Generic.List[object]]::new()
    $checks.Add(@{id='syntax';command='Test-AgentSyntax';action={ Test-AgentSyntax $Root $selected }})
    $checks.Add(@{id='json-schema';command='Test-AgentSchemas';action={ Test-AgentSchemas $Root $selected }})
    if($Profile -eq 'Checkpoint' -or @($impact|Where-Object { $_ -match '^(\.agents/|scripts/|schemas/|docs/agents/)' }).Count) {
        $checks.Add(@{id='ownership';command='Test-AgentOwnership';action={ Test-AgentOwnership $Root $Scope }})
    }
    if($Profile -eq 'Checkpoint' -or @($selected|Where-Object { $_ -match 'memory|AGENTS\.md' }).Count) {
        $checks.Add(@{id='knowledge';command='Test-AgentKnowledge';action={ Test-AgentKnowledge $Root }})
    }
    if($Profile -eq 'Checkpoint' -or $selected -contains 'docs/agents/sources.json') {
        $checks.Add(@{id='sources';command='Test-AgentSources';action={ Test-AgentSources $Root }})
    }
    if($Scope -eq 'Provider' -and ($Profile -eq 'Checkpoint' -or @($impact|Where-Object { $_ -match '^(scripts|tests|schemas|docs/agents)/' }).Count)) {
        $checks.Add(@{id='size';command='Test-AgentSizes';action={ Test-AgentSizes $Root }})
        $checks.Add(@{id='regression';command='pwsh -NoProfile -File tests/test-workflow.ps1';action={
            $out=@(& pwsh -NoProfile -File (Join-Path $Root 'tests/test-workflow.ps1') 2>&1)
            if($LASTEXITCODE -ne 0) { throw ($out -join "`n") }; $out
        }})
    }
    if($Scope -eq 'Provider' -and $Profile -eq 'Checkpoint') {
        $checks.Add(@{id='evidence';command='Test-AgentEvidence';action={ Test-AgentEvidence $Root }})
        $checks.Add(@{id='package';command='export-release-package.ps1';action={ & (Join-Path $Root 'scripts/export-release-package.ps1') }})
    }
    if($Scope -eq 'Consumer') {
        foreach($check in (Get-ProjectSettings $Root).consumer_checks) {
            if(-not $check.ContainsKey('path') -or -not $check.ContainsKey('arguments')) { throw 'Invalid consumer command registration.' }
            $script=Resolve-SafePath $Root $check.path
            if([IO.Path]::GetExtension($script) -ne '.ps1') { throw 'Register a project-owned PowerShell test wrapper.' }
            $argsCopy=@($check.arguments); $scriptCopy=$script
            $action={
                $out=@(& pwsh -NoProfile -File $scriptCopy @argsCopy 2>&1)
                if($LASTEXITCODE -ne 0) { throw ($out -join "`n") }; $out
            }.GetNewClosure()
            $checks.Add(@{id="product:$($check.path)";command="pwsh -NoProfile -File $($check.path) $($argsCopy -join ' ')";action=$action})
        }
    }
    $checks.Add(@{id='diff';command='git diff [--cached] --check -- <selected paths> (working tree and index)';action={
        if($selected.Count) {
            $null=Invoke-AgentGit $Root (@('diff','--check','--')+$selected)
            $null=Invoke-AgentGit $Root (@('diff','--cached','--check','--')+$selected)
        }
    }})
    $receipts=[Collections.Generic.List[object]]::new(); $seen=@{}
    foreach($check in $checks) {
        if($seen.ContainsKey($check.id)) { throw "Duplicate checkpoint action: $($check.id)" }; $seen[$check.id]=$true
        $timer=[Diagnostics.Stopwatch]::StartNew(); $result='passed'; $exit=0; $details=@()
        try { $details=@(& $check.action|ForEach-Object {[string]$_}) }
        catch { $result='failed';$exit=1;$details=@($_.Exception.Message) }
        $timer.Stop()
        $receipts.Add([ordered]@{id=$check.id;command=$check.command;result=$result;exit_code=$exit;duration_ms=$timer.ElapsedMilliseconds;retry_count=0;details=$details})
    }
    if($inputDigest -ne (Get-SourceDigest $Root $receiptPaths)) { throw 'Validation inputs changed during checkpoint; receipts cannot be reused.' }
    return @{started_utc=$started.ToString('o');finished_utc=[DateTime]::UtcNow.ToString('o');scope=$Scope;profile=$Profile;input_digest=$inputDigest;
        paths=$selected;host=@{powershell=$PSVersionTable.PSVersion.ToString();git=[string](& git --version)};
        checks=@($receipts.ToArray());passed=(@($receipts|Where-Object result -NE 'passed').Count -eq 0)}
}
