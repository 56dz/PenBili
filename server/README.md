# PenBili 转码服务器（直播服务端）

PenBili 的直播功能需要一台**自建转码服务器**：词典笔拿到 B 站直播源后既解不动 720p、也无法直接播 https，
所以由服务端拉流、转码，再以 **HLS 分片**回传，笔端把分片地址当"点播地址"播放。

- 源码：`live_proxy.py`（单文件，Python 标准库，无第三方依赖）
- 版本：**v5.1**（协议与笔端 `2.9.x` 配套）
- 依赖：Python 3.7+、系统 `ffmpeg`（需带 libx264）

---

## 一、如何使用

### 1. 部署

```bash
# ① 放置文件（或直接解压发布包 PenBili-server.zip）
mkdir -p /opt/penbili/server
cp live_proxy.py penbili-live.service requirements.txt /opt/penbili/server/
cd /opt/penbili/server

# ② 装 ffmpeg（只要系统级这一个依赖）
sudo apt install ffmpeg          # Debian/Ubuntu

# ③ 前台试跑（确认能起来）
python3 live_proxy.py --port 2050
# → [hls_proxy] v5.1 listening on http://0.0.0.0:2050 (ffmpeg=..., max_sessions=6, ...)
```

常驻运行用 systemd：把 `penbili-live.service` 拷到 `/etc/systemd/system/`，
改好 `User` / `WorkingDirectory` / `ExecStart` 路径后：

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now penbili-live
journalctl -u penbili-live -f      # 日志
```

命令行参数：

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--host` | `0.0.0.0` | 监听地址 |
| `--port` | `2050` | 监听端口 |
| `--max-sessions` | `6` | 并发转码会话上限，超出返回 503 |
| `--dev-src-allow` | 关 | 允许 `?src=` 注入自测源（仅供本机调试，**别在公网开**） |

### 2. 笔端配置

词典笔 →「我的 → **直播设置**」：

1. **服务器地址**：填 `http://<服务器IP>:2050`（笔端只认 **http**，不认 https）；
   不填则点直播会提示「未填写转码服务器地址，无法播放」。
2. 点**测连**：应显示版本号与负载；通过后会把 `/health` 回传的 host 缓存为
   **重定向解析结果**（应用每次启动后台探测 2s，失效自动重新解析）。
3. 按需调**画质档**与**服务端分片窗口**（见下节参数说明）。

同网段直填内网 IP 即可；人在外面则需自行打通一条 **http** 的隧道/端口映射
（Cloudflare 等强制 https 的 CDN 会 **301 并丢掉 query**，笔端播不了，须绕开）。

### 3. 验证

```bash
# 健康检查：返回 ok/version/host/lan/load
curl -s http://127.0.0.1:2050/health

# 建会话：302 → 播放列表地址（room 换成真实在播房间号）
curl -si 'http://127.0.0.1:2050/live?room=123456&res=254' | grep -i location

# 用得到的 m3u8 播放验证（本机或笔端均可）
ffplay -v error '<上一步 Location 的完整地址>'

# 也可以不用 ffplay，直接看分片是否持续生成
curl -s '<m3u8 地址>' | tail -5
```

### 4. 接口协议

```
GET /health
  → 200 {ok, version, host, lan:[...], load:{sessions,load1,high}, max_sessions}
     host = 请求的 Host 头，笔端拿它缓存"重定向解析结果"，避免每次都吃 301

GET /live?room=<房间号>[&ck=<cookie>][&res=254|360|480][&bv=700k][&trans=1|0]
              [&buf=6000][&seg=ts|fmp4]
  → 302 Location: http://<Host>/hls/<sid>/index.m3u8

GET /hls/<sid>/index.m3u8     播放列表（live，无 ENDLIST）
GET /hls/<sid>/<分片>          ts / m4s / init.mp4
```

| 参数 | 默认 | 含义 |
| --- | --- | --- |
| `room` | 必填 | B 站直播间号 |
| `ck` | 空 | 笔端登录 cookie。带上它服务端才拉得到 **720p 源**（未登录会被风控限到低清），同时按 cookie 哈希隔离多用户 |
| `res` | **254** | 转码输出**高度**。254 = 笔端视口 452×254，1:1 无缩放（详见实现思路）；360/480 仅供调试，真机会卡 |
| `bv` | `700k` | 视频码率，钳在 300k~2500k，服务端按 VBV 锁上限 |
| `trans` | `1` | `1` 转码（默认）；`0` 直通 `-c copy`，零编码开销，但分片时长随源关键帧走 |
| `buf` | `6000` | **服务端播放列表窗口**（ms，2000~20000）：保留几个分片给笔端落后时取回的余量。**不影响起播时间** |
| `seg` | `ts` | 分片类型，`ts` 最稳；`fmp4` 亦可 |

### 5. 运行特性

- **会话复用**：`sid = sha1(room|ck|res|bv|trans|seg)[:12]`，同参数重入复用同一会话 →
  断线重连 / 退出再进 = **秒开**（不重起转码）。
- **回收**：25s 无任何列表/分片请求 → 停 ffmpeg、删目录；同 cookie 换房间 → 旧会话让位。
- **重试**：ffmpeg 因源站断流/直链过期退出时自动重取新地址重开（单会话上限 30 次）。
- **限载**：会话数 ≥ `--max-sessions` → 503；`/health` 的 `load.high` 可被上游网关用于摘流。

### 6. 常见问题

| 现象 | 原因与处理 |
| --- | --- |
| 启动即退 `FATAL: ffmpeg not found` | PATH 里没有 ffmpeg，装好再起 |
| `/live` 返回 503 | 并发会话满，等 25s 空闲回收或调大 `--max-sessions` |
| 播放列表 504 `playlist timeout` | 上游拉流失败（房间已下播 / cookie 失效 / 网络），看日志里 `playurl fail` |
| 分片 404 `segment gone` | 客户端落后太多，分片已滑出窗口 → 调大笔端「服务端分片窗口」 |
| 笔端提示「未填写转码服务器地址」 | 直播设置里没填服务器地址 |
| 笔端填了地址仍播不了 | 地址不是 **http**（https 笔端不支持），或中间有 301/HTTPS 强制跳转，需直连 http 端口 |
| 输出卡顿 | 确认 `res=254`；360/480 在词典笔上解不动（见下） |

---

## 二、实现思路

### 1. 为什么是 HLS，而不是一条长连接（v4 → v5）

早期版本把转码结果拼成一条 **chunked FLV 长连接**推给笔端，于是两端强耦合：
服务端要"蓄水"控制起播、首块必须凑齐媒体 tag、换线要剥新流的 FLV 头；
笔端要墙钟节流、丢音频快进、按字节算缓冲……每一处都是真机踩坑换来的补丁。

v5 改成 **HLS**：服务端只负责"把直播切成 1 秒一片的 m3u8 + 分片"，
**笔端用 ffmpeg 自带的 hls demuxer，把它当普通点播地址播**：

- 笔端点播代码**几乎零改动**（拉列表、取分片、维持窗口全由 ffmpeg 内部完成）；
- 断线/重启天然容忍——播放列表被重写即可，不存在"流中途必须无缝续接"的问题；
- 分片是无状态文件，多客户端、并发、重试都好处理。

### 2. 会话模型

```
/live?room&ck&res&bv&trans&seg
        │
        ├─ sid = sha1(全部参数)[:12]  ── 命中已有会话 → 直接 302（秒开）
        ├─ 会话满 → 503
        └─ 新会话 → 起 1 个 ffmpeg 线程
                     │  拉 getRoomPlayInfo（带 cookie 解锁 720p，只收 h264 线路）
                     │  → 转码 → 写 hls_cache/<sid>/index.m3u8 + seg*.ts
                     └─ ffmpeg 退出 → 重取新源地址重开（CDN 直链会过期）
```

- **按 cookie 哈希隔离用户**：多用户并行互不干扰；同 cookie 换房间时旧会话让位（半开僵尸连接清理）。
- **janitor 线程**每 5s 扫一遍，超过 25s 无请求即回收，进程退出后的残留目录下次启动时清掉。
- 播放列表请求在首片生成前会**阻塞等待**（最长 12s），让笔端一次拿到有效列表而不是 404。

### 3. 转码参数的取舍（每条都是真机实测结论）

| 参数 | 取值 | 原因 |
| --- | --- | --- |
| `-probesize` | 256KB / 1s | 太小约 1/3 概率探不到音频流 → 整段无声 |
| 视频码率 | `-b:v` + `-maxrate` + `-bufsize 2×` | `-b:v` 只是平均目标，高运动画面实测超发 2 倍 → 必须 VBV 锁上限 |
| 音频 | **只给 `-b:a`，绝不加裸 `-maxrate`** | 无配套 `-bufsize` 时 ffmpeg 会**静默丢弃整条音频流**（不报错） |
| `-preset` | `veryfast`（非 `ultrafast`） | ultrafast 在低码率下块效应极重（"马赛克"） |
| 滤镜 | 不加 `-skip_loop_filter` | 跳去块滤波会把源的块效应带进重编码 |
| GOP | `-g 30 -keyint_min 30 -sc_threshold 0` | 与 1s 分片对齐 → 每片恰好 1 个关键帧，时长稳定、可独立解码 |
| 分片 | `-hls_time 1` + `delete_segments` + `omit_endlist` + `epoch` 分片号 | 1s 片低延迟；分片号唯一 → 重启后客户端不会命中旧缓存 |
| 源选择 | 只收 h264/avc，FLV 优先、m3u8 末选 | HEVC 线路笔端解码启动实测失败 |

### 4. 输出高度为什么默认 254

词典笔的视频视口就是 **452×254**（800×254 逻辑屏扣掉两侧 174 宽的栏）。
真机实测（同码率 700k 的 5 秒真实直播分片，跑笔端播放器同款
`scale→transpose→rgb32` 链）：

| 服务端输出 | 笔端解码吞吐 |
| --- | --- |
| 854×480 | **0.83x**（跌破实时 → 卡成 ~4fps） |
| 640×360 | 1.03x（零余量） |
| **452×254** | **2.3~2.8x**（充足） |

关键在于：**输出尺寸 == 视口尺寸时，sws_scale 走恒等快路径被跳过**，不只是像素少。
出更高分辨率只会被笔端再缩回去，纯烧 CPU。所以服务端按 `-vf scale=-2:254` 输出
（16:9 源正好 452×254），`res=360/480` 仅保留作调试档。

### 5. 笔端与服务端的分工

| | 服务端 | 笔端 |
| --- | --- | --- |
| 拉源 / 转码 | ✅ 带 cookie 拉 720p，ffmpeg 转 254p | — |
| 切片 | ✅ 1s 分片 + 滑动窗口 | — |
| 播放 | 只发文件 | ffmpeg hls demuxer 当点播播，`durationMs=0`（不判播完、不提供定位） |
| 音画同步 | 分片内音视频 PTS 对齐 | 墙钟节拍 + 自适应对齐伺服 |
| 地址解析 | `/health` 回传 host | 重定向解析缓存，启动 2s 探测、失效重解析 |

### 6. 安全说明

- **cookie 只保留可打印 ASCII**（防 HTTP header 注入），上限 600 字符。
- 分片/目录名**白名单正则校验**，杜绝路径穿越。
- 上游 URL **白名单**只放行 B 站 CDN 域名（防 SSRF / 开放代理）；`?src=` 注入默认关闭。
- 服务端**无鉴权**，`/live` 带 cookie 时 cookie 会出现在 URL 里 ——
  **请部署在内网或自建加密隧道后面，不要把 2050 端口直接暴露到公网**。
