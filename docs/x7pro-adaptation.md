# X7 Pro 播放器适配记录（黑屏修复）

> 版本：PenBili 3.6.0（基于上游 3.5.5，只支持 X6 Pro）
> 目标：让视频在**有道词典笔 X7 Pro** 上正常出画，不再黑屏；X6 Pro 行为保持不变。
> 状态：代码/构建/单测已完成，**真机验收待做**（本次设备未连 adb，见 §7）。

---

## 0. 一句话结论

3.5.5 的播放链路把 **X6 Pro 的屏幕几何和 DRM 平面编号写死在了 C 代码里**：
`plane-id=76`、`PANEL_X_OFFSET=107`、逻辑面 `960x266`、面板行数 `960`。
X7 Pro 的视口是 `936x280`（`/etc/miniapp/resources/cfg.json`），逻辑带 280 宽落在
480 宽面板里偏移是 **100**，面板有效行数是 **936**，而且 plane 76 在该固件上
不一定存在/空闲。三处全错 → `kmssink` 申请不到平面或把画面摆到屏幕外 →
视频分支静默失败 → **播放页黑屏（音频往往还在响）**。

修法不是把 76/107 换成另一组魔法数字，而是把设备参数变成**运行时可探测、
可覆盖**的值，并让 `kmssink` 在未知机型上自己挑空闲平面。

---

## 1. 设备事实对照（三份资料的交叉结论）

| 项 | X6 Pro（原支持） | X7 Pro（本次目标） | 依据 |
| --- | --- | --- | --- |
| SoC / 架构 | RK3562 / aarch64 + glibc | RK3562/G52 / aarch64 + glibc 2.36 | PenBili README；`YouDaoPen-SDK/scripts/setup-toolchain.sh`（x7 = aarch64+glibc）；`direct/docs/总体技术方案.md` §9 |
| Falcon 逻辑视口 | 960×266 | **936×280**（部分资料记 960×280，故必须运行时读 `$falcon.env`） | `direct/docs/真机安装与排障.md` §2（cfg.json screen）；`总体技术方案.md` §1 |
| 物理面板 | 480×960，`direction=270` | 480×960（DRM envelope），`direction=270` | `docs/x6pro-video-playback.md` §2；GPU 包 `tools_GPU_RENDER_CLEANUP_STATUS.md`（DRM 480x960） |
| cfg `yoffset`（面板列偏移） | 107 = (480−266)/2 | **100 = (480−280)/2** | `YouDaoPen-SDK/docs/wiki/system_env_compile/sys_conf.md` 的 screen 字段定义 |
| UI 合成平面 | `Esmart0-win0`（zpos 0） | 同类，窗口名可能不同 | `docs/x6pro-video-playback.md` §3.1 |
| 视频 overlay 平面 | `Esmart1-win0` / plane 76（zpos 2） | plane id **不固定** | 同上；GPU 包 `tools_GPU_FRAME_SYNC.md` / 渲染清理记录 |
| framebuffer | 无 `/dev/fb0` | 无 `/dev/fb0`（`captureFB` 返回 −1） | `x6pro-video-playback.md` §1；`真机安装与排障.md` §10.2 |
| 官方 `mplayer` 模块 | 不可用（仅阿里云开发板） | 不可用 | `YouDaoPen-SDK/docs/wiki/jsapi/bashi/mplayer.md` + API 文档 §637 |
| 原生库装载 | 吃 `libs/` 根目录 | PMS 按 SKU **整个选一个** `libs/<sku>/`，实测吃过 `libs/arm64-y02_1/` | `真机安装与排障.md` §2 |
| 挖洞 | `.page-live` 根节点透明 + `<hole>` | 必须保持 alpha=0（合成层可能 zpos 更高） | GPU 包 `tools_GPU_RENDER_CLEANUP_STATUS.md`（hole-only app 全屏 alpha=0、真实 NV12 / H.264 MPP 回放） |

> GPU 教程包（`GPU引擎使用教程.md` §1/§3）补充了另一条重要事实：RK3562 的 Mali G52
> 有 CPU/GPU 混合档，视频走 DRM/KMS + DMABuf，帧同步有 IN_FENCE_FD 等红线。
> PenBili 的 player2 链路（进程内 GStreamer → `mppvideodec` → `kmssink`）与之同源，
> 因此**保留这套硬解链路**是正确的，不需要改用 WPE/WebKit 那套视频协议。

---

## 2. 黑屏的五个可能原因（按证据强度排序）

1. **`plane-id=76` 硬编码**（`session.c`）：别的机型上该 id 可能是 UI 平面、可能不存在。
   `kmssink` 抢不到平面时视频分支建不起来，README 记录过同类症状「只剩音频在响」。
2. **几何硬编码**（`session.c` 的 `PB2_PANEL_X_OFFSET=107`、`screen.js` 的
   `LOGICAL/PHYSICAL`、`x6pro-video-playback.md` 的 960 行面板）：X7 Pro 上
   `render-rectangle` 的 y 会算多 24 行、x 会偏 7 列，叠加旋转后画面可能整个
   落到可见区之外。
3. **原生库没装进设备**（PMS SKU 目录）：`import('player2')` 失败 → JS 抛异常 →
   页面渲染中断（`总体技术方案.md` §10 记录过「初始化异常 → 渲染循环不启动 →
   永久黑屏」这一类故障形态）。
4. **UI 层盖住视频平面**：X6 Pro 上视频 zpos 2 > UI zpos 0；X7 Pro 若自动挑到
   zpos 更低的平面，就要靠 `.page-live` + `<hole>` 的全透明挖洞才能透出画面。
5. **GStreamer 插件/MPP 差异**：`mppvideodec` / `kmssink` / `h264parse` 缺失时
   同样静默无画面（取证脚本会逐项检查）。

---

## 3. 改了什么

### 3.1 native（播放器核心）

| 文件 | 改动 |
| --- | --- |
| `native/csrc/session/session.h` | `pb2_open_req_t` 增加 `panelOffset` / `planeId`，语义见 §4 |
| `native/csrc/session/session.c` | ① `g_w` 增加 `panel_x_offset` / `kms_plane_id`，每次 OPEN 由请求赋值；② 新增 `kms_plane_arg()`：`planeId<0` 时不写 `plane-id`，交给 kmssink 自动选；③ 点播/直播/裸流三处管线都用动态值；④ `SET_RECT` 用动态列偏移；⑤ 新增 `pb2_session_panel_offset()` / `pb2_session_plane_id()` getter |
| `native/csrc/jsapi_player2.c` | ① `open()` 新增第 17/18 个可选参数；② `info()` 返回**实际生效**的 planeId / panelXOffset；③ 新增只读 `kmsProbe()`，原样返回 `/sys/kernel/debug/dri/0/summary`；④ 版本 3.6.0 |
| `native/csrc/video/kms_plane.c/.h` | ① 平面巡检不再死认 `Esmart1-win0`：找不到时扫描其它 ACTIVE 的非 UI 窗口；② 新增 `pb2_kms_summary()`（使用调用方缓冲，JS 线程可安全调用） |

X6 Pro 缺省参数完全不变（107 / 76），老版本 JS 不传新参数时逐字节等价。

### 3.2 JS（设备自适应）

| 文件 | 改动 |
| --- | --- |
| `src/services/device_profile.js`（新增） | 从 `$falcon.env.deviceWidth/Height` 判机型并算 profile：视口、面板行列、列偏移、`setViewPort(960)` 缩放系数、planeId、transpose；支持 storage 覆盖；`playVideoRectsFor()` 按 profile 算信箱矩形 + 旋转变换 |
| `src/services/play_session.js` | `openVideo()` 用 profile 算矩形并把 `panelOffset` / `planeId` 下发给 native |
| `src/services/player.js` | `openSession()` 追加第 17/18 个位置参数（`buildOpenArgs` 的 16 参契约保持不变，单测不破）；新增 `kmsProbe()` 透出 |
| `src/app.js` | 注释说明**为何保持 `setViewPort(960)` 不变**（改成 936 会裁掉右侧 24px），缩放交给 profile |
| `src/pages/index/index.vue` | `created()` 幂等预热 profile（不阻塞首屏） |
| `src/pages/diag/diag.vue` | 自检页新增一行：生效的 profile、native planeId、KMS 里 ACTIVE 的窗口 |
| `test/run.js` | 7 条新回归：X6 Pro 逐字段等价、X7 Pro 几何、越界、覆盖、诊断串 |

### 3.3 构建 / 取证

| 文件 | 改动 |
| --- | --- |
| `tools/build-wsl.sh`（新增） | WSL 一键：zig 交叉编译三个 `.so` → ELF 校验 → 铺 SKU 目录 → 打 AMR |
| `tools/probe-x7p.sh` + `.py`（新增） | adb **只读取证**：cfg.json、DRM summary、modetest、GStreamer 插件、日志，生成 `suggested-profile.json` 与 `REPORT.md` |
| `libs/arm64/`、`libs/arm64-y02_1/` | 构建时铺满三个 `.so`；PMS 无论选根目录还是 SKU 目录都能装载 player2 |

---

## 4. 参数契约（给后续维护者）

`open()` 位置参数（前 16 个与 3.5.5 完全一致）：

```
... fpsCap(15), isLive(16), panelOffset(17), planeId(18)
```

| 参数 | 值 | 语义 |
| --- | --- | --- |
| `panelOffset` | `-1` | 用 X6 Pro 默认 107（老 JS 不传时） |
| | `>=0` | 显式面板列偏移，X7 Pro 默认算得 100 |
| `planeId` | `0` | 用 X6 Pro 默认 76 |
| | `-1` | **不写 plane-id**，kmssink 自动挑空闲 overlay（X7 Pro 默认） |
| | `>0` | 显式 plane id（真机取证锁定后可用） |

storage 覆盖键 `bili_device_profile`（JSON，可覆盖任意字段）：

```json
{
  "name": "x7p", "width": 936, "height": 280,
  "panelXOffset": 100, "panelYOffset": 0, "direction": 270,
  "planeId": -1, "transpose": 2
}
```

写入方式（DevTools 9307）：

```js
$falcon.jsapi.storage.setStorage({
  key: 'bili_device_profile',
  data: JSON.stringify({ name: 'x7p', width: 936, height: 280, planeId: -1 })
})
```

**默认值已经能覆盖主流 X7 Pro 固件，大多数情况不需要手工写。**

---

## 5. 构建（WSL）

```bash
cd PenBili
npm ci                       # 项目依赖
NODE_BIN=$PWD/node_modules/node/bin/node.exe bash tools/build-wsl.sh
# 只编译 native：SKIP_PACK=1 bash tools/build-wsl.sh
```

产物：`8002026110200001.3_6_0.amr`（`.js.bin` 字节码形态，与官方发布包一致）。

脚本分四步：zig 编译三个 `.so` → ELF 校验 → 铺 SKU 目录 → 两阶段打包：

1. `aiot-vue-cli -c -p` 生成 `.falcon_`（rollup + manifest + `.js` 源码）；
2. `python3 tools/pack_falcon.py` 用 **Linux 版 qjsc** 把 `.js` 编成
   `.js.bin`、重算 `manifest.cert`、组装 zip。

> 为什么不用 CLI 自带的 `-q`：它在 Windows 上调 32 位 `qjsc20200705.exe`，
> 写 `<name>.js.c` 时固定报 `Permission denied`（PowerShell 与 WSL interop
> 都复现）。CLI 自带的 `cli-libs/aiot-qjsc-tool/assets/bin/linux/qjsc20200705`
> 在 WSL 里能正常跑，所以改用它。`pack_falcon.py` 复现了 CLI 的两个关键行为：
> `-M <外部模块>` 声明（否则 `diag.js` 会找不到 `./client-xxxx.js`）与
> 「先全部编译、最后统一删 `.js`」；cert 重算规则与
> `direct/tools/mk_amr.py` 的 audit/derive 一致。

工具链位置（本次环境实测）：

- zig 0.16.0：官网 tarball 直连很慢，改用 PyPI 的 Linux wheel
  （`pip download ziglang==0.16.0 --platform manylinux_2_12_x86_64 ...`，
  解压后 `~/tools/zig016/ziglang/zig`，`ZIG_EXE` 可覆盖）。
- 打包器：`aiot-vue-cli@1.0.32`（本机在
  `C:\Users\lenovo\Desktop\direct\undertale-pen\node_modules\aiot-vue-cli`），
  `AIOT_CLI` 可覆盖。
- **打包器必须用 Node 18**：Node 22+ 会因 `spdy/http-deceiver` 触发
  `Error: No such module: http_parser`（YouDaoPen-SDK README 里也记录了这条坑）。
  用 `NODE_BIN=<node18>` 指定，例如项目内 `node_modules/node/bin/node.exe`
  （`npm i --no-save node@18.20.8` 即可获得）。
- WSL 里的 `node` 常是 Windows node.exe（interop），脚本会自动把 CLI 路径
  用 `wslpath -w` 转成 `C:\...` 形式。
- 项目需要 `tsconfig.json` + `src/typecheck.ts` 占位：CLI 无条件挂
  `@rollup/plugin-typescript`，一个 `.ts` 输入都没有会抛 TS18003 中断构建。
- 交叉审计（用 `direct/tools` 里现成的脚本）：

```bash
python3 /mnt/c/Users/lenovo/Desktop/direct/tools/mk_amr.py \
  --src 8002026110200001.3_6_0.amr --audit-only
# ✓ cert 与包内实体逐条一致（含 size/md5）
```

校验：`python3 tools/inspect_elf.py libs/libjsapi_player2.so --target aarch64`
应输出 aarch64 / ELFCLASS64 / `custom_init_jsapis` 已导出；`node test/run.js`
应为 223 passed / 0 failed。

> `npm ci` 生成的 `node_modules/` 与 `libjsapi_*.so` 都是构建产物，不提交。

---

## 6. 真机验证 SOP

设备当前未连接 adb（`adb devices` 为空），以下步骤留给用户插上 X7 Pro 后执行。

```bash
# 1) 只读取证（不改设备任何文件）
ADB=/path/to/adb.exe bash tools/probe-x7p.sh
#    看 REPORT.md：视口/列偏移/ACTIVE 窗口/缺失插件/suggested-profile.json

# 2) 安装（与其它 AMR 相同）
adb push 8002026110200001.3_6_0.amr /tmp/
adb shell "miniapp_cli install /tmp/8002026110200001.3_6_0.amr"   # 必须 "ret":0
adb shell "miniapp_cli start 8080222437664451"                      # 先回桌面
adb shell "miniapp_cli start 8002026110200001 diag"                   # 自检页

# 3) 自检页第一行应显示
#    profile=x7p/env viewport=936x280 scale=0.975 panelOffset=100 planeId=auto
#    | native plane=-1 | Esmart1-win0 ACTIVE（或本机实际的窗口名）

# 4) 播一个视频，另开一个 shell 抓平面
adb shell "grep -a 'Esmart' -A9 /sys/kernel/debug/dri/0/summary"
#    dst pos/rect 必须落在 100..380 列内，且 buf[0] addr != 0

# 5) 画布截图看不到硬件平面（这是正常的），像素取证要用
adb shell "gst-launch-1.0 -q kmssrc plane-id=<id> num-buffers=1 ! videoconvert ! pngenc ! filesink location=/tmp/v.png"
```

判据：自检页第一行机型正确、播放中 DRM summary 有 NV12/overlay 窗口 ACTIVE、
`buf[0] addr` 在翻页、画面不黑。**`miniapp_cli capture` 全黑/全白都不算证据**
（X7 Pro 上背光关闭时连系统应用都拍成纯白，见 `真机安装与排障.md` §10.2）。

---

## 7. 已知限制 / 待真机确认

1. **本次没有真机**：所有 X7 Pro 数值来自三份资料的实测记录，代码做到运行时
   探测 + 可覆盖，但没有在本机 X7 Pro 上验收。插上设备后请先跑 §6 的取证脚本。
2. **视口宽度有分歧**：`真机安装与排障.md` 的 cfg.json 是 936×280，
   `总体技术方案.md` 写 960×280。代码以 `$falcon.env` 为准，两者都能算对；
   若真机报第三个值，用 `bili_device_profile` 覆盖即可，无需改代码。
3. **plane 自动选择**：`planeId=-1` 让 kmssink 自选，绝大多数情况最稳；
   若真机上自动选到了 zpos 低于 UI 的平面导致画面仍被盖住，用取证脚本的
   modetest 候选 id 逐个试并显式写 `planeId`。
4. **UI 底部留白**：保持 `setViewPort(960)` 时，X7 Pro 的 280 高视口里
   CSS 960×266 等比缩放到 936×259，底部约 21px 不画内容（黑底）。视频矩形
   已按同一缩放系数计算，与 `<hole>` 对齐；若要铺满需要重排 CSS，未在本次范围。
5. **v1 回退仍带 X6 Pro 硬编码**：`player.c`（v1）只在 X5/回退路径使用，
   未改。X7 Pro 必须走 v2（默认），若自检页显示 `PLAYER_UNSUPPORTED` 说明
   `.so` 没装载，先查 SKU 目录。
6. **GStreamer 插件**：若取证报告显示 `mppvideodec`/`kmssink` 缺失，则本机
   不能用 player2 的硬解链路，需要另接该机型的播放后端（GPU 教程包描述的
   WPE 视频协议 v3 / hole 方案可作参考），这属于下一阶段工作。

---

## 8. 资料来源

- `C:\Users\lenovo\Desktop\direct\docs\总体技术方案.md`（X7 Pro 平台事实、黑屏形态、aarch64/glibc）
- `C:\Users\lenovo\Desktop\direct\docs\真机安装与排障.md`（cfg.json 936×280、PMS SKU 目录、无 fb0、capture 全白）
- `C:\Users\lenovo\Desktop\direct\YouDaoPen-SDK\docs\wiki\system_env_compile\sys_conf.md`（screen 字段语义）
- `C:\Users\lenovo\Desktop\direct\YouDaoPen-SDK\docs\wiki\jsapi\bashi\mplayer.md` + `docs\词典笔小程序开发API文档.md`（mplayer 不可用）
- `C:\Users\lenovo\Desktop\direct\YouDaoPen-SDK\scripts\setup-toolchain.sh`、`README.md`（x7 工具链、Node 18 坑）
- `C:\Users\lenovo\Desktop\66777\GPU引擎使用教程+分支文档包\...\GPU引擎使用教程.md`（RK3562/G52、DRM、混合档）
- 同包 `tools_GPU_RENDER_CLEANUP_STATUS.md`、`tools_MIGRATION_STATUS.md`、`tools_GITHUB_GPU_TWITCH_20261006.md`（480x960、H.264 MPP、alpha-zero hole）
- 仓库内 `docs/x6pro-adaptation.md`、`docs/x6pro-video-playback.md`、`README.md`（X6 Pro 基线与所有真机踩坑）
