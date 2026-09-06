#requires -Version 7.0
param([Parameter(Mandatory)][ValidateSet('Save','Resume')][string]$Action, [string]$InputPath, [string]$Id, [string]$Root)
. "$PSScriptRoot/agent-core.ps1"
if(-not $Root) { $Root=Get-AgentRoot }
$settings=Get-ProjectSettings $Root
$schema=Get-AgentAsset $Root 'schemas/task-state.schema.json'
if($Action -eq 'Save') {
    $inputData=Read-AgentJson (Resolve-SafePath $Root $InputPath)
    $Id=$inputData.id
}
if($Id -notmatch '^[A-Za-z0-9_-]+$') { throw 'A safe task ID is required.' }
$statePath=Resolve-SafePath $Root "$($settings.runtime_directory)/tasks/$Id.json"
if($Action -eq 'Save') {
    $inputData['schema_version']='agents-task/v3'
    $inputData['source_commit']=[string](Invoke-AgentGit $Root @('rev-parse','HEAD') | Select-Object -First 1)
    $inputData['updated_utc']=[DateTime]::UtcNow.ToString('o')
    foreach($path in $inputData.boundaries) {
        if(Test-Path -LiteralPath (Resolve-SafePath $Root $path) -PathType Container) { throw 'Checkpoint boundaries must name files, not directories.' }
    }
    $inputData['files']=@($inputData.boundaries | ForEach-Object { @{path=$_;sha256=(Get-AgentHash (Resolve-SafePath $Root $_))} })
    $json=$inputData|ConvertTo-Json -Depth 15
    if(-not (Test-Json -Json $json -SchemaFile $schema)) { throw 'Invalid task state.' }
    Write-AgentJson $statePath $inputData
}
$state=Read-AgentJson $statePath $schema
$changes=@($state.files | Where-Object { (Get-AgentHash (Resolve-SafePath $Root $_.path)) -ne $_.sha256 } | ForEach-Object path)
$head=[string](Invoke-AgentGit $Root @('rev-parse','HEAD') | Select-Object -First 1)
@{checkpoint=$state;current_commit=$head;changed_files=$changes;
    git_status=@(Invoke-AgentGit $Root @('status','--short'));
    requires_reinspection=($changes.Count -gt 0 -or $head -ne $state.source_commit);
    guidance='Verify real state and next steps. Never replay completed external actions automatically.'}|ConvertTo-Json -Depth 20
