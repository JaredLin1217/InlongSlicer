#requires -Version 7.4
param([ValidateSet('Provider','Consumer')][string]$Scope='Provider',
      [ValidateSet('Changed','Checkpoint')][string]$Profile='Changed',[string[]]$Path=@(),[string]$Root,[switch]$Json,[switch]$RequireReleaseReady)
. "$PSScriptRoot/agent-checks.ps1"
$root=if($Root){[IO.Path]::GetFullPath($Root)}else{Get-AgentRoot}
$Path=@($Path|ForEach-Object { $_ -split ',' }|Where-Object { $_ })
$report=Invoke-AgentChecks -Root $root -Scope $Scope -Profile $Profile -Path $Path -RequireReleaseReady:$RequireReleaseReady
if($Json) { $report|ConvertTo-Json -Depth 20 }
else {
    foreach($check in $report.checks) { '{0}: {1} ({2} ms) {3}' -f $check.id,$check.result,$check.duration_ms,($check.details -join '; ') }
    "Receipt: $($report.receipt_path); result=$($report.result); release_ready=$($report.release_ready)"
}
if(-not $report.passed) { exit 1 }
