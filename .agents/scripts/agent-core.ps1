#requires -Version 7.0
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-SafePath {
    param([string]$Root, [string]$Path)
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('/','\')
    if ([string]::IsNullOrWhiteSpace($Path) -or [IO.Path]::IsPathRooted($Path) -or
        $Path -match '(^|[\\/])\.\.([\\/]|$)|:|[\x00-\x1F]') { throw "Unsafe relative path: $Path" }
    foreach($part in ($Path -split '[\\/]')) {
        if($part -eq '.') { continue }
        if($part -ne $part.TrimEnd(' ','.') -or $part -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') {
            throw "Unsafe Windows path component: $Path"
        }
    }
    $full = [IO.Path]::GetFullPath([IO.Path]::Combine($rootFull, $Path))
    if (-not $full.StartsWith($rootFull + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escapes root: $Path"
    }
    $cursor = $full
    while ($cursor -and $cursor.Length -ge $rootFull.Length) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -Force -LiteralPath $cursor).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Linked paths are not supported: $Path"
            }
        }
        $cursor = Split-Path -Parent $cursor
    }
    return $full
}
function Get-AgentRoot {
    param([string]$Start = $PSScriptRoot)
    $dir = [IO.Path]::GetFullPath($Start)
    while ($dir) {
        if ((Test-Path -LiteralPath (Join-Path $dir 'agents.json')) -or
            (Test-Path -LiteralPath (Join-Path $dir '.agents/managed.json'))) { return $dir }
        $dir = Split-Path -Parent $dir
    }
    throw 'No Provider or Consumer root found.'
}
function Read-AgentJson {
    param([string]$Path, [string]$SchemaPath)
    $text = Get-Content -LiteralPath $Path -Raw
    if ($SchemaPath -and -not (Test-Json -Json $text -SchemaFile $SchemaPath -ErrorAction Stop)) {
        throw "Schema validation failed: $Path"
    }
    return ConvertFrom-Json -InputObject $text -AsHashtable -Depth 80 -ErrorAction Stop
}
function Write-AgentJson {
    param([string]$Path, $Value, [switch]$NoClobber)
    $parent = Split-Path -Parent $Path
    [IO.Directory]::CreateDirectory($parent) | Out-Null
    $temp = "$Path.$([guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllText($temp, (($Value | ConvertTo-Json -Depth 80).Replace("`r`n","`n") + "`n"), [Text.UTF8Encoding]::new($false))
        [IO.File]::Move($temp, $Path, -not $NoClobber)
    } finally { if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp } }
}
function Get-AgentHash {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return 'missing' }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Get-TextHash {
    param([string]$Text)
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($Text))).ToLowerInvariant()
}
function Invoke-AgentGit {
    param([string]$Root, [string[]]$Arguments)
    $out = @(& git -c core.quotepath=false -c core.longpaths=true -C $Root @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') failed: $($out -join ' ')" }
    return $out
}
function Get-AgentFiles {
    param([string]$Root)
    return @(Invoke-AgentGit $Root @('ls-files','--cached','--others','--exclude-standard') |
        Sort-Object -Unique | Where-Object { Test-Path -LiteralPath (Resolve-SafePath $Root $_) -PathType Leaf })
}
function Get-SourceDigest {
    param([string]$Root, [string[]]$Paths)
    $records = @($Paths | Sort-Object -Unique | ForEach-Object { "$_|$(Get-AgentHash (Resolve-SafePath $Root $_))" })
    return Get-TextHash ($records -join "`n")
}
function Get-AgentAsset {
    param([string]$Root, [string]$Source)
    $managed = Join-Path $Root '.agents/managed.json'
    if (Test-Path -LiteralPath $managed) {
        $match = @((Read-AgentJson $managed).files | Where-Object source -EQ $Source)
        if ($match.Count -ne 1) { throw "Missing or ambiguous managed asset: $Source" }
        return Resolve-SafePath $Root $match[0].path
    }
    return Resolve-SafePath $Root $Source
}
function Get-ProjectSettings {
    param([string]$Root)
    $settings = @{knowledge_directory='docs/memory/entries';runtime_directory='.agents/runtime';consumer_checks=@()}
    $path = Join-Path $Root 'agents.json'
    if (Test-Path -LiteralPath $path) {
        $read = Read-AgentJson $path
        foreach ($key in @($settings.Keys)) { if ($read.ContainsKey($key)) { $settings[$key] = $read[$key] } }
    }
    if($settings.runtime_directory -ne '.agents/runtime' -or $settings.knowledge_directory -ne 'docs/memory/entries') {
        throw 'Project state and knowledge paths are fixed protected boundaries.'
    }
    return $settings
}
