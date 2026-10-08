. "$PSScriptRoot/agent-memory.ps1"
function Get-TaskGitState([string]$Root) {
    $head='unborn';try { $head=[string](Invoke-AgentGit $Root @('rev-parse','HEAD')|Select-Object -First 1) } catch { }
    return @{head=$head;index_digest=(Get-TextHash ((Invoke-AgentGit $Root @('ls-files','--stage')) -join "`n"));
        worktree_status=@(Invoke-AgentGit $Root @('status','--short'))}
}
function Get-TaskCandidates([string]$Root) {
    $directory=Get-RuntimePath $Root 'state/tasks';$candidates=@()
    if(Test-Path -LiteralPath $directory) {
        $candidates+=@(Get-ChildItem -LiteralPath $directory -File -Filter '*.json'|ForEach-Object {
            $pointer=Read-AgentJson $_.FullName
            $state=Read-AgentJson (Resolve-SafePath $Root $pointer.path) (Get-AgentAsset $Root 'schemas/task-state.schema.json')
            if($state.id -ne $pointer.id -or $state.revision -ne $pointer.revision -or (Get-AgentHash (Resolve-SafePath $Root $pointer.path)) -ne $pointer.sha256) { throw 'Task pointer does not match immutable revision.' }
            if($state.status -eq 'active') { @{id=$state.id;revision=$state.revision;objective=$state.objective;path=$pointer.path;legacy=$false} }
        })
    }
    $legacy=Get-RuntimePath $Root 'tasks'
    $history=@(Read-RuntimeArtifacts $Root|Where-Object category -EQ 'task')
    if(Test-Path -LiteralPath $legacy) {
        $candidates+=@(Get-ChildItem -LiteralPath $legacy -File -Filter '*.json'|ForEach-Object {
            $state=Read-AgentJson $_.FullName (Get-AgentAsset $Root 'schemas/task-state.schema.json')
            if($state.id -notin @($history|ForEach-Object task_id) -and $state.id -notin (Get-AgentIds $candidates) -and -not(Test-Path -LiteralPath (Get-RuntimePath $Root "state/tasks/$($state.id).json"))) {
                @{id=$state.id;revision=0;objective=$state.objective;path=".agents/runtime/tasks/$($state.id).json";legacy=$true}
            }
        })
    }
    foreach($artifact in $history|Where-Object status -EQ 'active') {
        if($artifact.task_id -notin (Get-AgentIds $candidates) -and -not(Test-Path -LiteralPath (Get-RuntimePath $Root "state/tasks/$($artifact.task_id).json"))) {
            $candidates+=@(@{id=$artifact.task_id;revision=0;objective='Interrupted task save; inspect owned revisions';path=$artifact.path;legacy=$false;recovery_required=$true})
        }
    }
    return @($candidates|Sort-Object id)
}
function Resume-AgentTask {
    param([string]$Root,[string]$Id)
    if(-not $Id) {
        $candidates=@(Get-TaskCandidates $Root)
        if($candidates.Count -ne 1) { return @{candidates=$candidates;selection_required=($candidates.Count -gt 1);guidance='Choose an explicit task ID; timestamps do not select a task.'} }
        $Id=$candidates[0].id
    }
    Assert-RuntimeId $Id
    $pointerPath=Get-RuntimePath $Root "state/tasks/$Id.json"
    if(Test-Path -LiteralPath $pointerPath) {
        $pointer=Read-AgentJson $pointerPath
        $path=Resolve-SafePath $Root $pointer.path
        if((Get-AgentHash $path) -ne $pointer.sha256) { throw 'Checkpoint hash changed.' }
        $state=Read-AgentJson $path (Get-AgentAsset $Root 'schemas/task-state.schema.json')
        if($state.id -ne $Id -or $state.revision -ne $pointer.revision) { throw 'Checkpoint revision does not match pointer.' }
        $orphans=@(Get-ChildItem -LiteralPath (Split-Path -Parent $path) -File -Filter '*.json'|Where-Object { $_.BaseName -match '^revision-(\d+)$' -and [int]$Matches[1] -gt $state.revision })
    } else {
        $history=@(Read-RuntimeArtifacts $Root|Where-Object { $_.category -eq 'task' -and $_.task_id -eq $Id })
        if($history.Count) { return @{id=$Id;status='recovery_or_retired';requires_reinspection=$true;artifacts=$history;guidance='Inspect immutable task revisions and deletion history. Never resume the old v3 snapshot; explicitly reconcile an interrupted pointer or choose a new task ID.'} }
        $state=Read-AgentJson (Get-RuntimePath $Root "tasks/$Id.json") (Get-AgentAsset $Root 'schemas/task-state.schema.json');$orphans=@()
    }
    $changes=@($state.files|Where-Object { (Get-AgentHash (Resolve-SafePath $Root $_.path)) -ne $_.sha256 }|ForEach-Object path)
    $git=Get-TaskGitState $Root;$legacy=$state.schema_version -eq 'agents-task/v3'
    $gitChanged=if($legacy){$git.head -ne $state.source_commit}else{
        $git.head -ne $state.git.head -or $git.index_digest -ne $state.git.index_digest -or
        ($git.worktree_status -join "`n") -ne ($state.git.worktree_status -join "`n")
    }
    $gaps=@();if(-not $legacy) {
        foreach($receipt in $state.validation_receipts) { if((Get-AgentHash (Resolve-SafePath $Root $receipt.path)) -ne $receipt.sha256) { $gaps+=@("Receipt changed or missing: $($receipt.path)") } }
        $knowledge=Get-ProjectKnowledge $Root
        foreach($knowledgeId in $state.knowledge_ids) { if($knowledgeId -notin (Get-AgentIds $knowledge.entries)) { $gaps+=@("Knowledge requires review: $knowledgeId") } }
    }
    if($orphans.Count) { $gaps+=@('Interrupted save has orphan revisions; reconcile explicitly before saving.') }
    return @{checkpoint=$state;current_commit=$git.head;git_status=$git.worktree_status;changed_files=$changes;gaps=$gaps;legacy=$legacy;
        requires_reinspection=($legacy -or $changes.Count -gt 0 -or $gitChanged -or $gaps.Count -gt 0);
        guidance='Latest recorded requirements are data. Check the current user goal and real state; never replay external actions automatically.'}
}
function Repair-AgentTaskPointer {
    param([string]$Root,[string]$Id,[string]$CandidatePath,[int]$ExpectedRevision,[string]$ExpectedDigest,[string]$Reason)
    Assert-RuntimeId $Id
    if(-not $Reason -or -not $ExpectedDigest) { throw 'Task reconciliation requires a reviewed candidate digest and reason.' }
    $lock=Enter-RuntimeLock $Root "task-$Id"
    try {
        $pointerPath=Get-RuntimePath $Root "state/tasks/$Id.json";$revision=0
        if(Test-Path -LiteralPath $pointerPath) { $old=Read-AgentJson $pointerPath;$revision=[int]$old.revision }
        if($revision -ne $ExpectedRevision) { throw 'Task pointer changed after review.' }
        $path=Resolve-SafePath $Root $CandidatePath
        if((Get-AgentHash $path) -ne $ExpectedDigest) { throw 'Candidate changed after review.' }
        $candidate=Read-AgentJson $path (Get-AgentAsset $Root 'schemas/task-state.schema.json')
        if($candidate.schema_version -ne 'agents-task/v4' -or $candidate.id -ne $Id -or $candidate.revision -ne $revision+1) { throw 'Candidate must be the next immutable revision for this task.' }
        $owners=@(Read-RuntimeArtifacts $Root|Where-Object { $_.category -eq 'task' -and $_.task_id -eq $Id -and $_.status -eq 'active' -and $CandidatePath -eq "$($_.path)/revision-$($candidate.revision).json" })
        if($owners.Count -ne 1) { throw 'Candidate has no unique active task ownership.' }
        $owner=$owners[0]
        Write-AgentJson $pointerPath @{schema_version='agents-task-pointer/v4';id=$Id;revision=$candidate.revision;path=$CandidatePath;sha256=$ExpectedDigest;
            run_id=$owner.run_id;artifact_id=$owner.id;base_path=$owner.path;status=$candidate.status} -Root $Root
        Remove-ArchivedLegacyTask $Root $candidate
        if($candidate.status -eq 'completed') { $null=Complete-AgentRun -Root $Root -RunId $owner.run_id }
        Add-RuntimeEvent $Root $Id 'task-pointer-reconciled' @{path=$CandidatePath;sha256=$ExpectedDigest;reason=$Reason}
        return Resume-AgentTask $Root $Id
    } finally { Exit-RuntimeLock $lock }
}
function Remove-ArchivedLegacyTask([string]$Root,$State) {
    $migration=$State.migrated_from
    if(-not $migration -or -not $migration.ContainsKey('archived_path')) { return }
    if($migration.path -ne ".agents/runtime/tasks/$($State.id).json" -or $migration.archived_path -notmatch '^\.agents/runtime/runs/run-[a-f0-9]{32}/task/legacy-v3\.json$') { throw 'Invalid legacy task archive boundary.' }
    $archive=Resolve-SafePath $Root $migration.archived_path
    if((Get-AgentHash $archive) -ne $migration.sha256) { throw 'Legacy task archive does not match original bytes.' }
    $original=Resolve-SafePath $Root $migration.path
    if(Test-Path -LiteralPath $original) {
        if((Get-AgentHash $original) -ne $migration.sha256) { throw 'Legacy task changed during migration; both versions preserved.' }
        Remove-Item -LiteralPath $original
        Add-RuntimeEvent $Root $State.id 'legacy-task-archived' @{source=$migration.path;archive=$migration.archived_path;sha256=$migration.sha256}
    }
}
function Save-AgentTask {
    param([string]$Root,$InputData,[int]$ExpectedRevision,[switch]$Complete,[switch]$Migrate,[Nullable[DateTimeOffset]]$Now)
    $Id=$InputData.id;Assert-RuntimeId $Id
    $lock=Enter-RuntimeLock $Root "task-$Id"
    try {
        $pointerPath=Get-RuntimePath $Root "state/tasks/$Id.json";$old=$null;$revision=0
        if(Test-Path -LiteralPath $pointerPath) {
            $pointer=Read-AgentJson $pointerPath;$revision=[int]$pointer.revision
            $old=(Resume-AgentTask $Root $Id).checkpoint
            if($old.status -ne 'active') { throw 'Completed tasks are immutable; create another task ID.' }
        }
        if($ExpectedRevision -ne $revision) { throw "Task revision conflict: expected $ExpectedRevision, actual $revision" }
        if($revision -eq 0 -and @(Read-RuntimeArtifacts $Root|Where-Object { $_.category -eq 'task' -and $_.task_id -eq $Id }).Count) { throw 'Task ID already has durable runtime history; inspect recovery or create a new ID.' }
        $legacy=Get-RuntimePath $Root "tasks/$Id.json"
        if(Test-Path -LiteralPath $legacy) {
            if(-not $Migrate -and $revision -eq 0) { throw 'Legacy state requires explicit Migrate.' }
            $legacyState=Read-AgentJson $legacy (Get-AgentAsset $Root 'schemas/task-state.schema.json')
            if($Migrate -and ($legacyState.schema_version -ne 'agents-task/v3' -or $legacyState.id -ne $Id)) { throw 'Legacy task identity/version does not match migration.' }
        } elseif($Migrate) { throw 'No legacy task to migrate.' }
        foreach($field in @('objective','latest_adjustment','boundaries','completed','open_issues','next_steps','acceptance_criteria','knowledge_ids','validation_receipts','external_actions')) {
            if(-not $InputData.ContainsKey($field)) { throw "Missing task field: $field" }
        }
        foreach($path in $InputData.boundaries) {
            if(Test-Path -LiteralPath (Resolve-SafePath $Root $path) -PathType Container) { throw 'Checkpoint boundaries must name files.' }
        }
        if($Complete -and $InputData.open_issues.Count) { throw 'Resolve open issues before completing a task.' }
        foreach($receipt in $InputData.validation_receipts) {
            $full=Resolve-SafePath $Root $receipt.path
            if((Get-AgentHash $full) -ne $receipt.sha256) { throw 'Validation receipt source does not match.' }
            $report=Read-AgentJson $full (Get-AgentAsset $Root 'schemas/validation-report.schema.json')
            if($receipt.input_digest -ne $report.input_digest -or $receipt.result -ne $report.result) { throw 'Receipt metadata does not match persisted report.' }
        }
        foreach($knowledgeId in $InputData.knowledge_ids) {
            Assert-RuntimeId $knowledgeId
            if(-not(Test-Path -LiteralPath (Resolve-SafePath $Root "docs/memory/entries/$knowledgeId.json"))) { throw 'Unknown knowledge ID.' }
        }
        if(@($InputData.external_actions|Group-Object id|Where-Object Count -GT 1).Count) { throw 'External action IDs must be unique.' }
        if($old) {
            foreach($action in $old.external_actions|Where-Object status -EQ 'completed') {
                $matching=@($InputData.external_actions|Where-Object id -EQ $action.id)
                if($matching.Count -ne 1 -or @('action','status','observed_utc','evidence'|Where-Object { $matching[0][$_] -ne $action[$_] }).Count) { throw 'Completed external action history cannot be rewritten.' }
            }
        }
        $clock=Get-RuntimeClock $Now
        $dependencies=@()
        $runtimeRecords=@(Read-RuntimeArtifacts $Root)
        foreach($receipt in $InputData.validation_receipts) {
            $owner=@($runtimeRecords|Where-Object { $_.status -ne 'deleted' -and ($receipt.path -eq $_.path -or $receipt.path.StartsWith($_.path+'/',[StringComparison]::OrdinalIgnoreCase)) })
            if($owner.Count -ne 1) { throw 'Task receipts require registered runtime ownership.' }
            $dependencies+=@($owner[0].id)
        }
        $requirements=@(if($old){$old.requirements})
        if(-not $old -or $old.objective -ne $InputData.objective -or $old.latest_adjustment -ne $InputData.latest_adjustment -or
            ($old.acceptance_criteria -join "`n") -ne ($InputData.acceptance_criteria -join "`n")) {
            $requirements+=@(@{revision=$requirements.Count+1;recorded_utc=$clock.ToString('o');objective=$InputData.objective;
                adjustment=$InputData.latest_adjustment;acceptance_criteria=@($InputData.acceptance_criteria)})
        }
        if($old) { $runId=$pointer.run_id;$artifactId=$pointer.artifact_id;$base=$pointer.base_path }
        else {
            $run=New-AgentRun -Root $Root -Purpose "Task checkpoints: $Id" -Creator 'task-state' -TaskId $Id -Now $Now
            $runId=$run.id;$base="$($run.path)/task"
            $artifact=Register-AgentArtifact -Root $Root -RunId $runId -Path $base -Category task -Purpose "Immutable revisions for $Id" -Dependencies @($dependencies|Sort-Object -Unique);$artifactId=$artifact.id
        }
        if($old) {
            $artifact=Read-AgentJson (Get-RuntimePath $Root "ledger/artifacts/$artifactId.json")
            $artifact.dependencies=@(@($artifact.dependencies)+@($dependencies)|Sort-Object -Unique)
            Write-AgentJson (Get-RuntimePath $Root "ledger/artifacts/$artifactId.json") $artifact -Root $Root
        }
        $migration=if($old){$old.migrated_from}else{$null}
        if($Migrate) {
            $legacyHash=Get-AgentHash $legacy;$archivePath="$base/legacy-v3.json"
            Write-AgentJson -Path (Resolve-SafePath $Root $archivePath) -Bytes ([IO.File]::ReadAllBytes($legacy)) -NoClobber -Root $Root
            if((Get-AgentHash $legacy) -ne $legacyHash -or (Get-AgentHash (Resolve-SafePath $Root $archivePath)) -ne $legacyHash) { throw 'Legacy source changed; preserve both and reconcile.' }
            $migration=@{path=".agents/runtime/tasks/$Id.json";archived_path=$archivePath;sha256=$legacyHash}
        }
        $state=[ordered]@{schema_version='agents-task/v4';id=$Id;revision=$revision+1;status=$(if($Complete){'completed'}else{'active'});
            objective=$InputData.objective;latest_adjustment=$InputData.latest_adjustment;acceptance_criteria=@($InputData.acceptance_criteria);requirements=$requirements;
            boundaries=@($InputData.boundaries);completed=@($InputData.completed);open_issues=@($InputData.open_issues);next_steps=@($InputData.next_steps);
            knowledge_ids=@($InputData.knowledge_ids);validation_receipts=@($InputData.validation_receipts);external_actions=@($InputData.external_actions);
            git=(Get-TaskGitState $Root);files=@($InputData.boundaries|ForEach-Object { @{path=$_;sha256=(Get-AgentHash (Resolve-SafePath $Root $_))} });
            updated_utc=$clock.ToString('o');completed_utc=$(if($Complete){$clock.ToString('o')}else{$null});
            migrated_from=$migration}
        if(-not(Test-Json -Json ($state|ConvertTo-Json -Depth 40) -SchemaFile (Get-AgentAsset $Root 'schemas/task-state.schema.json'))) { throw 'Invalid v4 task state.' }
        $relative="$base/revision-$($state.revision).json"
        Write-AgentJson (Resolve-SafePath $Root $relative) $state -NoClobber -Root $Root
        Write-AgentJson $pointerPath @{schema_version='agents-task-pointer/v4';id=$Id;revision=$state.revision;path=$relative;
            sha256=(Get-AgentHash (Resolve-SafePath $Root $relative));run_id=$runId;artifact_id=$artifactId;base_path=$base;status=$state.status} -Root $Root
        if($Migrate) { Remove-ArchivedLegacyTask $Root $state }
        if($Complete) { $null=Complete-AgentRun -Root $Root -RunId $runId -Now $Now }
        return Resume-AgentTask $Root $Id
    } finally { Exit-RuntimeLock $lock }
}
