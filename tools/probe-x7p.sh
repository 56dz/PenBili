#!/usr/bin/env bash
# ============================================================================
# PenBili X7 Pro 真机只读取证
#
# 为什么需要它：X7 Pro 的视口尺寸、cfg.json 的 yoffset、DRM 平面编号、
# GStreamer 插件集合都和 X6 Pro 不同，写死在代码里的 960x266 / 107 / plane 76
# 在别的机型上会让 kmssink 建不起视频分支 → 播放页黑屏。
# 本脚本**只读**（cat / ls / grep / modetest -p / gst-inspect），不改设备任何文件。
#
# 用法（WSL 或 Git Bash 都行）：
#   ADB=/path/to/adb.exe bash tools/probe-x7p.sh [输出目录]
# 默认输出目录：./x7p-probe/<时间戳>/
#
# 产出：
#   cfg.json                 设备 /etc/miniapp/resources/cfg.json 原文
#   drm-summary.txt          /sys/kernel/debug/dri/0/summary 原文（含各窗口 ACTIVE/dst）
#   modetest-planes.txt      平面 id 列表（若设备有 modetest）
#   gst-plugins.txt          mppvideodec / kmssink / h264parse ... 是否存在
#   env.txt                  uname / /dev/dri / /dev/fb* / 机型 / miniapp 版本
#   app-log.txt              PenBili 与 player2 相关日志（grep -a）
#   suggested-profile.json   建议写进 storage 的 bili_device_profile 覆盖值
#   REPORT.md                人读的结论 + 下一步
# ============================================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/x7p-probe/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"

# adb：优先环境变量，其次常见的 Windows 路径（WSL 可直接执行 .exe）
if [ -z "${ADB:-}" ]; then
  for c in \
    "/mnt/c/Users/lenovo/Desktop/8520/PenADB工具箱/PenADB工具箱1/bin/adb.exe" \
    "/mnt/c/Users/lenovo/Desktop/direct/123/adb.exe" \
    "/c/Users/lenovo/Desktop/8520/PenADB工具箱/PenADB工具箱1/bin/adb.exe" \
    "/c/Users/lenovo/Desktop/direct/123/adb.exe"; do
    if [ -x "$c" ]; then ADB="$c"; break; fi
  done
  ADB="${ADB:-adb}"
fi
if ! command -v "$ADB" >/dev/null 2>&1 && [ ! -x "$ADB" ]; then
  echo "找不到 adb，请用 ADB=/绝对路径/adb.exe 指定" >&2
  exit 2
fi

echo "==> adb = $ADB"
DEVICES="$("$ADB" devices 2>/dev/null | grep -v '^List' | grep -E '\s(device|recovery)$' || true)"
if [ -z "$DEVICES" ]; then
  echo "没有在线设备。请插上 X7 Pro 并确认已授权 USB 调试。" >&2
  echo "（脚本只做只读采集，不会安装/修改/删除设备上的东西）" >&2
  exit 3
fi
echo "==> 在线设备:"; echo "$DEVICES"

sh() { "$ADB" shell "$1" 2>/dev/null || true; }
grab() { sh "$2" > "$OUT/$1"; echo "  $(wc -c < "$OUT/$1") bytes  $1"; }

echo "==> 采集设备事实"
grab cfg.json              "cat /etc/miniapp/resources/cfg.json"
grab drm-summary.txt       "cat /sys/kernel/debug/dri/0/summary"
grab modetest-planes.txt   "modetest -M rockchip -p 2>&1 | head -200"
grab sysfs-planes.txt      "for d in /sys/class/drm/card0-*; do echo \$d; done; ls -l /dev/dri /dev/fb* 2>&1"
grab env.txt               "uname -a; echo ---; cat /proc/version; echo ---; getprop ro.product.model 2>/dev/null; echo ---; cat /etc/miniapp/version 2>/dev/null; echo ---; ls /etc/miniapp/jsapis 2>/dev/null; echo ---; ls /userdata/miniapp/data/mini_app/pkg 2>/dev/null | head -20"
{
  for p in mppvideodec kmssink h264parse h265parse matroskademux faad alsasink souphttpsrc flvdemux videorate; do
    printf '%-18s ' "$p"
    sh "gst-inspect-1.0 $p 2>/dev/null | head -1" | tr -d '\r'
    echo
  done
} > "$OUT/gst-plugins.txt"
echo "  $(wc -c < "$OUT/gst-plugins.txt") bytes  gst-plugins.txt"
grab app-log.txt           "grep -a -E '\[device\]|\[diag\]|\[player\]|PB2|pb2|plane|PLAYER|错误|black' /userdata/applog/*.log 2>/dev/null | tail -120"

echo "==> 生成 suggested-profile.json"
if command -v python3 >/dev/null 2>&1; then
  python3 "$ROOT/tools/probe-x7p.py" "$OUT" || echo "  (解析失败，请人工看 REPORT.md)"
else
  echo "  (没有 python3，跳过自动解析)" >&2
fi

echo
echo "==> 完成：$OUT"
echo "    把 suggested-profile.json 的内容写进设备 storage 的键 bili_device_profile"
echo "    （DevTools 9307 里执行 \$falcon.jsapi.storage.setStorage），或按 REPORT.md 调整。"
