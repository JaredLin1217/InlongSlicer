#requires -Version 7.4
[CmdletBinding()]
param(
    [Parameter(Position=0)][ValidateSet('help','init','plan','validate','memory','task','runtime','deploy','doctor','package')][string]$Command='help',
    [Parameter(Position=1)][string]$Subcommand='',
    [string]$Root,
    [ValidateSet('Provider','Consumer')][string]$Scope='Provider',
    [ValidateSet('Changed','Checkpoint')][string]$Profile='Changed',
    [string[]]$Path=@(),
    [string]$Id,
    [string]$InputPath,
    [int]$ExpectedRevision=-1,
    [string]$ExpectedDigest,
    [string]$Reason,
    [string]$Layout='auto',
    [string]$TargetPath,
    [string]$Provider,
    [string]$OutputPath,
    [switch]$Json,
    [switch]$ReadOnly,
    [switch]$DryRun,
    [switch]$Apply,
    [switch]$RequireReleaseReady,
    [switch]$Expand,
    [switch]$VerifyOfficial,
    [Parameter(ValueFromRemainingArguments=$true)][string[]]$Remaining
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. "$PSScriptRoot/agent-core.ps1"

function Write-AgentCliResult($Value,[int]$ExitCode=0) {
    if($Json -or $Value -is [Collections.IDictionary] -or $Value -is [pscustomobject]) {
        $Value|ConvertTo-Json -Depth 80 -Compress
    } else { [string]$Value }
    if($ExitCode -ne 0) { exit $ExitCode }
}
function Get-CliRoot {
    if($Root) { return [IO.Path]::GetFullPath($Root) }
    $scriptContainer=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
    $candidates=@($scriptContainer,[IO.Path]::GetFullPath((Split-Path -Parent $scriptContainer)))|Select-Object -Unique
    foreach($candidate in $candidates) {
        if((Test-Path -LiteralPath (Join-Path $candidate 'agents.json')) -or
           (Test-Path -LiteralPath (Join-Path $candidate '.agents/managed.json')) -or
           (Test-Path -LiteralPath (Join-Path $candidate 'plugin.json'))) {
            return $candidate
        }
    }
    try { return Get-AgentRoot $PSScriptRoot } catch { return Get-AgentRoot }
}
function Get-CliRemainingQuery {
    return @($Remaining|Where-Object { $_ -and $_ -notmatch '^--?(json|readonly|expand|verify-official|dry-run|apply)$' })
}
function Resolve-CliScriptPath([string]$Script) {
    $root=Get-CliRoot
    $candidates=@($Script)
    if($Script -match '^scripts/') { $candidates+=@(".agents/$Script") }
    foreach($candidate in $candidates) {
        $full=Resolve-SafePath $root $candidate
        if(Test-Path -LiteralPath $full -PathType Leaf) { return $full }
    }
    throw "CLI script is not deployed for this layout: $Script"
}
function Get-AgentPlan([string]$ProjectRoot) {
    $files=@(Get-AgentFiles $ProjectRoot)
    $status=@(Invoke-AgentGit $ProjectRoot @('status','--short'))
    $changed=@($status|ForEach-Object { if($_.Length -gt 3){$_.Substring(3).Trim()} }|Where-Object { $_ })
    $selected=if($Path.Count){@($files|Where-Object { $_ -in $Path })}else{$files}
    $plan=[ordered]@{
        schema_version='agents-plan/v5';root=[IO.Path]::GetFullPath($ProjectRoot);command_version='5.0.0'
        source_digest=(Get-SourceDigest $ProjectRoot $selected);changed_paths=$changed;selected_paths=$selected
        acceptance=@('Actual source and Git state are rechecked before mutation','Only authorized managed paths may change','A persisted receipt identifies every executed check')
        checks=@(@{id='syntax';mode='changed'},@{id='json-schema';mode='changed'},@{id='ownership';mode='delivery'},@{id='knowledge';mode='affected'},@{id='runtime';mode='delivery'})
        side_effects=@('Plan is read-only','Validation may write registered runtime receipts','Deployment and cleanup require separate explicit commands')
        telemetry='disabled';native_memory='supplementary only';status='planned'
    }
    $plan.digest=Get-TextHash (ConvertTo-AgentCanonicalJson $plan)
    return $plan
}
function Invoke-CliScript([string]$Script,[string[]]$Arguments) {
    $full=Resolve-CliScriptPath $Script
    $out=@(& pwsh -NoProfile -File $full @Arguments 2>&1)
    $code=$LASTEXITCODE
    if($Json) {
        $text=($out -join "`n"); try { $text|ConvertFrom-Json -AsHashtable -Depth 80|ConvertTo-Json -Depth 80 -Compress } catch { $text }
    } else { $out }
    if($code -ne 0){exit $code}
}
try {
    $projectRoot=Get-CliRoot
    $effectivePaths=@($Path|ForEach-Object { $_ -split ',' }|Where-Object { $_ })
    if($effectivePaths.Count){$Path=$effectivePaths}
    switch($Command) {
        help {
            Write-AgentCliResult ([ordered]@{name='Agents';version='5.0.0';usage='agents.ps1 <command> <subcommand>';commands=@('init','plan','validate changed|checkpoint','memory recall|promote|retract','task save|resume|complete|reconcile','runtime list|plan-cleanup|cleanup|reconcile','deploy dry-run|apply|rollback','doctor','package');telemetry='disabled'})
        }
        plan { Write-AgentCliResult (Get-AgentPlan $projectRoot) }
        validate {
            $p=if($Subcommand -eq 'checkpoint'){'Checkpoint'}else{'Changed'}
            $args=@('-Scope',$Scope,'-Profile',$p,'-Root',$projectRoot);if($Path.Count){$args+=@('-Path',($Path -join ','))};if($Json){$args+=@('-Json')};if($RequireReleaseReady){$args+=@('-RequireReleaseReady')}
            Invoke-CliScript 'scripts/validate.ps1' $args
        }
        memory {
            $action=switch($Subcommand){'recall'{'Recall'}'promote'{'Promote'}'retract'{'Retract'}'index'{'Index'}default{'Recall'}}
            $args=@('-Action',$action);if($InputPath){$args+=@('-InputPath',$InputPath)};if($Json){$args+=@('-Expand')};if($Expand){$args+=@('-Expand')};if($VerifyOfficial){$args+=@('-VerifyOfficial')};if($ReadOnly){$args+=@('-ReadOnly')};if($Remaining){$args+=@('-Query')+$Remaining};Invoke-CliScript 'scripts/project-memory.ps1' $args
        }
        task {
            $action=switch($Subcommand){'save'{'Save'}'resume'{'Resume'}'complete'{'Complete'}'reconcile'{'Reconcile'}default{'List'}}
            $args=@('-Action',$action);foreach($pair in @(@('-Id',$Id),@('-InputPath',$InputPath),@('-ExpectedDigest',$ExpectedDigest),@('-Reason',$Reason))){if($pair[1]){$args+=$pair}};if($ExpectedRevision -ge 0){$args+=@('-ExpectedRevision',$ExpectedRevision)};if($ReadOnly){$args+=@('-ReadOnly')};Invoke-CliScript 'scripts/task-state.ps1' $args
        }
        runtime {
            $action=switch($Subcommand){'list'{'List'}'plan-cleanup'{'PlanCleanup'}'cleanup'{'Cleanup'}'reconcile'{'Reconcile'}'retire'{'Retire'}default{'List'}}
            $args=@('-Action',$action);foreach($pair in @(@('-Id',$Id),@('-InputPath',$InputPath),@('-ExpectedDigest',$ExpectedDigest),@('-Reason',$Reason))){if($pair[1]){$args+=$pair}};if($ReadOnly){$args+=@('-ReadOnly')};if($Expand){$args+=@('-Expand')};Invoke-CliScript 'scripts/runtime-artifacts.ps1' $args
        }
        deploy {
            if(-not $TargetPath){throw 'TargetPath is required for deploy.'}
            $args=@('-TargetPath',$TargetPath,'-LayoutProfile',$Layout,'-Provider',$projectRoot);if($DryRun -or $Subcommand -eq 'dry-run'){$args+=@('-DryRun')};if($ExpectedDigest){$args+=@('-ExpectedPlanDigest',$ExpectedDigest)};Invoke-CliScript 'scripts/deploy-agents-workflow.ps1' $args
        }
        init {
            if(-not $TargetPath){$TargetPath=$projectRoot};$args=@('-TargetPath',$TargetPath,'-LayoutProfile',$Layout,'-Provider',$projectRoot);if(-not $Apply){$args+=@('-DryRun')}elseif(-not $ExpectedDigest){throw 'init -Apply requires ExpectedDigest from the dry-run plan'}else{$args+=@('-ExpectedPlanDigest',$ExpectedDigest)};Invoke-CliScript 'scripts/deploy-agents-workflow.ps1' $args
        }
        doctor {
            $settings=Get-ProjectSettings $projectRoot
            $versionPath=Join-Path $projectRoot 'agents.json'
            if(-not (Test-Path -LiteralPath $versionPath -PathType Leaf)) { $versionPath=Join-Path $projectRoot '.agents/managed.json' }
            Write-AgentCliResult ([ordered]@{schema_version='agents-doctor/v5';root=$projectRoot;version=(Read-AgentJson $versionPath).version;powershell=$PSVersionTable.PSVersion.ToString();git=[string](& git --version);git_root=([string](Invoke-AgentGit $projectRoot @('rev-parse','--show-toplevel')));telemetry='disabled';runtime=(Test-Path (Join-Path $projectRoot '.agents/runtime'));managed=(Test-Path (Join-Path $projectRoot '.agents/managed.json'));status='ready'})
        }
        package { $args=@('-Root',$projectRoot);if($OutputPath){$args+=@('-OutputPath',$OutputPath)};Invoke-CliScript 'scripts/package-plugin.ps1' $args }
    }
} catch {
    $cliError=[ordered]@{ok=$false;schema_version='agents-error/v5';error_code='AGENTS_CLI_ERROR';message=$_.Exception.Message;command=$Command;subcommand=$Subcommand}
    $cliError|ConvertTo-Json -Depth 20 -Compress
    exit 2
}
