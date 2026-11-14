#!/usr/bin/env bash
# ============================================================================
# PenBili WSL 构建（交叉编译 native + 铺 SKU 目录 + 打 AMR）
#
# 与 native/build.ps1 等价，但全部在 WSL / Linux 里完成：
#   zig cc -target aarch64-linux-gnu.2.17  → 三个 libjsapi_*.so
#   python3 tools/inspect_elf.py           → ELF class/machine 校验
#   复制 .so 到 SKU 子目录                 → 保证 X7 Pro 的 PMS 能装载
#   node aiot-vue-cli/src/cli.js -c -q -p → 生成 .amr
#
# 用法：
#   bash tools/build-wsl.sh                 # 全套（默认 aarch64）
#   SKIP_PACK=1 bash tools/build-wsl.sh    # 只编译 native + 铺目录
#   PENBILI_TARGET=arm bash tools/build-wsl.sh   # 32 位 X5（不编 player2）
#   AIOT_CLI=/path/to/cli.js NODE_BIN=node bash tools/build-wsl.sh
#
# 需要：zig 0.16.x（Linux 原生）、python3、node。
# 若 zig 不在常见位置，用 ZIG_EXE=/abs/path/to/zig 指定。
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
TARGET="${PENBILI_TARGET:-aarch64}"
VARIANT="${PENBILI_VARIANT:-full}"

echo "==> 项目：$ROOT"
echo "==> 目标：$TARGET / 变体：$VARIANT"

# ---------------- 1. 找 zig ----------------
ZIG="${ZIG_EXE:-}"
if [ -z "$ZIG" ]; then
  for c in \
    "$HOME/tools/zig016/ziglang/zig" \
    "$HOME/tools/zig-x86_64-linux-0.16.0/zig" \
    "$HOME/tools/zig-x86_64-linux-0.15.2/zig" \
    "/opt/zig/zig" \
    "$(command -v zig 2>/dev/null || true)"; do
    if [ -n "$c" ] && [ -x "$c" ]; then ZIG="$c"; break; fi
  done
fi
if [ -z "$ZIG" ] || [ ! -x "$ZIG" ]; then
  echo "找不到 Linux 版 zig。可先执行：" >&2
  echo '  curl -L https://ziglang.org/download/0.16.0/zig-x86_64-linux-0.16.0.tar.xz -o /tmp/zig.tar.xz' >&2
  echo '  tar -xf /tmp/zig.tar.xz -C "$HOME/tools"' >&2
  echo '（或 ZIG_EXE=/path/to/zig bash tools/build-wsl.sh）' >&2
  exit 2
fi
echo "==> zig：$("$ZIG" version 2>/dev/null || echo '?') ($ZIG)"

# python3 既用于 ELF 校验也用于打包；必须放在 SKIP_NATIVE 分支外面，
# 否则「跳过 native 只重打 JS」的第二次构建会 unbound variable。
PY="${PYTHON_BIN:-python3}"

if [ "${SKIP_NATIVE:-0}" != "1" ]; then

# ---------------- 2. 编译参数 ----------------
STUB="$ROOT/native/stubs-aarch64"
GST_STUB="$ROOT/native/stubs-aarch64-gst"
INC="$ROOT/native/iot-miniapp-sdk/include"
LIBS="$ROOT/libs"
mkdir -p "$LIBS"

if [ "$TARGET" = "aarch64" ]; then
  TRIPLE="aarch64-linux-gnu.2.17"
  EXTRA=(-DPENBILI_KMS_ONLY=1)
  BUILD_P2=1
elif [ "$TARGET" = "arm" ]; then
  TRIPLE="arm-linux-gnueabihf.2.23"
  STUB="$ROOT/native/stubs"
  EXTRA=()
  BUILD_P2=0
else
  echo "未知 PENBILI_TARGET=$TARGET（只支持 aarch64 / arm）" >&2
  exit 2
fi

COMMON=(cc -target "$TRIPLE" -O2 -g0 -Wl,-s -fPIC -shared -fvisibility=hidden
        -I "$INC" "${EXTRA[@]}")

echo "==> 编译 libjsapi_player.so"
"$ZIG" "${COMMON[@]}" -o "$LIBS/libjsapi_player.so" \
  "$ROOT/native/csrc/player.c" "$STUB/libquickjs.so" \
  -lpthread -Wl,-soname,libjsapi_player.so

echo "==> 编译 libjsapi_httpjson.so"
"$ZIG" "${COMMON[@]}" -o "$LIBS/libjsapi_httpjson.so" \
  "$ROOT/native/csrc/httpjson.c" "$STUB/libquickjs.so" "$STUB/libcurl.so.4" \
  -lpthread -Wl,-soname,libjsapi_httpjson.so

if [ "$BUILD_P2" = "1" ]; then
  echo "==> 编译 libjsapi_player2.so（GStreamer/MPP/KMS）"
  P2SRC=(
    "$ROOT/native/csrc/jsapi_player2.c"
    "$ROOT/native/csrc/core/log.c"
    "$ROOT/native/csrc/core/clock.c"
    "$ROOT/native/csrc/core/queue.c"
    "$ROOT/native/csrc/session/session.c"
    "$ROOT/native/csrc/session/cmd.c"
    "$ROOT/native/csrc/video/kms_plane.c"
    "$ROOT/native/csrc/audio/audio_out.c"
    "$ROOT/native/csrc/source/src_http.c"
  )
  "$ZIG" "${COMMON[@]}" -I "$ROOT/native/csrc" \
    "${P2SRC[@]}" \
    "$GST_STUB/libgstreamer-1.0.so.0" \
    "$GST_STUB/libgobject-2.0.so.0" \
    "$GST_STUB/libglib-2.0.so.0" \
    "$STUB/libquickjs.so" \
    -lpthread -ldl -Wl,-soname,libjsapi_player2.so \
    -o "$LIBS/libjsapi_player2.so"
fi

# ---------------- 3. ELF 校验 ----------------
echo "==> ELF 校验"
for so in libjsapi_player.so libjsapi_httpjson.so libjsapi_player2.so; do
  [ -f "$LIBS/$so" ] || continue
  "$PY" "$ROOT/tools/inspect_elf.py" "$LIBS/$so" --target "$TARGET" | tail -4
done

# ---------------- 4. 铺 SKU 目录 ----------------
# 真机事实（docs/真机安装与排障.md §2）：PMS 会按架构/SKU **整个选取一个**
# libs 子目录，只把那个目录里的 .so 装进设备。X6 Pro 吃根目录，X7 Pro 的
# 固件吃过 libs/arm64-y02_1/。每个候选目录都铺满三份 .so，选哪个都不会
# 出现「player2 模块未随包安装」→ import 失败 → 播放页黑屏。
echo "==> 铺 SKU 目录（X7 Pro PMS 兼容性）"
for d in arm64 arm64-y02_1; do
  mkdir -p "$LIBS/$d"
  for so in "$LIBS"/*.so; do
    [ -f "$so" ] && cp -f "$so" "$LIBS/$d/$(basename "$so")"
  done
  echo "  libs/$d/ : $(ls -1 "$LIBS/$d" | tr '\n' ' ')"
done

fi  # SKIP_NATIVE

if [ "${SKIP_PACK:-0}" = "1" ]; then
  echo "==> SKIP_PACK=1，完成（未打包 AMR）"
  exit 0
fi

# ---------------- 5. 打 AMR ----------------
CLI="${AIOT_CLI:-}"
if [ -z "$CLI" ]; then
  for c in \
    "$ROOT/../env/node_modules/aiot-vue-cli/src/cli.js" \
    "/mnt/c/Users/lenovo/Desktop/direct/undertale-pen/node_modules/aiot-vue-cli/src/cli.js" \
    "/c/Users/lenovo/Desktop/direct/undertale-pen/node_modules/aiot-vue-cli/src/cli.js"; do
    if [ -f "$c" ]; then CLI="$c"; break; fi
  done
fi
if [ -z "$CLI" ] || [ ! -f "$CLI" ]; then
  echo "找不到 aiot-vue-cli，请用 AIOT_CLI=/path/to/src/cli.js 指定" >&2
  exit 2
fi
NODE="${NODE_BIN:-node}"
if ! command -v "$NODE" >/dev/null 2>&1 && [ ! -x "$NODE" ]; then
  # 非 login shell 时 PATH 里可能没有 ~/.local/bin（该 node 是 WSL interop 到
  # Windows node.exe 的 shim），补几个常见位置。
  for c in "$HOME/.local/bin/node" \
           "/mnt/c/Program Files/nodejs/node.exe" \
           /mnt/c/Users/*/.workbuddy/binaries/node/versions/*/node.exe; do
    if [ -x "$c" ]; then NODE="$c"; break; fi
  done
fi
# WSL 里常见的 node 其实是 Windows node.exe（process.platform==='win32'）：
# 它不认 /mnt/c/... 形式的脚本参数，必须用 wslpath 转成 C:\... 形式。
# cwd 会被 interop 自动映射成 C:\...，所以 CLI 自己找项目根目录没问题。
CLI_ARG="$CLI"
if [ "$("$NODE" -p process.platform 2>/dev/null || echo linux)" = "win32" ] && [[ "$CLI" == /mnt/* ]]; then
  if command -v wslpath >/dev/null 2>&1; then
    CLI_ARG="$(wslpath -w "$CLI")"
  fi
fi
# ---------------- 5.1 构建变体（diag-only） ----------------
# 自检包只登记 diag 一页：没有首页、没有播放页，开机直接进链路自检。
# app.json / build_flags.js 是构建期改的，结束（含失败）必须还原。
APP_JSON="$ROOT/src/app.json"
FLAGS_JS="$ROOT/src/build_flags.js"
APP_BAK="$(mktemp)"
FLAGS_BAK="$(mktemp)"
RESTORED=0
restore_variant() {
  if [ "$RESTORED" = "1" ]; then return 0; fi
  cp -f "$APP_BAK" "$APP_JSON"
  cp -f "$FLAGS_BAK" "$FLAGS_JS"
  rm -f "$APP_BAK" "$FLAGS_BAK"
  RESTORED=1
}
if [ "$VARIANT" = "diag" ]; then
  cp -f "$APP_JSON" "$APP_BAK"
  cp -f "$FLAGS_JS" "$FLAGS_BAK"
  trap restore_variant EXIT
  cat > "$APP_JSON" <<'JSON'
{
  "pages": {
    "index": "pages/diag/diag.js",
    "diag": "pages/diag/diag.js"
  },
  "options": {
    "style": {
      "lessPaths": ["styles"],
      "theme": "theme-dark"
    },
    "alias": {}
  }
}
JSON
  sed -i 's/export const DIAG_ONLY = false;/export const DIAG_ONLY = true;/' "$FLAGS_JS"
  echo "==> 变体 diag-only：index/diag 两个路由都指向 pages/diag（必须两条，\n      单页会触发 CLI 的 single-js-bundle 内联，页面模块没有独立 .js.bin，\n      真机按路径 import 时找不到 → 黑屏）"
fi

# 阶段 1：rollup + manifest（**不带 -q**）。CLI 的 -q 会调 32 位 Windows
# qjsc.exe，写 <name>.js.c 时报 Permission denied（本机实测）。
echo "==> 打包 1/2：$NODE $CLI_ARG -c -p"
rm -rf "$ROOT/.falcon_"
"$NODE" "$CLI_ARG" -c -p
# 阶段 2：用 WSL 里的 Linux 版 qjsc 把 .js 编译成 .js.bin（与官方发布包同形态），
# 重算 manifest cert 后组包。产物覆盖阶段 1 的源码形态 amr。
if [ "$VARIANT" = "diag" ]; then
  OUT_AMR="$ROOT/8002026110200001.3_5_6_diag.amr"
else
  OUT_AMR="$ROOT/8002026110200001.3_5_6.amr"
fi
echo "==> 打包 2/2：$PY tools/pack_falcon.py --out $(basename "$OUT_AMR")"
"$PY" "$ROOT/tools/pack_falcon.py" --falcon-dir "$ROOT/.falcon_" --out "$OUT_AMR"
if [ "$VARIANT" = "diag" ]; then
  restore_variant
  echo "==> 已还原 src/app.json 与 build_flags.js"
fi

echo "==> 产物："
ls -lh "$ROOT"/*.amr 2>/dev/null || true
