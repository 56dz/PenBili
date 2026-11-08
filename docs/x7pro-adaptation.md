# X7 Pro 播放器适配记录（黑屏修复）

> 版本：PenBili **3.5.6**（基于上游 3.5.5）
> 说明：文中出现的 3.6.0 / 3.6.1 / 3.6.2 / 3.6.3 是**开发期的内部构建号**，
> 只用于区分「哪一轮改了什么」；对外发布的包统一用 3.5.6（安装器按这个版本号校验）。
> 目标：让视频在**有道词典笔 X7 Pro** 上正常出画、UI 不留黑边、音频设备被占时也不整体失败；
> X6 Pro 行为保持不变（960×266 上所有几何值逐像素一致，单测守护）。
> 状态：黑屏、首页黑边、自检页几何、错误消息为空、音频设备被占判死均已修复；
> 3.5.6 内还在收敛「视频比例不对 / 占不满」：§3.8 修 videoscale 目标方向，
> §3.9 试过默认回硬件缩放，§3.10 把洞改成 16:9 并与矩形同源、加 `bufScale`
> 几何四档；**§3.11 的五张真机照片定案** —— 缓冲尺寸对显示无效，四档
> 「全部靠下」的根因是 `panelXOffset` 多加了 100。
> 现在 X7 默认 `panelXOffset=0`（X6 仍 107）、`swScale=1`、洞 16:9，
> 日志页降噪并注入 KMS 平面 dst 供残留黑边定案。
> **真机验收待做**（开发机无 adb 设备，见 §7）。

---

## 0. 一句话结论

3.5.5 的播放链路把 **X6 Pro 的屏幕几何和 DRM 平面编号写死在了 C 代码里**：
`plane-id=76`、`PANEL_X_OFFSET=107`、逻辑面 `960x266`、面板行数 `960`。
X7 Pro 的视口是 `936x280`（`/etc/miniapp/resources/cfg.json`），逻辑带 280 宽落在
480 宽面板里偏移是 **100**，面板有效行数是 **936**，而且 plane 76 在该固件上
不一定存在/空闲。三处全错 → `kmssink` 申请不到平面或把画面摆到屏幕外 →
视频分支静默失败 → **播放页黑屏（音频往往还在响）**。

修法不是把 76/107 换成另一组魔法数字，而是把设备参数变成**运行时可探测、
可覆盖**的值。

3.6.1 又补了三件事（真机截图反馈）：

1. **UI 右侧/底部黑边**：根因是 `setViewPort(960)` + CSS 写死 960×266。X7 Pro 的
   视口只有 936×280，整页先被压到 0.975，底部剩 21px；首页 5 张 166 宽的卡片
   在缩放后正好卡在临界值，第 5 列整列换行 → 右侧空出 180px。现在**画布 = 视口**
   （1:1 不缩放），各栏尺寸由 `src/services/layout.js` 按视口算，网格留 4~12px
   防换行余量。
2. **画面贴底/被压扁**：3.6.0 让 `kmssink` 自选平面（`planeId=-1`），结果挑到
   **没有缩放能力的 Smart 平面** —— 720p/1080p 的 buffer 塞不进 253×457 的窗口，
   driver 只能裁一块贴上去。RK3562 上带缩放器的是 **Esmart**，改回 X6 Pro 验证过的
   `76`；并新增 `rowOffset`（面板行原点）描述「936 行 UI 带在 960 行面板里的起点」。
3. **概率性无声**：DASH 的第二路音频 URL 偶发失效时，ffmpeg 的可选映射会静默
   丢掉音轨，画面完全正常、就是没声；而 `status().audioDead` 以前是写死的 0，
   页面那条「无声」提示永远进不去。现在 native 会在「有音轨却 0 字节」时按当前
   位置**原地重建管线自愈（每会话 2 次）**，`audioDead` 报真值，`audioHeals` 给
   自愈次数。

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
| `src/app.js` | 3.6.1：`setViewPort(profile.cfg.width)` —— X6=960、X7=936，**1:1 不缩放** |
| `src/pages/index/index.vue` | `created()` 幂等预热 profile（不阻塞首屏） |
| `src/pages/diag/diag.vue` | 自检页新增一行：生效的 profile、native planeId、KMS 里 ACTIVE 的窗口 |
| `test/run.js` | 7 条新回归：X6 Pro 逐字段等价、X7 Pro 几何、越界、覆盖、诊断串 |

### 3.3 构建 / 取证

| 文件 | 改动 |
| --- | --- |
| `tools/build-wsl.sh`（新增） | WSL 一键：zig 交叉编译三个 `.so` → ELF 校验 → 铺 SKU 目录 → 打 AMR |
| `tools/probe-x7p.sh` + `.py`（新增） | adb **只读取证**：cfg.json、DRM summary、modetest、GStreamer 插件、日志，生成 `suggested-profile.json` 与 `REPORT.md` |
| `libs/arm64/`、`libs/arm64-y02_1/` | 构建时铺满三个 `.so`；PMS 无论选根目录还是 SKU 目录都能装载 player2 |

### 3.4 3.6.1：UI 重排 + 语音自愈（真机截图反馈的三条）

| 文件 | 改动 |
| --- | --- |
| `src/services/layout.js`（新增） | 弹性布局纯函数：按视口算导航栏、网格列数/卡宽、播放态三列、进度条轨道、图文正文宽。960×266 上**逐字段等于 3.5.5 的硬编码**（单测守护），936×280 上把多出的 24 列/14 行分给各栏 |
| `src/app.js` | 画布宽改为 `profile.cfg.width`（X6 960 / X7 936），1:1 不再缩放 |
| `src/services/device_profile.js` | `videoArea/liveVideoArea` 改由 layout 推导（挖洞与平面天然一致）；新增 `rowOffset`；新增 **env 可信度闸门**（宽高比 <2.2 视为报的是面板尺寸，整组丢弃） |
| `src/pages/index/index.vue` | 所有容器改用 `:style="L.*"`（页面/导航/内容/网格/卡片/封面/信息块/三列/视频/进度条/正文/整行块），列数改 `L.num.cols`，进度条轨道改 `L.seekTrackW`，图文图片宽改 `articleImgW()` |
| `native/csrc/session/session.c` | ①音频自愈：`check_audio_stall()` 在「有音轨却 0 字节/分支没建起来」时按当前位置重建（每会话 2 次）；②OPEN 时重置 `audio_heals`/`first_frame_ms`/`plane_fallback_done`（以前只在 STOP 清，连播第二支就没配额、平面兜底也不再生效） |
| `native/csrc/jsapi_player2.c` | `audioDead` 接真值 + `audioHeals`；`info()` 增加 `rect`（本次真正下发的 `<x,y,w,h>`） |
| `src/pages/diag/diag.vue` | 自检页那行再补 `rect=<...>`，播放中直接看到平面摆在哪 |
| `tools/probe-x7p.py` | 从 DRM 摘要里解析每个窗口的 `dst pos/rect`，据此给出 `rowOffset`；`planeId` 建议改回 76 |

**为什么「网格余量」要单独留 4~12px**：falcon 的 `scroller` 是拿自己测到的
视口宽（视口 − 导航栏）决定内容宽的，不照抄 CSS 的 884。5 张卡刚好等于容器宽时，
flex 会把第 5 张整列换行 —— 这就是截图里「只有 4 列 + 右侧 180px 空白」的成因。
960 上留 7px（与 3.5.5 完全一致），936 上留 8px。

### 3.5 3.6.2：真机自检页暴露的四个问题

2026-11-07 23:59 在 X7 Pro 上跑自检页，截图为证（`bugs/_diag1_right.png`）：

| 现象 | 根因 | 改动 |
| --- | --- | --- |
| `起播 playing out=240x427` | `probe.js` 用的是 `screen.js::probeVideoRects`（X6 常量 469x240），而且没下发 `panelOffset`/`planeId` → 平面按 107 偏移、按 X6 矩形摆放 | 改用 `ensureProfile() + playVideoRectsFor()`，并把 `panelOffset`/`planeId` 一起传给 `openSession` |
| `回读5s 播放错误：`（后面空白） | ① 页面读的是 `st.error`，native 的字段叫 `lastErrorMsg`；② 部分错误只 patch 了码没给文本 | ① 改读 `lastErrorMsg`/`lastError`；② native 在文本为空时用码名兜底（`plane-lost` / `pipeline` / `no-first-frame` …） |
| `profile=...` 那行被屏幕右缘裁掉、返回按钮切角 | 自检页 `.page` 写死 960x266，X7 只有 936x280 | `data()` 里按 profile 算 `page/videoCol/panel/logHead/logScroll`；文字压到 10px 并缩短字段 |
| 5s 内状态变 error | **我的音频自愈在单文件会话上误触发**：`abytes==0` 就重建，而单流（自检页/durl）可能本来就没音轨，重建瞬间的暂态被页面读到 | 自愈条件收窄为 `input2` 非空（只救 DASH 双输入，也是用户真实遇到的场景） |

同时补了排障基建（真机没有 adb 时唯一可用的一条路）：

- `/tmp/pb2.log` **默认开启**（`PENBILI_PB2_LOG=0` 可关），>256KB 自动截断；
- native 新增 `logTail(maxBytes)`，把日志尾部（≤8KB）返回给 JS；
- 自检页底部新增「日志」按钮 → 全屏日志页（9px 单行、可滚动），
  bus error 原文、`plane missing`、`OPEN rect: logical=... -> panel <x,y,w,h>`、
  音频自愈记录都在里面；
- `回读5s` 的详情里新增 `音=<bytes>/有|无`：画面正常而 `音=0` 就是丢音轨，
  不用再靠用户描述有没有声音；
- 日志里新增 `OPEN rect: logical=... -> panel <...>` 一行，画面位置不对时可直接对账。

**为什么 `rowOffset` 是独立参数而不是塞进 `panelHeight`**：面板固定 480×960，
视口 936 行时多出 24 行，合成器可能居中（UI 从第 12 行开始）也可能贴第 0 行。
视频平面走面板绝对坐标，猜错就是整体偏移；用独立参数便于真机取证后一行 JSON 覆盖。

---

### 3.6 3.6.3：plane-id 76 在 X7 Pro 上根本不可用

2026-11-08 00:10 的真机自检页 + 机内日志（`bugs/_log_p2.png`）给出了完整现场：

```
(6399.792) start gate: timeout in 4021ms (v=1/1 frames=0 a=1/1 want_audio=1)
(6399.794) set_state(PLAYING) -> 0 (+4186ms)        ← 0 = GST_STATE_CHANGE_FAILURE
(6399.795) teardown: state->NULL qr=0 cur=3 pend=1   ← cur=3 说明管线一直停在 PAUSED
```

读法：视频分支建起来了（`v=1`），但 **4 秒内一个 buffer 都没有**（`frames=0`），
`set_state(PLAYING)` 直接返回 FAILURE。这不是解码慢、也不是网络慢 ——
是 sink 在 PAUSED 的异步启动阶段就失败了（`set_state(PAUSED)` 返回 ASYNC，
错误经 bus 报出，而 worker 正卡在 4s 的缓冲门里，**没人读 bus**，于是真实
原因被后来的 `set_state(PLAYING) failed` 覆盖）。

结论：**DRM plane 对象号不能跨固件赌。** 76 在 X6 Pro 上是 Esmart1-win0，
在 X7 Pro 的固件上不存在/不可用；`planeId=-1` 能出画但可能挑到没有缩放能力
的 Smart 平面（3.6.0 的「画面贴底被压扁」）。

#### 改法：不再要求平面具备缩放能力

| 层 | 改动 |
| --- | --- |
| `session.h` | `pb2_open_req_t` 新增 `swScale`（第 19 个位置参数） |
| `session.c` | 新增 `video_post_rate_desc()`：`swScale=1` 时在 `videorate` 之后插 `videoscale ! video/x-raw,width=W,height=H,framerate=…`，**两条建链路径都用它**（pad-added 动态分支 + 裸流固定分支） |
| `session.c` | 新增 `peek_bus_error()`：缓冲门内每 20ms 非阻塞读一次 bus ERROR，一旦出错立刻中止并把原文带进日志/错误 |
| `session.c` | 新增 `open_fail_or_retry()` + `want_plane_retry` + `open_req_dup()`：**显式 plane-id 起不来时自动切 auto+videoscale 重试一次**（每会话一次）。不能递归 `do_open` —— 它开头会 `free(g_w.req)`，那正是要复用的对象 |
| `session.c` | 视频 desc 缓冲 1024 → 1280，并加截断检查（被截断的 desc 会 parse 失败 → 整片黑，日志却看不出来） |
| `device_profile.js` | X7P：`planeId=-1` + `swScale=1`；X6P 保持 `76` + `swScale=0`。`planeId<0` 时 `swScale` 默认 1（覆盖时也一致） |
| `player.js` | `deviceArgs()` 追加第 19 位 `swScale` |
| `diag.vue` | 设备行改为显示 **native 的生效值**（`info().planeId/swScale`），并带 `vs0/1` |

为什么这样一定不会裁画面：buffer 尺寸 = `render-rectangle` 尺寸，sink 侧
**不需要任何缩放**，1:1 上屏即可。几何完全由 JS 算出的矩形决定，跟在哪个
平面、那个平面有没有缩放器都无关。

X6 Pro 不受影响：`swScale=0`，desc 与 3.5.5 逐字节相同。

#### 自检页现在该显示什么

```
x7p 936x280 5x161 off100 row0 | plane=auto vs1 · Esmart1-win0 ACTIVE
```

`plane=auto` + `vs1` 是期望值。若想换回硬件缩放（确认自选到了 Esmart），
storage 覆盖 `{"swScale":0}` 即可。

### 3.7 3.5.6：音频设备被占时，不要连画面一起判死

2026-11-08 00:25 的真实日志（`bugs/_log_*`）：

```
start gate: bus error in 705ms (v=1/1 frames=4 a=1/1 want_audio=1)
start gate: bus error -> Could not open audio device for playback.
                          Device is being used by another application.
first frame on screen +855ms (budget 1500ms) OK
audio: alsasink device=(default,speaker) vol=11%
```

也就是说：**画面 855ms 就正常上屏了（frames=4）**，挂的是 `alsasink` ——
音频设备被别的进程占着（系统提示音 / 上一个会话还没释放 PCM）。
而 3.6.3 把所有 bus 错误都当致命错误，于是把一条好管线拆了，
用户看到「播放错误」。这正是「有概率没声音」的真身：它是**概率性的设备占用**，
不是音轨解析问题。

改法（`session.c`）：

| 机制 | 作用 |
| --- | --- |
| `is_audio_bus_error()` | 按关键字（`audio device` / `alsasink` / `alsa` / `pcm`）把音频类错误摘出来 |
| `open_audio_retry()` | 第一次：等 **400ms** 原样重建（设备通常已释放）；第二次：仍失败就 `audio=0` **只播视频** |
| `process_cmd` 的兜底循环 | 把「平面兜底」与「音频兜底」统一成最多 3 轮重建，每轮 `open_req_dup()` 深拷贝 |
| `status().audioOff` | 上报「主动降级为无声」，页面提示「无声：音频设备被占用，已静音播放」 |
| `reset_session_fields()` | 会话结束清掉 audioOff/重试计数，避免下一支视频直接无声启动 |

**为什么不是简单重试一次就完**：ALSA 的 busy 有两种 —— 几十毫秒内释放（提示音），
和被别的进程长期占用。前者靠 400ms 重试救回来（用户能听到声音），
后者重试再多次也没用，必须降级，否则用户连画面都没有。

**为什么错误分类放在缓冲门里**：`poll_bus()` 只在 worker 主循环跑，而缓冲门是
阻塞轮询（最长 4s），这期间没人读 bus —— 3.6.2 之前连错误原文都拿不到。
现在门内每 20ms 取一条，既能立刻分流，也能把原文写进日志页。

### 3.8 3.5.6（第一试）：视频被压扁，且不同比例压扁程度不一样

> 注意：本节与 §3.9 都是**历史推理**，结论已被 §3.10 的四档真机截图证伪
> （四档的下发矩形完全相同，问题不在缩放方式）。「按 rotate 互换目标」
> 现在重新成为 X7 Pro 默认值（`swScale=1`），并新增 `bufScale` 控制 buffer 尺寸。

**现象**：3.5.6 已经能正常出画（真机日志 `first frame on screen +855ms`），
但画面比例不对；换不同宽高比的稿件，变形程度还不一样。

**根因**：3.6.3 为了绕开「X7 Pro 上 plane-id 76 不可用」，在解码后插了

```
... ! mppvideodec rotation=270 ! videorate ! videoscale !
     video/x-raw,width=<rect.w>,height=<rect.h> ! kmssink render-rectangle=<rect>
```

`mppvideodec rotation=270` 给的是**旋转意图**（最终由 kmssink 在面板坐标系里
转 90 度上屏），`videoscale` 看到的仍然是解码出来的**横向帧**（1920×1080）。
而 `<rect>` 是 JS 按 `logicalRectToPhysicalRect()` 算出的**旋转后物理竖矩形**
（X7 Pro 16:9 → `<100,274,253,450>`）。把横帧直接缩到 253×450，等于把 16:9
硬拉成 9:16 竖条；源比例越偏离 16:9，拉伸量越不同 —— 这就是「不同比例
压扁程度不一样」的来源。

顺带还有第二个放大因素：这些矩形含奇数（253、337、193…）。NV12 的 UV 平面
按半宽采样，`video/x-raw,width=253` 是非法 caps，videoscale 可能协商成别的尺寸，
kmssink 再按 `render-rectangle` 拉一次，形成二次变形。

**改法（`session.c`）**：

| 改动 | 作用 |
| --- | --- |
| `video_post_rate_desc()` | 从 `g_w.rect`（真正下发的 render-rectangle，SET_RECT 会改它）解析宽高；`swScale=1` 且 `rotate` 为 90/270 时**互换目标宽高**；宽高向下取偶数 |
| `normalize_video_rect()`（新增） | OPEN / SET_RECT / SET_RECT restore 三处都把 `render-rectangle` 的宽高对齐到偶数，保证 buffer 与窗口严格等大、1:1 上屏 |
| `pad-added` 日志 | 增加 `rotate` / `rect` / `videoscale -> WxH`，日志页一行就能对账 |

以 16:9 为例：`rect` 对齐成 `<100,274,252,450>`，videoscale 目标互换成
**450×252**（横，与 1920×1080 等比）；kmssink 转 90 度后正好 252×450，
与 `render-rectangle` 1:1 对齐，平面不需要任何缩放能力。

`swScale=2` 是**免打包回退开关**：若某固件的 `mppvideodec` 真的把输出 buffer
转成竖帧（此时不该互换），在 DevTools 写 `{"swScale":2}` 即可，不用重打包。

X6 Pro 不受影响：`swScale=0`，整条 desc 与 3.5.5 逐字节相同。

---

### 3.9 3.5.6（第二试）：默认回到硬件缩放 + 自检页四档 A/B

> **已被 §3.10 证伪，本节仅为历史推理记录。** 四档真机截图显示它们的
> `out=253x450` 完全相同，「换缩放方式」根本改变不了画面大小与位置。

两张真机照片（同一版本、不同宽高比稿件）转正后的读数：

| 稿件 | 期望 | 实际 |
| --- | --- | --- |
| 16:9 | 画面铺满中列（450×253 量级） | **贴底的扁横带**，上方约 1/3 屏幕高全黑 |
| 9:16 | 中间一条窄竖条（142×253） | **居中的小方块**，四周全黑 |

两者共同点：**宽度基本对，高度严重不足，且贴底**。这不是"目标比例算错"能
解释的 —— 更像 videoscale 输出的**横向** buffer 被 kmssink 当成「未旋转」帧，
等比塞进**竖向** `render-rectangle`，于是自动 letterbox 成小横条并按底边对齐。

也就是说：**插 `videoscale` 本身就破坏了 `mppvideodec rotation=270` 的语义**，
无论目标宽高互换（§3.8 的 C1）还是不互换（D2）都救不回来。3.5.5 在 X6 Pro
上从未插过 videoscale，比例与位置一直正确；而 X7 Pro 在 3.6.0（无 videoscale、
plane 自选）时也能出画，只是当时 `rect` 还错用 X6 常量。现在 rect /
panelOffset / rowOffset 都已修正，所以回到同一条链路最可能正确。

**改动**：`X7P_DEFAULTS.swScale` 从 1 改回 **0**（不插 videoscale，旋转与缩放
全交给 kmssink，与 3.5.5 的 desc 逐字节相同）。X6 不受影响。

#### 自检页「比例」按钮：一次装包验证四种组合

真机几何无法在 WSL 里验证，反复重打包成本太高，所以把候选做成**运行时
storage 覆盖**，自检页底部新增一个显示档位名的按钮（A0/B0/C1/D2），
点一下写覆盖、清 profile 缓存、自动重跑自检，不用 DevTools、不用重打包：

| 档 | `swScale` | `panelXOffset` | 验证什么 |
| --- | --- | --- | --- |
| **A0** | 0 | 默认 100 | 当前默认：硬件缩放。X6 同构链路 |
| **B0** | 0 | **0** | 若「视频在最下面」是列偏移猜错（UI 带贴面板第 0 列而非居中） |
| **C1** | 1 | 默认 100 | videoscale + 按 rotate 互换目标（§3.8） |
| **D2** | 2 | 默认 100 | videoscale + 不互换（§3.8 的回退档） |

换档会清掉上一档写过的 `swScale` / `panelXOffset`，避免串档。档位持久化在
`bili_device_profile.ratioMode`，进自检页会显示真值。

**请这样定案**：装包 → 进自检页 → 依次点「比例」切到 A0/B0/C1/D2，每档
等自检跑完并看画面（或回到首页播一个 16:9 和一个竖屏稿件），把「哪档画面
比例与位置都正常」告诉我，我把那一档固化成默认值并删掉 A/B 代码。

#### 日志增强（一张截图就能定位）

- `pad-added: source=<W>x<H> (encoded) rotate=<..> rect=<..> swScale=<..>`
  —— 直接看到源帧编码尺寸与本次真正生效的档位；
- `video: hw scale, NO videoscale (rect=... rotate=...)`（A0/B0）
  或 `video: videoscale -> WxH (panel rect ... mode=...)`（C1/D2）。

点底部「日志」按钮，全屏日志页（最新在上）里这几行一定在最前面。

> **历史说明**：§3.8 / §3.9 的两轮结论（"videoscale 破坏 rotation 语义"、
> "默认回硬件缩放"）已被 §3.10 的四档真机截图证伪，保留下来只为记录推理链，
> 不要再照抄其中的默认值。

---

### 3.10 3.5.6（第三试）：四档截图定案 —— 问题从来不是缩放方式

用户把 A0 / B0 / C1 / D2 各截了一张，四张照片转正后读数一致：
`playing out=253x450 24fps`、`qin16 640x360`、`off100 row0 plane=auto`。
**四档的下发矩形一像素都没变**，画面也都只占视频区的一小块（A/C/D 的黑边在
左/上，B 在左/下）。用户结论："没一个可以占满整个的"。

这说明前两轮把方向找错了：四档只改「缩放方式 / 面板列偏移」，而决定画面
大小的 **render-rectangle 尺寸、起点，以及送进平面的 buffer 像素尺寸**始终没动。
靠换缩放方式永远试不出"占满"。

两个确定的错位点：

1. **自检页的洞与下发的矩形不是同一个东西**。洞是「视口宽 − 352 面板」=
   X7 的 `584×280`，而 `probe.js` 借用的是首页播放区（三列布局的中列：
   `x=208` 起、`457×253`）。于是画面天生偏在洞的一角、还比洞小一圈，
   四周大片黑。首页自己没这个问题，是自检页把两套几何混用了。
2. **X7 自选的 overlay 平面很可能只做 1:1 贴 buffer**。A0（无 videoscale）
   那张是把 640×360 横帧按窗口裁出的窄条，C1/D2（有 videoscale）才有完整
   画面 —— 画面尺寸跟着 **buffer 尺寸**变，而不是跟着 `render-rectangle` 变。
   对这种平面，改矩形完全无效。

**改动**：

| 项 | 旧 | 新 |
| --- | --- | --- |
| 自检页视频洞 | `W − 352` = 584×280（2.09:1） | `min(W−320, H×16/9)` = **498×280（16:9）**，右侧面板随之变宽 |
| probe 的矩形基准 | 首页 `profile.videoArea` | 页面传入的 `ctx.videoBox`（自己的洞）；不传时仍是首页区 |
| X7 默认 `swScale` | `0` 硬件缩放 | **`1`** videoscale 互换档（A0 那张是四张里最差的） |
| buffer 尺寸 | 恒等于矩形 | 新增 `bufScale`（50..400%，缺省 100），videoscale 目标等比放大 |
| 自检页取证 | 只在 created() 读一次设备信息 | 切档 / 每轮自检跑完都刷新，新增「几何」行显示洞 / 矩形 / 缓冲 / 倍率 / 档位 |

`bufScale` 只放大 **buffer**，不动 `render-rectangle`：矩形始终与挖洞区等大、
不越出 480×960 面板，kmssink 不会因为矩形非法而拒绝上屏；等比放大也不引入
比例变形。放大后若平面按矩形缩放，画面尺寸不变（洞对齐已经让它变大）；
若平面 1:1 贴 buffer，画面随之变大 —— 两种驱动行为都覆盖到。

#### 新四档（自检页「比例」按钮，A→B→C→D 循环）

| 档 | `swScale` | `bufScale` | 验证什么 |
| --- | --- | --- | --- |
| **A** | 1（互换/横帧） | 100 | 默认。矩形与 16:9 洞严格同源，640×360 应零黑边铺满 |
| **B** | 2（不互换/竖帧） | 100 | 若该固件的 mppvideodec 真的把 buffer 转成竖帧 |
| **C** | 1 | **190** | 平面只认 buffer 像素时，把画面放大到铺满窗口 |
| **D** | 2 | **190** | 放大 + 不互换 |

换档会清掉上一档（含旧版 A0/B0/C1/D2）写过的 `swScale` / `bufScale` /
`panelXOffset`，不会串档。每档的几何数字都不同，自检页「几何」行与
`playing out=... ·bufN` 会跟着变 —— 这是判断档位是否真的生效的第一凭据。

**定案方法**：装包 → 进自检页 → 依次点「比例」A→B→C→D，每档等自检跑完看
画面并拍一张（务必带上「几何」行）。哪一档画面既铺满 16:9 洞、比例又正常，
就把那一档的 `swScale` / `bufScale` 写进 `X7P_DEFAULTS` 固化。若四档都不行，
点「日志」拍一页：`OPEN device profile: ... bufScale=N%`、
`OPEN rect: logical=... -> panel <...>`、`video: videoscale -> WxH (... buf=N%)`、
`pad-added: source=WxH` 四行合起来就能定位是矩形、buffer 还是平面能力的问题。

> **历史说明**：§3.10 的 `bufScale` 假设（"平面只认 buffer 像素"）已被
> §3.11 的五张真机照片证伪；四档保留只是因为无害，真正的问题是列偏移。

---

### 3.11 3.5.6（第四试）：四档「全部靠下」→ 列偏移不该是 100

用户装第四轮包后拍了 5 张（A/B/C/D 四档 + 一页日志），反馈"没有正常的，
**全部靠下**"。四张档位截图转正后读数：

| 项 | 四档读数 |
| --- | --- |
| `playing out=` | 全部 `280x498`（矩形没变） |
| 生效矩形 | 全部 `<100,438,280,498>` |
| 几何行缓冲 | A `498x280` / B `280x498` / C `946x532` / D `532x946`（确实都变了） |
| 画面 | **四张一模一样，贴在屏幕下方，上方一条黑边** |
| 平面 | 设备行 `Esmart0-win0 ACTIVE`（带缩放器的平面） |

**结论 1**：缓冲尺寸对最终显示毫无影响 —— 这块平面会按 `render-rectangle`
缩放，`bufScale` 是无效旋钮（保留但不再指望它）。四档画面相同，说明问题
100% 出在**矩形位置**，不是缩放方式、也不是 buffer 像素数。

**结论 2（关键）**：回头重看第三轮 B0 档（`panelXOffset=0`）那张照片 ——
画面是当时四张里**最大**的、上方黑边几乎消失。用户那句"b 是左侧和下方
一小条"说的是**剩下的少量黑边**，不是画面小。当时误读成「B 更差」，于是
第四轮把唯一的 0 档删掉，四档统一回落到公式值 `(480-280)/2 = 100`，
这才是"全部靠下"的直接原因。

**根因**：`screen.js::logicalRectToPhysicalRect` 产出的 `physical.x =
logical.y` 已经是**面板绝对列坐标**（逻辑带内 0..280）；`do_open()` 又加了
一次 `panelXOffset=100`，把整块画面往面板列 +100 推。超出 280 宽逻辑带的
部分被裁掉 → 屏幕下方只剩约 180 高、贴底、上方留黑。

**改动**：

| 项 | 旧 | 新 |
| --- | --- | --- |
| X7 `panelXOffset` | `(480-280)/2 = 100`（套 X6 公式） | **`0`**（`X7P_DEFAULTS.panelXOffset`） |
| X6 `panelXOffset` | `(480-266)/2 = 107` | 不变（3.5.5 起验证过的硬编码） |
| 生效矩形 | `<100,438,280,498>` | **`<0,438,280,498>`** |
| 自检页日志 | `logTail(6000)` 被 amixer 刷屏刷满，看不到 `OPEN rect` | `logTail(24000)` + 噪音只留 3 行 + 几何关键行置顶 + **KMS 摘要的窗口名/dst 插到最前** |

预期：洞 `498x280`（16:9）与矩形 `<0,438,280,498>` 严格对应，640×360
恰好铺满左侧洞，上下左右零黑边。

**若仍有少量黑边**：现在点「日志」拍一页即可定案 —— 最前面的 `[KMS]`
行给出 UI / 视频平面的真实 `dst`，与 `OPEN rect` 一对就能算出还差多少
（多半是 `rowOffset` 那 12/24 行，用 `{"rowOffset":12}` 覆盖即可，
不用重新打包）。

---

## 4. 参数契约（给后续维护者）

`open()` 位置参数（前 16 个与 3.5.5 完全一致）：

```
... fpsCap(15), isLive(16), panelOffset(17), planeId(18), swScale(19), bufScale(20)
```

| 参数 | 值 | 语义 |
| --- | --- | --- |
| `panelOffset` | `-1` | 用 X6 Pro 默认 107（老 JS 不传时） |
| | `>=0` | 显式面板列偏移；X7 Pro 默认 **0**（不是 `(480-280)/2=100`，见 §3.11），X6 Pro 默认 107 |
| `planeId` | `0` | 用 X6 Pro 默认 76（老 JS 不传时） |
| | `-1` | 不写 plane-id，kmssink 自选。**X7 Pro 默认**，配合 `swScale` 生效 |
| | `>0` | 显式 plane id（X6 Pro 默认 76；X7 Pro 上 76 不可用，勿照抄） |
| `swScale` | `0` | 不插 `videoscale`，硬件缩放（**X6 Pro 默认**，与 3.5.5 逐字节相同） |
| | `1` | 插 `videoscale` 且**按 `rotate` 互换目标宽高**（**X7 Pro 默认**，见 §3.10 档 A/C） |
| | `2` | 插 `videoscale` 但**不互换**，给「解码器真的把 buffer 转成竖帧」的固件留的免打包回退开关（档 B/D） |
| `bufScale` | `100` | videoscale 输出与矩形等大（缺省，所有老调用方行为不变） |
| | `50..400` | 输出宽高按该百分比**等比放大**；只改 buffer，不动 `render-rectangle`（§3.10 档 C/D = 190） |

storage 覆盖键 `bili_device_profile`（JSON，可覆盖任意字段）：

```json
{
  "name": "x7p", "width": 936, "height": 280,
  "panelXOffset": 0, "panelYOffset": 0, "rowOffset": 0,
  "planeId": -1, "swScale": 1, "bufScale": 100, "transpose": 2
}
```

`rowOffset`：UI 带在面板行方向的起点（面板 960 行里视口只占 936 行）。
真机取证看 UI 平面 `dst pos:(x,y)` 的 y；若为 12 就写 12。默认 0（X6 实测值）。

写入方式（DevTools 9307）：

```js
$falcon.jsapi.storage.setStorage({
  key: 'bili_device_profile',
  data: JSON.stringify({ name: 'x7p', width: 936, height: 280, planeId: -1, swScale: 1 })
})
// 若换成 swScale=1 后画面反而横向变形（说明该固件真的旋转了解码 buffer）：
//   data: JSON.stringify({ name: 'x7p', swScale: 2 })
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

产物：`8002026110200001.3_5_6.amr`（`.js.bin` 字节码形态，与官方发布包一致）。

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
> SDK 工具包 `mk_amr.py` 的 audit/derive 一致。

工具链位置（本次环境实测）：

- zig 0.16.0：官网 tarball 直连很慢，改用 PyPI 的 Linux wheel
  （`pip download ziglang==0.16.0 --platform manylinux_2_12_x86_64 ...`，
  解压后 `~/tools/zig016/ziglang/zig`，`ZIG_EXE` 可覆盖）。
- 打包器：`aiot-vue-cli@1.0.32`。默认从 `node_modules/.bin/aiot-vue-cli`
  找；找不到时用环境变量 `AIOT_CLI` 指向 `<aiot-vue-cli>/src/cli.js`。
- **打包器必须用 Node 18**：Node 22+ 会因 `spdy/http-deceiver` 触发
  `Error: No such module: http_parser`（YouDaoPen-SDK README 里也记录了这条坑）。
  用 `NODE_BIN=<node18>` 指定，例如项目内 `node_modules/node/bin/node.exe`
  （`npm i --no-save node@18.20.8` 即可获得）。
- WSL 里的 `node` 常是 Windows node.exe（interop），脚本会自动把 CLI 路径
  用 `wslpath -w` 转成 Windows 路径形式。
- 项目需要 `tsconfig.json` + `src/typecheck.ts` 占位：CLI 无条件挂
  `@rollup/plugin-typescript`，一个 `.ts` 输入都没有会抛 TS18003 中断构建。
- 交叉审计（用 SDK 工具包里现成的 `mk_amr.py`，路径由 `MK_AMR` 指定）：

```bash
python3 "$MK_AMR" --src 8002026110200001.3_5_6.amr --audit-only
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
adb push 8002026110200001.3_5_6.amr /tmp/
adb shell "miniapp_cli install /tmp/8002026110200001.3_5_6.amr"   # 必须 "ret":0
adb shell "miniapp_cli start 8080222437664451"                      # 先回桌面
adb shell "miniapp_cli start 8002026110200001 diag"                   # 自检页

# 3) 自检页设备行 / 几何行应显示（默认档 A，列偏移 0）
#    x7p 936x280 5x161 off0 row0 | plane=auto vs1 buf100 · Esmart0-win0 ACTIVE
#    洞498x280 矩形280x498 缓冲498x280 x1.0 档A 生效<0,438,280,498>
#    视频应铺满左侧 16:9 洞、上下左右零黑边；首页再确认 5 列铺满、无黑边
#
# 3b) 【仍有少量黑边】先点底部「日志」按钮拍一页：
#     最前面 [KMS] 行是 UI / 视频平面的真实 dst 几何，后面紧跟
#       OPEN device profile: panelXOffset=0 planeId=auto swScale=1 bufScale=100%
#       OPEN rect: logical=0,438,280,498 -> panel <0,438,280,498> (offset=0)
#       video: videoscale -> 498x280 (panel rect 280x498, rotate=270, mode=1, buf=100%)
#       pad-added: source=640x360 (encoded) rotate=270 rect=... swScale=1 buf=100%
#     对照 dst 与 OPEN rect 即可算出还差多少（多半是 rowOffset 的 12/24 行，
#     用 bili_device_profile = {"rowOffset":12} 覆盖，不用重新打包）。
# 3c) 也可点「比例」切 B/C/D 比较横帧/竖帧与 1.9 倍缓冲：四档矩形相同，
#     只影响 buffer；画面位置由列偏移决定，别再指望倍率能改变大小。

# 4) 播一个视频，另开一个 shell 抓平面
adb shell "grep -a 'Esmart' -A9 /sys/kernel/debug/dri/0/summary"
#    dst pos/rect 必须落在 0..280 列内，且 buf[0] addr != 0
#    同时对照自检页的 rect=<x,y,w,h>：两者应当一致。
#    若画面整体上下偏移：看 UI 平面 dst 的 y（0 或 12），
#    把它写进 bili_device_profile.rowOffset，再重进播放页。

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
3. **plane 号不可跨固件复用（3.6.3 已实测证伪）**：76 是 X6 Pro 上 Esmart1-win0
   的对象号，X7 Pro 上不可用（sink 起不来 → `set_state(PLAYING)` FAILURE）。
   现在 X7P 默认 `planeId=-1`（自选）+ `swScale=1`（videoscale，见 §3.10 四档截图
   判定：硬件缩放档只出一条窄画面）+ `panelXOffset=0`（§3.11 五张照片定案：
   再加 100 会把画面推出逻辑带 → 全部靠下），不依赖任何 plane 对象号；
   native 还留了一道兜底：显式 plane-id 起不来时自动切 auto+videoscale 重试一次
   （日志 `OPEN fail (...) -> retry`）。X6 Pro 仍是 `76` + `swScale=0` +
   `bufScale=100` + `panelXOffset=107`。
4. **面板行原点（rowOffset）待真机确认**：X6 的 960 行正好占满面板，X7 的 936 行
   还差 24 行，合成器可能居中（12）也可能贴 0。默认 0；真机若整体偏 12px，
   用 `{"rowOffset":12}` 覆盖。取证脚本会直接从 UI 平面的 dst y 读出来。
5. **v1 回退仍带 X6 Pro 硬编码**：`player.c`（v1）只在 X5/回退路径使用，
   未改。X7 Pro 必须走 v2（默认），若自检页显示 `PLAYER_UNSUPPORTED` 说明
   `.so` 没装载，先查 SKU 目录。
6. **GStreamer 插件**：若取证报告显示 `mppvideodec`/`kmssink` 缺失，则本机
   不能用 player2 的硬解链路，需要另接该机型的播放后端（GPU 教程包描述的
   WPE 视频协议 v3 / hole 方案可作参考），这属于下一阶段工作。
7. **无声自愈的上限**：每会话最多重建 2 次（`PB2_AUDIO_HEALS`）。若是音频 URL
   整体失效（B 站 m4s 直链带 deadline，约 2 小时过期），重建也救不回来，此时
   `status().audioDead=1`、页面提示「无声：音轨未加载，已自动重试」。真正的修法
   是重新取 playurl —— 属于下一版的前端重取逻辑，本次未做。
8. **音频设备被占是概率性的，只能兜底不能根治**：`audioOff=1` 时说明本机
   的 PCM 被别的进程长期占用（不是片源问题）。可以先退出后台的其它音频应用；
   重进这一集（新 OPEN）会自动再试一次带音频的完整链路。

---

## 8. 资料来源

以下资料由项目外部提供（X7 Pro 平台资料包 / SDK / GPU 分支教程），
只写文件名，不记录本机路径：

- `总体技术方案.md`（X7 Pro 平台事实、黑屏形态、aarch64/glibc）
- `真机安装与排障.md`（cfg.json 936×280、PMS SKU 目录、无 fb0、capture 全白）
- `YouDaoPen-SDK/docs/wiki/system_env_compile/sys_conf.md`（screen 字段语义）
- `YouDaoPen-SDK/docs/wiki/jsapi/bashi/mplayer.md`、
  `YouDaoPen-SDK/docs/词典笔小程序开发API文档.md`（mplayer 不可用）
- `YouDaoPen-SDK/scripts/setup-toolchain.sh`、`YouDaoPen-SDK/README.md`
  （x7 工具链、Node 18 坑）
- `GPU引擎使用教程.md`（RK3562/G52、DRM、混合档）
- 同包 `tools_GPU_RENDER_CLEANUP_STATUS.md`、`tools_MIGRATION_STATUS.md`、
  `tools_GITHUB_GPU_TWITCH_20261006.md`（480x960、H.264 MPP、alpha-zero hole）
- 仓库内 `docs/x6pro-adaptation.md`、`docs/x6pro-video-playback.md`、`README.md`
  （X6 Pro 基线与所有真机踩坑）
