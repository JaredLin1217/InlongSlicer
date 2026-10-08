. "$PSScriptRoot/agent-core.ps1"

function Get-RuntimeClock([Nullable[DateTimeOffset]]$Now) {
    if($null -ne $Now) { return $Now.ToUniversalTime() }
    return [DateTimeOffset]::UtcNow
}
function Get-RuntimePath([string]$Root,[string]$Path) {
    return Resolve-SafePath $Root ".agents/runtime/$Path"
}
function Assert-RuntimeId([string]$Id) {
    if($Id -notmatch '^[A-Za-z0-9_-]+$') { throw 'Invalid runtime ID.' }
}
function Get-RuntimePolicy([string]$Root) {
    $policy=Read-AgentJson (Join-Path $PSScriptRoot '../docs/agents/runtime-policy.json') (Join-Path $PSScriptRoot '../schemas/runtime-policy.schema.json')
    return $policy
}
function Enter-RuntimeLock([string]$Root,[string]$Name='runtime') {
    Assert-RuntimeId $Name
    $path=Get-RuntimePath $Root "state/locks/$Name.lock"
    [IO.Directory]::CreateDirectory((Split-Path -Parent $path))|Out-Null
    $stream=[IO.File]::Open($path,[IO.FileMode]::CreateNew,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    $owner=@{pid=$PID;process_start_utc=(Get-Process -Id $PID).StartTime.ToUniversalTime().ToString('o');nonce=[guid]::NewGuid().ToString('N')}
    $bytes=[Text.Encoding]::UTF8.GetBytes(($owner|ConvertTo-Json -Compress));$stream.Write($bytes);$stream.Flush($true)
    return @{stream=$stream;path=$path}
}
function Exit-RuntimeLock($Lock) {
    $Lock.stream.Dispose(); Remove-Item -LiteralPath $Lock.path
}
function Get-RuntimeFootprint([string]$Root,[string]$Path,[switch]$PathsOnly) {
    $full=Resolve-SafePath $Root $Path
    if(-not(Test-Path -LiteralPath $full)) { throw "Missing artifact: $Path" }
    $records=[Collections.Generic.List[object]]::new()
    $pending=[Collections.Generic.Stack[string]]::new();$pending.Push($full)
    while($pending.Count) {
        $current=$pending.Pop();$attributes=[IO.File]::GetAttributes($current)
        $relative=[IO.Path]::GetRelativePath($Root,$current).Replace('\','/')
        if($attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked artifact content: $relative" }
        if($attributes -band [IO.FileAttributes]::Directory) {
            $records.Add([ordered]@{path=$relative;kind='directory';sha256=''})
            foreach($child in [IO.Directory]::EnumerateFileSystemEntries($current)) { $pending.Push($child) }
        } else { $records.Add([ordered]@{path=$relative;kind='file';sha256=$(if($PathsOnly){''}else{Get-AgentHash $current})}) }
    }
    $items=@($records.ToArray()|Sort-Object { $_.path })
    return @{items=$items;digest=(Get-TextHash ($items|ConvertTo-Json -Depth 8 -Compress))}
}
function Test-RuntimeArtifactFootprint($Entry,$Footprint) {
    if($Entry.sha256 -eq $Footprint.digest) { return $true }
    # Older records hashed traversal order. Accept that order only when its
    # recorded digest and every current path, kind and file hash still match.
    $recorded=@(foreach($item in $Entry.items) {
        [ordered]@{path=[string]$item.path;kind=[string]$item.kind;sha256=[string]$item.sha256}
    })
    if((Get-TextHash ($recorded|ConvertTo-Json -Depth 8 -Compress)) -ne $Entry.sha256) { return $false }
    $canonical=@($recorded|Sort-Object { $_.path })
    return (Get-TextHash ($canonical|ConvertTo-Json -Depth 8 -Compress)) -eq $Footprint.digest
}
function Read-RuntimeArtifacts([string]$Root) {
    $directory=Get-RuntimePath $Root 'ledger/artifacts'
    if(-not(Test-Path -LiteralPath $directory)) { return @() }
    $records=@(Get-ChildItem -LiteralPath $directory -File -Filter '*.json'|ForEach-Object {
        $relative=[IO.Path]::GetRelativePath($Root,$_.FullName).Replace('\','/')
        Read-AgentJson (Resolve-SafePath $Root $relative) (Join-Path $PSScriptRoot '../schemas/runtime-artifact.schema.json')
    })
    if(@($records|Group-Object id|Where-Object Count -GT 1).Count) { throw 'Duplicate artifact IDs.' }
    foreach($entry in $records) {
        if($entry.path -notlike ".agents/runtime/runs/$($entry.run_id)/*") { throw 'Artifact outside its run.' }
        $null=Resolve-SafePath $Root $entry.path
    }
    return $records
}
function Add-RuntimeEvent([string]$Root,[string]$Id,[string]$Action,$Details) {
    Assert-RuntimeId $Id
    $path=Get-RuntimePath $Root "ledger/events/$([guid]::NewGuid().ToString('N')).json"
    Write-AgentJson $path @{schema_version='agents-runtime-event/v4';id=$Id;action=$Action;utc=[DateTimeOffset]::UtcNow.ToString('o');details=$Details} -NoClobber -Root $Root
}
function New-AgentRun {
    param([string]$Root,[string]$Purpose,[string]$Creator='Agents',[string]$TaskId='', [Nullable[DateTimeOffset]]$Now)
    if(-not $Purpose) { throw 'Run purpose is required.' }
    $id='run-'+[guid]::NewGuid().ToString('N');$path=".agents/runtime/runs/$id"
    $run=[ordered]@{schema_version='agents-run/v4';id=$id;path=$path;creator=$Creator;purpose=$Purpose;task_id=$TaskId;
        created_utc=(Get-RuntimeClock $Now).ToString('o');completed_utc=$null;status='active'}
    [IO.Directory]::CreateDirectory((Resolve-SafePath $Root $path))|Out-Null
    Write-AgentJson (Get-RuntimePath $Root "ledger/runs/$id.json") $run -NoClobber -Root $Root
    return $run
}
function Register-AgentArtifact {
    param([string]$Root,[string]$RunId,[string]$Path,[ValidateSet('scratch','package','validation','task','backup')][string]$Category,
        [string]$Purpose,[string]$Creator='Agents',[string[]]$Dependencies=@(),[string[]]$RetainReasons=@(),[Nullable[DateTimeOffset]]$Now)
    Assert-RuntimeId $RunId
    $run=Read-AgentJson (Get-RuntimePath $Root "ledger/runs/$RunId.json")
    if($run.status -ne 'active') { throw 'Only active runs accept new artifacts.' }
    if($run.id -ne $RunId -or $run.path -ne ".agents/runtime/runs/$RunId" -or $Path -notlike "$($run.path)/*") { throw 'Artifacts must be within their canonical run.' }
    if([IO.Path]::GetRelativePath($Root,(Resolve-SafePath $Root $Path)).Replace('\','/') -cne $Path) { throw 'Noncanonical artifact path.' }
    if(-not $Purpose) { throw 'Artifact purpose is required.' }
    $records=@(Read-RuntimeArtifacts $Root)
    foreach($other in $records|Where-Object status -NE 'deleted') {
        if($Path -eq $other.path -or $Path.StartsWith($other.path+'/',[StringComparison]::OrdinalIgnoreCase) -or $other.path.StartsWith($Path+'/',[StringComparison]::OrdinalIgnoreCase)) { throw 'Artifact paths overlap.' }
    }
    foreach($dependency in $Dependencies) { if($dependency -notin (Get-AgentIds @($records|Where-Object status -NE 'deleted'))) { throw "Unknown or deleted dependency: $dependency" } }
    $full=Resolve-SafePath $Root $Path
    if(-not(Test-Path -LiteralPath $full)) { [IO.Directory]::CreateDirectory($full)|Out-Null }
    $footprint=Get-RuntimeFootprint $Root $Path
    $entry=[ordered]@{schema_version='agents-artifact/v4';id='artifact-'+[guid]::NewGuid().ToString('N');run_id=$RunId;task_id=$run.task_id;
        creator=$Creator;purpose=$Purpose;category=$Category;path=$Path;created_utc=(Get-RuntimeClock $Now).ToString('o');
        completed_utc=$null;expires_utc=$null;status='active';sha256=$footprint.digest;items=$footprint.items;
        dependencies=@($Dependencies);retain_reasons=@($RetainReasons);deleted_utc=$null}
    Write-AgentJson (Get-RuntimePath $Root "ledger/artifacts/$($entry.id).json") $entry -NoClobber -Root $Root
    Add-RuntimeEvent $Root $entry.id 'registered' @{path=$Path;purpose=$Purpose}
    return $entry
}
function Complete-AgentRun {
    param([string]$Root,[string]$RunId,[ValidateSet('completed','failed','interrupted')][string]$Status='completed',[Nullable[DateTimeOffset]]$Now)
    Assert-RuntimeId $RunId
    $lock=Enter-RuntimeLock $Root
    try {
        $runPath=Get-RuntimePath $Root "ledger/runs/$RunId.json";$run=Read-AgentJson $runPath
        if($run.status -ne 'active') { throw 'Run is already closed; use explicit retirement to resolve a failure.' }
        $clock=Get-RuntimeClock $Now;$policy=Get-RuntimePolicy $Root
        $entries=@(Read-RuntimeArtifacts $Root|Where-Object run_id -EQ $RunId)
        $prepared=@(foreach($entry in $entries) {
            $footprint=Get-RuntimeFootprint $Root $entry.path
            $entry.sha256=$footprint.digest;$entry.items=$footprint.items;$entry.status=$Status;$entry.completed_utc=$clock.ToString('o')
            if($Status -ne 'completed') { $entry.retain_reasons=@($entry.retain_reasons)+@("unresolved-$Status") }
            $entry.expires_utc=if($entry.retain_reasons.Count){$null}else{$clock.AddDays($policy.days[$entry.category]).ToString('o')}
            $entry
        })
        foreach($entry in $prepared) { Write-AgentJson (Get-RuntimePath $Root "ledger/artifacts/$($entry.id).json") $entry -Root $Root }
        $run.status=$Status;$run.completed_utc=$clock.ToString('o');Write-AgentJson $runPath $run -Root $Root
        Add-RuntimeEvent $Root $RunId $Status @{artifacts=@($prepared.id)}
        return $run
    } finally { Exit-RuntimeLock $lock }
}
function Retire-AgentArtifact {
    param([string]$Root,[string]$Id,[string]$Reason,[Nullable[DateTimeOffset]]$Now)
    Assert-RuntimeId $Id
    if([string]::IsNullOrWhiteSpace($Reason)) { throw 'Explicit retirement reason is required.' }
    $lock=Enter-RuntimeLock $Root
    try {
        $entry=Read-AgentJson (Get-RuntimePath $Root "ledger/artifacts/$Id.json")
        if($entry.status -eq 'deleted' -or $entry.status -eq 'active') { throw 'Close the run before retirement.' }
        if($entry.category -eq 'backup') {
            $journalPath=Resolve-SafePath $Root "$($entry.path)/journal.json"
            if((Test-Path -LiteralPath $journalPath) -and (Read-AgentJson $journalPath).status -eq 'applying') { throw 'Interrupted deployment must be rolled back or reconciled before retiring its backup.' }
        }
        $footprint=Get-RuntimeFootprint $Root $entry.path
        if(-not(Test-RuntimeArtifactFootprint $entry $footprint)) { throw 'Artifact changed; reconcile before retirement.' }
        $previousDigest=$entry.sha256;$entry.sha256=$footprint.digest;$entry.items=$footprint.items
        $clock=Get-RuntimeClock $Now;$entry.status='completed';$entry.retain_reasons=@();$entry.completed_utc=$clock.ToString('o')
        $entry.expires_utc=$clock.AddDays((Get-RuntimePolicy $Root).days[$entry.category]).ToString('o')
        Write-AgentJson (Get-RuntimePath $Root "ledger/artifacts/$Id.json") $entry -Root $Root
        Add-RuntimeEvent $Root $Id 'retired' @{reason=$Reason;previous_sha256=$previousDigest;sha256=$entry.sha256}
        return $entry
    } finally { Exit-RuntimeLock $lock }
}
function Get-RuntimeInventory([string]$Root,[switch]$Expand) {
    $runtime=Get-RuntimePath $Root '.'
    $records=@(Read-RuntimeArtifacts $Root);$unknown=[Collections.Generic.List[string]]::new();$issues=[Collections.Generic.List[string]]::new()
    if(Test-Path -LiteralPath $runtime) {
        try { $items=(Get-RuntimeFootprint $Root '.agents/runtime' -PathsOnly).items } catch { $issues.Add($_.Exception.Message);$items=@() }
        $owners=@{}
        foreach($record in $records) {
            if($record.status -eq 'deleted') { continue }
            if(-not $owners.ContainsKey($record.run_id)) { $owners[$record.run_id]=[Collections.Generic.List[string]]::new() }
            $owners[$record.run_id].Add($record.path)
        }
        foreach($item in $items) {
            $p=$item.path
            if($item.kind -eq 'file' -and $p -match '^\.agents/runtime/(ledger/(artifacts/artifact-[a-f0-9]{32}\.json|runs/run-[a-f0-9]{32}\.json|events/[a-f0-9]{32}\.json|writes/[a-f0-9]{32}\.(start|end)\.json|deployments/deploy-[a-f0-9]{32}\.json)|state/(tasks/[A-Za-z0-9_-]+\.json|hooks/[a-f0-9]{64}\.json|staging/[a-f0-9]{32}\.tmp|locks/[A-Za-z0-9_-]+\.lock|memory-index\.json))$') { continue }
            $covered=$false
            if($p -match '^\.agents/runtime/runs/([^/]+)/') {
                $runId=$Matches[1]
                if($owners.ContainsKey($runId)) {
                    foreach($owned in $owners[$runId]) {
                        if($p -eq $owned -or $p.StartsWith($owned+'/',[StringComparison]::OrdinalIgnoreCase) -or ($item.kind -eq 'directory' -and $owned.StartsWith($p+'/',[StringComparison]::OrdinalIgnoreCase))) { $covered=$true;break }
                    }
                }
            }
            if(-not $covered -and ($item.kind -eq 'file' -or $p -match '^\.agents/runtime/runs/[^/]+/.+')) { $unknown.Add($p) }
        }
        $writes=Get-RuntimePath $Root 'ledger/writes'
        foreach($start in Get-ChildItem -LiteralPath $writes -File -Filter '*.start.json' -ErrorAction SilentlyContinue) {
            if(-not(Test-Path -LiteralPath (Join-Path $writes ($start.Name.Replace('.start.json','.end.json'))))) { $issues.Add("Unreconciled write: $($start.Name)") }
        }
    }
    $listed=@(if($Expand){$records}else{foreach($record in $records) {
        $summary=[ordered]@{};foreach($key in @('id','run_id','task_id','creator','purpose','category','path','created_utc','completed_utc','expires_utc','status','sha256','dependencies','retain_reasons','deleted_utc')) { $summary[$key]=$record[$key] }
        $summary['item_count']=$record.items.Count;$summary
    }})
    return @{artifacts=@($listed);unknown=@($unknown.ToArray());issues=@($issues.ToArray());
        excluded_controls=@('Git metadata','Codex native state','unregistered external tool caches')}
}
function Get-RuntimeCleanupPlan {
    param([string]$Root,[Nullable[DateTimeOffset]]$Now,[switch]$IgnoreRuntimeLock)
    $clock=Get-RuntimeClock $Now;$inventory=Get-RuntimeInventory $Root -Expand
    $blocked=[Collections.Generic.List[string]]::new();foreach($x in @($inventory.unknown)+@($inventory.issues)) { $blocked.Add($x) }
    $locks=Get-RuntimePath $Root 'state/locks'
    if(Test-Path -LiteralPath $locks) {
        foreach($file in Get-ChildItem -LiteralPath $locks -File) { if($file.Name -ne 'runtime.lock' -or -not $IgnoreRuntimeLock) { $blocked.Add("Active or interrupted lock: $($file.Name)") } }
    }
    foreach($file in Get-ChildItem -LiteralPath (Get-RuntimePath $Root 'state/staging') -Filter '*.tmp' -File -ErrorAction SilentlyContinue) { $blocked.Add("Unreconciled staging: $($file.Name)") }
    $entries=@($inventory.artifacts);$candidates=[Collections.Generic.List[object]]::new()
    foreach($entry in $entries|Where-Object { $_.status -eq 'completed' -and $_.expires_utc -and -not $_.retain_reasons.Count }) {
        if($clock -lt [DateTimeOffset]::Parse($entry.expires_utc)) { continue }
        if(@($entries|Where-Object { $_.status -ne 'deleted' -and $_.id -ne $entry.id -and $entry.id -in $_.dependencies }).Count) { continue }
        try {
            $footprint=Get-RuntimeFootprint $Root $entry.path
            if(-not(Test-RuntimeArtifactFootprint $entry $footprint)) { throw "Changed or unknown artifact content: $($entry.id)" }
            $pointers=@()
            if($entry.category -eq 'task') {
                $taskDirectory=Get-RuntimePath $Root 'state/tasks'
                foreach($file in Get-ChildItem -LiteralPath $taskDirectory -Filter '*.json' -File -ErrorAction SilentlyContinue) {
                    $pointer=Read-AgentJson $file.FullName
                    if($pointer.artifact_id -ne $entry.id) { continue }
                    if($pointer.status -ne 'completed') { throw 'An active task still references this artifact.' }
                    $pointers+=@(@{path=[IO.Path]::GetRelativePath($Root,$file.FullName).Replace('\','/');sha256=(Get-AgentHash $file.FullName)})
                }
            }
            $candidates.Add(@{id=$entry.id;path=$entry.path;sha256=$entry.sha256;record_sha256=(Get-AgentHash (Get-RuntimePath $Root "ledger/artifacts/$($entry.id).json"));state_pointers=$pointers})
        } catch { $blocked.Add($_.Exception.Message) }
    }
    $plan=[ordered]@{schema_version='agents-cleanup-plan/v4';root=[IO.Path]::GetFullPath($Root);as_of_utc=$clock.ToString('o');
        candidates=@($candidates.ToArray()|Sort-Object { $_.id });blocked=@($blocked.ToArray()|Sort-Object -Unique)}
    $plan['digest']=Get-TextHash (ConvertTo-AgentCanonicalJson $plan)
    return $plan
}
function Invoke-RuntimeCleanup {
    param([string]$Root,$Plan,[string]$ExpectedDigest,[Nullable[DateTimeOffset]]$Now)
    if(-not $ExpectedDigest -or $ExpectedDigest -ne $Plan.digest) { throw 'Cleanup requires the preview digest.' }
    if($Plan.root -ne [IO.Path]::GetFullPath($Root)) { throw 'Cleanup root differs from preview.' }
    $payload=[ordered]@{};foreach($k in @('schema_version','root','as_of_utc','candidates','blocked')) { $payload[$k]=$Plan[$k] }
    if((Get-TextHash (ConvertTo-AgentCanonicalJson $payload)) -ne $ExpectedDigest) { throw 'Cleanup plan was altered.' }
    $lock=Enter-RuntimeLock $Root
    try {
        $fresh=Get-RuntimeCleanupPlan $Root ([DateTimeOffset]::Parse($Plan.as_of_utc)) -IgnoreRuntimeLock
        if($fresh.digest -ne $ExpectedDigest -or $fresh.blocked.Count) { throw "Cleanup preview no longer safe: $($fresh.blocked -join '; ')" }
        $clock=Get-RuntimeClock $Now
        if($clock -lt [DateTimeOffset]::Parse($Plan.as_of_utc)) { throw 'Cleanup clock precedes preview.' }
        # Check every candidate before deleting any payload.
        foreach($candidate in $fresh.candidates) {
            $entry=Read-AgentJson (Get-RuntimePath $Root "ledger/artifacts/$($candidate.id).json")
            if($clock -lt [DateTimeOffset]::Parse($entry.expires_utc) -or -not(Test-RuntimeArtifactFootprint $entry (Get-RuntimeFootprint $Root $entry.path))) { throw 'Cleanup inputs changed.' }
            foreach($pointer in $candidate.state_pointers) { if((Get-AgentHash (Resolve-SafePath $Root $pointer.path)) -ne $pointer.sha256) { throw 'Task pointer changed.' } }
        }
        foreach($candidate in $fresh.candidates) {
            $recordPath=Get-RuntimePath $Root "ledger/artifacts/$($candidate.id).json";$entry=Read-AgentJson $recordPath
            $full=Resolve-SafePath $Root $entry.path
            if(-not(Test-RuntimeArtifactFootprint $entry (Get-RuntimeFootprint $Root $entry.path))) { throw 'Concurrent artifact change; remaining payloads preserved.' }
            Add-RuntimeEvent $Root $entry.id 'delete-started' @{path=$entry.path;sha256=$entry.sha256;plan_digest=$ExpectedDigest}
            # The footprint proves every child, including directories, is owned and not a link.
            Remove-Item -LiteralPath $full -Recurse -Force
            foreach($pointer in $candidate.state_pointers) {
                $pointerFull=Resolve-SafePath $Root $pointer.path
                if($pointer.path -notmatch '^\.agents/runtime/state/tasks/[A-Za-z0-9_-]+\.json$' -or (Get-AgentHash $pointerFull) -ne $pointer.sha256) { throw 'Task pointer changed after payload removal; reconcile before resuming.' }
                Remove-Item -LiteralPath $pointerFull
            }
            $entry.status='deleted';$entry.deleted_utc=$clock.ToString('o');$entry.items=@()
            Write-AgentJson $recordPath $entry -Root $Root
            Add-RuntimeEvent $Root $entry.id 'deleted' @{path=$entry.path;sha256=$entry.sha256;plan_digest=$ExpectedDigest}
            $runPath=Get-RuntimePath $Root "runs/$($entry.run_id)"
            if((Test-Path -LiteralPath $runPath) -and @(Get-ChildItem -Force -LiteralPath $runPath).Count -eq 0) {
                [IO.Directory]::Delete($runPath,$false)
                Add-RuntimeEvent $Root $entry.run_id 'empty-run-removed' @{path=".agents/runtime/runs/$($entry.run_id)"}
            }
        }
        return @{deleted=@($fresh.candidates.id);plan_digest=$ExpectedDigest}
    } finally { Exit-RuntimeLock $lock }
}
function Invoke-AgentChild {
    param([string]$Root,[string]$RunId,[scriptblock]$Action)
    $relative=".agents/runtime/runs/$RunId/child-temp-$([guid]::NewGuid().ToString('N'))"
    $temp=Resolve-SafePath $Root $relative
    [IO.Directory]::CreateDirectory($temp)|Out-Null
    $artifact=Register-AgentArtifact $Root $RunId $relative 'scratch' 'Supported subprocess temporary files'
    $old=@{TEMP=$env:TEMP;TMP=$env:TMP;TMPDIR=$env:TMPDIR;AGENTS_RUN_ID=$env:AGENTS_RUN_ID}
    try { $env:TEMP=$temp;$env:TMP=$temp;$env:TMPDIR=$temp;$env:AGENTS_RUN_ID=$RunId; & $Action }
    finally { foreach($key in $old.Keys) { [Environment]::SetEnvironmentVariable($key,$old[$key],'Process') } }
}
function Get-LegacyPackageProof([string]$Root,$Manifest) {
    # Manifest hashes alone are not origin proof. Require a complete catalog from a known local source commit.
    foreach($commit in Invoke-AgentGit $Root @('log','-100','--format=%H')) {
        try {
            $project=((Invoke-AgentGit $Root @('show',"${commit}:agents.json")) -join "`n")|ConvertFrom-Json -AsHashtable
            if($project.version -ne $Manifest.version) { continue }
            $catalog=((Invoke-AgentGit $Root @('show',"${commit}:docs/agents/deployment.json")) -join "`n")|ConvertFrom-Json -AsHashtable
            if($catalog.files.Count -ne $Manifest.files.Count) { continue }
            $matched=$true
            foreach($file in $Manifest.files) {
                $null=Resolve-SafePath $Root $file.source
                $source=@($catalog.files|Where-Object source -EQ $file.source)
                if($source.Count -ne 1 -or $source[0][$Manifest.layout] -ne $file.path) { $matched=$false;break }
                $text=(Invoke-AgentGit $Root @('show',"${commit}:$($file.source)")) -join "`n"
                if((Get-TextHash ($text+"`n")) -ne $file.sha256) { $matched=$false;break }
            }
            if($matched) { return $commit }
        } catch { continue }
    }
    throw 'No complete matching source commit proves this legacy package; preserve it for review.'
}
