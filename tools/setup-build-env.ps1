param(
  [string]$CliVersion = '1.0.32'
)
$ErrorActionPreference = 'Stop'
# Prepare the PenBili build environment: aiot-vue-cli (Falcon miniapp build CLI).
#
# package.json build scripts read ../env/node_modules/aiot-vue-cli/src/cli.js,
# so the npm package is installed into the sibling env/ directory (not repo node_modules).
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/setup-build-env.ps1
#   powershell ... -File tools/setup-build-env.ps1 -CliVersion 1.0.32

$scriptPath = $MyInvocation.MyCommand.Path
if (-not $scriptPath) { $scriptPath = $PSCommandPath }
$repo = Split-Path -Parent (Split-Path -Parent $scriptPath)
$envDir = Join-Path (Split-Path -Parent $repo) 'env'
$cli = Join-Path $envDir 'node_modules\aiot-vue-cli\src\cli.js'

if (Test-Path $cli) {
  Write-Host "aiot-vue-cli ready: $cli"
  exit 0
}

$npm = (Get-Command npm.cmd -ErrorAction SilentlyContinue)
if (-not $npm) { $npm = (Get-Command npm -ErrorAction SilentlyContinue) }
if (-not $npm) {
  Write-Error 'npm not found: install Node.js (16+) first, then rerun this script.'
}

New-Item -ItemType Directory $envDir -Force | Out-Null
Write-Host "Installing aiot-vue-cli@$CliVersion into $envDir ..."
& $npm.Source install --prefix $envDir --no-save --no-audit --no-fund "aiot-vue-cli@$CliVersion"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not (Test-Path $cli)) {
  Write-Error "Install finished but $cli is missing. Check the npm output."
}
Write-Host "Done: $cli"
Write-Host 'Next: npm run build, then run tools\qjsbuild\qjs-style-scale.exe as described in README.'
