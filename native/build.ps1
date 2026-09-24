$ErrorActionPreference = 'Stop'
# 交叉编译 native 模块（zig 工具链，与 video_player_x5 / deepseek-x5 同源）：
#   libjsapi_player.so  = player.c v1.1.0（open() 尾部 userAgent/referer）
#   libjsapi_httpjson.so = httpjson.c（form POST 原样 body；B 站 TV 登录 POST 必需）
#   两者均已在 youdao-x5-fw3.4.6 真机验证；产物由 aiot-vue-cli 原样打包进 AMR libs/
# 路径可移植：$base 由脚本位置推出；zig 用 $env:ZIG_EXE，或回退本机开发布局（../env/pyzig）
$base = Split-Path -Parent $PSScriptRoot
$zig = $env:ZIG_EXE
if (-not $zig) {
  $cand = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'env\pyzig\ziglang\zig.exe'
  if (Test-Path $cand) { $zig = $cand }
}
if (-not $zig -or -not (Test-Path $zig)) { Write-Error '找不到 zig：请设置 ZIG_EXE 指向 zig.exe（ziglang.org 或 pip install ziglang）' }
$cache = if ($env:ZIG_CACHE) { $env:ZIG_CACHE } else { Join-Path $env:TEMP 'penbili-zig-cache' }
$env:ZIG_GLOBAL_CACHE_DIR = $cache
$env:ZIG_LOCAL_CACHE_DIR = $cache + '-local'

if (-not (Test-Path $zig)) { Write-Error "找不到 zig: $zig" }

$common = @(
  # -g0：不生成 DWARF（否则本机源码绝对路径会进 .so——开源前敏感终检的命中项）
  # -Wl,-s：strip 符号表（保留 .dynsym 导出，custom_init_jsapis 不受影响，构建脚本自检）
  'cc', '-target', 'arm-linux-gnueabihf.2.23', '-O2', '-g0', '-Wl,-s', '-fPIC', '-shared', '-fvisibility=hidden',
  '-I', "$base\native\iot-miniapp-sdk\include"
)
$zargs = $common + @(
  '-o', "$base\libs\libjsapi_player.so",
  "$base\native\csrc\player.c",
  "$base\native\stubs\libquickjs.so",
  '-lpthread',
  '-Wl,-soname,libjsapi_player.so'
)
& $zig @zargs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$hargs = $common + @(
  '-o', "$base\libs\libjsapi_httpjson.so",
  "$base\native\csrc\httpjson.c",
  "$base\native\stubs\libquickjs.so",
  "$base\native\stubs\libcurl.so.4",
  '-lpthread',
  '-Wl,-soname,libjsapi_httpjson.so'
)
& $zig @hargs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Get-ChildItem "$base\libs\*.so" | Select-Object Name, Length
# ELF 结构 / 导出符号检查（进入 AMR 前的必做项）
foreach ($so in @('libjsapi_player.so', 'libjsapi_httpjson.so')) {
  python "$base\tools\inspect_elf.py" "$base\libs\$so" | Select-Object -Last 5
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
