#!/usr/bin/env bash
# ============================================================================
# 一次产出两个 AMR：
#
#   ① 8002026110200001.3_5_6_x7.amr    全量包（X7 全系自适应）
#        - 首页 + 播放页 + 自检页
#        - 运行时读 $falcon.env 视口，按 layout.js 弹性排布；不再假设 936x280
#
#   ② 8002026110200001.3_5_6_diag.amr  自检包（只有 diag 一页）
#        - 开机直接进「链路自检」，没有主界面/播放页
#        - 用于逐台 X7 验屏幕尺寸、KMS 平面、解码链路、进度
#        - 屏幕顶部那行会显示生效 profile 与原始 env，便于发现「面板当视口上报」
#
# 用法（WSL）：
#   export NODE_BIN=/mnt/c/Users/lenovo/.workbuddy/binaries/node/versions/22.22.2-6/node.exe
#   bash tools/build-variants.sh
#
# 两个包共用同一个 appid，安装后者会覆盖前者；验完自检包请重装全量包。
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

FULL="$ROOT/8002026110200001.3_5_6.amr"
FULL_OUT="$ROOT/8002026110200001.3_5_6_x7.amr"
DIAG_OUT="$ROOT/8002026110200001.3_5_6_diag.amr"

echo "============================================================"
echo "[1/2] 全量包（X7 全系自适应）"
echo "============================================================"
bash tools/build-wsl.sh
if [ ! -f "$FULL" ]; then
  echo "x 全量包没有生成：$FULL" >&2
  exit 1
fi
cp -f "$FULL" "$FULL_OUT"

echo "============================================================"
echo "[2/2] 自检包（只有 diag 一页）"
echo "============================================================"
# native 上一步已经编好，这里只重打 JS/manifest，省一半时间。
SKIP_NATIVE=1 PENBILI_VARIANT=diag bash tools/build-wsl.sh
if [ ! -f "$DIAG_OUT" ]; then
  echo "x 自检包没有生成：$DIAG_OUT" >&2
  exit 1
fi

echo "============================================================"
echo "完成："
ls -l "$FULL_OUT" "$DIAG_OUT"
echo
echo "  全量包：$(basename "$FULL_OUT")  （安装后是完整 UI）"
echo "  自检包：$(basename "$DIAG_OUT")  （只有链路自检页）"
echo "============================================================"
