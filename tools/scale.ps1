param(
  [double]$FontScale = 2.0,
  [double]$HeightScale = (390.0 / 266.0),
  [switch]$NoHeight
)
$ErrorActionPreference = 'Stop'
# PenBili style scale switch.
#   -FontScale 2.0   large font (default, same as pre-sync UI)
#   -FontScale 1.0   original design font; heights still scaled for 640x260
#   -NoHeight        do not scale heights (for non-390 layouts)
# Default input/output: .falcon_\index.js.bin -> .falcon_\index_scaled.js.bin
# The scaled file replaces index.js.bin after success.
# Run this ONCE on a fresh `npm run build` output; running twice scales twice.

$scriptPath = $MyInvocation.MyCommand.Path
if (-not $scriptPath) { $scriptPath = $PSCommandPath }
$repo = Split-Path -Parent (Split-Path -Parent $scriptPath)
$exe = Join-Path $repo 'tools\qjsbuild\qjs-style-scale.exe'
$inBin = Join-Path $repo '.falcon_\index.js.bin'
$outBin = Join-Path $repo '.falcon_\index_scaled.js.bin'

if (-not (Test-Path $exe)) { Write-Error "missing $exe" }
if (-not (Test-Path $inBin)) { Write-Error "missing $inBin; run npm run build first" }

$a = @('--font-scale', $FontScale)
if ($NoHeight) { $a += '--no-height' }
else { $a += @('--height-scale', $HeightScale) }
$a += @($inBin, $outBin)

& $exe @a
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Move-Item $outBin $inBin -Force
Write-Host "scaled: font x$FontScale, height $(if ($NoHeight) { 'off' } else { 'x' + $HeightScale })"
