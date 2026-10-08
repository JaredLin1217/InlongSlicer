#requires -Version 7.0
param([ValidateSet('Recall','Promote','Index','Migrate','Retract')][string]$Action='Recall',
    [string[]]$Query=@(),[string[]]$Module=@(),[string[]]$Tag=@(),[string[]]$File=@(),[string]$Scope='',
    [string]$InputPath,[string]$Root,[switch]$Expand,[switch]$Automatic,[switch]$VerifyOfficial,[switch]$ReadOnly,[Nullable[DateTimeOffset]]$Now)
. "$PSScriptRoot/agent-memory.ps1"
if(-not $Root) { $Root=Get-AgentRoot }
if(($ReadOnly -or $env:AGENTS_READ_ONLY -eq '1') -and $Action -ne 'Recall') { throw 'Read-only memory commands only recall verified data.' }
if($Action -in @('Promote','Migrate','Retract')) {
    if(-not $InputPath) { throw 'A reviewed entry file is required.' }
    Publish-ProjectKnowledge -Root $Root -InputPath $InputPath -Action $Action -Automatic:$Automatic -VerifyOfficial:$VerifyOfficial -Now $Now
}
$result=Get-ProjectKnowledge -Root $Root -Query $Query -Module $Module -Tag $Tag -File $File -Scope $Scope -Expand:$Expand -VerifyOfficial:($VerifyOfficial -and $Action -in @('Recall','Index')) -Now $Now
if($Action -ne 'Recall') {
    $all=Get-ProjectKnowledge -Root $Root -Now $Now
    Write-AgentJson (Get-RuntimePath $Root 'state/memory-index.json') @{schema_version='agents-memory-index/v4';entries=$all.entries;gaps=$all.gaps} -Root $Root
}
$result|ConvertTo-Json -Depth 30
