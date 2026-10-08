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
    $options=@{InputObject=$text;AsHashtable=$true;Depth=80;ErrorAction='Stop'}
    if((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')) { $options.DateKind='String' }
    return ConvertFrom-Json @options
}
function Write-AgentJson {
    param([string]$Path, $Value, [switch]$NoClobber, [string]$Root,[byte[]]$Bytes)
    $Path = [IO.Path]::GetFullPath($Path)
    if (-not $Root) {
        if ($Path.Replace('\','/') -match '^(.*)/\.agents/runtime/') { $Root=$Matches[1] }
        else { try { $Root=Get-AgentRoot (Split-Path -Parent $Path) } catch { $Root=Get-AgentRoot } }
    }
    $Root=[IO.Path]::GetFullPath($Root)
    $relative=[IO.Path]::GetRelativePath($Root,$Path).Replace('\','/')
    $null=Resolve-SafePath $Root $relative
    $stageRoot=Resolve-SafePath $Root '.agents/runtime/state/staging'
    $journalRoot=Resolve-SafePath $Root '.agents/runtime/ledger/writes'
    [IO.Directory]::CreateDirectory($stageRoot) | Out-Null
    [IO.Directory]::CreateDirectory($journalRoot) | Out-Null
    $id=[guid]::NewGuid().ToString('N')
    $temp=Resolve-SafePath $Root ".agents/runtime/state/staging/$id.tmp"
    $text=(($Value | ConvertTo-Json -Depth 80).Replace("`r`n","`n") + "`n")
    $raw=$PSBoundParameters.ContainsKey('Bytes')
    $digest=if($raw){[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()}else{Get-TextHash $text}
    # Immutable, flushed journal frames bootstrap atomic writes without recursively staging the journal itself.
    $start=@{schema_version='agents-write/v4';id=$id;phase='started';destination=$relative;
        staging=".agents/runtime/state/staging/$id.tmp";sha256=$digest;utc=[DateTimeOffset]::UtcNow.ToString('o')}
    Write-AgentJournalFrame (Join-Path $journalRoot "$id.start.json") $start
    $outcome='failed'
    try {
        [IO.Directory]::CreateDirectory((Split-Path -Parent $Path)) | Out-Null
        if($raw) { [IO.File]::WriteAllBytes($temp,$Bytes) }
        else { [IO.File]::WriteAllText($temp, $text, [Text.UTF8Encoding]::new($false)) }
        [IO.File]::Move($temp, $Path, -not $NoClobber)
        $outcome='committed'
    } finally {
        if (Test-Path -LiteralPath $temp) { $null=Resolve-SafePath $Root $start.staging; Remove-Item -LiteralPath $temp }
        Write-AgentJournalFrame (Join-Path $journalRoot "$id.end.json") @{
            schema_version='agents-write/v4';id=$id;phase=$outcome;destination=$relative;sha256=$digest;
            staging_removed=$true;utc=[DateTimeOffset]::UtcNow.ToString('o')}
    }
}
function Write-AgentJournalFrame {
    param([string]$Path,$Value)
    $stream=[IO.File]::Open($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try {
        $bytes=[Text.Encoding]::UTF8.GetBytes((($Value|ConvertTo-Json -Depth 80 -Compress)+"`n"))
        $stream.Write($bytes,0,$bytes.Length); $stream.Flush($true)
    } finally { $stream.Dispose() }
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
function ConvertTo-AgentCanonicalValue($Value) {
    if($Value -is [Collections.IDictionary]) {
        $ordered=[ordered]@{};$keys=[string[]]@($Value.Keys);[Array]::Sort($keys,[StringComparer]::Ordinal)
        foreach($key in $keys) { $ordered[$key]=ConvertTo-AgentCanonicalValue $Value[$key] }
        return $ordered
    }
    if($Value -is [Collections.IEnumerable] -and $Value -isnot [string]) {
        $items=@(foreach($item in $Value) { ConvertTo-AgentCanonicalValue $item })
        return ,$items
    }
    return $Value
}
function ConvertTo-AgentCanonicalJson($Value) {
    return ConvertTo-Json -InputObject (ConvertTo-AgentCanonicalValue $Value) -Depth 80 -Compress
}
function Get-AgentIds($Items) { foreach($item in $Items) { $item.id } }
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
