<#
.SYNOPSIS
Authenticode-signs and verifies Windows PE files in an InlongSlicer release tree.

.DESCRIPTION
Signs unsigned EXE, DLL, and PYD files with an Authenticode code-signing
certificate from the Windows certificate store. Existing valid signatures are
preserved. Files with invalid signatures are rejected instead of overwritten.

The certificate must already be trusted by the target App Control policy.
Creating or trusting a development certificate is intentionally outside this
script because doing so would not satisfy an enterprise signer policy.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string[]]$Path,

    [string]$CertificateThumbprint,

    [ValidateSet('CurrentUser', 'LocalMachine')]
    [string]$CertificateStoreLocation = 'CurrentUser',

    [string]$TimestampUrl = 'http://timestamp.digicert.com',

    [switch]$VerifyOnly
)

$ErrorActionPreference = 'Stop'

function Find-SignTool {
    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $windowsKitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (-not (Test-Path -LiteralPath $windowsKitsRoot -PathType Container)) {
        throw "signtool.exe was not found. Install the Windows SDK."
    }

    $hostArchitecture = switch ($env:PROCESSOR_ARCHITECTURE) {
        'ARM64' { 'arm64' }
        'x86'   { 'x86' }
        default { 'x64' }
    }

    $sdkVersions = Get-ChildItem -LiteralPath $windowsKitsRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+(\.\d+)+$' } |
        Sort-Object { [version]$_.Name } -Descending

    foreach ($sdkVersion in $sdkVersions) {
        foreach ($architecture in @($hostArchitecture, 'x64')) {
            $candidate = Join-Path $sdkVersion.FullName "$architecture\signtool.exe"
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return $candidate
            }
        }
    }

    throw "signtool.exe was not found under '$windowsKitsRoot'. Install the Windows SDK."
}

function Test-PortableExecutable {
    param([Parameter(Mandatory = $true)][string]$LiteralPath)

    $stream = [System.IO.File]::OpenRead($LiteralPath)
    try {
        if ($stream.Length -lt 2) {
            return $false
        }
        return ($stream.ReadByte() -eq 0x4d -and $stream.ReadByte() -eq 0x5a)
    }
    finally {
        $stream.Dispose()
    }
}

function Invoke-SignTool {
    param(
        [Parameter(Mandatory = $true)][string]$Tool,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$Operation
    )

    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "SignTool $Operation failed with exit code $LASTEXITCODE."
    }
}

$resolvedFiles = New-Object System.Collections.Generic.List[System.IO.FileInfo]
foreach ($requestedPath in $Path) {
    if (-not (Test-Path -LiteralPath $requestedPath)) {
        throw "Signing path does not exist: $requestedPath"
    }

    $item = Get-Item -LiteralPath $requestedPath
    if ($item.PSIsContainer) {
        Get-ChildItem -LiteralPath $item.FullName -File -Recurse |
            Where-Object { $_.Extension.ToLowerInvariant() -in @('.exe', '.dll', '.pyd') } |
            ForEach-Object { $resolvedFiles.Add($_) }
    }
    elseif ($item.Extension.ToLowerInvariant() -in @('.exe', '.dll', '.pyd')) {
        $resolvedFiles.Add($item)
    }
    else {
        throw "Unsupported signing file type: $($item.FullName)"
    }
}

$signableFiles = @($resolvedFiles |
    Sort-Object FullName -Unique |
    Where-Object { Test-PortableExecutable -LiteralPath $_.FullName })

if ($signableFiles.Count -eq 0) {
    throw 'No Windows PE files were found to sign or verify.'
}

$signTool = Find-SignTool
$certificate = $null
$normalizedThumbprint = $CertificateThumbprint -replace '\s', ''

if (-not $VerifyOnly) {
    if ([string]::IsNullOrWhiteSpace($normalizedThumbprint)) {
        throw 'CertificateThumbprint is required unless VerifyOnly is selected.'
    }

    $certificatePath = "Cert:\$CertificateStoreLocation\My\$normalizedThumbprint"
    $certificate = Get-Item -LiteralPath $certificatePath -ErrorAction SilentlyContinue
    if (-not $certificate -or -not $certificate.HasPrivateKey) {
        throw "A code-signing certificate with a private key was not found at '$certificatePath'."
    }

    $codeSigningOid = '1.3.6.1.5.5.7.3.3'
    if ($certificate.NotBefore -gt (Get-Date) -or $certificate.NotAfter -le (Get-Date) -or
        $certificate.EnhancedKeyUsageList.ObjectId.Value -notcontains $codeSigningOid) {
        throw "Certificate '$normalizedThumbprint' is not a currently valid code-signing certificate."
    }
    if ([string]::IsNullOrWhiteSpace($TimestampUrl)) {
        throw 'TimestampUrl must not be empty.'
    }
}

$signedCount = 0
$preservedCount = 0
foreach ($file in $signableFiles) {
    $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
    if ($VerifyOnly) {
        if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
            throw "Signature verification failed for '$($file.FullName)': $($signature.Status)."
        }
    }
    elseif ($signature.Status -eq [System.Management.Automation.SignatureStatus]::Valid) {
        $preservedCount++
    }
    elseif ($signature.Status -eq [System.Management.Automation.SignatureStatus]::NotSigned) {
        $signArguments = @(
            'sign', '/fd', 'SHA256', '/sha1', $normalizedThumbprint,
            '/s', 'My', '/tr', $TimestampUrl, '/td', 'SHA256',
            '/d', 'InlongSlicer', $file.FullName
        )
        if ($CertificateStoreLocation -eq 'LocalMachine') {
            $signArguments = @('sign', '/sm', '/fd', 'SHA256', '/sha1', $normalizedThumbprint,
                '/s', 'My', '/tr', $TimestampUrl, '/td', 'SHA256',
                '/d', 'InlongSlicer', $file.FullName)
        }
        Invoke-SignTool -Tool $signTool -Arguments $signArguments -Operation "signing '$($file.FullName)'"
        $signedCount++
    }
    else {
        throw "Refusing to overwrite the $($signature.Status) signature on '$($file.FullName)'."
    }

    Invoke-SignTool -Tool $signTool -Arguments @('verify', '/pa', '/all', '/q', $file.FullName) `
        -Operation "verification of '$($file.FullName)'"
}

Write-Output "Verified $($signableFiles.Count) Windows PE files; signed $signedCount; preserved $preservedCount."
