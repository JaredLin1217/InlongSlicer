. "$PSScriptRoot/agent-runtime.ps1"
function Test-AgentSyntax([string]$Root,[string[]]$Files) {
    foreach($p in $Files | Where-Object { $_ -like '*.ps1' }) {
        $tokens=$null; $errors=$null
        $null=[Management.Automation.Language.Parser]::ParseFile((Resolve-SafePath $Root $p),[ref]$tokens,[ref]$errors)
        if($errors.Count) { throw "AST failed for $p : $($errors.Message -join '; ')" }
    }
}
function Test-AgentSchemas([string]$Root,[string[]]$Files) {
    $schemas=@{'agents-knowledge/v3'='knowledge';'agents-knowledge/v4'='knowledge';'agents-task/v3'='task-state';'agents-task/v4'='task-state';'agents-managed/v3'='managed';
        'agents-sources/v3'='sources';'agents-runtime-evidence/v5'='release-evidence';'agents-runtime-evidence/v6'='release-evidence';'agents-project/v4'='project';'agents-deployment/v3'='deployment';
        'agents-runtime-policy/v4'='runtime-policy';'agents-artifact/v4'='runtime-artifact';'agents-validation/v4'='validation-report';'agents-source-review/v4'='source-review';'agents-project-decision/v4'='project-decision'}
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
    $seen=@{};$review=@()
    foreach($entry in $data.sources) {
        if($seen.ContainsKey($entry.id)) { throw "Duplicate source: $($entry.id)" }; $seen[$entry.id]=$true
        $checked=[DateTime]::ParseExact($entry.checked,'yyyy-MM-dd',[Globalization.CultureInfo]::InvariantCulture)
        $due=[DateTime]::ParseExact($entry.next_review_due,'yyyy-MM-dd',[Globalization.CultureInfo]::InvariantCulture)
        if($due -ne $checked.AddDays($entry.refresh_interval_days) -or $checked -gt [DateTime]::UtcNow.Date) { throw "Invalid freshness dates: $($entry.id)" }
        if($due -le [DateTime]::UtcNow.Date) { $review+=@("Source requires review: $($entry.id)") }
    }
    return @{status=$(if($review.Count){'needs_review'}else{'passed'});details=$review}
}
function Test-AgentSizes([string]$Root) {
    $production=@(Get-AgentFiles $Root | Where-Object { $_ -like 'scripts/*.ps1' -and $_ -notlike 'scripts/test-*' })
    $bytes=($production|ForEach-Object { (Get-Item -LiteralPath (Resolve-SafePath $Root $_)).Length }|Measure-Object -Sum).Sum
    $catalog=Read-AgentJson (Join-Path $Root 'docs/agents/deployment.json')
    $deployBytes=($catalog.files.source|Sort-Object -Unique|ForEach-Object {(Get-Item -LiteralPath (Resolve-SafePath $Root $_)).Length}|Measure-Object -Sum).Sum
    "production_bytes=$bytes; deployable_bytes=$deployBytes"
}
function Test-AgentEvidence([string]$Root,[switch]$Capture) {
    if($Capture) { return @{status='not_applicable';details=@('Capture validates committed source; new evidence is checked after generation.')} }
    $path=(Read-AgentJson (Join-Path $Root 'agents.json')).release_evidence
    $full=Resolve-SafePath $Root $path
    if(-not(Test-Path -LiteralPath $full)) { return @{status='needs_review';details=@('Release evidence absent; source checkpoint only.')} }
    $entry=Read-AgentJson $full (Join-Path $Root 'schemas/release-evidence.schema.json')
    if($entry.excluded_paths.Count -ne 1 -or $entry.excluded_paths[0] -ne $path) { throw 'Unexpected evidence exclusions.' }
    $files=@(Get-AgentFiles $Root|Where-Object { $_ -ne $path })
    if($entry.schema_version -ne 'agents-runtime-evidence/v6' -or $entry.workflow_version -ne (Read-AgentJson (Join-Path $Root 'agents.json')).version) { return @{status='needs_review';details=@('Release evidence belongs to an older contract.')} }
    if([DateTimeOffset]::UtcNow -ge [DateTimeOffset]::Parse($entry.review_after_utc)) { return @{status='needs_review';details=@('Release evidence requires review.')} }
    if($entry.validated_content_digest -ne (Get-SourceDigest $Root $files)) { return @{status='needs_review';details=@('Release evidence does not match current source.')} }
    if(@(Invoke-AgentGit $Root @('status','--porcelain')|Where-Object { $_.Substring(3) -ne $path }).Count) { return @{status='needs_review';details=@('Source has uncommitted changes.')} }
    $null=Invoke-AgentGit $Root @('merge-base','--is-ancestor',$entry.source_commit,'HEAD')
    $changes=@(Invoke-AgentGit $Root @('diff','--name-only',"$($entry.source_commit)..HEAD"))
    if(@($changes|Where-Object { $_ -ne $path }).Count) { throw 'Evidence commit range contains undeclared source changes.' }
    if($entry.result -ne 'passed' -or @($entry.commands|Where-Object { $_.result -ne 'passed' -and -not($_.id -eq 'evidence' -and $_.result -eq 'not_applicable') }).Count) { throw 'Release evidence contains incomplete or failed checks.' }
    if(@($entry.commands|Group-Object id|Where-Object Count -GT 1).Count) { throw 'Duplicate evidence commands.' }
    foreach($id in @('syntax','json-schema','ownership','knowledge','sources','size','regression','regression-v4','evidence','package','diff')) {
        if($id -notin $entry.commands.id) { throw "Required evidence check missing: $id" }
    }
    return @{status='passed';details=@('Evidence matches committed source content and current contract.')}
}
function Invoke-AgentChecks {
    param([string]$Root,[ValidateSet('Provider','Consumer')][string]$Scope,[ValidateSet('Changed','Checkpoint')][string]$Profile,
        [string[]]$Path=@(),[switch]$RequireReleaseReady,[switch]$EvidenceCapture)
    $started=[DateTimeOffset]::UtcNow;$run=New-AgentRun $Root "Validation: $Scope/$Profile" 'validate'
    $artifact=Register-AgentArtifact $Root $run.id "$($run.path)/validation" 'validation' 'Validation receipt and diagnostics'
    $receiptPath="$($artifact.path)/report.json";$receipts=[Collections.Generic.List[object]]::new()
    $selected=@();$inputDigest=Get-TextHash '';$hostInfo=@{powershell=$PSVersionTable.PSVersion.ToString();git=[string](& git --version)}
    $failed=$false;$releaseReady=$false
    try {
        if($RequireReleaseReady -and ($Scope -ne 'Provider' -or $Profile -ne 'Checkpoint')) { throw 'RequireReleaseReady requires a Provider Checkpoint.' }
        $all=if($Scope -eq 'Provider'){@(Get-AgentFiles $Root)}else{@((Read-AgentJson (Resolve-SafePath $Root '.agents/managed.json')).files.path)+@('.agents/managed.json')}
        if($Profile -eq 'Checkpoint') { $selected=$all }
        else {
            if(-not $Path.Count) { $Path=@(Invoke-AgentGit $Root @('diff','--name-only'))+@(Invoke-AgentGit $Root @('diff','--cached','--name-only'))+@(Invoke-AgentGit $Root @('ls-files','--others','--exclude-standard')) }
            $selected=@($all|Where-Object { $_ -in $Path })
        }
        $impact=if($Profile -eq 'Checkpoint'){$all}else{$Path}
        $receiptPaths=if($Profile -eq 'Checkpoint'){$all}else{@($selected)+@($all|Where-Object { $_ -match '^(?:\.agents/)?(scripts|schemas|docs/agents)/|^agents\.json$|^\.agents/managed\.json$' })}
        $inputDigest=Get-SourceDigest $Root $receiptPaths
        $checks=[Collections.Generic.List[object]]::new()
        $checks.Add(@{id='syntax';command='Test-AgentSyntax';action={Test-AgentSyntax $Root $selected}})
        $checks.Add(@{id='json-schema';command='Test-AgentSchemas';action={Test-AgentSchemas $Root $selected}})
        if($Profile -eq 'Checkpoint' -or @($impact|Where-Object { $_ -match '^(\.agents/|scripts/|schemas/|docs/agents/)' }).Count) {
            $checks.Add(@{id='ownership';command='Test-AgentOwnership';action={Test-AgentOwnership $Root $Scope}})
        }
        if($Profile -eq 'Checkpoint' -or @($selected|Where-Object { $_ -match 'memory|AGENTS\.md' }).Count) {
            $checks.Add(@{id='knowledge';command='Test-AgentKnowledge';action={Test-AgentKnowledge $Root}})
        }
        if($Profile -eq 'Checkpoint' -or @($selected|Where-Object { $_ -match 'docs/agents/sources\.json$' }).Count) {
            $checks.Add(@{id='sources';command='Test-AgentSources';action={Test-AgentSources $Root}})
        }
        if($Scope -eq 'Provider' -and ($Profile -eq 'Checkpoint' -or @($impact|Where-Object { $_ -match '^(scripts|tests|schemas|docs/agents)/' }).Count)) {
            $checks.Add(@{id='size';command='Test-AgentSizes (measurement only)';action={Test-AgentSizes $Root}})
            foreach($test in @(@{id='regression';path='tests/test-workflow.ps1'},@{id='regression-v4';path='tests/test-v4.ps1'})) {
                $scriptCopy=Resolve-SafePath $Root $test.path
                $action={ Invoke-AgentChild $Root $run.id {
                    $out=@(& pwsh -NoProfile -File $scriptCopy 2>&1)
                    if($LASTEXITCODE -ne 0) { throw ($out -join "`n") };$out
                } }.GetNewClosure()
                $checks.Add(@{id=$test.id;command="pwsh -NoProfile -File $($test.path)";action=$action})
            }
        }
        if($Scope -eq 'Provider' -and $Profile -eq 'Checkpoint') {
            $checks.Add(@{id='evidence';command='Test-AgentEvidence';action={Test-AgentEvidence $Root -Capture:$EvidenceCapture}})
            $checks.Add(@{id='package';command='export-release-package.ps1';action={& (Join-Path $Root 'scripts/export-release-package.ps1')}})
        }
        if($Scope -eq 'Consumer') {
            foreach($check in (Get-ProjectSettings $Root).consumer_checks) {
                $scriptCopy=Resolve-SafePath $Root $check.path;$argsCopy=@($check.arguments)
                if([IO.Path]::GetExtension($scriptCopy) -ne '.ps1') { throw 'Register a project-owned PowerShell test wrapper.' }
                $action={ Invoke-AgentChild $Root $run.id {
                    $out=@(& pwsh -NoProfile -File $scriptCopy @argsCopy 2>&1)
                    if($LASTEXITCODE -ne 0) { throw ($out -join "`n") };$out
                } }.GetNewClosure()
                $checks.Add(@{id="product:$($check.path)";command="pwsh -NoProfile -File $($check.path) $($argsCopy -join ' ')";action=$action})
            }
        }
        $checks.Add(@{id='diff';command='git diff --check (working tree and index)';action={
            if($selected.Count) { $null=Invoke-AgentGit $Root (@('diff','--check','--')+$selected);$null=Invoke-AgentGit $Root (@('diff','--cached','--check','--')+$selected) }
        }})
        $seen=@{}
        foreach($check in $checks) {
            if($seen.ContainsKey($check.id)) { throw "Duplicate checkpoint action: $($check.id)" };$seen[$check.id]=$true
            $timer=[Diagnostics.Stopwatch]::StartNew();$result='passed';$exit=0;$details=@()
            try {
                $output=@(& $check.action)
                if($output.Count -eq 1 -and $output[0] -is [Collections.IDictionary] -and $output[0].Contains('status')) {
                    $result=$output[0].status;$details=@($output[0].details)
                } else { $details=@($output|ForEach-Object {[string]$_}) }
            } catch { $result='failed';$exit=1;$details=@($_.Exception.Message) }
            $timer.Stop()
            $receipts.Add([ordered]@{id=$check.id;command=$check.command;result=$result;exit_code=$exit;duration_ms=$timer.ElapsedMilliseconds;retry_count=0;details=$details})
        }
        if($inputDigest -ne (Get-SourceDigest $Root $receiptPaths)) { throw 'Validation inputs changed during the run; receipts cannot be reused.' }
        $failed=@($receipts|Where-Object result -EQ 'failed').Count -gt 0
        $releaseReady=($Scope -eq 'Provider' -and $Profile -eq 'Checkpoint' -and -not $EvidenceCapture -and
            @($receipts|Where-Object { $_.result -notin @('passed','not_applicable') }).Count -eq 0 -and
            @($receipts|Where-Object { $_.id -eq 'evidence' -and $_.result -eq 'passed' }).Count -eq 1)
        if($RequireReleaseReady -and -not $releaseReady) {
            $receipts.Add(@{id='release-gate';command='RequireReleaseReady';result='failed';exit_code=1;duration_ms=0;retry_count=0;details=@('Release evidence or required source reviews are incomplete.')});$failed=$true
        }
    } catch {
        $failed=$true
        $receipts.Add(@{id='fatal';command='checkpoint setup/integrity';result='failed';exit_code=1;duration_ms=0;retry_count=0;details=@($_.Exception.Message)})
    }
    $report=[ordered]@{schema_version='agents-validation/v4';run_id=$run.id;receipt_path=$receiptPath;started_utc=$started.ToString('o');finished_utc=[DateTimeOffset]::UtcNow.ToString('o');
        scope=$Scope;profile=$Profile;input_digest=$inputDigest;paths=@($selected);host=$hostInfo;checks=@($receipts.ToArray());
        result=$(if($failed){'failed'}elseif(@($receipts|Where-Object result -EQ 'needs_review').Count){'needs_review'}else{'passed'});
        passed=(-not $failed);release_ready=$releaseReady;require_release_ready=[bool]$RequireReleaseReady}
    $schema=Get-AgentAsset $Root 'schemas/validation-report.schema.json'
    if(-not(Test-Json -Json ($report|ConvertTo-Json -Depth 40) -SchemaFile $schema)) { throw 'Receipt schema invalid; cannot report validation success.' }
    Write-AgentJson (Resolve-SafePath $Root $receiptPath) $report -Root $Root
    $persisted=Read-AgentJson (Resolve-SafePath $Root $receiptPath) $schema
    $expectedText=(($report|ConvertTo-Json -Depth 80).Replace("`r`n","`n")+"`n")
    if((Get-AgentHash (Resolve-SafePath $Root $receiptPath)) -ne (Get-TextHash $expectedText)) { throw 'Saved receipt differs from report.' }
    $null=Complete-AgentRun $Root $run.id $(if($failed){'failed'}else{'completed'})
    return $report
}
