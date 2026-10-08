#requires -Version 7.0
param([ValidateSet('Create','Register','Complete','List','PlanCleanup','Cleanup','Retire','Reconcile')][string]$Action='List',
    [string]$Root,[string]$RunId,[string]$Id,[string]$Path,[string]$Purpose,[string]$TaskId='',
    [ValidateSet('scratch','package','validation','task','backup')][string]$Category='scratch',
    [ValidateSet('completed','failed','interrupted')][string]$Status='completed',
    [string[]]$Dependencies=@(),[string[]]$RetainReasons=@(),[string]$InputPath,[string]$ExpectedDigest,[string]$Reason,
    [switch]$ReadOnly,[switch]$Expand,[Nullable[DateTimeOffset]]$Now)
. "$PSScriptRoot/agent-runtime.ps1"
if(-not $Root) { $Root=Get-AgentRoot }
if(($ReadOnly -or $env:AGENTS_READ_ONLY -eq '1') -and ($Action -notin @('List','PlanCleanup','Reconcile') -or ($Action -eq 'Reconcile' -and ($Id -or $InputPath)))) { throw 'Read-only runtime commands only inspect or preview.' }
$result=switch($Action) {
    Create { New-AgentRun -Root $Root -Purpose $Purpose -TaskId $TaskId -Now $Now }
    Register { Register-AgentArtifact -Root $Root -RunId $RunId -Path $Path -Category $Category -Purpose $Purpose -Dependencies $Dependencies -RetainReasons $RetainReasons -Now $Now }
    Complete { Complete-AgentRun -Root $Root -RunId $RunId -Status $Status -Now $Now }
    List { Get-RuntimeInventory $Root -Expand:$Expand }
    PlanCleanup { Get-RuntimeCleanupPlan -Root $Root -Now $Now }
    Cleanup { Invoke-RuntimeCleanup -Root $Root -Plan (Read-AgentJson (Resolve-SafePath $Root $InputPath)) -ExpectedDigest $ExpectedDigest -Now $Now }
    Retire { Retire-AgentArtifact -Root $Root -Id $Id -Reason $Reason -Now $Now }
    Reconcile {
        if(-not $Id -and -not $InputPath) { Get-RuntimeInventory $Root; break }
        if(-not $ExpectedDigest -or -not $Reason) { throw 'Reconciliation requires an observed digest and explicit reason.' }
        if($InputPath) {
            if($InputPath -notmatch '^\.agents/runtime/packages/[a-f0-9]{32}$') { throw 'Only manifest-proven legacy packages can be migrated here.' }
            $source=Resolve-SafePath $Root $InputPath;$footprint=Get-RuntimeFootprint $Root $InputPath
            if($footprint.digest -ne $ExpectedDigest) { throw 'Legacy package changed.' }
            $manifest=Read-AgentJson (Join-Path $source '.agents/managed.json') (Get-AgentAsset $Root 'schemas/managed.schema.json')
            . "$PSScriptRoot/agent-deployment.ps1"
            $expected=@('.agents/managed.json')+@($manifest.files.path)
            if(@($expected|Group-Object|Where-Object Count -GT 1).Count) { throw 'Duplicate legacy package paths.' }
            foreach($file in $manifest.files) {
                Assert-OwnedPath $file.path
                if((Get-AgentHash (Resolve-SafePath $source $file.path)) -ne $file.sha256) { throw 'Legacy ownership proof failed.' }
            }
            $actual=@($footprint.items|Where-Object kind -EQ 'file'|ForEach-Object { [IO.Path]::GetRelativePath($source,(Resolve-SafePath $Root $_.path)).Replace('\','/') })
            if(@(Compare-Object ($actual|Sort-Object) ($expected|Sort-Object)).Count) { throw 'Unknown legacy package content.' }
            $expectedDirectories=@($InputPath)
            foreach($file in $expected) {
                $parent=Split-Path -Parent $file
                while($parent) { $expectedDirectories+=@("$InputPath/"+$parent.Replace('\','/'));$parent=Split-Path -Parent $parent }
            }
            if(@($footprint.items|Where-Object { $_.kind -eq 'directory' -and $_.path -notin $expectedDirectories }).Count) { throw 'Unknown legacy package directory.' }
            $sourceCommit=Get-LegacyPackageProof $Root $manifest
            $run=New-AgentRun $Root 'Migrate manifest-proven legacy package'
            $destination="$($run.path)/package";$artifact=Register-AgentArtifact $Root $run.id $destination 'package' 'Legacy package with verified managed hashes'
            foreach($file in $expected) {
                $target=Resolve-SafePath $Root "$destination/$file"
                [IO.Directory]::CreateDirectory((Split-Path -Parent $target))|Out-Null
                [IO.File]::Copy((Resolve-SafePath $source $file),$target,$false)
            }
            if((Get-RuntimeFootprint $Root $InputPath).digest -ne $ExpectedDigest) { throw 'Legacy source changed while migrating; both copies preserved.' }
            $null=Complete-AgentRun $Root $run.id
            $null=Resolve-SafePath $Root $InputPath
            Remove-Item -LiteralPath $source -Recurse -Force
            Add-RuntimeEvent $Root $artifact.id 'legacy-migrated' @{source=$InputPath;sha256=$ExpectedDigest;source_commit=$sourceCommit;reason=$Reason}
            @{migrated=$InputPath;destination=$destination;artifact_id=$artifact.id}
        } elseif($Id -match '^artifact-[a-f0-9]{32}$') {
            $recordPath=Get-RuntimePath $Root "ledger/artifacts/$Id.json"
            if((Get-AgentHash $recordPath) -ne $ExpectedDigest) { throw 'Artifact record changed.' }
            $entry=Read-AgentJson $recordPath
            $events=@(Get-ChildItem -LiteralPath (Get-RuntimePath $Root 'ledger/events') -File -Filter '*.json'|ForEach-Object { Read-AgentJson $_.FullName }|Where-Object { $_.id -eq $Id -and $_.action -eq 'delete-started' -and $_.details.sha256 -eq $entry.sha256 })
            if(-not $events.Count -or $entry.status -ne 'completed' -or $entry.retain_reasons.Count -or -not $entry.expires_utc -or (Get-RuntimeClock $Now) -lt [DateTimeOffset]::Parse($entry.expires_utc)) { throw 'Only eligible interrupted deletions can be reconciled.' }
            $lock=Enter-RuntimeLock $Root
            try {
                $records=@(Read-RuntimeArtifacts $Root)
                if(@($records|Where-Object { $_.status -ne 'deleted' -and $Id -in $_.dependencies }).Count) { throw 'Artifact acquired a protected dependency.' }
                $inventory=Get-RuntimeInventory $Root
                if($inventory.unknown.Count -or $inventory.issues.Count) { throw 'Runtime inventory needs review before deletion recovery.' }
                if(@(Get-ChildItem -LiteralPath (Get-RuntimePath $Root 'state/locks') -File|Where-Object Name -NE 'runtime.lock').Count) { throw 'Other runtime locks prevent reconciliation.' }
                if((Get-AgentHash $recordPath) -ne $ExpectedDigest) { throw 'Artifact changed after lock acquisition.' }
                $tasks=Get-RuntimePath $Root 'state/tasks';$pointers=@()
                foreach($file in Get-ChildItem -LiteralPath $tasks -File -Filter '*.json' -ErrorAction SilentlyContinue) {
                    $pointer=Read-AgentJson $file.FullName
                    if($pointer.artifact_id -ne $Id) { continue }
                    if($pointer.status -ne 'completed') { throw 'Active task pointer prevents reconciliation.' }
                    $null=Resolve-SafePath $Root ([IO.Path]::GetRelativePath($Root,$file.FullName).Replace('\','/'))
                    $pointers+=@(@{path=$file.FullName;sha256=(Get-AgentHash $file.FullName)})
                }
                $full=Resolve-SafePath $Root $entry.path
                if(Test-Path -LiteralPath $full) {
                    $remaining=Get-RuntimeFootprint $Root $entry.path
                    foreach($item in $remaining.items) {
                        $original=@($entry.items|Where-Object path -EQ $item.path)
                        if($original.Count -ne 1 -or $original[0].sha256 -ne $item.sha256 -or $original[0].kind -ne $item.kind) { throw 'Interrupted deletion contains unknown or changed content.' }
                    }
                    Remove-Item -LiteralPath $full -Recurse -Force
                }
                foreach($pointer in $pointers) {
                    if((Get-AgentHash $pointer.path) -ne $pointer.sha256) { throw 'Task pointer changed during recovery.' }
                    Remove-Item -LiteralPath $pointer.path
                }
                $entry.status='deleted';$entry.deleted_utc=(Get-RuntimeClock $Now).ToString('o');$entry.items=@()
                Write-AgentJson $recordPath $entry -Root $Root
                Add-RuntimeEvent $Root $Id 'deletion-reconciled' @{reason=$Reason;sha256=$entry.sha256}
                @{reconciled=$Id;status='deleted'}
            } finally { Exit-RuntimeLock $lock }
        } elseif($Id -like 'lock-*') {
            $name=$Id.Substring(5);Assert-RuntimeId $name;$lockPath=Get-RuntimePath $Root "state/locks/$name.lock"
            if((Get-AgentHash $lockPath) -ne $ExpectedDigest) { throw 'Lock changed.' }
            $owner=Read-AgentJson $lockPath;$process=Get-Process -Id $owner.pid -ErrorAction SilentlyContinue
            if($process -and $process.StartTime.ToUniversalTime().ToString('o') -eq $owner.process_start_utc) { throw 'Lock owner is still running.' }
            $handle=[IO.File]::Open($lockPath,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None);$handle.Dispose()
            Remove-Item -LiteralPath $lockPath
            Add-RuntimeEvent $Root $Id 'lock-reconciled' @{sha256=$ExpectedDigest;reason=$Reason}
            @{reconciled=$Id}
        } else {
            if($Id -notmatch '^[a-f0-9]{32}$') { throw 'Specify a staging write ID or lock name.' }
            $startPath=Get-RuntimePath $Root "ledger/writes/$Id.start.json"
            if((Get-AgentHash $startPath) -ne $ExpectedDigest) { throw 'Staging journal changed.' }
            $start=Read-AgentJson $startPath;$end=Get-RuntimePath $Root "ledger/writes/$Id.end.json"
            if(Test-Path -LiteralPath $end) { throw 'Write is already reconciled.' }
            if($start.staging -ne ".agents/runtime/state/staging/$Id.tmp") { throw 'Invalid staging journal path.' }
            $stage=Resolve-SafePath $Root $start.staging
            if(Test-Path -LiteralPath $stage) {
                if((Get-AgentHash $stage) -ne $start.sha256) { throw 'Staging hash changed; preserved for review.' }
                Remove-Item -LiteralPath $stage
            }
            $committed=(Get-AgentHash (Resolve-SafePath $Root $start.destination)) -eq $start.sha256
            Write-AgentJournalFrame $end @{schema_version='agents-write/v4';id=$Id;phase='reconciled';committed=$committed;reason=$Reason;utc=[DateTimeOffset]::UtcNow.ToString('o')}
            @{reconciled=$Id;destination_matches=$committed;guidance='Inspect destination before resuming; no external action replay.'}
        }
    }
}
$result|ConvertTo-Json -Depth 80
