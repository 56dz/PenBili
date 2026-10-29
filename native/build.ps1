param(
  [string]$Target = ''
)
$ErrorActionPreference = 'Stop'
# Cross-compile native modules (zig toolchain):
#   libjsapi_player.so   = player.c (v3.0.0 KMS overlay + mppvideodec hw decode)
#   libjsapi_httpjson.so = httpjson.c (form POST raw body; bilibili TV login POST)
# Output is packaged into the AMR libs/ by aiot-vue-cli as-is.
#
# ===== Target arch (confirm before build!) =====
#   X5 (youdao-x5, fw3.4.6)    : armv7 hard-float 32-bit -> dynamic loader wants ELFCLASS32
#   X6 Pro (melon_pro, fw4.3.5): aarch64 64-bit          -> loading 32-bit lib fails with
#                                "wrong ELF class: ELFCLASS32:dlopen in dynamic_load_jsapi"
# Default target: aarch64 (current daily driver is X6 Pro).
# For X5: .\build.ps1 -Target arm   (or set env PENBILI_TARGET=arm|aarch64)
if (-not $Target) {
  $Target = if ($env:PENBILI_TARGET) { $env:PENBILI_TARGET } else { 'aarch64' }
}
if ($Target -ne 'aarch64' -and $Target -ne 'arm') { Write-Error "bad -Target: $Target" }

# Portable paths: $base derived from script location; zig via $env:ZIG_EXE,
# else local dev layout (../env/pyzig).
$base = Split-Path -Parent $PSScriptRoot
$zig = $env:ZIG_EXE
if (-not $zig) {
  $cand = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'env\pyzig\ziglang\zig.exe'
  if (Test-Path $cand) { $zig = $cand }
}
if (-not $zig -or -not (Test-Path $zig)) { Write-Error 'zig not found: set ZIG_EXE to zig.exe (ziglang.org, or pip install ziglang)' }
$cache = if ($env:ZIG_CACHE) { $env:ZIG_CACHE } else { Join-Path $env:TEMP 'penbili-zig-cache' }
$env:ZIG_GLOBAL_CACHE_DIR = $cache
$env:ZIG_LOCAL_CACHE_DIR = $cache + '-local'

if ($Target -eq 'aarch64') {
  # aarch64 needs ELFCLASS64 import stubs for libquickjs/libcurl
  $triple = 'aarch64-linux-gnu.2.17'
  $stubDir = "$base\native\stubs-aarch64"
  # X6 Pro has no /dev/fb0: player.c drops the fbdev backend and uses the KMS overlay plane
  $extra = @('-DPENBILI_KMS_ONLY=1')
  Write-Host "target = aarch64 (X6 Pro)"
} else {
  $triple = 'arm-linux-gnueabihf.2.23'
  $stubDir = "$base\native\stubs"
  $extra = @()
  Write-Host "target = arm 32-bit (X5)"
}
if (-not (Test-Path $stubDir)) { Write-Error "stub dir not found: $stubDir" }

$common = @(
  # -g0: no DWARF (otherwise absolute source paths land in the .so - a sensitive-info hit)
  # -Wl,-s: strip symtab (keeps .dynsym exports; custom_init_jsapis unaffected; build self-check)
  'cc', '-target', $triple, '-O2', '-g0', '-Wl,-s', '-fPIC', '-shared', '-fvisibility=hidden',
  '-I', "$base\native\iot-miniapp-sdk\include"
) + $extra

$zargs = $common + @(
  '-o', "$base\libs\libjsapi_player.so",
  "$base\native\csrc\player.c",
  "$stubDir\libquickjs.so",
  '-lpthread',
  '-Wl,-soname,libjsapi_player.so'
)
& $zig @zargs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$hargs = $common + @(
  '-o', "$base\libs\libjsapi_httpjson.so",
  "$base\native\csrc\httpjson.c",
  "$stubDir\libquickjs.so",
  "$stubDir\libcurl.so.4",
  '-lpthread',
  '-Wl,-soname,libjsapi_httpjson.so'
)
& $zig @hargs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Get-ChildItem "$base\libs\*.so" | Select-Object Name, Length
# ELF structure / export check (mandatory before packaging into AMR)
# inspect_elf.py validates EI_CLASS + e_machine per target: aarch64 must be ELFCLASS64/183
foreach ($so in @('libjsapi_player.so', 'libjsapi_httpjson.so')) {
  python "$base\tools\inspect_elf.py" "$base\libs\$so" --target $Target | Select-Object -Last 5
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
