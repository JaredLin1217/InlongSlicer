#requires -Version 7.4
param([string]$OutputPath,[string]$Root)
. "$PSScriptRoot/agent-runtime.ps1"

$root=if($Root){[IO.Path]::GetFullPath($Root)}else{try{Get-AgentRoot}catch{[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))}}
$plugin=if(Test-Path -LiteralPath (Join-Path $root 'plugins/agents-governance/plugin.json')){Resolve-SafePath $root 'plugins/agents-governance'}else{$root}
$pluginSchema=if(Test-Path -LiteralPath (Join-Path $root 'schemas/plugin.schema.json')){Resolve-SafePath $root 'schemas/plugin.schema.json'}else{Resolve-SafePath $plugin 'schemas/plugin.schema.json'}
$manifest=Read-AgentJson (Join-Path $plugin 'plugin.json') $pluginSchema
$projectVersion=if(Test-Path -LiteralPath (Join-Path $root 'agents.json')){(Read-AgentJson (Join-Path $root 'agents.json')).version}else{$manifest.version}
if($manifest.name -ne 'agents-governance' -or $manifest.version -ne $projectVersion) { throw 'Plugin version does not match the project.' }
$files=@(Get-ChildItem -LiteralPath $plugin -Recurse -File|Where-Object { $_.FullName -notmatch '[\\/](\.git|runtime|__pycache__)[\\/]' }|Sort-Object { [IO.Path]::GetRelativePath($plugin,$_.FullName).Replace('\','/') })
foreach($file in $files) {
    $relative=[IO.Path]::GetRelativePath($plugin,$file.FullName).Replace('\','/')
    if($relative -match '(?i)(^|/)(\.env|.*\.pem|.*\.key|.*transcript.*|.*secret.*)$') { throw "Potential secret or transcript in plugin: $relative" }
    if($relative -match '(^|/)hooks(/|$)') { throw 'Lifecycle hooks are not part of the public plugin package.' }
}
$run=New-AgentRun $root 'Package Agents Governance plugin' 'package'
$artifact=Register-AgentArtifact $root $run.id "$($run.path)/plugin" 'package' 'Deterministic Agents v5 plugin package'
$stage=Resolve-SafePath $root $artifact.path
$zipPath=Resolve-SafePath $root "$($artifact.path)/agents-governance-$($manifest.version).zip"
$shaPath=Resolve-SafePath $root "$($artifact.path)/agents-governance-$($manifest.version).sha256"
$stream=[IO.File]::Open($zipPath,[IO.FileMode]::Create,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
try {
    $archive=[IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create,$false)
    foreach($file in $files) {
        $relative=[IO.Path]::GetRelativePath($plugin,$file.FullName).Replace('\','/')
        $entry=$archive.CreateEntry($relative,[IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime=[DateTimeOffset]::Parse('1980-01-01T00:00:00Z')
        $input=[IO.File]::OpenRead($file.FullName);$output=$entry.Open()
        try {$input.CopyTo($output)} finally {$output.Dispose();$input.Dispose()}
    }
    $archive.Dispose()
} finally {$stream.Dispose()}
$hash=(Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText($shaPath,"$hash  agents-governance-$($manifest.version).zip`n",[Text.UTF8Encoding]::new($false))
if($OutputPath) {
    $destination=[IO.Path]::GetFullPath($OutputPath)
    [IO.Directory]::CreateDirectory((Split-Path -Parent $destination))|Out-Null
    [IO.File]::Copy($zipPath,$destination,$true)
}
$null=Complete-AgentRun $root $run.id
@{schema_version='agents-plugin-package/v5';name=$manifest.name;version=$manifest.version;path="$($artifact.path)/agents-governance-$($manifest.version).zip";sha256=$hash;files=$files.Count;telemetry='disabled';hooks='excluded'}|ConvertTo-Json -Compress
