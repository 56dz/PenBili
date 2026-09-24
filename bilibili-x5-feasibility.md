# 有道词典笔 X5 上编写 B 站 App 的可行性分析

- 分析对象：有道词典笔 X5（YDPX5-1 / Cvitek CV1826），固件 appVersion 3.4.6，profile_id `youdao-x5-fw3.4.6`
- API 依据：[xieren58/bilibili-API-collect](https://github.com/xieren58/bilibili-API-collect)（SocialSisterYi/bilibili-API-collect 的镜像；上游已 archived）
- 设备依据：本机既有三个 X5 项目（`deepseek-x5` / `video_player_x5` / `x5_cloud_browser`）的 profile + 本次在在线设备 `BCC06********40`（序列号已脱敏）上的**只读探测与真实 B 站流播放实测**
- 本次**未创建任何项目**，只在仓库根新增本分析文档

---

## 0. 结论

**技术上可行，且已有真机证据支撑；瓶颈是 B 站风控与 API 稳定性，不是设备能力。**

| 维度 | 结论 |
|---|---|
| 播放 B 站视频 | **可行**。真机用设备自带 ffmpeg 4.4 软解真实 B 站流，360p 全链路（解码→缩放→旋转→rgb32）达 **1.5–1.9x 实时** |
| 画质天花板 | **360p 舒适、480p 勉强（1.38x）、720p 不可用（0.76x）**；无可用硬件解码器 |
| 匿名可用 | **不需要登录即可拿元数据 + 播放 360p**（html5 单文件路径），MVP 可零登录 |
| 元数据 API | 可行（`view` / `related` / `popular` / WBI `playurl` 全部匿名跑通），但 **`-352` 风控真实存在**（本次已复现） |
| UI 承载 | 可行，`800x254` 逻辑条带 + 既有 `<hole>` 视频方案已在该机型验证 |
| 最大不确定性 | 风控触发阈值未知、B 站对第三方客户端的持续封堵、扫码登录取 Cookie 需额外手段 |
| 工作量 | MVP（匿名浏览 + 360p 播放 + 分P + 进度）中等；弹幕覆盖/登录/直播是后续增量 |

---

## 1. 设备事实（决定边界的硬约束）

来源：三个既有项目的 `profiles/youdao-x5.md` + 本次实测。

```yaml
soc:    Cvitek CV1826 (cv182x), armv7l, 单核 Cortex-A53, NEON, 无 GPU
内存:   MemTotal 352 MB；实测 MemAvailable 175~222 MB
系统:   Linux 4.19.164 / Buildroot 2021.05-rc3, glibc 2.23
屏幕:   逻辑合成面 800x254；物理 254x800 旋转 270；/dev/fb0 32bpp 双缓冲
JSAPI:  $falcon.jsapi 只有 http / modal / storage
        自定义头字段名是 header(单数)；不暴露响应头；非 2xx 拿不到响应体
HTTPS:  无 CA 库（/etc/ssl/certs 为空），但运行时与 ffmpeg 都不校验证书 → 可直连 https
媒体:   无 GStreamer、无 DRM/KMS、无 WebView/浏览器内核、无复杂文本整形
        ffmpeg 4.4 软件解码 h264/hevc/av1/vp9 + aac/mp3/opus/flac；音频走 aplay
        /dev/dri 不存在；/dev/video100+ 是 CVI ISP 通道，非解码器
native: zig 0.16 交叉编译 arm-linux-gnueabihf.2.23（env/pyzig），可产出 .so
        既有可用模块：libjsapi_httpjson.so（libcurl 原始请求）、libjsapi_player.so（ffmpeg→fb 播放器）
```

### 本次新增探测结论

| 项 | 结果 |
|---|---|
| 笔能否上外网 | **能**。`curl -k https://api.bilibili.com/x/web-interface/nav` 返回 `code:-101` + `wbi_img`，`ip_region:CN` |
| 笔上 curl 直连 https | 默认**证书校验失败**（无 CA），`-k` 后正常 → CA 缺失只影响命令行 curl，不影响运行时/ffmpeg |
| ffmpeg 证书策略 | `-tls_verify` 默认 0，且实测 https 拉流无证书错误 → **无需 CA** |
| 硬件解码 | **不可用**。`-c:v h264_v4l2m2m` → `can't configure decoder / Invalid argument`（编进构建但没有可用的 V4L2 M2M 节点） |
| 可用解码器 | h264 / hevc / av1 / vp9（纯软解）；aac / mp3 / opus / flac |

---

## 2. B 站 API 面（工程上真正要处理的部分）

### 2.1 匿名即可用的接口（本次实测跑通）

| 接口 | 用途 | 实测 |
|---|---|---|
| `x/web-interface/nav` | 取 WBI 密钥 `wbi_img`（未登录也返回） | code -101，含 wbi_img ✓ |
| `x/frontend/finger/spi` | 取 `buvid3` / `buvid4`（**在 body 里返回**） | code 0 ✓ |
| `x/web-interface/view?bvid=` | 标题 / 封面 / cid / 分P / 时长 | code 0 ✓ |
| `x/web-interface/archive/related?bvid=` | 相关推荐（40 条）→ 可做无限流 | code 0 ✓ |
| `x/player/wbi/playurl` | 取流（**必须 WBI 签名**） | code 0 ✓ |
| `x/web-interface/popular` / `ranking/v2` | 首页 feed | popular 可用；**ranking/v2 复现了 `-352`** |

### 2.2 WBI 签名（必须自己实现）

```
mixin_key = (img_key + sub_key)[ 64 项置换表 ] 取前 32
w_rid = md5( 参数按 key 升序 urlencode 拼接 + mixin_key )
```

- 置换表与实测 test vector 完全一致（`ea1db124af3c7062474693fa704f4ff8`），实现已在本地验证通过。
- 密钥**每日轮换**，需缓存 + 刷新；签名错误/缺失的典型表现是 `code:0` 但返回 `v_voucher`（不是报错，容易误判）。
- QuickJS 无内置 crypto → 需**纯 JS MD5**（约 100 行，注意 `|0`/`>>>` 保持 32 位语义）。

### 2.3 取流与画质（实测）

| 路径 | 结果 |
|---|---|
| DASH（`fnval=16`，qn=64 请求） | 匿名**只给到 480p/360p 的 AVC**（id=32 852x480、id=16 640x360）；`quality:64` 字段是虚的，dash.video 里没有 720p 流 |
| html5 单文件（`platform=html5&high_quality=1`） | **单个 MP4（H.264+AAC）**，qn=32/16 → **640x360**；qn=64 → **1280x720** |
| 音频 | 30216/30232/30280，DASH 里独立 AAC 流 |
| CDN 防盗链 | DASH 链接**必须带 Referer + 浏览器 UA**，否则 403；html5 单文件路径**不校验 Referer** |
| URL 有效期 | 120 分钟（`deadline` 参数），过期需重新取流 |

实测防盗链对照（同一 URL）：笔上 `curl -e https://www.bilibili.com/` → **206**；不带头的 ffmpeg → **403**；带上 `-headers 'Referer: ...\r\n'` 的 ffmpeg → **成功**。

### 2.4 风控（本次实测踩到）

- `ranking/v2` 在短时间内被调用数次后返回 **`-352`（风控校验失败）**；`view` / `related` / `playurl` 仍正常。
- 缓解手段：`buvid3`/`buvid4`（body 可取）、`bili_ticket`（需 HMAC-SHA256，可选）、真实 UA + Referer、低请求频率、WBI 密钥缓存、`-352/-412` 指数退避。
- **具体速率阈值文档没有**，只能按保守设计（低 QPS + 结果缓存 + 失败退避）。

### 2.5 登录的坑（设计上的关键约束）

- 网页扫码登录成功后，`SESSDATA`/`bili_jct` 是**通过 `Set-Cookie` 下发**的 —— 而笔的 `jsapi.http` 和现有 native 模块**都只返回 body、不暴露响应头**。
- 可行的两条路：
  1. **TV/APP 版扫码登录**（`x/passport-tv-login/qrcode/auth_code`）——`access_token` 与 `data.cookie_info.cookies`（含 `SESSDATA`）**在 body 里**，无需读响应头；代价是需实现 appkey+appsec 签名（md5，容易）。
  2. 给自有 native 模块加 `CURLOPT_HEADERFUNCTION` 收集响应头（改自己已开源的 `httpjson.c`，成本很低）。
- 另：Cookie 刷新流程要 RSA-OAEP + SHA-256，在 QuickJS 里代价高 → **建议过期直接重扫码**，不做 refresh。

---

## 3. 真机实测数据（本次核心证据）

全部在笔上跑，输入是**真实 B 站 CDN 流**（签名 URL 在开发机生成，笔端播放；笔端自身上外网已单独验证）。

目标管线：`ffmpeg -i <bilibili url> -vf scale=452:254:flags=fast_bilinear,transpose=2,format=rgb32`（452x254 = 16:9 信箱适配 800x254 逻辑画布，transpose=2 为既有项目实测的 fb 写入方向）。

| 路径 | 分辨率 | 耗时（每 10s 内容） | 实时倍率 |
|---|---|---|---|
| html5 单文件 360p | 640x360 | 5.6–6.3 s | **1.6–1.75x** |
| html5 单文件 360p（第二个视频 BV1mauH6AEYR） | 640x360 | 6.26 s | **1.6x** |
| DASH AVC 360p（带 Referer） | 640x360 | 5.34 s | **1.87x** |
| DASH AVC 480p（带 Referer） | 852x480 | 7.27 s | **1.38x** |
| DASH 480p 视频+音频双输入混流 | 852x480 | 5s 内容 150 帧 | **1.72x** |
| DASH 音频 30232 AAC | — | 0.82 s | 12x（可忽略） |
| **html5 单文件 720p** | 1280x720 | 13.1 s | **0.76x ✗ 不可用** |

补充实测：

- **真实帧输出**：html5 360p 3 秒 → 产出 90 帧 rgb32，`41330880 B = 452×254×4×90`，正好 3.00 s / 30fps，**1.47x 实时**（含写 41MB 文件的开销，实际写 fb 更便宜）。
- **seek 可用**：`-ss 60` 在 html5 单文件（走 MP4 Range）与 DASH（带 Referer）上都成功定位并继续解码（1.53x / 1.28x）。
- 内存占用：ffmpeg 最大 RSS 约 18–28 MB，设备可用内存 175 MB+，无压力。

### 工程注意点（本次踩到的）

- `-t` 要作为**输出选项**（放在所有 `-i` 之后）；放在第一个 `-i` 之前只会截断输入 0，音频会一直被读完（表现为"卡住几分钟"）。
- 笔上**没有 `timeout` 命令**（busybox 无），长命令要用 `-t` 或外部调度控制。
- ffmpeg 要带 Referer 必须是真的 CRLF（`'\r\n'`），shell 里的 `\r\n` 字面量无效。
- URL 里含 `&`/`%`，拼到 shell 命令行必须整体单引号包裹（若走 native，**用参数数组 fork/exec，不要拼 shell 字符串**）。

---

## 4. 架构选型

| 方案 | 判定 | 理由 |
|---|---|---|
| **A. 笔端直接调 API + ffmpeg 播放（推荐）** | ✅ | 本次已端到端实测；复用 `video_player_x5` 的 fb 播放器与 `deepseek-x5` 的网络/存储适配层；无中间服务器 |
| B. 云端渲染（照搬 `x5_cloud_browser`） | ❌ | 视频要按帧传 JPEG，800x180@24fps ≈ 10–20 Mbps，且引入服务器；只适合"看网页版 B 站"，不适合播放 |
| C. 服务端转码后再喂给笔 | ⚠️ 备选 | 笔端 360p 已能实时解，无需转码；只有当风控无法在笔端绕过时才用它兜底 |
| D. WebView 内嵌 B 站 | ❌ | 设备上**不存在任何浏览器内核**（无 webkit/chromium/EGL） |

**推荐 A**，并把 C 当作"风控兜底方案"（笔端只需访问自己的服务器，HTTP 明文 + 无风控）。

---

## 5. 推荐方案（分层与复用）

```text
src/
  services/
    bili/
      wbi.js          # 纯 JS MD5 + WBI 签名 + 密钥缓存（每日刷新）
      client.js       # API 客户端：UA/Referer/Cookie、-352/-412 退避、结果缓存
      playurl.js      # 取流与画质降级链：html5-360p → DASH-360p(+audio) → 480p
      danmaku.js      # 弹幕（XML deflate / protobuf seg.so，建议走 native）
      auth.js         # 可选：TV 扫码登录，Cookie 仅存本地
    net.js            # 直接复用 deepseek-x5：jsapi.http + native httpjson 双通道
    storage.js        # 直接复用：schema 化 + 串行写 + 读回校验
    im.js             # 直接复用：系统输入法（搜索词输入）
    screen.js         # 直接复用：800x254 分区与坐标
  pages/              # 首页 / 搜索 / 详情 / 播放 / 设置
native/               # 复用 libjsapi_player.so（改为接收 URL + headers），按需扩展
libs/libjsapi_player.so
```

播放链路（笔端已验证）：

```text
首选  ffmpeg -user_agent <browser UA> -i <html5 单文件 360p URL>
      -an -vf scale=452:254:flags=fast_bilinear,transpose=2,format=rgb32 → /dev/fb0
      + 同进程/第二进程 -i <同一 URL> -vn -f s16le → aplay（该路径音频就在同一文件里）

兜底  ffmpeg -user_agent <UA> -headers 'Referer: https://www.bilibili.com/\r\n'
      -i <DASH video 360p> -i <DASH audio 30232> → 视频写 fb、音频写 aplay
```

画质选择策略：**默认 360p**；480p 作为可选（"省电/流畅"之外的"清晰"档）；≥720p 不下发（不可用）。UI 上不要暴露 720p/1080p，避免用户选了播不动。

---

## 6. 实施阶段

| 阶段 | 内容 | 风险 |
|---|---|---|
| 0. 验证（1 天内） | 在笔上用 `jsapi.http` / native 直连 `api.bilibili.com` 跑通 WBI view+playurl；确认笔端自取流能播 | 低 |
| 1. MVP（匿名） | 首页 feed（popular + related 续流）、详情页、播放页（360p + 分P + 进度记忆）、设置页 | 低-中 |
| 2. 搜索与个人化 | 搜索（`wbi/search/type`，**需 buvid3 + 高频风控**）、登录（TV 扫码）→ 收藏/历史/关注/稍后再看 | 中 |
| 3. 体验增强 | 弹幕列表 → 弹幕覆盖（native + freetype 合成到 fb）；倍速/字幕；直播（FLV/HLS，弹幕 WSS 需 native） | 中-高 |

---

## 7. 风险与未验证清单

**风险**

1. **风控（主要风险）**：`-352/-412` 阈值未知；已实测触发。缓解：buvid3/4 + bili_ticket + 真实 UA/Referer + 低 QPS + 缓存 + 退避；笔在家庭网络（住宅 IP）通常优于机房 IP。
2. **API 漂移与封堵**：上游文档仓库已 archived；B 站近年在收紧第三方客户端。**仅个人自用，不对外分发**，并接受"某天失效"的可能。
3. **扫码登录取 Cookie**：运行时读不到 `Set-Cookie` → 必须走 TV 扫码 body 路径，或给自有 native 模块加响应头采集。
4. **单核 CPU**：360p 只剩 ~1.5–1.9x 余量，播放时应保持 UI 静态（不做动画/列表预取），否则掉帧。
5. **800x254 条带 UI**：视频区 452x254，右侧 348px 放标题/简介/弹幕列表，需要专门的窄屏信息设计。
6. **弹幕覆盖**：视频由 native 直接写 fb，Vue 层盖不上去；要做覆盖需在 native 里用 freetype 合成（设备有 `libfreetype.so.6.17.4`）。

**未验证（不要当成已通过）**

- 笔端**自己**发 WBI 请求（本次 URL 在开发机生成，笔端只验证了"能拉流+解码"；笔端 curl 直连 API 成功，但 `jsapi.http`/native 直连 api.bilibili.com 尚未跑过）。
- 长视频（20 分钟以上）连续播放、弱网/断网恢复、多次进出页面的资源回收。
- 弹幕接口的实际返回（XML deflate / protobuf 体积与时延）。
- 直播取流（FLV/HLS）在笔上 ffmpeg 的可用性与画质档位。
- PGC/番剧（大会员内容基本拿不到高画质；匿名 480p 上限）。
- 触摸命中播放控件的真机手感（既有项目记录：`@click` 无坐标，需用 `touchstart/touchend` 判定点击）。

---

## 8. 参考资料

- API 收集项目：<https://github.com/xieren58/bilibili-API-collect>（含 `docs/misc/sign/wbi.md`、`docs/video/videostream_url.md`、`docs/login/login_action/QR.md`、`docs/misc/buvid3_4.md`、`docs/misc/errcode.md`、`docs/misc/sign/APPKey.md`、`docs/misc/sign/bili_ticket.md`、`docs/misc/sign/v_voucher.md`）
- 上游（已归档）：<https://github.com/SocialSisterYi/bilibili-API-collect>
- 同机既有项目：`deepseek-x5`（HTTPS/网络/输入法/存储 真机契约）、`video_player_x5`（ffmpeg→fb 播放器与性能基准）、`x5_cloud_browser`（设备能力清单）
