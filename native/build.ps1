param(
  [string]$Target = ''
)
$ErrorActionPreference = 'Stop'
# Cross-compile native modules (zig toolchain):
#   libjsapi_player.so   = player.c (v1 fallback: fb0 + aplay, X5 path)
#   libjsapi_player2.so  = player2 (v2: in-process GStreamer/MPP/KMS, X6 Pro path)
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

# player2 (v2) links against the device GStreamer/GLib import stubs. They live
# next to the other stub dirs so the build stays self-contained (no .tmp dep).
$gstStubDir = "$base\native\stubs-aarch64-gst"

if ($Target -eq 'aarch64') {
  # aarch64 needs ELFCLASS64 import stubs for libquickjs/libcurl
  $triple = 'aarch64-linux-gnu.2.17'
  $stubDir = "$base\native\stubs-aarch64"
  # X6 Pro has no /dev/fb0: player.c drops the fbdev backend and uses the KMS overlay plane
  $extra = @('-DPENBILI_KMS_ONLY=1')
  $buildPlayer2 = $true
  Write-Host "target = aarch64 (X6 Pro)"
} else {
  $triple = 'arm-linux-gnueabihf.2.23'
  $stubDir = "$base\native\stubs"
  $extra = @()
  # player2 targets X6 Pro only: the X5 has no GStreamer and no /dev/dri at all.
  $buildPlayer2 = $false
  Write-Host "target = arm 32-bit (X5) - player2 skipped (X6 Pro only)"
}
if (-not (Test-Path $stubDir)) { Write-Error "stub dir not found: $stubDir" }
if ($buildPlayer2 -and -not (Test-Path $gstStubDir)) {
  Write-Error "GStreamer stub dir not found: $gstStubDir"
}

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

# ---- player2 (v2): in-process GStreamer, X6 Pro only ----
if ($buildPlayer2) {
  # Source list is explicit (no wildcards) so a stray file can never enter the .so.
  $p2src = @(
    "$base\native\csrc\jsapi_player2.c",
    "$base\native\csrc\core\log.c",
    "$base\native\csrc\core\clock.c",
    "$base\native\csrc\core\queue.c",
    "$base\native\csrc\session\session.c",
    "$base\native\csrc\session\cmd.c",
    "$base\native\csrc\video\kms_plane.c",
    "$base\native\csrc\audio\audio_out.c",
    "$base\native\csrc\source\src_http.c"
  )
  $p2args = $common + @('-I', "$base\native\csrc") + $p2src + @(
    # Device GStreamer/GLib import stubs (SONAME only; never shipped to device).
    "$gstStubDir\libgstreamer-1.0.so.0",
    "$gstStubDir\libgobject-2.0.so.0",
    "$gstStubDir\libglib-2.0.so.0",
    "$stubDir\libquickjs.so",
    '-lpthread',
    '-Wl,-soname,libjsapi_player2.so',
    '-o', "$base\libs\libjsapi_player2.so"
  )
  & $zig @p2args
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Get-ChildItem "$base\libs\*.so" | Select-Object Name, Length
# ELF structure / export check (mandatory before packaging into AMR)
# inspect_elf.py validates EI_CLASS + e_machine per target: aarch64 must be ELFCLASS64/183
$checkList = @('libjsapi_player.so', 'libjsapi_httpjson.so')
if ($buildPlayer2) { $checkList += 'libjsapi_player2.so' }
foreach ($so in $checkList) {
  python "$base\tools\inspect_elf.py" "$base\libs\$so" --target $Target | Select-Object -Last 5
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
