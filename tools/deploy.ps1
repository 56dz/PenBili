# 真机部署与验证脚本（有道 X5，fw3.4.6，profile: profiles/youdao-x5.md）
#
# 流程：
#   1) 设备与产物检查（AMR + native ELF）
#   2) 安装（要求 ret: 0）+ 核对安装 slot 的 manifest / .so 落位
#   3) 切桌面 → 冷启动到规范启动页 index（先切走再启动，保证重建页面实例）
#   4) 取证：中段 captureFB（大概率正播着）→ 30s 后整页截屏 + [bili] 日志
#   5) 两次进出循环：每轮退出后 ffmpeg/aplay 残留 = 0，再次进入重新取证
#   6) 关闭自检种子 → 干净冷启动终态截图（交付状态：无自检）
#   7) 证据落 profiles/evidence/
#
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File tools/deploy.ps1 [-Autotest]
param(
  [string]$StartPage = 'index',
  [switch]$Autotest
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
# adb 定位：$env:BILI_ADB → PATH 中的 adb → 本机开发布局（../env/adb）
$adb = $env:BILI_ADB
if (-not $adb) { $c = Get-Command adb -ErrorAction SilentlyContinue; if ($c) { $adb = $c.Source } }
if (-not $adb) { $cand = Join-Path $root '..\env\adb\adb.exe'; if (Test-Path $cand) { $adb = (Resolve-Path $cand).Path } }
if (-not $adb) { throw '找不到 adb：请设置 BILI_ADB 指向 adb.exe，或将 adb 加入 PATH' }
$appid = '8002026091900004'
$desktop = '8080222437664451'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$ev = Join-Path $root 'profiles\evidence'
$prefsDir = "/userdisk/miniapp/data/mini_app/pkg/$appid/data/sharedpreferences"
$seedFile = Join-Path $root '.deploy\preferences.json'
$wait = if ($Autotest) { 45 } else { 12 }   # 慢网络下自检全程可达 40s+（单请求上界 15s）
New-Item -ItemType Directory -Force -Path $ev | Out-Null

function Step($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }

function Pull-Capture([string]$remote, [string]$local) {
  & $adb pull $remote $local | Out-Null
  if (Test-Path $local) { Write-Host "evidence: $local" } else { Write-Host "MISSING: $local" -ForegroundColor Yellow }
}

function Push-Seed([string]$mode) {
  node (Join-Path $root 'tools\seed_settings.js') $mode | Out-Null
  & $adb shell "mkdir -p $prefsDir" | Out-Null
  & $adb push $seedFile "$prefsDir/preferences.json" | Out-Null
  Write-Host "seed autotest=$mode → $prefsDir/preferences.json"
}

Step '1/7 设备'
& $adb devices -l

Step '2/7 产物与 native 检查'
# 交付包为 PenBili.amr（应用名）；回退 appid.*.amr（aiot-vue-cli 下次构建仍用 appid 命名）
$amr = Get-ChildItem -Path $root -Filter 'PenBili*.amr' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $amr) { $amr = Get-ChildItem -Path $root -Filter "$appid.*.amr" | Sort-Object LastWriteTime -Descending | Select-Object -First 1 }
if (-not $amr) { throw '找不到 AMR 包，请先 npm run build' }
Write-Host "AMR: $($amr.Name) ($([math]::Round($amr.Length/1KB,1)) KB)"
python (Join-Path $root 'tools\inspect_elf.py') (Join-Path $root 'libs\libjsapi_player.so') | Select-Object -Last 5
python (Join-Path $root 'tools\inspect_elf.py') (Join-Path $root 'libs\libjsapi_httpjson.so') | Select-Object -Last 5

Step '3/7 安装（ret:0 校验）'
& $adb push $amr.FullName /tmp/bili_app.amr
$ins = & $adb shell "miniapp_cli install /tmp/bili_app.amr"
$ins
if ("$ins" -notmatch '"ret":\s*0') { throw 'install 未返回 ret:0' }
& $adb shell "find /userdisk/miniapp/data/mini_app/pkg/$appid -maxdepth 3 -name '*.so'"
& $adb shell "find /userdisk/miniapp/data/mini_app/pkg/$appid -maxdepth 3 -name 'manifest*'"

Step '4/7 冷启动首轮（规范启动页' + $StartPage + '，Autotest=' + $Autotest + '）'
if ($Autotest) { Push-Seed 'on' } else { Push-Seed 'off' }
& $adb shell "miniapp_cli start $desktop" | Out-Null
Start-Sleep -Seconds 2
$st = & $adb shell "miniapp_cli start $appid $StartPage"
$st
Start-Sleep -Seconds 10
$fb1 = "/tmp/bili_${stamp}_app1_fb.png"
& $adb shell "miniapp_cli captureFB $fb1"
Pull-Capture $fb1 (Join-Path $ev "app1_${stamp}_fb.png")   # 可能截到播放中帧（自检中段）
Start-Sleep -Seconds ([Math]::Max(2, $wait - 10))
$cap1 = "/tmp/bili_${stamp}_app1.png"
& $adb shell "miniapp_cli capture $cap1"
Pull-Capture $cap1 (Join-Path $ev "app1_${stamp}.png")

$log1 = & $adb shell "grep -h '\[bili\]' /userdata/applog/DictPen_*.log | tail -n 60"
$log1
$log1 | Out-File -FilePath (Join-Path $ev "app1_${stamp}_log.txt") -Encoding utf8
$imgLog = & $adb shell "grep -h 'ImageLoader' /userdata/applog/DictPen_*.log | tail -n 8"
$imgLog

Step '5/7 进出循环 1：退出 → 残留检查 → 再进入'
& $adb shell "miniapp_cli start $desktop" | Out-Null
Start-Sleep -Seconds 3
$left1 = & $adb shell "ps | grep -c '[f]fmpeg'"
$left1a = & $adb shell "ps | grep -c '[a]play'"
Write-Host "exit1 残留: ffmpeg=$left1 aplay=$left1a (期望 0/0)"
& $adb shell "miniapp_cli start $appid $StartPage" | Out-Null
Start-Sleep -Seconds $wait
$cap2 = "/tmp/bili_${stamp}_app2.png"
& $adb shell "miniapp_cli capture $cap2"
Pull-Capture $cap2 (Join-Path $ev "app2_${stamp}.png")
$log2 = & $adb shell "grep -h '\[bili\]' /userdata/applog/DictPen_*.log | tail -n 40"
$log2 | Out-File -FilePath (Join-Path $ev "app2_${stamp}_log.txt") -Encoding utf8

Step '6/7 进出循环 2：退出 → 残留检查 → 关种子干净启动'
& $adb shell "miniapp_cli start $desktop" | Out-Null
Start-Sleep -Seconds 3
$left2 = & $adb shell "ps | grep -c '[f]fmpeg'"
$left2a = & $adb shell "ps | grep -c '[a]play'"
Write-Host "exit2 残留: ffmpeg=$left2 aplay=$left2a (期望 0/0)"
Push-Seed 'off'   # 交付状态：不带自检
& $adb shell "miniapp_cli start $appid $StartPage" | Out-Null
Start-Sleep -Seconds 10
$cap3 = "/tmp/bili_${stamp}_final.png"
& $adb shell "miniapp_cli capture $cap3"
Pull-Capture $cap3 (Join-Path $ev "final_${stamp}.png")
& $adb shell "grep -h 'dynamic_load_jsapi' /userdata/applog/DictPen_*.log | tail -n 5"

Step '7/7 汇总'
& $adb shell "miniapp_cli memoryApp" | Select-Object -First 6 | Tee-Object -FilePath (Join-Path $ev "memory_${stamp}.txt") | Out-Null
Write-Host "证据目录: $ev"
Write-Host "检查点：install ret:0 / AUTOTEST PASS 日志 / app1 截图像素非黑 / 残留 0/0 / final 无自检冷启动"
