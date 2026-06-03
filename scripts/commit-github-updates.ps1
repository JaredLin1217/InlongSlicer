[CmdletBinding()]
param(
[string]$BranchName = ""
)
$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($BranchName)) {
if (-not [string]::IsNullOrWhiteSpace($env:GITHUB_REF_NAME)) {
$BranchName = $env:GITHUB_REF_NAME
}
}
if ([string]::IsNullOrWhiteSpace($BranchName)) {
throw "BranchName is required outside GitHub Actions."
}
git add -- docs/github-updates.md
if ($LASTEXITCODE -ne 0) {
throw "Unable to stage docs/github-updates.md."
}
git diff --cached --quiet -- docs/github-updates.md
$diffExitCode = $LASTEXITCODE
if ($diffExitCode -eq 0) {
Write-Host "No public update document changes."
exit 0
}
elseif ($diffExitCode -ne 1) {
throw "Unable to inspect staged public update document changes."
}
git config user.name "github-actions[bot]"
if ($LASTEXITCODE -ne 0) {
throw "Unable to configure git user.name."
}
git config user.email "41898282+github-actions[bot]@users.noreply.github.com"
if ($LASTEXITCODE -ne 0) {
throw "Unable to configure git user.email."
}
git commit -m "docs: update GitHub update log [skip ci]"
if ($LASTEXITCODE -ne 0) {
throw "Unable to commit docs/github-updates.md."
}
git push origin "HEAD:$BranchName"
if ($LASTEXITCODE -ne 0) {
throw "Unable to push docs/github-updates.md update."
}