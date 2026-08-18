function New-AgentTomlResult {
    param([bool]$Available, [bool]$Success, [string]$Parser, [string[]]$Errors = @())
    [pscustomobject]@{
        available = $Available
        success = $Success
        parser = $Parser
        errors = @($Errors)
    }
}

function Test-AgentTomlFile {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return New-AgentTomlResult $true $false "filesystem" @("missing TOML file: $Path")
    }

    $program = @'
import json, sys
try:
    import tomllib
except ImportError:
    import tomli as tomllib
try:
    with open(sys.argv[1], "rb") as source:
        tomllib.load(source)
    value = [True, "python-" + tomllib.__name__, []]
except Exception as error:
    value = [False, "python-" + tomllib.__name__, [str(error)]]
print(json.dumps(value))
'@
    foreach ($name in @("python", "py")) {
        $command = Get-Command $name -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $command) { continue }
        try {
            $prefix = if ($name -eq "py") { @("-3") } else { @() }
            $raw = & $command.Source @prefix -c $program $Path 2>$null
            if ($LASTEXITCODE -ne 0) { continue }
            $value = ($raw -join "`n") | ConvertFrom-Json -ErrorAction Stop
            return New-AgentTomlResult $true ([bool]$value[0]) ([string]$value[1]) @($value[2])
        }
        catch { continue }
    }

    $converter = Get-Command ConvertFrom-Toml -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $converter) {
        return New-AgentTomlResult $false $false "unavailable" @("No standard TOML parser is available.")
    }
    try {
        [void](Get-Content -LiteralPath $Path -Raw | & $converter -ErrorAction Stop)
        return New-AgentTomlResult $true $true "powershell-convertfrom-toml"
    }
    catch {
        return New-AgentTomlResult $true $false "powershell-convertfrom-toml" @($_.Exception.Message)
    }
}
