# 弹幕（Danmaku）

> 3.5.14 引入，3.5.15/3.5.16 修了拉丝、字号档位、描边与丢帧预算，3.5.17 修了滚动弹幕半路入场，3.5.18 把预算判定提到抢泳道之前并加了「半路冒出」直接计数器，3.5.19 改了默认值，3.5.20 把默认档从全屏改成 1/4。本文记录**为什么这样实现**、**接口与数据流**、**真机实测数据**、**踩过的坑**。
> 想改这块代码之前先读完「设计约束」和「并发模型」两节。

## 1. 设计约束：为什么非烧帧不可

X6 Pro 的显示是两级叠加：

| 层 | DRM plane | zpos | 归谁管 |
|---|---|---|---|
| UI | `Esmart0-win0` | 0 | Falcon 渲染，`miniapp_cli capture` 能截到 |
| 视频 | `Esmart1-win0` | 2 | native kmssink 直送，**截图截不到** |

`zpos` 是驱动给的，**视频永远压住 UI**。所以：

- 想让文字出现在画面上 → 只能写进视频帧本身（或者占用第三个 plane，但那是另一套工程）。
- CC 字幕走的是 UI 字幕带（`.cc-bar`），它是**在画面之外**另占一条 18px 的带子，不是浮在画面上——这是同一个约束的另一种绕法。

于是弹幕必须烧进 NV12 帧。

## 2. 为什么不用 GStreamer 文字元素

真机 `gst-inspect-1.0` 的结论：

- **没有** `textoverlay` / `pango` / `cairo` / `assrender` / `freetype` / `rsvg` 元素。
- 有的是 `gdkpixbufoverlay`、`subtitleoverlay`、`dvbsuboverlay`、`dvdspu`——都不适合逐帧变化的弹幕。

系统库这边反而是齐的：`libcairo.so.2`（2.11.600）、`libfreetype.so.6`（6.17.4）、`libfontconfig.so.1`（1.12.0），
缺的是 `libpango*` / `libharfbuzz*` / `libfribidi*`。弹幕只需要「一行 CJK 文字量尺寸 + 画出来」，
不需要复杂 shaping，所以 **cairo 的 toy API 够用**，不需要 pango。

字体全在 `/etc/miniapp/resources/fonts/`（`/usr/share/fonts` 目录不存在，所以 fontconfig 的族名查询不可靠）。
默认用 `HarmonyOS_Sans_SC_Regular.ttf`——`.ttf` 比 `.otf` 的 freetype 兼容面更广。

## 3. 数据流

```
                JS（QuickJS）                        native（worker 线程）        native（streaming 线程）
  ┌──────────────────────────────┐        ┌──────────────────────┐      ┌─────────────────────────┐
  │ danmaku.js                   │        │ PB2_CMD_DM_MODE      │      │ on_vsink_buffer          │
  │  seg.so → parseSegReply      │        │  → dm_init/dm_set_mode│      │  → scrub_edge_green      │
  │  dedupeByKey（按 idStr）      │ ─────► │ PB2_CMD_DM_FEED      │ ───► │  → dm_apply              │
  │  buildFeedText（每行 6 字段） │        │  → dm_feed_text      │      │     → dm_render（持锁）   │
  └──────────────────────────────┘        │ PB2_CMD_DM_CLEAR     │      └─────────────────────────┘
                                          └──────────────────────┘
```

### 3.1 接口地址

```
GET https://api.bilibili.com/x/v2/dm/web/seg.so?type=1&oid=<cid>&pid=<aid>&segment_index=<n>
```

- `segment_index` 从 **1** 开始，每片 **6 分钟**（`SEG_SECONDS = 360`）。
- 返回体是**未压缩的 protobuf**（`application/octet-stream`）。开发机实测：不加
  `Accept-Encoding` 拿到 163273 B 明文；加了 `--compressed` 才是 gzip 80379 B。
  `native/csrc/httpjson.c:141` 的 `CURLOPT_ACCEPT_ENCODING` 是**注释掉的**，
  所以设备侧拿到的本来就是明文——**QuickJS 里不需要任何解压**，别去加。
- 走 `client.getBinary()` → `net.js` 的 `httpGetBinary()` → native `httpjson.getBinary`
  （ArrayBuffer 保字节）。注意不能走 `jsapi.http`：实测它会把 1.2MB 的 body 截成 2 字节。

### 3.2 protobuf 字段（`DmSegMobileReply`，实测确认）

顶层 `field 1` = repeated `DanmakuElem`。每个 elem：

| 字段 | wire type | 含义 | 实测 |
|---|---|---|---|
| 1 `id` | 0 varint | 弹幕 id | 全唯一 |
| 2 `progress` | 0 varint | 出现时刻（ms，相对本片开头） | 0..355653 |
| 3 `mode` | 0 varint | 1 滚动 / 4 底部 / 5 顶部 / 6 逆向 / 7 高级 / 8 脚本 / 9 BAS | `{1:2742, 4:516, 5:1743, 7:862}` |
| 4 `fontsize` | 0 varint | 字号 | 主流 25，另有 9/10/11/12/18/20/24/30/36/100…127 |
| 5 `color` | 0 varint | RGB 整数 | 251 种 |
| 6 `midHash` | 2 string | 发送者 hash | 8 字符 |
| 7 `content` | 2 string | 正文 UTF-8 | 最长 228 字符 |
| 8 `ctime` / 9 `weight` | 0 varint | 时间戳 / 权重 | — |
| 12 `idStr` | 2 string | id 的字符串形式 | 去重主键 |

### 3.3 去重：按 `idStr`，**不按 content**

真实响应体（`api-mock/fixtures/dm_seg.bin`，1236855 B / 5863 条）的实测：

- `id` 唯一 **5863/5863**
- `content` 唯一只有 **2351/5863** —— 也就是 **3512 条是「同一句话」**

这 3512 条里绝大多数是用户真的刷了同样的话（「前方高能」「awsl」），**属于合法弹幕，不能去重**。
重复只可能来自**分片边界重叠**或**同一片被拉了两次**，而这两种情况下 `idStr` 一定相同。

所以 `dedupeByKey()` 用 `idStr` 作键，跨分片用同一个 `seen` 集合。这是与「按内容去重」的关键区别
——按内容去重会把 3512 条正常弹幕吞掉。

### 3.4 feed 文本格式

`buildFeedText()` 每行 6 个字段，竖线分隔：

```
idStr|progress|mode|color|fontsize|content
```

- 竖线、`\r`、`\n` **直接删掉**（不是换成空格），否则会把 native 侧的行切分打烂。
- `content` 截到 80 字符（`DM_FEED_TEXT_MAX`）。
- `fontsize` **原样下传**（`0` 也照传），钳位在 native 侧做——这样规则只有一处。

## 4. native 实现（`native/csrc/video/dm_overlay.c`）

### 4.1 挂在哪个 pad

`session.c` 的 `on_vsink_buffer()` 挂在 **kmssink 本体的 sink pad** 上，
拿到的是 `videoscale` 之后、与 `render-rectangle` 等大的最终 buffer（真机 716×1280 NV12）。
这是唯一正确的位置——挂在分支 ghost pad 上拿到的是 640×360 的源帧。

**必须与 `scrub_edge_green()` 并列调用，不能塞进它里面**：后者的第一条语句是
`if (!g_w.sw_scale) return 0;`，X6 的 `swScale = 0`，塞进去就等于弹幕永不生效。

### 4.2 NV12 合成

```
total = w * h * 3 / 2
gst_buffer_extract(buf, 0, scratch, total)     ← 绝不用 GstMapInfo（自抄结构体布局会栈越界）
yp = scratch
uv = yp + w * h                                 ← 注意是 w*h 字节偏移，不是「w*h/2 行偏移」
dm_render(yp, uv, w, h, rotate, pos_ms, wall_ms)
if (drawn > 0) gst_buffer_fill(buf, 0, scratch, total)   ← drawn==0 时不写回，省 15 万字节拷贝
```

像素公式（`dm_put_px()`）：

```
a8 = A ; a = a8 / 255
若 a8 != 255 : sr = R*255/a8 , sg = G*255/a8 , sb = B*255/a8    （还原预乘 alpha）
否则         : sr = R , sg = G , sb = B                          （快路径，省逐像素除法）

Y =  0.299*sr + 0.587*sg + 0.114*sb
U = -0.168736*sr - 0.331264*sg + 0.5*sb     + 128
V =  0.5*sr      - 0.418688*sg - 0.081312*sb + 128

dstY = dstY*(1-a) + Y*a                       （a8==255 时直接 dstY = Y）

仅当 ((fx | fy) & 1) == 0 ：                   （NV12 的 UV 是 2x2 下采样）
    ui = (fy>>1)*w + (fx & ~1)
    dstU = dstU*(1-a) + U*a ;  dstV = dstV*(1-a) + V*a
```

**UV 只在偶数/偶数点写是关键**：一个字覆盖几十像素却只贡献一个色度采样，
否则彩色边缘会糊成一块，色度还会写出几百级的偏移。

### 4.3 旋转

面板是 480×960 竖屏，逻辑 960×266 横屏，`g_w.rotate = 2`（=270°）。
`dm_map()` 把逻辑坐标映到帧坐标：

```
rotate  90 : fx = w-1-ly , fy = lx
rotate 180 : fx = w-1-lx , fy = h-1-ly
rotate 270 : fx = ly     , fy = h-1-lx      ← 真机走这条
rotate   0 : fx = lx     , fy = ly
```

`dm_norm_rot()` 同时吃度数（`session.c` 的 `rotation_for_transpose()` 返回的是度数）和枚举（1/2/3）。
实现上不做整帧转置：`dm_blit()` 每行只算一次 `dm_map()`，行内用差分推进（映射是线性的）。

### 4.4 字体加载：双路线，B 优先

- **路线 B（首选）**：`dlopen("libfreetype.so.6")` → `FT_Init_FreeType` → `FT_New_Face` 三个候选字体 →
  `cairo_ft_font_face_create_for_ft_face(face, 0)` → `cairo_set_font_face`。能精确指定文件，
  不依赖 fontconfig 的族名匹配。
- **路线 A（兜底）**：`dlopen("libfontconfig.so.1")` → `FcConfigAppFontAddFile(NULL, path)` →
  6 个族名 + `cairo_select_font_face`。
- **成败判据是「量得出字」而不是「函数返回非 NULL」**：`dm_probe_cjk()` 量一个 `弹` 字，
  要求 `x_advance > 1.0 && height > 0.5`。cairo 在缺字体时会「成功」建一个画不出字的 face，
  不实测就会拿到一整屏空白弹幕。

cairo / freetype / fontconfig **全部 dlopen**，不进链接行——`build.ps1` 的 stub 目录里没有这三个库。

### 4.5 性能取舍

- `DM_MAX_ONSCREEN = 24`：每帧最多画 24 条，超出的只计入 `visible` 不画。
- `DM_CACHE_MAX = 48` 条 + `DM_CACHE_BYTES = 6MB` **双约束**，按帧号 `stamp` 做 LRU。
  选 LRU 不选 FIFO 是因为弹幕有大量「同一条反复进出视野」的重复命中。
- `dm_feed_text()` 只保留 `t_ms` 升序数组，渲染时二分定位窗口起点（`dm_lower_bound(pos - DM_SCROLL_MS)`）。
- seek 检测：`pos_ms` 回跳 > 500ms 或前跳 > 3000ms 就重置泳道表，否则「跳回去之后一半弹幕再也不显示」。

## 5. 并发模型（**这一节是踩过坑的**）

`dm_render()` 跑在 **GStreamer 的 streaming 线程**（pad probe 里），
而 `dm_feed_text()` / `dm_clear()` / `dm_set_mode()` 跑在**命令队列的 worker 线程**。

两者无锁并发操作 `g_items` 时会撞车：worker 侧 `realloc()` 让 render 正在遍历的旧指针立刻失效。
**真机表现是整条管线卡死**——`RECOVER` 之后 `teardown: set_state(NULL)` 再没有重建，
帧率掉到 0，而两个 ffmpeg 进程都还活着。

所以 `dm_overlay.c` 里有一把 `static pthread_mutex_t g_lock`，保护「弹幕表 + 泳道表 + 统计量」，
**不保护字形缓存**（字形缓存只在 render 里动）。

实现方式：所有会碰共享状态的函数都拆成 `xxx_locked()`（内部实现）+ `xxx()`（公开包装，加锁）。
`dm_shutdown_locked()` 内部调的是 `dm_clear_locked()`——调 `dm_clear()` 会自己再抢一次锁 → 死锁。

锁粒度是**整帧合成**。实测中位数 1~3ms（首帧字形未命中 ~140ms），
worker 每 50ms 才跑一次命令，被挡几百毫秒没有任何功能影响。

> 教训：子 agent 的接口文档里写「所有 `dm_*` 只在 worker 线程调用」是**错的**，
> 照着它写就会漏锁。线程归属要以实际 call site 为准（`gst_pad_add_probe` 是 streaming 线程）。

## 6. 开关与 UI

四档切换式（点一下转一档）：

| 模式 | 值 | 文案 | 泳道数（真机） |
|---|---|---|---|
| `off` | 0 | 弹幕 | 0（零开销，`dm_render()` 首条语句返回） |
| `full` | 1 | 全屏 | 25 |
| `half` | 2 | 1-2 | 12 |
| `quarter` | 3 | 1-4 | 6 |

「1/2」「1/4」指**画面上方 1/2 或 1/4 的高度区域**（弹幕不覆盖下方）。

入口两处：

- 播放页左列第 5 颗键（信息卡下方那排等分键的最后一颗）。
- 「我的 → 弹幕显示」一行，与画质/字幕同款轮转交互。

设置持久化在 `bili_dm_settings`（`{version:1, mode}`）。**模式持久化、数据不持久化**：
换视频时保留模式（用户拨到 1/2 后换片期望还是 1/2），但 `dm.list` / `segIdx` 清空。

字号自适应：`dmFontPx() = clamp(round(videoH * 0.09), 14, 36)`，266 高上算出来是 **24**。

## 7. 真机实测

X6 Pro（960×266 逻辑 / 266×472 帧）：

| 项 | 数据 |
|---|---|
| 字形加载 | `dm: overlay ready font=.../HarmonyOS_Sans_SC_Regular.ttf px=24`（走路线 B） |
| 首帧 | `dm: first render 716x1280 rotate=270 lanes=25 took=20508us`（冷缓存） |
| 热态合成 | `took=81us / 445us / 783us / 1865us / 2878us`（1~3ms） |
| 帧率 | 弹幕全屏开启下 `vid_fps=30.00`、`big_gaps(>80ms)=0` |
| 泳道数 | `mode=1 lanes=25` / `mode=2 lanes=12` / `mode=3 lanes=6` |
| 平面几何 | `dst: pos[107,274] rect[266x472]`、`zpos: 2`、`src 716x1280`、`pitch 716` |
| 统计日志 | 每 5 秒一行 `dm: <w>x<h> rot=<r> mode=<m> lanes=<n> vis=<v> drawn=<d> items=<i> took=<us>` |

**截图看不到弹幕**：`miniapp_cli capture` 只截 Falcon UI 层，不含 KMS 视频平面。
验证只能靠日志 + 肉眼。

排查话术：`vis > 0 && drawn == 0` = 泳道排满或字形构建失败；`items=0` = 数据没喂进来（查 `DM_FEED` 日志）。

### 3.5.17 滚动弹幕半路冒出（迟到入场）

横向位置公式是 `x = lw - elapsed * (lw + bw) / DM_SCROLL_MS`，`elapsed = pos_ms - it->t_ms`。
也就是说位置完全由「本该出现的时刻」推出来，不取决于什么时候真的抢到泳道。于是：

| 场景 | 现象 |
| --- | --- |
| 播放中途打开弹幕 / 从 1-4 切到全屏 | 窗口里 `t_ms` 早已过去的条目下一帧集体抢泳道，`elapsed` 已几秒，`x` 落在左侧甚至屏幕外 |
| 泳道排满 | `dm_take_lane()` 返回 0 时 `lane` 保持 -1 逐帧重试，等某泳道空出来这条已经迟到很久 |
| 起播缓冲门放行前渲染过若干帧 | 位置一次性跳变 |

修法：`#define DM_LATE_MS 300` / `#define DM_LANE_DEAD (-2)`。`elapsed > 300` 时置 `lane = DM_LANE_DEAD`
并 `continue`，**本次播放不再尝试分配泳道**；`lane == DM_LANE_DEAD` 的条目在循环入口直接跳过。
判定必须放在抢泳道**之前** —— 抢到泳道再发现迟到，泳道占用表已经被写脏。
统计日志加了 `late=` 字段，真机实测（`BV1Nxs26uE46`，3952 条弹幕）打开瞬间 `late=226` 逐帧递减，
丢弃的正是那批本来会半路冒出来的条目；同片热态仍 `vid_fps=30.00 big_gaps=0`。

### 3.5.18 预算前置与「半路冒出」直接计数

3.5.17 只解决了「该丢的丢掉」，但丢得太多：主循环按 `t_ms` 升序扫描，
`if (drawn >= g_budget) continue;` 却在 `dm_take_lane()` **之后**。于是抢到预算的永远是已经飞了
5~8 秒、快到左边的老条目，刚该从右边入场的新条目拿不到预算 —— 但它已经先把
泳道占住了，那个位置被锁 8 秒。等老条目寿命耗尽腾出预算，新条目首次被画出来时
`elapsed` 已经一两秒，`x` 落在画面中段。

修法：把顺序改成**先判迟到 → 再查预算 → 最后抢泳道**。预算用完的条目直接
`continue`，**不再为它预留泳道**。代码形状（`dm_overlay.c` 的渲染主循环）：

```c
if (it->lane == DM_LANE_DEAD) continue;
if (it->lane < 0 && elapsed > DM_LATE_MS) { it->lane = DM_LANE_DEAD; dead++; continue; }
if (drawn >= g_budget) { hungry++; continue; }   /* 关键：在抢泳道之前 */
if (it->lane < 0 && !dm_take_lane(it, kind, lanes, pos_ms)) { visible++; continue; }
```

两个新计数器写进了 5 秒一条的统计日志：

| 字段 | 含义 | 健康值 |
|---|---|---|
| `hungry=` | 本窗口被预算挡在门外、不再占泳道的条数 | 密集稿子上每 5 秒两三百条 |
| `mid=` | 首次画出时 `elapsed > DM_LATE_ENTRY_MS`（160ms）的条数，即「半路冒出」 | 应接近 0 |

真机实测（X6 Pro，720x1280 缓冲区，`px=59 lanes=11`）：

```
dm: 720x1280 rot=270 mode=1 px=59 lanes=11 vis=6 drawn=6 late=7  hungry=331 mid=0 budget=6  items=597  took=11434us
dm: 720x1280 rot=270 mode=1 px=59 lanes=11 vis=3 drawn=3 late=3  hungry=111 mid=0 budget=24 items=597  took=1640us
dm: 1280x720 rot=270 mode=1 px=72 lanes=16 vis=6 drawn=6 late=30 hungry=302 mid=0 budget=6  items=1478 took=44400us
```

`vis` 与 `drawn` 从修复前的「`vis=34 drawn=6`」（判可见却画不出）变成相等，帧率仍
`vid_fps=29.50~30.00`、`big_gaps` 0~1。落帧取证（`.tmp/an_dm.py`，NV12 → `np.flipud(B).T`）显示修复后
左右两侧边缘都有墨迹（之前右缘 3% 恒为 0）。

> 为什么还不改「位置从开始画起算」：那会让任何迟到条目都从右缘进场，但
> 时间轴与视频轻度脱钩。先用预算前置把迟到的根源（泳道被占）消掉，`mid`
> 仍然不为 0 时再论。

### 3.5.19 默认值：弹幕开、字号小、字幕关

| 项 | 3.5.18 及之前 | 3.5.19 |
|---|---|---|
| 弹幕开关 | `DM_MODES` 默认 `'off'`，`normalizeDmSettings` 脏数据也回 `off` | **`DM_MODE_DEFAULT = 'quarter'`**（3.5.19 曾为 `'full'`，3.5.20 改 1/4） |
| 弹幕字号 | `DM_SIZE_DEFAULT = 'mid'`（22px） | **`'small'`**（16px） |
| 字幕 | 「从没动过开关且稿子有轨道」就自动打开（`autoCc`） | **默认关闭**，只有 `this.cc.on` 为真才拉字幕文件 |

三个默认值在三个地方同步，改一个就会不一致，所以全部抽成常量：

- `PenBili/src/services/bili/danmaku.js` 的 `DM_MODE_DEFAULT` / `DM_SIZE_DEFAULT`
- `PenBili/src/pages/index/index.vue` 的 `dm` data 初值（`mode: DM_MODE_DEFAULT, size: DM_SIZE_DEFAULT`）
- `PenBili/test/run.js` 的 `danmaku: 字号档位`与`danmaku: 分片下标`两条断言

字幕那边删掉了旧版 `initExtras` 里的整块 `autoCc` 逻辑，只留
`if (this.cc.on) await this.loadCcCues(myGen, info.subtitles);`。`ccSettingHint` 仍负责把
「为什么没字幕」写在「我的 → CC 字幕」那行下面。

## 8. 已知限制

- 直播没有弹幕（`tickDm()` 里 `isLivePlay` 直接 return）。
- 弹幕一次性按整批替换（`DM_FEED` 语义是「这一批替换掉旧的」），不做增量追加。
- 高级弹幕（`mode=7`，带 pos/animation JSON）按普通滚动弹幕画，不解析其定位参数。
- 底部弹幕（`mode=4`）走固定 4 秒停留，与顶部弹幕共用固定泳道池。
