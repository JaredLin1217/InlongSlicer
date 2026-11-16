#requires -Version 7.4
param(
    [string]$Root,
    [string]$Provider,
    [ValidateSet('Plan','Apply')][string]$Action='Plan',
    [string]$ExpectedDigest,
    [string]$Reason,
    [switch]$Json
)
. "$PSScriptRoot/agent-runtime.ps1"
. "$PSScriptRoot/agent-deployment.ps1"

if(-not $Root) { throw 'Root is required.' }
$Root=[IO.Path]::GetFullPath($Root)
if(-not $Provider) { $Provider=$Root }
$Provider=[IO.Path]::GetFullPath($Provider)
$manifestPath=Resolve-SafePath $Root '.agents/managed.json'
if(-not(Test-Path -LiteralPath $manifestPath)) { throw 'v4 managed manifest is missing.' }
$legacySchema=Join-Path $PSScriptRoot '../migrations/v4/schemas/managed.schema.json'
$old=Read-AgentJson $manifestPath $legacySchema
if($old.schema_version -ne 'agents-managed/v3') { throw 'Target is not a v4 managed Consumer.' }
if($old.layout -notin @('root-layout','dot-agents-layout')) { throw 'Unsupported v4 layout.' }

$oldFiles=@($old.files|ForEach-Object {
    $full=Resolve-SafePath $Root $_.path
    $actual=Get-AgentHash $full
    if($actual -ne $_.sha256) { throw "Managed v4 file changed: $($_.path)" }
    [ordered]@{source=$_.source;path=$_.path;sha256=$_.sha256}
})
$newFiles=@($oldFiles)
if($Provider -ne $Root) {
    $newFiles=@(Get-DeploymentEntries $Provider $old.layout)
    $oldByPath=@{};foreach($entry in $oldFiles){$oldByPath[$entry.path]=$entry}
    foreach($entry in $newFiles) {
        if(-not $oldByPath.ContainsKey($entry.path)) { continue }
        if($oldByPath[$entry.path].sha256 -ne (Get-AgentHash (Resolve-SafePath $Root $entry.path))) { throw "Target changed during v4 inventory: $($entry.path)" }
    }
}
$targetVersion=if($Provider -eq $Root){'5.0.0'}else{(Read-AgentJson (Join-Path $Provider 'agents.json')).version}
$newManifest=[ordered]@{schema_version='agents-managed/v5';version=$targetVersion;layout=$old.layout;files=$newFiles}
$manifestText=($newManifest|ConvertTo-Json -Depth 30).Replace("`r`n","`n")+"`n"
$plan=[ordered]@{
    schema_version='agents-migration-plan/v5';from_version=$old.version;to_version=$newManifest.version
    root=$Root;provider=$Provider;layout=$old.layout;managed_files=@($oldFiles.path)
    new_manifest=$newManifest;operations=@(@{path='.agents/managed.json';before=(Get-AgentHash $manifestPath);after=(Get-TextHash $manifestText);action='update'})
    protections=@('Business files are outside the managed manifest.','Changed or unowned managed files stop Apply.','A registered backup is created before the manifest update.')
}
$plan.digest=Get-TextHash (ConvertTo-AgentCanonicalJson $plan)
if($Action -eq 'Plan') {
    $plan|ConvertTo-Json -Depth 40 -Compress
    exit 0
}
if(-not $ExpectedDigest -or $ExpectedDigest -ne $plan.digest) { throw 'Apply requires the exact reviewed migration plan digest.' }
if([string]::IsNullOrWhiteSpace($Reason)) { throw 'Apply requires a reviewed migration reason.' }
$run=New-AgentRun $Root "Migrate v4 Consumer to v5" 'migration'
$backup=Register-AgentArtifact $Root $run.id "$($run.path)/backup" 'backup' 'v4 manifest and managed-file rollback backup' -RetainReasons @('v5 migration rollback dependency')
$backupRoot=Resolve-SafePath $Root $backup.path
foreach($file in $oldFiles) {
    $source=Resolve-SafePath $Root $file.path
    $destination=Join-Path $backupRoot $file.path
    [IO.Directory]::CreateDirectory((Split-Path -Parent $destination))|Out-Null
    [IO.File]::Copy($source,$destination,$true)
}
Write-AgentJson (Join-Path $backupRoot 'migration-v4.json') @{schema_version='agents-migration-backup/v5';source_manifest=$old;source_manifest_sha256=(Get-AgentHash $manifestPath);reason=$Reason;plan_digest=$plan.digest} -Root $Root
Write-AgentJson $manifestPath $newManifest -Root $Root
$null=Complete-AgentRun $Root $run.id
$result=@{status='migrated';from_version=$old.version;to_version=$newManifest.version;layout=$old.layout;backup=$backup.path;plan_digest=$plan.digest}
if($Json) { $result|ConvertTo-Json -Depth 40 -Compress } else { $result|ConvertTo-Json -Depth 40 }
