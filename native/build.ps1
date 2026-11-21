param()
$ErrorActionPreference = 'Stop'
# Cross-compile native modules for PenBili S6 Pro (armv7 hard-float, glibc 2.23).
#
#   libjsapi_player.so   = player.c   (ffmpeg 软解 -> /dev/fb0 + aplay)
#   libjsapi_httpjson.so = httpjson.c (native HTTP / 下载 / DNS 自愈)
#
# Output goes to ../libs and is packaged into the AMR by aiot-vue-cli as-is.
# X6/X7 Pro 的 player2（GStreamer/MPP/KMS）源码保留在 native/csrc 作为上游参考，
# 本分支不编译、不装载。
#
# zig: 优先 $env:ZIG_EXE，其次 ../env/pyzig/ziglang/zig.exe（本地开发布局）。
$scriptPath = $MyInvocation.MyCommand.Path
if (-not $scriptPath) { $scriptPath = $PSCommandPath }
$base = Split-Path -Parent (Split-Path -Parent $scriptPath)
$zig = $env:ZIG_EXE
if (-not $zig) {
  $cand = Join-Path (Split-Path -Parent $base) 'env\pyzig\ziglang\zig.exe'
  if (Test-Path $cand) { $zig = $cand }
}
if (-not $zig -or -not (Test-Path $zig)) { Write-Error 'zig not found: set ZIG_EXE to zig.exe (ziglang.org, or pip install ziglang)' }
$cache = if ($env:ZIG_CACHE) { $env:ZIG_CACHE } else { Join-Path $env:TEMP 'penbili-zig-cache' }
$env:ZIG_GLOBAL_CACHE_DIR = $cache
$env:ZIG_LOCAL_CACHE_DIR = $cache + '-local'

$triple = 'arm-linux-gnueabihf.2.23'
$stubDir = "$base\native\stubs"
if (-not (Test-Path $stubDir)) { Write-Error "stub dir not found: $stubDir" }

$common = @(
  # -g0: no DWARF; -Wl,-s: strip symtab (keeps .dynsym exports)
  'cc', '-target', $triple, '-O2', '-g0', '-Wl,-s', '-fPIC', '-shared', '-fvisibility=hidden',
  '-I', "$base\native\iot-miniapp-sdk\include"
)

$zargs = @(
  'cc', '-target', $triple, '-Os', '-g0', '-Wl,-s', '-fPIC', '-shared',
  '-I', "$base\native\iot-miniapp-sdk\include",
  '-o', "$base\libs\libjsapi_player.so",
  "$base\native\csrc\player.c",
  "-Wl,-version-script=$base\native\player_exports.map",
  '-lpthread'
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
foreach ($so in @('libjsapi_player.so', 'libjsapi_httpjson.so')) {
  python "$base\tools\inspect_elf.py" "$base\libs\$so" --target arm | Select-Object -Last 5
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
