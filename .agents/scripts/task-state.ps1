#requires -Version 7.0
param([ValidateSet('Save','Resume','List','Complete','Migrate','Reconcile')][string]$Action='Resume',
    [string]$InputPath,[string]$Id,[string]$Root,[int]$ExpectedRevision=-1,[string]$ExpectedDigest,[string]$Reason,[switch]$ReadOnly,[Nullable[DateTimeOffset]]$Now)
. "$PSScriptRoot/agent-tasks.ps1"
if(-not $Root) { $Root=Get-AgentRoot }
if(($ReadOnly -or $env:AGENTS_READ_ONLY -eq '1') -and $Action -notin @('List','Resume')) { throw 'Read-only task commands inspect existing state only.' }
$result=switch($Action) {
    List { @{candidates=@(Get-TaskCandidates $Root)} }
    Resume { Resume-AgentTask $Root $Id }
    Reconcile {
        if($ExpectedRevision -lt 0 -or -not $InputPath) { throw 'Reconcile needs reviewed immutable InputPath and ExpectedRevision.' }
        Repair-AgentTaskPointer $Root $Id $InputPath $ExpectedRevision $ExpectedDigest $Reason
    }
    default {
        if($ExpectedRevision -lt 0 -or -not $InputPath) { throw 'Saving requires a reviewed input and explicit ExpectedRevision (0 for a new task).' }
        $inputData=Read-AgentJson (Resolve-SafePath $Root $InputPath)
        Save-AgentTask -Root $Root -InputData $inputData -ExpectedRevision $ExpectedRevision -Complete:($Action -eq 'Complete') -Migrate:($Action -eq 'Migrate') -Now $Now
    }
}
$result|ConvertTo-Json -Depth 40
