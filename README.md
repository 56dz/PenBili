# PenBili · S6 Pro 分支

把哔哩哔哩装进有道词典笔。这是 [56dz/PenBili](https://github.com/56dz/PenBili) 的 **S6 Pro 分支**：
在保持上游 3.5.20 功能基线的前提下，把整套播放与布局适配到 **有道词典笔 S6 Pro**（CV1826）。

- 上游：`3.5.20`（X6 Pro / X7 Pro，RK3562，aarch64，GStreamer + MPP 硬解）
- 本分支：`4.0.2`（S6 Pro，CV1826，armv7，640×260，ffmpeg 软解 + fbdev）
- 本分支**已移除弹幕**：不包含 3.5.14–3.5.20 的 `danmaku.js`、`dm_overlay.c` 与弹幕设置/渲染。

![version](https://img.shields.io/badge/version-4.0.2-blue)
![license](https://img.shields.io/badge/license-GPL--3.0-green)

---

## 这个分支做了什么

### 设备适配（S6 Pro 专属）

- 逻辑画布 **960×390** → 物理面板 **640×260**（k = 2/3）；构建期必须跑
  `qjs-style-scale`（高度 ×390/266，字号保持设计值），否则整页只占上半屏。
- 视频走**自研 native 播放器**（`native/csrc/player.c`，模块 `libjsapi_player.so`）：
  ffmpeg 软解 → rgb32 rawvideo → `/dev/fb0` 双缓冲；音频独立 pipe → `aplay`。
  支持 `setRect` 运行期改落点、`setZoom` 视频放大、智能跳帧、音画自愈与 DNS 自愈（`httpjson.c`）。
- **视频缩放**：播放页菜单 / 「我的 → 视频缩放」开关；开启后双指捏合 1..4x，
  native 取画面中心 1/zoom 区域放大，不重启解码、不改矩形。使用
  `libjsapi_picpinch.so`（与文件管理 3.1.9 图片页同一模块）。
- 全屏 shell 播放器 UI：顶栏 96 + 视频带 216 + 底栏 78；点视频区隐藏/显示栏，
  双击暂停/继续，控件 5 秒无操作自动收起。
- 播放页「样式」：流畅（按视频带解码）/ 清晰（按无栏分辨率解码后等比显示，
  不裁切、不变形）。
- 离线缓存（`/userdisk/PenBili/`）、独立「下载 / 历史」页、UP 主页、搜索记录、
  列表滚动位置恢复、返回栈修复等 S6 Pro 侧的长期改动全部保留。

### 从上游 3.5.20 同步的功能

- **3.5.9**：互动视频（`x/stein/edgeinfo_v2`）、CC 字幕（`x/player/v2` + 候选池
  「时长吻合度」打分）、分P / 合集（`x/web-interface/view` 一次响应）、播放控件重做。
- **3.5.10**：投币 `-401` 风控（缺 buvid）自动补齐重试；控件自动收起。
- **3.5.13**：双击暂停、单击沉浸（隐藏顶底栏）、全屏按钮下线。
- **3.5.14–3.5.20 的弹幕系统：已删除**，不参与构建。
- 上游 3.5.16 的字幕候选池与偏差判定（`subtitleFit`）保留。

### S6 Pro 上的功能差异

| 能力 | X6/X7 Pro 上游 | S6 Pro 本分支 |
|---|---|---|
| 解码 | GStreamer + MPP 硬解 + KMS overlay | ffmpeg 软解 + fbdev 直写 |
| 最高画质 | 1080P60 | 360p（默认，可切 480p；受 CV1826 性能限制） |
| 字幕渲染 | 中列字幕带 / 顶栏 | 视频带下方 30px 字幕带（native 帧盖不住 UI） |
| 互动题面 | 右列面板 | 整页替换 + `pauseRender` |
| 视频缩放 | 双指 1..4x | 双指 1..4x + 开关（native `setZoom`） |
| 弹幕 | 3.5.14+ 烧帧渲染 | **无** |

---

## 构建

安装包：[Releases · v4.0.2](https://github.com/Noguimc2/PenBili/releases/tag/v4.0.2)
（`PenBili-4.0.2-s6pro.amr`）；上游旧版见
[v4.0.0](https://github.com/noguimc/PenBili/releases/tag/v4.0.0)。

前置：Node 16+、Python 3、adb；重编 native 时另需 zig。

```powershell
# 1) 首次：准备构建 CLI（aiot-vue-cli 1.0.32）
npm run setup

# 2) 前端（产出 .falcon_\ 与未缩放的 4_0_2.amr，后者只作第 5 步的底包）
npm run build

# 3) 必做：样式缩放（大字号 + 高度 ×390/266）
npm run scale

# 4) 可选：重编 native（libs/ 已提交与真机一致的预编译 .so）
powershell -NoProfile -ExecutionPolicy Bypass -File native/build.ps1

# 5) 重打包：以刚构建的 AMR 为底包，把缩放后的 index.js.bin 与 manifest/cert 一起更新
python tools\pack_standalone.py 8002026110200001.4_0_2.amr .falcon_ 4.0.2 `
  8002026110200001.4_0_2_pkg.amr --appid 8002026110200001 --appname PenBili --full

# 6) 装机（装 _pkg.amr，不要装未缩放的 4_0_2.amr）
adb -s <serial> push 8002026110200001.4_0_2_pkg.amr /tmp/p.amr
adb -s <serial> shell "miniapp_cli install /tmp/p.amr"
adb -s <serial> shell "miniapp_cli start 8002026110200001 index"
```

`npm run build` **不会**自动跑样式缩放，而且每次 build 都会重新生成未缩放的 `index.js.bin`
——所以缩放后**不要再次 `npm run build`**，直接用第 5 步的 `pack_standalone.py` 重打包。
`pack_standalone.py` 默认以第 2 步产出的 AMR 为底包即可，不需要旧版本安装包；
完整步骤与踩坑说明见 [docs/s6pro.md](docs/s6pro.md)。

### aiot-vue-cli（构建 CLI）

- npm 公开包：`aiot-vue-cli@1.0.32`（MIT）：
  https://www.npmjs.com/package/aiot-vue-cli
- 获取方式（仓库根目录执行，任选其一）：

```powershell
npm run setup
# 等价于：
npm install --prefix ../env --no-save --no-audit --no-fund aiot-vue-cli@1.0.32
```

- `package.json` 的 `build` / `build:dev` / `build:prod` 固定读取
  `../env/node_modules/aiot-vue-cli/src/cli.js`，所以安装到仓库上一级的 `env/`。
- **固定 1.0.32**：本分支的包与真机验证都基于该版本；升级前先做产物 A/B。

### qjs-style-scale（样式缩放，必做）

仓库已内置源码与 Windows 二进制，不依赖外部目录：

| 文件 | 说明 |
|---|---|
| `tools/qjsbuild/qjs-style-scale.exe` | Windows x64 可执行文件（直接可用） |
| `tools/qjsbuild/qjs-style-scale.c` | 源码（改写 QuickJS 字节码里的 style 原子） |
| `tools/qjsbuild/quickjs.tar.xz` | QuickJS 2020-07-05 源码（编译依赖） |

作用：直接改写 `index.js.bin` 里的样式值——

- `fontSize` ×2.0（同步前的大字号 UI；Falcon 960→640 后物理约 1.33×）；
- `height >= 70` 按 `390/266` 放大（266→390、240→364、192→282、74→108…）；
  `390 / 264 / 242 / 96 / 216 / 78` 已是最终设计值，跳过；
- `width` 与小于 70 的高度不动（图标/进度条保持比例）。

```powershell
npm run scale          # 大字号（font ×2.0）+ 高度 ×390/266（默认）
npm run scale:normal   # 原字号：font ×1.0，高度仍 ×390/266
```

> 4.0.2 起恢复同步前的大字号；如果后续觉得太大，可用 `scale:normal` 出原字号包。

不跑的结果：960×390 设计稿被缩成 640×177，底部 83px 黑屏，登录二维码和播放底栏被裁。

从源码重建（PowerShell + zig；MinGW/gcc 同理，需把 4 个 `.c` 一起编译）：

```powershell
tar -xf tools\qjsbuild\quickjs.tar.xz -C tools\qjsbuild
zig cc -O2 -I tools\qjsbuild\quickjs-2020-07-05 `
  tools\qjsbuild\qjs-style-scale.c `
  tools\qjsbuild\quickjs-2020-07-05\libregexp.c `
  tools\qjsbuild\quickjs-2020-07-05\libunicode.c `
  tools\qjsbuild\quickjs-2020-07-05\cutils.c `
  -o tools\qjsbuild\qjs-style-scale.exe -lm
```

### 视频缩放（4.0.2）

- **PenBili**：播放页菜单「缩放」与「我的 → 视频缩放」开关，默认开。
  - 菜单「缩放」：首次点击开启并 1.5x，之后 1.5→2→3→4→1 轮转（不依赖双指）；
  - 双指捏合 1..4x；捏合期间/结束后 800ms 内的点击不会误开顶底栏；
  - 关闭时忽略捏合并复位 1x。
  - 输入：`libjsapi_picpinch.so`（随包提交，读 `/dev/input/event4` 的 ABS_MT_*）；
  - 显示：native `player.setZoom(1..4)`，取源画面中心 1/zoom 区域最近邻放大，
    不改目标矩形、不重启解码，**开栏状态下同样有效**。
- **文件管理 3.1.11**：播放器菜单「缩放」与 PenBili 同样的修复——
  最新目标队列、捏合结束 800ms 抑制点击、点该行手动轮转 1.5→2→3→4→关；
  底层仍是 fbplayer 的 zoom 文件。
- 这是运行期开关，不涉及字节码重建；PenBili 侧需要 `libs/libjsapi_picpinch.so`
  与带 `setZoom` 导出的 `libjsapi_player.so` 一起打包。

---

## 已知限制

- S6 Pro 无硬件解码，1080p 软解约 1.05× 实时，默认 360p。
- 匿名状态下 B 站 `x/player/v2` 不返回字幕地址（`subtitle_url` 为空），
  CC 字幕需要登录后才有轨道。
- 视频由 native 直接写 framebuffer，Falcon UI 无法覆盖在画面上：
  弹幕、浮动控件等「画在视频上面」的形态在本机不可实现（这也是移除弹幕的原因之一）。

---

## 致谢与许可

- 上游与主要作者：[56dz/PenBili](https://github.com/56dz/PenBili)（GPL-3.0）
- 本分支由 S6 Pro 适配改动构成，同样以 **GPL-3.0** 发布。
- 第三方客户端，与哔哩哔哩无隶属关系；所有内容版权归原作者及平台所有。
