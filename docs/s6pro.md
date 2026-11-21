# S6 Pro 适配与构建说明

> 设备：有道词典笔 S6 Pro（日志 `model=YDPS6-1`），CV1826，armv7 / glibc 2.23，
> 单核 Cortex-A53，物理面板 **640×260**（竖屏 260×640，Falcon 画面旋转 270°）。
> appid `8002026110200001`，运行时 Falcon 小程序 + QuickJS 20200705。

## 1. 画布与坐标

- 设计画布 **960×390**，`setViewPort(960)` 映射到 640×260（k = 2/3）。
- JS 给 native 的矩形用「266×960 物理面」坐标（历史沿用），native 统一
  `scale_rect_for_fb()` 乘 k=2/3 并夹到 fb 范围。**不要在 JS 里再乘一次**。
- 构建期 `qjs-style-scale` 把 `fontSize` ×2.0、`height>=70px` 按 390/266 放大；
  `390 / 264 / 242 / 96 / 216 / 78` 是最终值，工具会跳过，写新固定高度时优先用这些。
- 新增的 26/30/40/44/54/60/64 等高度都在阈值以下，不会被二次放大。

## 2. 页面结构

单页 `src/pages/index/index.vue`，左栏纵向标签：
`推荐 / 热门 / 搜索 / 我的 / 下载 / 历史`。

`mode` 状态机：`browse` / `detail` / `play` / `article` / `mine` / `login(qr)` / `up`。

- **详情页**：左列信息卡（标题/UP/简介/提示 + 点赞投币收藏），中列封面，
  右列评论；多 P 或合集时右列顶部出现「评论 / 分P / 合集」页签。
- **播放页**：全屏 shell 播放器。
  - 顶栏 96 + 视频带 216 + 底栏 78；隐藏栏时视频带 390。
  - 开 CC 字幕时从视频带让出 30px 作为字幕带（`screen.js playAreaFor`）。
  - 单击画面：显示/隐藏顶底栏（沉浸）；双击：暂停/继续。
  - 控件展开后 5 秒无操作自动收起；暂停时强制展开。
  - `•••` 菜单：画质 / 直播画质 / 解码 / 样式 / **字幕** / 缓存（行高 54）。
- **互动视频**：片段结束/题面窗口出现时整页替换为选项页；native `pauseRender`
  冻结画面，选择后 `openVideo` 重建下一段（不回列表）。

## 3. 构建

```powershell
cd <repo>
npm run setup      # 首次：安装 aiot-vue-cli@1.0.32 到 ../env
npm run build      # 产出 .falcon_\ 与未缩放的 4_0_2.amr
npm run scale      # 大字号（font ×2.0）+ 高度 ×390/266（同步前 UI）
```

`npm run build` 自己**不会**跑样式缩放，而且每次 build 都会重新生成未缩放的
`index.js.bin` —— 缩放后**不要再 build**；用刚生成的 AMR 作底包重新打包：

```powershell
python tools\pack_standalone.py 8002026110200001.4_0_2.amr .falcon_ 4.0.2 `
  8002026110200001.4_0_2_pkg.amr --appid 8002026110200001 --appname PenBili --full
```

漏掉缩放时页面只占上半屏（266×2/3=177px），登录二维码、播放页底栏都会被裁掉。

工具位置：

- `tools/qjsbuild/qjs-style-scale.exe` / `.c` / `quickjs.tar.xz`（源码与二进制均已入库）；
  开关：`--font-scale <f>`（默认 2.0，大字号 UI）、`--height-scale <f>`、`--no-height`；
- `tools/scale.ps1`（`npm run scale`；`scale:normal` = font 1.0）；
- `tools/setup-build-env.ps1`（或 `npm run setup`）安装 aiot-vue-cli。

### 视频缩放（4.0.2）

- 播放页菜单「缩放」/「我的 → 视频缩放」开关；开 = 双指 1..4x，关 = 忽略并复位。
- 菜单「缩放」也提供手动轮转 1.5→2→3→4→1，不依赖双指；捏合结束 800ms 内
  的点击不会误开顶底栏。
- native：`player.setZoom(z)` 取源画面中心 1/zoom 区域放大，不重启解码、不改矩形。
- 输入：`libjsapi_picpinch.so`（与文件管理图片页同一模块，读 `/dev/input/event4`）。
- 文件管理 3.1.11：播放器菜单同一个「缩放」开关 + 手动轮转；最新目标队列与
  捏合后 800ms 抑制点击，底层走 fbplayer 的 zoom 文件。

### native

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File native/build.ps1
```

- armv7 hard-float，glibc 2.23；只构建 v1 播放器与 httpjson。
- `libjsapi_player.so`：`native/csrc/player.c`（ffmpeg + fbdev + aplay）。
- `libjsapi_httpjson.so`：`native/csrc/httpjson.c`（native HTTP / 下载 / DNS 自愈）。
- 产物直接写 `libs/`，打包时由 CLI 同步进 `.falcon_/libs/`。
- X6/X7 的 `player2`（GStreamer/MPP/KMS）源码仍在 `native/csrc` 里作为上游参考，
  但 **S6 Pro 分支不编译、也不装载**。

### 打包与装机

```powershell
python tools/pack_standalone.py <base.amr> .falcon_ 4.0.0 <out.amr> --appid 8002026110200001 --appname PenBili --full
adb -s <serial> push <out.amr> /tmp/p.amr
adb -s <serial> shell "miniapp_cli install /tmp/p.amr"
adb -s <serial> shell "miniapp_cli start 8002026110200001 index"
```

- 底包用第 2 步刚生成的 AMR 即可（同 appid、结构完整）；重打包会重算 manifest
  版本与 cert，把缩放后的 `index.js.bin` 正确登记。
- `miniapp_cli start` 不会重新挂载已挂载页面；刷新页面必须重装同一个 AMR。
- 设备重启后 ADB 认证会关闭：设置 → 法律监管，对页面文本连点 10~15 下。
- 开发机若同时有 emulator，所有 adb 命令都要带 `-s <serial>`。

## 4. native 播放链路

```
ffmpeg -ss <start> -i <input> -map 0:v:0 -vf scale=W:H,transpose=.. -r <fps>
       -pix_fmt rgb32 -f rawvideo pipe:3
       -map 0:a:0? -ac 2 -ar 44100 -f s16le pipe:4
→ video_thread 按帧读 pipe:3，按音画时钟贴 /dev/fb0（双缓冲都写）
→ audio_thread 把 pipe:4 写进 aplay（阻塞写 = 音频时钟）
```

`open()` 前 15 位是硬契约：

```
open(input, startMs, durationMs, fps, audio, transpose, x, y, w, h,
     audioDevice, userAgent, referer, input2, fpsCap, isLive, decodeW, decodeH)
```

- `input2` = DASH 音频轨；空 = durl/本地单文件。
- `isLive` = 1/0/-1（-1 = native 按启发式）。
- `decodeW/decodeH` = 清晰样式的解码尺寸。
- `setRect(x,y,w,h)` 只改显示落点，不重启解码。
- 本地缓存文件输入不能带 `-user_agent/-headers`；native 对非 http 输入已自动跳过。
- `open` 前会确保 `/tmp/resolv.conf` 首条是 `223.5.5.5`（DNS 自愈）。

## 5. 3.5.9 同步功能的落点

| 功能 | 服务模块 | 页面入口 |
|---|---|---|
| 分P / 合集 | `services/bili/video_meta.js` | 详情页右栏页签；起播数据来自 `view` |
| CC 字幕 | `services/bili/player_v2.js`、`subtitle.js` | 「我的 → CC 字幕」、播放菜单「字幕」、字幕带 |
| 互动视频 | `services/bili/interactive.js` | 片尾/题面窗口自动进入选项页 |
| 控件自动收起 | `services/play_chrome.js` | 播放页 5 秒无操作收起 |
| 投币 -401 | `services/bili/client.js` | 自动补 buvid 后重试 |

**弹幕**：本分支不包含 `services/bili/danmaku.js`，不调用 `dm_*` native 导出，
也没有弹幕设置项。上游 3.5.14–3.5.20 中与弹幕相关的界面/渲染代码未合入。
