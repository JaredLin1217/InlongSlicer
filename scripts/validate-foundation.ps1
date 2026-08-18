function Test-FoundationCreationIntegrity {
$startFailureCount = $Failures.Count
$requiredFiles = @(
".agents/docs/agents/openai-foundations.yaml",
"schemas/agents-openai-foundations.schema.json"
)
$allRequiredFilesExist = $true
foreach ($path in $requiredFiles) {
if (-not (Test-Path -LiteralPath (Get-RepoPath $path) -PathType Leaf)) {
Add-Failure ("Foundation creation required file is missing: {0}" -f $path)
$allRequiredFilesExist = $false
}
}
if (-not $allRequiredFilesExist) {
return
}
$canonicalFile = Get-Item -LiteralPath (Get-RepoPath ".agents/docs/agents/openai-foundations.yaml")
$foundationText = Get-Content -LiteralPath $canonicalFile.FullName -Raw
$aiRuntimeText = Get-Content -LiteralPath (Get-RepoPath ".agents/docs/agents/ai-runtime.yaml") -Raw
$schemasText = Get-Content -LiteralPath (Get-RepoPath ".agents/docs/agents/schemas.yaml") -Raw
$verifyText = Get-Content -LiteralPath (Get-RepoPath ".agents/docs/agents/verify.yaml") -Raw
$deployText = Get-Content -LiteralPath (Get-RepoPath ".agents/docs/agents/deploy.yaml") -Raw
foreach ($marker in @(
"official_docs_first",
"progressive_disclosure",
"one_state_owner",
"structured_outputs",
"conversation_state",
"agents_sdk",
"codex_instructions",
"codex_skills",
"codex_memories",
"subagents",
"prompt_caching",
"predicted_outputs",
"compaction",
"token_counting",
"evaluations",
"evaluation"
)) {
if (-not $foundationText.Contains($marker)) {
Add-Failure ("Foundation creation canonical is missing marker: {0}" -f $marker)
}
}
$foundationValues = Get-LightweightYamlPathValues -File $canonicalFile
foreach ($marker in @("source_matrix:", "checked:", "capability:", "url:", "boundary:", "drift_risk:", "refresh_interval_days:", "next_review_due:")) {
if (-not $foundationText.Contains($marker)) {
Add-Failure ("Foundation source matrix is missing field or value: {0}" -f $marker)
}
}
if (-not $foundationValues.ContainsKey("source_matrix.checked")) {
Add-Failure "Foundation source matrix top-level checked date is missing."
}
$sourceMatrix = @(
@{ Id = "structured_outputs"; Capability = "Structured Outputs"; Url = "https://developers.openai.com/api/docs/guides/structured-outputs" },
@{ Id = "conversation_state"; Capability = "conversation state"; Url = "https://developers.openai.com/api/docs/guides/conversation-state" },
@{ Id = "agents_sdk"; Capability = "Agents SDK"; Url = "https://openai.github.io/openai-agents-python/" },
@{ Id = "codex_instructions"; Capability = "Codex AGENTS.md"; Url = "https://learn.chatgpt.com/docs/agent-configuration/agents-md" },
@{ Id = "codex_skills"; Capability = "Codex skills"; Url = "https://learn.chatgpt.com/docs/build-skills" },
@{ Id = "codex_memories"; Capability = "Codex memories"; Url = "https://learn.chatgpt.com/docs/customization/memories" },
@{ Id = "subagents"; Capability = "Codex subagents"; Url = "https://learn.chatgpt.com/docs/agent-configuration/subagents" },
@{ Id = "prompt_caching"; Capability = "prompt caching"; Url = "https://developers.openai.com/api/docs/guides/prompt-caching" },
@{ Id = "predicted_outputs"; Capability = "predicted outputs"; Url = "https://developers.openai.com/api/docs/guides/predicted-outputs" },
@{ Id = "compaction"; Capability = "conversation compaction"; Url = "https://developers.openai.com/api/docs/guides/compaction" },
@{ Id = "token_counting"; Capability = "token counting"; Url = "https://developers.openai.com/api/docs/guides/token-counting" },
@{ Id = "evaluations"; Capability = "agent evaluations"; Url = "https://developers.openai.com/api/docs/guides/agent-evals" }
)
$culture = [System.Globalization.CultureInfo]::InvariantCulture
$todayUtc = (Get-Date).ToUniversalTime().Date
foreach ($source in $sourceMatrix) {
$prefix = "source_matrix.{0}" -f $source.Id
foreach ($field in @("capability", "checked", "refresh_interval_days", "next_review_due", "url", "boundary", "drift_risk")) {
$path = "{0}.{1}" -f $prefix, $field
if (-not $foundationValues.ContainsKey($path)) {
Add-Failure ("Foundation source matrix field is missing: {0}" -f $path)
}
}
if ($foundationValues.ContainsKey("$prefix.capability") -and [string] $foundationValues["$prefix.capability"] -ne [string] $source.Capability) {
Add-Failure ("Foundation source matrix capability mismatch: {0}" -f $source.Id)
}
if ($foundationValues.ContainsKey("$prefix.url") -and [string] $foundationValues["$prefix.url"] -ne [string] $source.Url) {
Add-Failure ("Foundation source matrix URL mismatch: {0}" -f $source.Id)
}
if ($foundationValues.ContainsKey("$prefix.checked") -and $foundationValues.ContainsKey("$prefix.refresh_interval_days") -and $foundationValues.ContainsKey("$prefix.next_review_due")) {
try {
$checked = [datetime]::ParseExact([string] $foundationValues["$prefix.checked"], "yyyy-MM-dd", $culture)
$refresh = [int] $foundationValues["$prefix.refresh_interval_days"]
$due = [datetime]::ParseExact([string] $foundationValues["$prefix.next_review_due"], "yyyy-MM-dd", $culture)
if ($refresh -ne 90) {
Add-Failure ("Foundation source matrix refresh interval must be 90 days: {0}" -f $source.Id)
}
if ($due.Date -ne $checked.AddDays($refresh).Date) {
Add-Failure ("Foundation source matrix due date does not match checked date plus refresh interval: {0}" -f $source.Id)
}
if ($due.Date -lt $todayUtc) {
Add-Failure ("Foundation source matrix review is overdue: {0}" -f $source.Id)
}
}
catch {
Add-Failure ("Foundation source matrix freshness fields are invalid: {0}" -f $source.Id)
}
}
}
foreach ($marker in @("foundation_creation", ".agents/docs/agents/openai-foundations.yaml")) {
if (-not $aiRuntimeText.Contains($marker)) {
Add-Failure ("Foundation creation route is missing marker: {0}" -f $marker)
}
}
if ($aiRuntimeText -notmatch 'foundation_creation:\s*\{\s*f:\s*\[[^\]]*".agents/docs/agents/openai-foundations\.yaml"[^\]]*".agents/docs/agents/schemas\.yaml"[^\]]*".agents/docs/agents/verify\.yaml"[^\]]*\]') {
Add-Failure "Foundation creation route must load openai-foundations, schemas, and verify."
}
foreach ($marker in @("foundation_creation", "Structured Outputs", "state owner")) {
if (-not $schemasText.Contains($marker)) {
Add-Failure ("Foundation creation schema registry is missing marker: {0}" -f $marker)
}
}
foreach ($marker in @("foundation_creation", "foundation_creation_integrity")) {
if (-not $verifyText.Contains($marker)) {
Add-Failure ("Foundation creation verify profile is missing marker: {0}" -f $marker)
}
}
if (-not $deployText.Contains('from: ".agents/docs/agents/openai-foundations.yaml"')) {
Add-Failure "Foundation creation deploy source is missing."
}
if ($Failures.Count -eq $startFailureCount) {
Add-Pass "Foundation creation integrity checks passed."
}
}
