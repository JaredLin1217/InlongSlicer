function Get-CanonicalEvidenceTemplateFields {
param([string] $Section)
if ($Section -eq "hard_isolation_evidence") {
return @(
"Task",
"Agent or controller",
"Requested guarantee",
"Forbidden paths or resources",
"User-approved external paths or actions",
"Evidence record location",
"Date",
"Classification",
"Control type",
"Control owner",
"Configuration source",
"Who can change or bypass the control",
"Allowed read roots",
"Allowed write roots",
"Forbidden read roots",
"Forbidden write roots",
"Credential or account scope",
"Network or cloud scope",
"Verifier",
"Verification command, UI state, or log",
"Result",
"Observed blocked-access behavior",
"Verification avoided reading forbidden content",
"If no, exact user authorization",
"Expiry, cleanup, or rollback step",
"System/global resources",
"Global Memory",
"Global Skill",
"Project-external reads",
"Project-external writes",
"Remaining risk"
)
}
if ($Section -eq "runtime_multi_agent_validation") {
return @(
"Validation id",
"Date",
"Controller",
"User request",
"Claim scope",
"Runtime id",
"Nickname",
"Mode",
"Role",
"Task",
"Runtime enforcement",
"Real employee runtime or authorized manual-detached evidence",
"Expected blocking behavior",
"Sidebar or user-control behavior",
"Ledger required",
"Ledger path",
"Ledger event ids reviewed",
"Roster required",
"Roster path",
"Roster snapshot version",
"Roster fields reviewed",
"Controller lease id",
"Controller lease status",
"Assignment path or prompt",
"Ledger requirement included",
"Ledger write permission included",
"Roster requirement included",
"Roster write permission included",
"Status-event rule included",
"Final report schema included",
"Owned write scope",
"Normalized owned write scope",
"Shared read scope",
"Ownership matrix status",
"Final report received",
"Final report matched runtime id",
"Required fields present",
"Expected id set",
"Requested count",
"Spawned count",
"Completed count",
"Closed count",
"Received ids",
"Missing ids",
"Duplicate ids",
"Invalid-format ids",
"Wrong ids",
"Failed ids",
"Running ids",
"Unclosed ids",
"Ack schema",
"Parser normalization",
"Protocol result",
"Task result boundary",
"Project-local skills read",
"System/global resources",
"Global Memory",
"Global Skill",
"Project-external reads",
"Project-external writes",
"Files changed",
"Verification",
"Risks",
"Git status after employee work",
"Out-of-scope changes",
"Ledger event",
"Poll event",
"Report event",
"Close event",
"Reconciled event",
"Matching snapshot version",
"Runtime close attempted",
"Runtime close result",
"Official list result",
"SQLite thread state result",
"session_index_result",
"Global unread state result",
"Rollout residue result",
"Delayed cleanup verification",
"History cleanup requested",
"History cleanup authorization",
"History cleanup target ids",
"History cleanup sources",
"History cleanup result",
"History cleanup evidence",
"Desktop UI refresh result",
"Pass condition met",
"Remaining runtime risk",
"Controller conclusion"
)
}
return @()
}

function Test-CompactEvidenceGroups {
param(
[string] $Section,
[string[]] $Fields
)
$groupsBySection = @{
hard_isolation_evidence = @(
"Task",
"Agent/controller",
"Requested guarantee",
"Forbidden resources",
"Authorized external paths/actions",
"Evidence location",
"Date",
"Classification",
"Control owner/config/bypass",
"Allowed/forbidden read/write roots",
"Credential/account scope",
"Network/cloud scope",
"Verifier/method/log/result",
"Blocked-access behavior",
"No forbidden-content read or authorization",
"Expiry/cleanup/rollback",
"GM/GS/XR/XW",
"Remaining risk"
)
runtime_multi_agent_validation = @(
"Validation id",
"Date",
"Controller",
"User request",
"Claim scope",
"Runtime enforcement",
"Runtime id/nickname/mode/role/task",
"Roster/ledger paths and snapshots",
"Assignment/prompt requirements",
"Owned/shared scope and matrix status",
"Report receipt and id match",
"Expected/spawned/completed/closed counts",
"Missing/duplicate/invalid/wrong/failed/running/unclosed ids",
"Ack/parser/protocol/task boundaries",
"Skills and external access",
"Files changed",
"Verification",
"Risks",
"Ledger/poll/report/close/reconciled evidence",
"Git status and out-of-scope changes",
"Runtime close/list/state cleanup evidence",
"History cleanup authorization/targets/result/evidence",
"UI refresh",
"Pass condition",
"Remaining runtime risk",
"Controller conclusion"
)
}
if (-not $groupsBySection.ContainsKey($Section)) {
return
}
foreach ($group in $groupsBySection[$Section]) {
if ($Fields -notcontains $group) {
Add-Failure ("{0} schema compact group is missing: {1}" -f $Section, $group)
}
}
}

function Get-RequiredEvidenceFields {
param(
[string] $Section,
[string] $NextSection
)
$canonicalFields = @(Get-CanonicalEvidenceTemplateFields -Section $Section)
$schemaContent = Get-Content -LiteralPath (Get-RepoPath ".agents/docs/agents/schemas.yaml") -Raw
$sectionStart = $schemaContent.IndexOf(("{0}:" -f $Section))
$sectionEnd = $schemaContent.IndexOf(("{0}:" -f $NextSection), $sectionStart + 1)
if ($sectionStart -lt 0 -or $sectionEnd -lt 0) {
Add-Failure ("Schema section boundary is missing: {0} -> {1}" -f $Section, $NextSection)
return @()
}
$sectionText = $schemaContent.Substring($sectionStart, $sectionEnd - $sectionStart)
$bulletFields = @([regex]::Matches($sectionText, '^\s*-\s+"([^"]+)"', [System.Text.RegularExpressions.RegexOptions]::Multiline) |
ForEach-Object { $_.Groups[1].Value })
if ($bulletFields.Count -gt 0) {
$fields = $bulletFields
Test-CompactEvidenceGroups -Section $Section -Fields $fields
if ($canonicalFields.Count -gt 0) {
return $canonicalFields
}
return $fields
}
$compactFields = [regex]::Match($sectionText, 'required_fields:\s*\[(.*?)\]', [System.Text.RegularExpressions.RegexOptions]::Singleline)
if ($compactFields.Success) {
$fields = @([regex]::Matches($compactFields.Groups[1].Value, '"([^"]+)"') | ForEach-Object { $_.Groups[1].Value })
Test-CompactEvidenceGroups -Section $Section -Fields $fields
if ($canonicalFields.Count -gt 0) {
return $canonicalFields
}
return $fields
}
if ($canonicalFields.Count -gt 0 -and $sectionText -match 'source:\s*"docs/.+\.template\.md') {
return $canonicalFields
}
return @()
}

function Get-TemplateEvidenceFields {
param([string] $Path)
$groupPrefixes = @(
"Scope",
"Runtime",
"Live evidence",
"Ledger/roster/lease",
"Assignment proof",
"Ownership",
"Final report",
"Batch proof",
"Boundary",
"Work result",
"Events",
"Close/cleanup",
"Dry run",
"Result"
)
$fields = New-Object 'System.Collections.Generic.HashSet[string]'
$content = Get-Content -LiteralPath (Get-RepoPath $Path)
foreach ($line in $content) {
if ($line -notmatch "^\s*-\s+(.+)$") {
continue
}
foreach ($part in ($Matches[1] -split "\|")) {
$field = $part.Trim()
if ([string]::IsNullOrWhiteSpace($field)) {
continue
}
if ($field -match "^([^:]+):\s*(.+)$") {
if ($groupPrefixes -contains $Matches[1].Trim()) {
$field = $Matches[2].Trim()
}
else {
$field = $Matches[1].Trim()
}
}
$field = $field.TrimEnd(":")
[void] $fields.Add($field)
}
}
return @($fields)
}

function Test-EvidenceTemplateSchemaCoverage {
$startFailureCount = $Failures.Count
$checks = @(
@{
Name = "hard-isolation evidence"
Paths = @(".agents/docs/hard-isolation-evidence.template.md", ".agents/docs/templates/agents/hard-isolation-evidence.template.md")
Markers = Get-RequiredEvidenceFields "hard_isolation_evidence" "runtime_multi_agent_validation"
},
@{
Name = "runtime multi-agent validation"
Paths = @(".agents/docs/runtime-multi-agent-validation.template.md", ".agents/docs/templates/agents/runtime-multi-agent-validation.template.md")
Markers = Get-RequiredEvidenceFields "runtime_multi_agent_validation" "runtime_dry_run_evidence"
},
@{
Name = "runtime dry-run evidence"
Paths = @(".agents/docs/runtime-dry-run-evidence.template.md", ".agents/docs/templates/agents/runtime-dry-run-evidence.template.md")
Markers = Get-RequiredEvidenceFields "runtime_dry_run_evidence" "memory_entry"
}
)
foreach ($check in $checks) {
foreach ($path in $check.Paths) {
$content = Get-Content -LiteralPath (Get-RepoPath $path) -Raw
foreach ($marker in $check.Markers) {
if (-not $content.Contains($marker)) {
Add-Failure ("{0} template is missing schema marker in {1}: {2}" -f $check.Name, $path, $marker)
}
}
foreach ($field in Get-TemplateEvidenceFields -Path $path) {
if ($check.Markers -notcontains $field) {
Add-Failure ("{0} schema is missing template field from {1}: {2}" -f $check.Name, $path, $field)
}
}
}
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Evidence template schema coverage checks passed."
}
}
