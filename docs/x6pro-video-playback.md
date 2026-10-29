# X6 Pro 视频播放链路（KMS overlay）与音频通路

本文记录 X6 Pro（RK3562，Buildroot 4.3.5，aarch64）上把视频真正画到屏幕上、
并把声音从扬声器放出来的整条链路。X5 那一套 `/dev/fb0` + fbdev blit 在 X6 Pro 上
**完全不可用**，原因见 §1。

相关代码：`native/csrc/player.c`（`render_*` 与 `mixer_*` 系列）、
`native/build.ps1`（`-Target aarch64|arm`）、`tools/inspect_elf.py`。

---

## 1. 为什么 X6 Pro 不能沿用 fbdev

| 事实 | 实测命令 | 结果 |
| --- | --- | --- |
| 没有 framebuffer 设备 | `ls /dev/fb*` | `No such file or directory` |
| 有 DRM | `ls /dev/dri` | `card0 card1 renderD128 renderD129` |
| 面板 | `modetest -M rockchip` | `DSI-1` connected，单模式 `480x960@59.88` |

X5 的 `fb_open()` 读 `/sys/class/graphics/fb0/*` 后 `mmap` `/dev/fb0` 逐行 memcpy，
在 X6 Pro 上第一句就失败（老代码会返回 `"/dev/fb0 打开失败（需要 32bpp 且可写）"`）。
因此 X6 构建用 `-DPENBILI_KMS_ONLY=1` 把整段 fbdev 探测编译掉，改用 DRM/KMS 硬件叠加平面。

## 2. 屏幕几何：逻辑面 ≠ 面板

Falcon 画布是 **960x266 逻辑面**，但物理面板是 **480x960 竖屏**，逻辑面被旋转
270° 后只占面板中间的一竖条：

```
逻辑 (x, y)  →  面板列 107 + y,  面板行 960 - x
```

- `PANEL_X_OFFSET = 107`（= (480 − 266) / 2，逻辑面在 480 宽面板里居中）
- 面板内容实测占列 `107..372`（宽 266）、行 `0..959`
- VOP 寄存器的 `dst pos[107,244] rect[266x473]` 与 JS 侧
  `playVideoRects()` 给出的 physical rect 完全一致：JS 给的是**逻辑面坐标里已旋转的
  面板坐标**，C 侧只需再加 `PANEL_X_OFFSET` 到 x。

所以 16:9 视频落点 = 面板 `x=107, y=458, w=240, h=427`；竖屏 9:16 = `x=107, y=604, w=240, h=135`
（v3 布局：视频区 448x240，底部 26px 让给进度条，见 §2.2）。

### 2.1 矩形变换是**真旋转**，不是「面板内居中」

`src/services/screen.js` 的 `logicalRectToPhysicalRect()` **v3 起已改成真变换**
（此前是「面板内居中」近似：

```js
x: Math.round((panel.width  - pw) / 2),   // pw 恒 == 266 ⇒ 恒为 0
y: Math.round((panel.height - ph) / 2),   // 面板内垂直居中
```

这是近似，不是旋转变换）。由上面的点映射推出矩形的正确变换是：

```
先算 (x, y) 和 (x+w, y+h) 两个角点的面板坐标，再取最小/最大：
  面板 x = 107 + logical_y
  面板 y = 960 − (logical_x + logical_w)
  面板 w = logical_h
  面板 h = logical_w
```

两者何时碰巧相等：只有当 `logical_x + logical_w / 2 == 480`（即视频本来就水平居中于
960 逻辑面）时，`(960 − w) / 2` 才等于 `960 − (x + w)`。老布局（左栏 176 + 视频列 608 +
右栏 176）的 16:9 恰好 `x=244, w=473` ⇒ `244 + 236.5 ≈ 480.5`，于是居中近似和真变换
只差 1px（243 vs 244），看不出问题。**一旦改变左右栏宽度，这个恒等式立刻破**。

对照（16:9，两种布局）：

| 布局 | logical rect | 真·面板 rect（含 107） | 居中近似会给出的 |
|---|---|---|---|
| X5 老（左 176 / 列 608 / 右 176，全高） | `x=244, w=473, h=266` | `(107, 243, 266, 473)` | `(107, 244, 266, 473)` — 差 1px，看不出 |
| X6 v3（视频区 64,0,448,240） | `x=75, w=427, h=240` | `(107, **458**, 240, 427)` | `(107, 458, 240, 427)` — 此处两式同值，但 y 与尺寸都变了 |

注意真变换里**面板 x = 107 + logical_y**，改左右栏宽度只改 `logical_x` ⇒ **只改面板 y，
不改面板 x 与视频尺寸**。这是设计新布局时最容易搞反的一点（面板两轴与逻辑两轴交叉映射）。

### 2.2 v3 布局：视频区高度 240，底部 26px 留给进度条

`BAR_WIDTH = 76`（主页左栏）、`ICON_BAR_WIDTH = 64`（播放态左图标栏）、
`VIDEO_AREA = {x:64, y:0, width:448, height:240}`。

**为什么高度是 240 而不是 266**：视频走 KMS overlay 平面，**盖在 Falcon UI 之上**，
UI 在视频矩形内画任何东西都会被覆盖。所以进度条必须落在视频矩形之外 ——
逻辑 `y∈[240,266)` 那 26px 是进度条带。由 `物理 x = logical_y` 直映，进度条占面板
`x∈[107+240, 107+266) = [347,373)`，视频必须满足 `物理 x + width <= 240`。
测试 `screen: 宽比例视频必须信箱进视频区` 对 7 种画幅断言了这一条（16:9 恰好贴边 347）。

`env/tmp/geom_exact.mjs` 是这个结论的可执行验证：对 9 种画幅 × 3 种布局输出
logical/exact/device/旧实现 四组矩形，并检查比例守恒、反变换闭合、不越出面板与 107..373 列。


## 3. 渲染后端：ffmpeg 只搬包，解码交给 MPP

X6 Pro 是 RK3562，带 MPP 视频解码单元。v3.0.0 起解码不再走 ffmpeg 软解：

```
ffmpeg -c:v copy -bsf:v h264_mp4toannexb -f h264  ──压缩 H.264 Annex-B──▶  /tmp/vp_vid.fifo
                                                                              │
        gst-launch-1.0 filesrc location=/tmp/vp_vid.fifo blocksize=65536 ─────┘
          ! h264parse
          ! mppvideodec rotation=<R>
          ! kmssink plane-id=76 driver-name=rockchip
                    sync=false force-aspect-ratio=false
                    render-rectangle=<107,y,W,H>
```

`ffmpeg` 只做 demux + 容器解包，CPU 从「软解 + 每帧 rgb32 拷贝」降到「只搬压缩包」。
实测 640×360@30 的直播源解码吞吐约 **33 倍实时**（40 秒素材 1.21 秒解完），
瓶颈完全落在网络侧。

### 3.1 为什么不能直接在 C 里 SETPLANE

1. **裸 SETPLANE 用哑分配缓冲会被拒**。自己 `CREATE_DUMB` 的 buffer 提交给
   plane 76 时 driver 返回 `ENOSPC`（rockchip 驱动要求 AFBC/dma-heap 的
   framebuffer 才能上 overlay plane），而 `modetest -P 76@68:...` 也是**静默 no-op**
   （`plane[76]: fb=0` 不变，退出码却是 0）。
2. **只有 kmssink 能真正占到该平面**：起链后
   `/sys/kernel/debug/dri/0/summary` 里出现
   `Esmart1-win0: ACTIVE, format NV12, zpos 2, dst pos[107,450] rect[240x427]`，
   而 UI 在 `Esmart0-win0 zpos 0` —— 视频硬件叠在 UI 之上，Falcon 的合成器完全不参与。
3. 用 FIFO + `filesrc blocksize=` 而不是 `fdsrc`：这台设备的 `fdsrc` 拿来做定长
   切分**会短读并崩流**（`gst_base_src_loop: streaming stopped, reason error (-5)`），
   而 `filesrc` + `blocksize` 是稳定的。
4. 不需要 `videoconvert`：`mppvideodec` 输出 NV12，`kmssink` 直接吃。

### 3.2 `force-aspect-ratio=false` 必须显式给
否则 kmssink 会在给定矩形内自己再做一次信箱缩放，画面会和 JS 算好的 rect 对不上。

### 3.3 旋转交给解码器，不要用 ffmpeg `transpose`
`mppvideodec` 自带 `rotation` 属性（Enum 0/90/180/270）。JS 侧恒传 `transpose: 2`
（面板 `direction=270`），C 侧 `rotation_for_transpose()` 映射成 `rotation=90`，
解码器直接把正立画面写进平面，省掉整帧旋转的 CPU 开销。

**踩过的坑**：`open()` 里写的是 `s->transpose`，而 `render_start()` 读的是
`g_rd.transpose`——全局 `g_rd` 初值是 0，于是 `rotation_for_transpose(0)` 恒走默认分支
输出 270，无论怎么改映射函数都毫无效果。症状是「画面上下倒置、改代码没反应」。
修法是在 `open()` 里把 `transpose` 同步写进 `g_rd.transpose`。

### 3.4 画面节拍：以音频为锚的低通 PLL
`video_thread()` 每帧按 `1/fps` 排程，再用「当前可闻音频位置 − 本帧应有位置」的
残余漂移做 ±1~3ms 的微调：

```c
int adj = 0;
long long d0 = apos - due;
if (d0 > AV_SKIP_MS) adj = 3;         /* 声音超前 → 缩短间隔 */
else if (d0 < -AV_SKIP_MS) adj = -3;  /* 画面超前 → 拉长 */
else if (d0 > 0) adj = 1;
else if (d0 < 0) adj = -1;
```

±3ms/帧 @30fps = 每秒最多纠 90ms，长期相位仍被 drift 锁向音频，大滞后照旧走
快进 / 强制重同步兜底。

**为什么不能硬等**：音频锚 `audio_anchor_ms()` 来自 `writer_bytes`，而 writer 以 8KB
为写块，换算成时间约 46ms 一台阶。旧代码 `while (apos < due) usleep(2000);` 硬等到
音频位置跨过阈值才放行 → 连续几帧卡在同一台阶上一起等、台阶一跳又整批放行 →
**约每秒一次的顿挫**。实测相邻两次上屏的最大间隔 126ms；改节拍器后降到 30~33ms
（正好一帧），`video_skips` 从 544 降到 2，音画漂移稳定在 ±60ms 内。

诊断开关：`export PENBILI_VID_DIAG=1` 后 `video_thread()` 每秒往 `/tmp/vp_vid.log`
落一行 `t/n/fps/wait/maxwait/wgap/rdgap/augap/fifo/sk/drift/aulen/st`（上限 300 行），
默认关闭。`wgap` 就是「上屏最大空档」，排查卡顿先看它。

### 3.5 孤儿 gst-launch 会永久占住平面（已修）
早期症状：重启 app 后视频冻在最后一帧，`plane 76: ACTIVE` 但 `buf[0] addr` 不变，
两次抓帧 diff = 0。原因是 `render_start()` 里 `fork()` 后子进程 `setsid()`，当模块被
JS 重启 / 页面重进 / app 被杀时，`gst-launch` 变成孤儿继续跑并**一直占着 plane 76**，
新实例的 kmssink 抢不到。

修复：`render_reap_orphans()`（`player.c` 约 405 行）扫描 `/proc/[0-9]*/cmdline`，
把 cmdline 同时包含 `gst-launch-1.0` 与 `KMS_FIFO_PATH`、且 pid ≠ 当前子进程的进程
`SIGKILL`；在 `render_start()` 中 `render_stop()` 之后、`mkfifo()` 之前调用。

## 4. 取证方法（关键：`miniapp_cli capture` 看不见 overlay）

**`miniapp_cli capture` / `captureFB` 只抓 Falcon/weston 的 UI 平面（plane 54）**，
不抓合成后的屏幕。播放中截图，视频那块永远是纯黑（md5 与纯 UI 截图一致）。

真正取证要用 kmssrc 直接抓 overlay 平面：

```bash
gst-launch-1.0 -q kmssrc plane-id=76 num-buffers=1 ! videoconvert ! pngenc \
  ! filesink location=/tmp/v1.png
sleep 1
gst-launch-1.0 -q kmssrc plane-id=76 num-buffers=1 ! videoconvert ! pngenc \
  ! filesink location=/tmp/v2.png
```

判据：两张 266x473 的 PNG 在视频矩形内像素不同（实测两帧差 12.5%，帧内时间码
`00:00:12.631` → `00:00:14.184`），且同一时刻 `/sys/kernel/debug/dri/0/summary`
里 plane 76 的 `buf[0] addr` 在翻页。`kmssrc plane-id=76` 只在 plane 真正在扫帧时
有数据，`fb=0` 时抓到 0 字节 —— 这本身也是一个健康指示。

## 5. 音频通路：`Playback Path` 出厂是 OFF

X6 Pro 上 `aplay` 明明在跑、`/proc/asound/card1/pcm0p/sub0/status` 是 `RUNNING`、
`Master 88%`，扬声器却**完全无声**。根因是 rk817 的 mixer 控件 `Playback Path`
（`amixer -c 0`，numid=45）开机默认 `OFF`，功放通路没接通：

```
$ amixer -c 0 sget 'Playback Path'
  ; Item #0 'OFF'
$ amixer -c 0 sset 'Playback Path' 'SPK'
  -> 立刻出声
```

X5 上有系统音频守护进程代劳；X6 Pro 的 PulseAudio 连不上
（`amixer: Mixer attach default error: Connection refused`），没人设。

`player.c` v2.3.0 起自己处理（`audio_route_ensure()`，在 `spawn_aplay()` 里 fork
aplay **之前**调用）：

- `popen("amixer -c 0 cget name='Playback Path'")` 读出当前枚举名；
- **只有当前值等于 `"OFF"` 才写 `SPK`**；已经是 `HP`/`HP_NO_MIC`/`BT`/`SPK` 一律不动
  —— 绝不把用户手动选的耳机抢回扬声器；
- 控件不存在（X5）或 amixer 失败时静默返回，不影响播放；
- 成功时打 `[PLAYER] Playback Path OFF -> SPK (bt=N)` 到 stderr。

蓝牙走 `bluealsa:` 时同样设 `SPK`：bluealsa 不经过 rk817 路由，改它没有副作用。

### 5.0 解析坑：`amixer cget` 的**第一行**也被单引号包着

`amixer -c 0 cget name='Playback Path'` 的真实输出：

```
numid=45,iface=MIXER,name='Playback Path'          <-- 首行！控件名也在单引号里
  ; type=ENUMERATED,access=rw------,values=1,items=11
  ; Item #0 'OFF'                                  <-- 枚举值在这里
  : values=0
```

v2.3.0/2.3.1 取「第一对单引号」⇒ 拿到的是控件名 `'Playback Path'`，与 `"OFF"`
永不相等 ⇒ **自愈静默失效**（代码看着完全正确，实际一次都没触发过）。
真机证据（`mixtest get`，修前/修后）：

```
current = 'Playback Path'      # v2.3.0 / 2.3.1 —— 错
current = 'OFF'                # v2.3.2   —— 对
```

v2.3.2 起只认 `; Item #` 那一行，并在该行内取第一对单引号。
**教训**：写 mixer 解析前先把 `cget` 的真实输出打印一遍，别假设第一对引号就是值。

### 5.1 `card1 Master` 会被别的进程重置
`amixer -c 1 sset 'Master' 200` 之后约 5 秒会自己变回 `75`。所以**不要靠
`Master=0` 保证静音**，要静音就把 `Playback Path` 置回 `OFF`。

### 5.2 判断音频是否真的在跑
`ps` 在这台设备上是残的（`ps -ef` / 扫 `/proc/*/comm` 常常一个都扫不到），
日志里的 `ab=` 增长只证明数据在产/在消费，**证明不了用户听得见**。客观判据用三个一起看：

```bash
amixer -c 0 sget 'Playback Path' | grep Item0      # SPK
for f in /proc/asound/card*/pcm*/sub*/status; do head -1 "$f"; done   # state: RUNNING
amixer -c 1 cget name='aw_dev_0_switch' | grep -i Item   # Enable
```

听感只能靠人。

### 5.3 蓝牙：`bluealsa-aplay -L` 在本机是**崩溃的**（比 Playback Path 更靠前的墙）

X6 Pro 固件 4.3.5 自带的 `bluealsa-aplay` v3.0.0（43344 B, Mar 18 2024）跑 `-L`
直接栈溢出：

```
$ /usr/bin/bluealsa-aplay -L
*** stack smashing detected ***: terminated
Aborted
```

耳机已 `Connected: yes`、Audio Sink UUID 也在时同样崩。由于 abort 前 stdout 没
flush，`popen()` 永远读到空列表 ⇒ `detect_bt_pcm()` 恒返回 -1 ⇒
`audio_is_bt` 恒 0 ⇒ **音频永远走内置扬声器，蓝牙耳机根本不可能出声**。
这就是「X5 上蓝牙能用、X6 Pro 上没反应」的直接原因之一。

`player.c` v2.3.1 起加 BlueZ 回退（`detect_bt_pcm_bluez()`）：

1. legacy 路径（`bluealsa-aplay -L`）先跑，拿到 `bluealsa:` 行就用；
2. 拿不到就 `bluetoothctl devices` 解析 `Device <MAC> <name>`（MAC 校验
   `strlen==17 && mac[2]==':' && mac[5]==':'`，最多 8 台）；
3. **先 `pclose()` 外层 pipe 再逐台 `bluetoothctl info '<MAC>'`**——不 pclose
   会管道缠绕，`info` 读不到东西；
4. 单台判定要求**同时**命中 `Connected: yes` 和 `Audio Sink`。只要 `Connected`
   不够：AVRCP-only 的设备能连上但放不出声，选它等于静默无声，比不选更坑；
5. 命中后自己拼 `bluealsa:DEV=<MAC>,PROFILE=a2dp`，并打
   `[PLAYER] bt via bluetoothctl: <pcm>` 到 stderr 方便 grep。

**不要用 `bluetoothctl devices Connected`**：这版 BlueZ 报
`Too many arguments`，必须 `devices` + 逐个 `info`。

真机实测（`env/tmp/bttest`，把上面逻辑原样抄出来的独立探针，纯只读、不出声）：

```
legacy FAILED (bluealsa-aplay -L crashed/empty) -> bluez fallback
scanned 1 paired device(s)
B0:A3:F2:AD:7E:FA -> connected_sink=1
bluez ok: bluealsa:DEV=B0:A3:F2:AD:7E:FA,PROFILE=a2dp
```

手写 PCM 名本身是通的（Lead 实测，可用作对照）：

```sh
aplay -D 'bluealsa:DEV=B0:A3:F2:AD:7E:FA,PROFILE=a2dp' \
      -t raw -f S16_LE -r 48000 -c 2 --duration=3 /dev/zero
# rc=0，bluealsa-pcm.c 打 .../a2dpsrc/sink: Starting IO loop: 10 → Stopping
```

所以调用方依然可以显式传 `audioDevice='bluealsa:...'` 覆盖自动检测
（`spawn_aplay()` 里 `use = device` 直接生效，`session_start()` 见到
`bluealsa` 前缀就设 `audio_is_bt=1`、采样率 48000）。两条路都通。

## 6. 构建

```powershell
# X6 Pro（默认）
pwsh native/build.ps1
# X5 / 32 位
pwsh native/build.ps1 -Target arm
# 或 PENBILI_TARGET=arm
```

aarch64 用 `zig cc -target aarch64-linux-gnu.2.17` + `native/stubs-aarch64/`
（`libquickjs.so`、`libcurl.so.4` 等 ELF64 空实现 stub，仅提供符号，不参与运行），
并带 `-DPENBILI_KMS_ONLY=1`。构建脚本末尾自动跑
`python tools/inspect_elf.py <so> --target <t>` 校验
（`arm` → class 1 / machine 40，`aarch64` → class 2 / machine 183），不匹配直接 exit 1。

## 7. 排查用速查

```bash
# 平面状态：谁是 ACTIVE、谁是 fb=0
grep -a -A9 'Esmart' /sys/kernel/debug/dri/0/summary

# 当前播放子进程
grep -a 'gst-launch\|ffmpeg' /proc/*/cmdline 2>/dev/null

# 播放趋势日志（帧号/位置/跳帧）
tail -20 /tmp/vp_trend.log

# 唤醒面板（面板睡着时 kmssrc 抓到 0 字节）
hal-screen on; hal-screen keep

# 日志一律 grep -a：文件含二进制字节，普通 grep 返回空
grep -a '\[bili\]' /userdata/applog/DictPen_*.log | tail -40
```
