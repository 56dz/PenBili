# Youdao Dictionary Pen X5 设备 profile（bilibili_x5）

profile_id: youdao-x5-fw3.4.6

本文件是本项目**唯一**的机型/固件事实来源。设计宽高、旋转、播放管线、native 目标 ABI
都只从这里取值；换型号或固件必须重新探测并新增 profile，不要改这里的默认值去覆盖别的设备。
（与 video_player_x5 同机型同固件，故沿用其真机探测结论；本项目自己的验证证据追加在文末。）

```yaml
profile_id: youdao-x5-fw3.4.6
model: "有道词典笔 X5 (YDPX5-1, codename Cherry / Cvitek CV1826)"
firmware: "appVersion=3.4.6, kernel Linux 4.19.164-tag--g23c8a94c91bf, Buildroot 2021.05-rc3"
runtime:
  falcon: "miniapp (/usr/bin/miniapp + libfalcon.so 6.3MB), appVersion 3.4.6"
  quickjs: "manifest quickjs.version=20200705, bigNum=false"
  api: "$falcon.App / $falcon.Page / $falcon.navTo(page, options) / $falcon.useDefaultBasePageClass"
  jsapi: "http / sp(storage) / modal / nvue / service / ams / updater / pm / global(输入法)"
abi:
  machine: "armv7l (Cvitek cv182x, Cv1826), 单核 Cortex-A53 r0p4, NEON"
  bits: 32
  libc: "glibc 2.23 (/lib/libc.so.6 符号版本到 GLIBC_2.23)"
  toolchain: "zig 0.16 cc -target arm-linux-gnueabihf.2.23（env/pyzig），纯 C，无 libstdc++ 依赖"
screen:
  logical: { width: 800, height: 254 }        # Falcon UI 合成面（miniapp_cli capture 实测）
  physical: { width: 254, height: 800, direction: 270, xoffset: 0, yoffset: 0 }
  touch: { direction: 270, xoffset: 113, yoffset: 0 }
  fb: "/dev/fb0 (cvifb) bpp=32 stride=1024B(256px) virtual=256x1600 双缓冲 pan 切换；rotate=0"
  design_width: 800                            # setViewPort(800)
  logical_to_physical: "px = ly, py = 799 - lx（由 150x40 按钮变成物理 40x150 且按逻辑 y 从左到右排列实测得出）"
  video_transpose: 2                           # 逻辑画布按 90° 逆时针写入 fb，故视频须 transpose=2
input_method:
  version: "有道输入法 appid=8001666679481944 (SystemApp)"
  global_text_edit: "可用（startTextEdit 同步返回 UUID）"
  textarea_soft_input: untested
media:
  gstreamer: "无（/usr/lib/libgst* 不存在，无 gst-launch-1.0）"
  drm_kms: "无 /dev/dri；只有 fbdev，故 kmssink 方案不可用"
  hw_decode: "无硬件视频解码器：/dev 下只有 vip-isp/dwa/img/sc/disp，无 vcodec/vdec 节点；libvpu.so 是 VI/VO/VPSS 中间件"
  decoder: "/usr/bin/ffmpeg 4.4（软解 h264/hevc/vp8/vp9/mjpeg）+ /usr/bin/ffprobe"
  ffmpeg_build: "--disable-outdevs --disable-indevs --disable-libdrm --disable-libv2l2 --enable-libfreetype --enable-openssl --disable-libx264"
  audio: "ALSA 4 张卡（cv182x dac/adc）；/usr/bin/aplay 可用；无 ffmpeg alsa 输出设备"
  render: "native 直接写 /dev/fb0 当前 pan 缓冲（mmap），音频走 aplay stdin"
package:
  appid: "8002026091900004"
  version: "1.0.0"
  icon: "app_icon.png (192x192, 由 tools/make_icon.js 生成)"
  app_name: "B站验证"
  start_page: "index"
  install_dir: "/userdisk/miniapp/data/mini_app/pkg/<appid>/<slot a|b>/"
  storage: "/userdisk/miniapp/data/mini_app/pkg/<appid>/data/sharedpreferences/preferences.json"
  native_lib: "AMR 内 libs/libjsapi_player.so（本项目构建=上游 player.c + open() 尾部两个可选参 userAgent/referer，PLAYER_VERSION 1.1.0）→ 装机后成为 <slot>/libs/libjsapi_player_<hash>.so"
  log: "/userdata/applog/DictPen_*.log（只落 console.warn/error）"
validation_upstream_video_player:
  # ↓ 同机型同固件、上游 video_player_x5 项目的真机证据（可直接复用的平台事实）；
  #   本项目（bilibili_x5）自己的安装/播放证据见文末 “## 本项目验证证据”。
  tested_at: "2026-09-19"
  amr: "8002026091900002.1_0_0.amr（含 libs/libjsapi_player.so，md5 见 .falcon_/manifest.json）"
  evidence:
    - "adb devices -l => BCC06********40（序列号已脱敏；伪装 Nexus 4/mako）；miniapp_cli install => {\"appid\":\"8002026091900002\",\"ret\":0}"
    - "装机后 native 库被展平重命名：<slot>/libs/libjsapi_player_<hash>.so；日志出现 [player] native 模块: 可用"
    - "证据 profiles/evidence/final_library.png：目录 3 部/30 集 + 三张封面（运行时 ImageLoader 直接拉 http 图片）"
    - "证据 profiles/evidence/run6_series.png：选集页 12 话网格 + '看到 第3话 5:00 / 共12话 · 已看3话'（进度追踪真实生效）"
    - "证据 profiles/evidence/run5_play_slime_e01_fb.png / run1_player_fb.png：captureFB 原始缓冲可见视频已贴入 fb 中间矩形（物理 y=174..626）"
    - "方向核对：tools/make_test_pattern.js 生成四角标测试片 → 设备 ffmpeg 编码 → 播放 → captureFB 裁切，transpose=1 时角标为(白/红/蓝/绿)，transpose=2 时为(绿/蓝/红/白)，与 '逻辑画布 90° 逆时针写入 fb' 的推导一致（证据 orient_test_t1.png / orient_test_t2.png / zoom_orient_t*.png）"
    - "自检 vp_autotest：pause => {state:playing→paused, positionMs:2333 冻结, frames:56 冻结}；resume => 继续；seek 300000 => {positionMs:300000, frames:0}"
    - "带音轨样片（mpeg4+aac，设备端生成）：status 报告 hasAudio=true, audioEnabled=true, audioBytes 143360→266240→356352 递增, audioDropped=0, 且 24.0 fps 实时"
    - "持续播放速率：两次 status 相隔 700ms => frames 18→33 / 222→239，均 ≈24fps；起播延迟约 2.8s"
    - "资源回收：从播放页返回动漫库后 ps 中 ffmpeg/aplay 计数 = 0"
    - "错误路径：目录中不存在的 episode（slime-e13）=> 页面显示『播放失败：找不到要播放的剧集』；文件缺失 => 显示 ffmpeg 的 404 错误文本"
    - "运行时行为取证：HttpApiExtension::downloadFile fail: savePath is empty!（须用 savePath）；<image src> 绝对路径被拼成 <approots>/<绝对路径> 而失败"
  unverified:
    - "触摸命中：设备无 sendevent/getevent/input（只有只读 evtest），按钮/画面的实际点击未做真机注入测试"
    - "扬声器是否真的出声：aplay 无错误地消费完整音轨，但未用耳朵确认"
    - "蓝牙音频路由：已实现 bluealsa-aplay -L 自动探测 + -D bluealsa:DEV=...,PROFILE=a2dp，但缺少蓝牙耳机实测"
    - "下拉面板浮层的自动暂停：本固件不向应用暴露任何信号（见上文），只能手动先暂停"
    - "20 分钟以上长片连续播放、弱网/断网恢复、多次进出页面的内存增量"
```

## 视频管线（真机确定）

```text
ffmpeg -nostdin -hide_banner -loglevel error -sws_flags fast_bilinear -ss <start>
       -i <url|path>
       -map 0:v:0 -vf scale=<logicalW>:<logicalH>,transpose=2,format=rgb32 -r <fps> -f rawvideo pipe:3
       -map 0:a:0? -ac 2 -ar 44100 -f s16le pipe:4
aplay -q [-D <device>] -t raw -f S16_LE -r 44100 -c 2 -
```

- 视频线程按帧读 `pipe:3`，写入 `/dev/fb0` 当前缓冲的物理矩形；音频线程把 `pipe:4` 阻塞写入 aplay
  （用音频时钟给整条管线限速）。
- **无音轨的容器**：`-map 0:a:0?` 会让第二个输出没有流，ffmpeg 整体失败 →
  播放器在第 0 帧失败后自动去掉音频输出、改为「仅视频 + 时钟节拍」重启一次（真机实测生效）。

## 性能基准（真机，单核，10 秒样本 / 300 帧，单位=秒）

| 管线 | 640x360@30 h264 | 1280x720@30 h264 |
|---|---|---|
| 纯解码 → null | 3.58（2.8x 实时） | 11.11（0.9x，吃满单核） |
| 解码 + 转 rgb32（不缩放） | 5.18（1.9x） | — |
| 解码 + scale=800:254 rgb32（bicubic） | 14.01（0.71x，不可用） | 27.55 |
| 同上 fast_bilinear | 8.98（1.11x，无余量） | — |
| MJPEG 640x360 纯解码 | 2.30（4.3x） | — |

结论：**360p 可用、720p 不可用**；`sws_scale` 是主要瓶颈，因此实际播放把画面缩到
本机可视的 452x254（16:9 信箱适配 800x254 的 UI）再交给 fb，配合 `fast_bilinear`。

## 运行时行为（本项目实测，与实现强相关）

- `<image src="http://...">` 由运行时 ImageLoader 自行下载并缓存
  （日志 `[ImageLoader] begin download url` / `succeeded, local file: %s`）。
- `<image src>` 的非 http 路径会被当作**相对应用根目录**的路径拼接：传绝对路径会变成
  `<approot>/<绝对路径>` 而加载失败（实测）。
- `jsapi.http.downloadFile` 的落盘根是 `/userdisk/miniapp/downloaded/`
  （libfalcon 内 `s_downloadedDir`），且参数名必须是 **savePath**，只传 filePath 会报
  `HttpApiExtension::downloadFile fail: savePath is empty!`。
- `miniapp_cli start <appid> <page>`：同一页面再次 start 不会重建页面实例（不会重读 onLoad options）；
  自动化验证需要先 start 另一个页面再 start 目标页。
- 设备没有 `sendevent`/`getevent`/`input`（只有只读的 evtest），无法注入触摸，
  因此播放控制（暂停/seek）通过 storage 开关 `vp_autotest` 驱动的自检钩子验证。
- **HTTP 重定向**：运行时 http 客户端与 ffmpeg **都跟随 301/302**，
  且 ffmpeg 在重定向之后 Range 仍有效（`ffprobe` 元数据一致、`ffmpeg -ss` 定位成功）。
  实测把重定向目标从 `:8080` 热改到 `:8081`，笔端配置不变，目录与视频立刻由新端口提供。
  → 「外网端口会变」的场景只需给笔端一个稳定的 302 入口（`tools/redirector.js`）。
- **JSAPI 表面（`$falcon` 自省实测）**：`$falcon.jsapi` 只有 **http / modal / storage**；
  `$falcon` 自身另挂 `$getTopApp / $appid / $workspace / $dataDir / navTo / closeApp /
  closePageByName / broadcast / $app / env / eventMap / theme`。
- **`<hole>` 标签可用**：`<hole class="video-hole">` 被 Vue 预编译器与运行时接受，
  视频区域不参与框架画布绘制。实测 20 次原始 fb 采样 0 黑帧、20/20 正常画面，
  且播放中每秒刷新进度（= 确实发生了整页重绘）也不闪 → 证明 hole 生效。
- **fb 双缓冲要两块都写**：框架 pan 切换缓冲，只写当前显示块会在切换瞬间露出另一块的旧帧
  （表现为"闪上一帧"）。`/sys/class/graphics/fb0/modes` 给可见高度、`virtual_size` 给总高度，
  两者相除得缓冲块数（本机 2）。
- **音频设备**：内置扬声器是 ALSA 默认设备；蓝牙耳机必须显式指定 bluealsa 的 PCM
  （`/etc/alsa/conf.d/20-bluealsa.conf` 定义 `pcm.bluealsa`，且是 `type plug`，所以能自动做采样率转换）。
  `bluealsa-aplay -L` 在**已连接**耳机时输出（真机实测，注意第一行的完整格式）：
  ```
  bluealsa:SRV=org.bluealsa,DEV=B0:A3:F2:AD:7E:FA,PROFILE=a2dp
      Redmi Buds 6, trusted audio-card, playback
      A2DP (SBC): S16_LE 2 channels 48000 Hz
  ```
  **坑**：解析时若匹配 `bluealsa:DEV=` 会永远匹配不到（DEV 前面还有 `SRV=org.bluealsa,`），
  必须匹配 `bluealsa:` 再取到空白符为止。未连接耳机时该命令无输出（rc=0）。
  实测 `aplay -D "bluealsa:SRV=...,DEV=...,PROFILE=a2dp" -t raw -f S16_LE -r 44100 -c 2 -`
  可直接打开耳机 sink 播放（rc=0；耳机本职是 48kHz，由 plug 转换）。
  系统自身播放走专有二进制 `/oem/YoudaoDictPen/output/SoundPlayer`，不走 aplay。
- **顶层应用检测**：`$falcon.$getTopApp()` 可用，但只在**真实应用切换**时变化
  （实测切到桌面返回 `8080222437664451`，回到本应用恢复自己 appid）。
  `$falcon.env` 只有 apiVersion/custom/deviceModel/platform/version，没有 topAppId。
- **`topAppChanged` 事件**：`$falcon.eventMap` 里确实注册了（另有 `system_env_custom_theme`），
  但 `$falcon.on('topAppChanged')` 与 `$falcon.$app.on(...)` 注册成功后**回调从不触发**
  （桌面真实切换时也不触发）；`$falcon.broadcast` 是函数不是事件总线。
- **下拉面板（appid 8080222501178405，Category=TOP_PANEL）是浮层**：
  切到它时框架日志为 `isTopChange=false`，不触发本页 `onHide`、不改变 `$getTopApp()`、
  也没有 `$getTopPanelShowing` 之类接口 —— 应用侧**无法感知面板弹出**；
  要避免原生视频盖住面板，只能由用户先暂停播放。
  （另外：`onHide` 里若调 `redraw()` 会把最后一帧贴回面板之上，这是"面板被挡住"的直接原因之一。）

## 网络与 B 站 CDN（bilibili_x5 真机探测）

- **设备无 CA 证书库**：笔端 `curl https://api.bilibili.com/...` →
  `curl: (60) SSL certificate problem: unable to get local issuer certificate`（须 `-k`）。
  运行时 jsapi http 与 ffmpeg **不校验证书**（ffmpeg `tls_verify` 默认 0），本项目不依赖 CA。
- **B 站 CDN UA 风控（关键）**：html5 单文件流 URL 用 ffmpeg **默认 UA（`Lavf/…`）** 拉取 →
  `HTTP error 403 Forbidden`；换浏览器 UA → `206 Partial Content` 正常。
  → `player.open()` 必须能给 ffmpeg 传 `-user_agent`（本项目对 player.c 的唯一 native 扩展）。
- DASH（fnval=4）流地址额外需要 `Referer: https://www.bilibili.com`，否则同样 403；
  html5（platform=html5&fnval=1）不做 referer 校验，只卡 UA —— 故本项目主路径走 html5 单文件。
- 匿名质量上限：DASH 最高 480p（id=32）；html5 `qn=32/16` → 单文件 640x360 AVC+AAC；
  `qn=64` → 1280x720（本机软解 0.76x 实时，**禁止提供**）。
- 流地址有效期约 120 分钟（`deadline`）；日志/取证中只允许记录 host+路径长度，禁止落完整签名 URL。

## 本项目验证证据（bilibili_x5，2026-09-22）

产物：
- AMR `8002026091900004.1_0_0.amr`（315856 B，SHA-256 `B652D7DADBB48E7F2ED964001DDB45AFA3C8DA29FD5F2C57F9F373B2F41E7758`），由 aiot-vue-cli@1.0.32 `-q -p` 打包
- native `libs/libjsapi_player.so` v1.1.0（上游 player.c + `open()` 尾部 `userAgent/referer` 两参）：
  ARM EABI5 hard-float、SONAME 正确、NEEDED 仅 libquickjs/libc/libpthread、`custom_init_jsapis` 已导出、GLIBC ≤2.17
- 安装：`miniapp_cli install` → `{"ret":0}`，slot 内展平为 `a/libs/libjsapi_player_1274419507.so`
- 单测：`node test/run.js` 34/34（MD5 对拍 node crypto + WBI 官方向量 + 四段错误语义 + 几何 + probe 成功/失败/取消）

真机全链路步骤日志（`console.warn` 逐步落盘，运行窗口零 `console.error`）：
```text
step net     ok | jsapi.http ✓ 0.0s                ← 笔端 jsapi 带 header 直连 api.bilibili.com（最大未知项关闭）
step wbi     ok | 密钥32位✓ code=-101 304ms         ← nav 匿名返回 wbi_img；纯 JS MD5+WBI 签名在笔端成立
step finger  ok | buvid3 46字符✓                    ← x/frontend/finger/spi 触点写入 bvp_session
step view    ok | cid=42027451985 206s
step playurl ok | qn16 640x360 0.2MB upos-lcdn-cqgl01.solseed.cn 492c
             （qn=32 请求本身成功、服务器匿名降级返回 quality=16；日志无“尝试失败”行）
step native  ok | player.so ✓                       ← 新 .so 经安装/加载链可用
step play    ok | playing out=254x452 30fps          ← ffmpeg -user_agent/-headers 扩展生效（Lavf 默认 UA 此前 403）
step watch   ok | 帧=133 位=4.4s ✓ 播放保持          ← 5s 回读帧数持续增长=真实解码渲染
PASS qn=16 frames=133 host=upos-lcdn-cqgl01.solseed.cn
```

UI/几何像素证据（当前模型不支持读图，改用脚本解码 PNG 统计）：
- **样式修复前反面证据**：capture 800x254 全黑（nonblack=0.0%，1436 B 纯黑 PNG），
  而 captureFB 非黑 56.1% ≈ 恰好视频矩形占比（2×452×254/256×1600），
  memoryApp 组件树存在（Elm=46）→ 树在、样式不在。
- **样式修复后**：`stageA_20260922-233916.png`（3s 未起播）/ `stageB_20260922-233916.png`（14s 播放中）
  nonblack=43.5% ≈ 348/800 面板宽、bright 2.4~2.9%（文字笔画）；fb nonblack=77.6% ≈ 视频 56% + 面板 21.9%。
  → UI 面板与视频同帧共存，逻辑 {0,0,452,254} ↔ 物理 {0,174,254,452} 变换与 profile 一致。

生命周期：
- 两次「切桌面 → 再启动 index」循环后 `ps` 中 `ffmpeg=0`、`aplay=0`（零残留）；
  onHide / onUnload 与手动停止共用同一条 `gen++ → stop → release` 路径。

本项目踩坑（构建链，均为真机/产物实测）：
1. **`.vue` 必须自带 `<style lang="less" scoped>` 块**：无 style 块时样式规则完全不进
   bundle（`app.js.bin` 里无 `#0e1116`/`348px`）→ 页面全黑；`app.json` 的 `lessPaths`
   只是 `@import` 搜索路径，**不是全局注入**。
2. 打包器把样式存成**无点号紧凑结构**（`g-ok`/`#3ea6ff`/`348px` 字面量），产物断言
   不要按 CSS 文本（`.g-ok`）形态去 grep。
3. aiot-vue-cli 的 qjsc 步骤需 spawn 子进程（工作区沙箱下 EPERM，需放行）。
4. 含中文的 `.ps1` 被 PowerShell 5.1 执行时必须存成 **UTF-8 with BOM**，否则按 GBK
   解析导致引号错乱。

未验证项（本项目边界）：
- 触摸命中：设备无 `sendevent`/`input`，按钮未做真机点击注入（页面均走 @click 真实路径 + 自检复用同代码路径）
- ~~蓝牙耳机出声~~（已多轮人耳验证：v1.6.2 音频独立进程 + 3.1s 抗断环，用户确认"声音彻底修复"）
- ~~系统输入法拉起~~（搜索关键词与写评论均经 global.startTextEdit 真机人工验证）
- ~~DASH 双输入/分离流~~（已实现：v1.4.0 双 -i → v1.6.0 音频独立进程，见下音频收档）
- **带 hole 播放时"简介并入滚动区"必闪**（2026-09-24 真机二分：简介固定=滑评论不闪 ✓；
  简介进 scroller=必闪；scroller/hole 显式透明背景无效）→ 已回退"简介固定+仅评论滚动"稳定结构；
  **"简介随动"= 已知未满足项**——恢复需攻 falcon 大滚动区合成路径（滚动期间 hole 区帧补偿）。
  判据：本 profile 早证"每秒整页重绘 hole 不闪"（hole 合成本身无缺陷）。
- scroller 滚动事件形态（`@scroll` 未验证）→「滑到底自动加载」未实现，以"刷新/加载更多"按钮替代
- 长视频（>5 分钟）、-412 封禁后的退避与恢复（2.4h 长片已多轮播放验证，"物理边界"见音频收档）
- qn≥64（720p）不提供：本机软解 0.76x 实时；DASH 阶梯锁 id16(640x360)，dash id32=480p 已实测击穿实时

## 2.1/2.2 版本收档（扫码登录 + 四修复 + 搜索，2026-09-23）

**扫码登录（TV 变体，登录信息持久化 ✓）**
- web 变体判死（tools/qr_login_diag.js 取证）：poll 成功 → `crossDomain?ticket=…`，
  **cookie 只在三跳 302 的 Set-Cookie 响应头**（jsapi 读不到）。
- TV 变体成功链：`POST passport.../passport-tv-login/qrcode/{auth_code,poll}` + APP sign
  （=md5(按key排序 k=v& + APPSEC)，与文档向量 `e134154ed6add881d28fbdf68653cd9c` 逐字符一致；
  GET=405 必须 POST → **form 原样 body 走 native `libjsapi_httpjson.so`**（zig 自编，双 so 入包）。
  状态机（外层 code）：0 成功 / 86039 未扫 / 86090 已扫未确认 / 86038 失效 / -3 签名错误。
- 成功态 `data.cookie_info.cookies[]` 明文含 SESSDATA(222)/bili_jct/DedeUserID/ckMd5/sid+expires。
- 跟修：`g.key→g.authCode` 字段错（undefined→poll 提交"无效码"→86038，与假码行为吻合）。
- 持久化实证：`session v2`（normalizeLogin 过滤）落 storage；**`session restored mid=1015677450 uname=56dz` 跨冷启动复现 ×2**。
- 日志卫生全链：不输出 cookie/token/二维码 url/auth_code（仅长度与尺寸）。

**四修复（用户实测报障 → 全部修复并过自检）**
1. 竖屏拉伸：播放链改**无条件 view** 取 `dimension{width,height,rotate}`（rotate 90/270 互换=autorotate 后显示向；
   无 dimension 兜 pages[0]，再兜 16:9）→ playVideoRects 真实比例信箱。
   真机实证：`open ok out=254x117`（竖柱）、`out=254x339`（第三比例）、`out=254x452`（横屏）同版本并存。
2. mine 操作行按钮化（card 底/圆角/按压态）；头像 `face` https→http 归一（无 CA 库）。
3. 推荐"下拉到底不加载"→ **按钮方案**：底部（刷新｜加载更多）双按钮；刷新=清缓存重拉
   （匿名 rcmd 同参 3 连拉全同、refresh_type/pn 无效——实测，登录态轮换待观察）；
   加载更多=**热门续底**（实测 `rcmd+hot 续底 +12 total=32`），热门页内翻页保留。
4. 直播 tab → **视频搜索**：`wbi/search/type`（WBI 签名复用基建，匿名 200）；
   结果剥 `<em>` 标签、`duration:"59:39"` 解析、`roomid>0` 滤直播卡、**无 cid 靠无条件 view 补齐**；
   关键词输入走 `global.startTextEdit`（deepseek 验证版封装 services/input.js，90s 看门狗+uuid 双通道）；
   自检实证 `AUTOTEST search n=17`。直播移除技术留档：唯一 avc 档 720p 软解 0.76x 不实时。

**自检最终形态（多轮 PASS 可重复）**
`AUTOTEST PASS frames=N hot=32 search=17 hist=… qr=ok`（含首帧轮询/seek恢复轮询/列表重试/QR 首轮 waiting）；
首轮帧延迟实测 1.0~5.5s（网络敏感，固定 sleep 误杀已全部改轮询 ≤8s）。

**产物与打包核对**
- AMR `8002026091900004.2_2_0.amr`（611.6KB，SHA-256 `0C069642498D915D5E47FFF4AB7FBBFA0271E0AF775FC821F9AE9D8BF71D1C2D`）
- 解包 9 文件：boot/index/diag/player chunk + icon + manifest + **双 so**（905KB 自编 httpjson + 940KB player）；
  无旧残留、无孤儿 chunk；manifest appid/version=2.2.0/icon ✓。
- 生产包（aiot-vue-cli -p）**含字面量混淆**：业务字符串/数字在 bundle 中变换
  （CJK/状态码搜不到但运行时日志原样输出）→ **产物断言只用结构标识符，功能以运行时日志为准**。
- 测试：`test/run.js` 74 项 + `test/qr_login_test.js` 21 项（jsQR 解码级 QR 金标准、appSign 文档向量、
  search 全链、竖屏几何、session v2、cookie 拼装）全绿。

## 蓝牙音频卡顿排查全史（2026-09-23/24，用户报障 → 架构重写 + 四层修复）

**症状演进与版本-参数表**（每档均真机人耳验证 + 计数器取证）：

| 档 | 配置 | 实测听感/证据 |
|---|---|---|
| v1 | 44100 无缓冲直写 | 轻微卡 + 左右单边（用户当时以为耳机没电） |
| v2 | 48000(BT) + `-B400` | 轻微卡；单边/无粗 |
| v3 | + `-B800` + `aresample=async` | **恶化**：往复断续；出现"声音变粗"（async 变速重采样） |
| v4 | `-B200/F25` 小缓冲 | 卡（async 未除，判断被污染） |
| v5 | 回 v2 + 删 async | 单边、变粗**根除**；残余闪断 |
| v1.3.0 | **音频推送层完全重写**（feeder 入环 + writer 整形 + 预蓄） | 闪断大减、单边/粗保持消失（用户确认） |
| v1.3.1 | 水位滞回 400/900 | 数据面归零计数；残余=网络供给穿透 |
| **v1.4.0** | **DASH 双流分离** + 快域选流 + `DASH_QN_LADDER=[16]`(360p) + rw_timeout | 自检首帧 1.5s、underrun 归零、PASS×N |
| 1.6s档 | 滞回 250/1200 + `-B600` | "一直卡"→"零星卡" |
| **v1.5.0** | 环 512KB + 滞回 250/2000(≈3.1s 防御) + FIFO 音频线程 + aplay nice-10 + ffmpeg `-threads 3` + pipe 1MB/256KB | **待真机**（本轮设备离线，代码/构建/测试已完成） |

**关键判据与取证手段（复现此问题时按序使用）**：
1. **白噪对照**：`ffmpeg -i /usr/share/sounds/alsa/Noise.wav -f s16le - | aplay -D bluealsa:...`
   本地连续源稳定 → **链路层清白**，责任在 app 网络供给/解码侧（决定性一锤）。
2. **连通矩阵**：`curl -k -m 8 -w t= https://<cdn-host>/` —— 实测
   `bilivideo.com:443≈0.3s` vs `mcdn.bilivideo.cn:8082 / *.edge.mountaintoys.cn:4483≈5-6s`
   （**8082/4483 端口到本设备被严重拖慢**——快域选流的依据；mountaintoys 不在 ALLOWED_HOST_SUFFIXES）。
3. **逐秒 tick 五计数**：`ab`(音频字节增速，实时=192KB/s@48k16b2ch) / `ad`(写错误丢弃) /
   `u`(滞回 hold+空环轮) / `w`(aplay 写失败) / `rd`(环满丢弃) + `frames/pos` 增速（实时=30/1000ms）。
   - 全线统一 70% 滑行 → 解码击穿实时（→qn语义坑）；ab 滞后他项正常 → 网络供给；u 暴涨 → 滞回震荡。
4. **aplay `-q` 摘除后** `/tmp/vp_aplay.log`：`underrun!!! Nms` + bluealsa `IO thread paused/resumed`
   —— 650-1200ms underrun 与"环400+alsa426=826ms 防御上限"逐个吻合（当时）。
5. **ffmpeg 现场取证**：`/proc/<pid>/cmdline`（-i 双输入/参数是否真到 spawn）、
   `fdinfo/3,4 pos`（输出是否在写）、`wchan`（poll=等IO非死锁）；
   **注意 spawn 用 O_TRUNC 覆盖 `/tmp/vp_player.log`——必须在失败当场读 stderr**。

**三个结构性真凶（全部已修）**：
1. **单流混载码率贴带宽**（646kbps WiFi + 稿件 300-500k → 音频供给被拖断 = "按稿件分化"）→
   **DASH 分离**：音频轨恒 66kbps 独占下载（带宽 1/10），视频轨大就大（画面可顿声音稳）。
2. **dash qn 语义 ≠ html5 qn**：dash `id32=852x480(480p)` 而 html5 `qn32=640x360` ——误取 480p 让
   A53 全线 0.7x 实时（pos/ab/frames 统一 70%、underrun 1.4s 循环=「一直卡」）→ **`DASH_QN_LADDER=[16]`**。
3. **服务端 main URL 分配烂端口节点**（上表 5-6s）→ 双输入探测 12s+ 零帧零 stderr →
   **选 URL 打分**（backupUrl 里挑 `bilivideo.com` 快域、非白名单淘汰）+ `-rw_timeout 15s` 兜底。
4. 次级坑：**`platform=html5` 与 `fnval=16` 互斥**（服务端见 html5 强制降级 durl，真机回退日志坐实）；
   **HEVC 软解≈解不动** → 选流 `avc1` 优先；`async=async` 变速=声音变粗+单边（v3 时间线）。

**当前音频架构（v1.5.0 终态）**：
```
DASH 双输入(fast域 443, 640x360 avc1 + 66k audio) → ffmpeg(-threads 3, probesize 64K, rw_timeout 15s)
  ├ video pipe 1MB → video_thread blit /dev/fb0（音画时钟跟音频）
  └ audio pipe 256KB → feeder(FIFO prio1) → 512KB 环(预蓄800ms, 滞回 250/2000ms)
        → writer(FIFO prio1) → aplay(nice -10, -B600ms/-F100, 48000 BT/44100 内置) → bluealsa
```
防御纵深：环 2.56s(上限) + alsa 0.6s ≈ **3.1s 抗断**；线程优先级防软解抢占；pipe 扩容防视频牵连音频。
**边界（如实记录）**：网络持续断供 >3.1s 或 CPU 长时间饱和仍会穿透为可闻顿挫——
本机软解 + 实测 646kbps WiFi 的物理现实；对照基准：系统网易云为本地文件无网络/解码压力。

## 2.3.0 收官收档（弹幕四档 + 评论面板 + 显示权，2026-09-24）

**平台级发现：blit 与 UI 的显示权冲突（本机 fb 直写架构的核心矛盾）**
- 三现象钉死同一根因：视频矩形像素归 `fb_blit`（33ms 写双缓冲）专属，**任何 UI 层覆盖
  同一区域都会与它交替抢帧**——①弹幕 UI 叠层=一帧弹幕一帧画面 ②评论面板=面板与画面交替
  ③下拉控制中心=同理；**暂停后全部正常 = blit 停止即无争抢**（用户观察的铁证）。
- 判据链：`page-live` 透明（清扫保 fb 像素）→ 二分（简介并入滚动放大但非根因）→
  profile 早证"每秒重绘不闪"（小区域）→ 大面积高频覆盖才可见拍频。
- **两个根治**（均已落地）：
  1. **弹幕迁出 UI → ffmpeg `drawtext` 烧进视频帧**（与 blit 同源=零互踩）：
     - 4 泳道 `reload=1` 动态换文本；**drawtext 必须在 transpose 之前**（字与内容同变换链，
       画在转置后=竖排 bug 真机实测）；字体 `fontconfig` 被禁 → 必须显式
       `fontfile=/etc/miniapp/resources/fonts/HarmonyOS_Sans_SC_Regular.ttf`（中文 CN_OK 验证）；
       滚动 `x=w-mod(t*SPEED\,w+text_w)`（逗号 `\,` 转义）。
     - **四档密度**：`danmakuMode 0关/1=1/4屏/2=1/2屏(默认)/3=全屏`；泳道 y=8/72/136/200，
       区域**从顶部算**（用户指定）：1/4=lane0 · 1/2=lane0+1 · 全屏=4 条；切档零重启
       （stop 清 4 泳道 → 按活跃集重启分派引擎 250ms tick）；`settings.danmakuMode`
       持久化（loadLocal 回读，新视频/新会话沿用上次）。
  2. **评论面板 `pauseRender()` 显示权交接**：开面板暂停 blit（**解码与音频照常**=边听边评），
     UI 独占零闪；关面板恢复。native `volatile render_paused` + `writeDm(idx,text)` 导出。
- **平台共性（无法根治，如实记录）**：**下拉系统控制中心**仍与 blit 争帧闪烁——系统 UI
  事件不可感知（fb 直写播放器通病；网易云无视频输出故不遇）。仅视觉、不影响播放。
- **字体/滤镜事实**：本机 ffmpeg `--disable-indevs`（无 lavfi 假源）**有 drawtext(libfreetype)、
  无 ass/subtitles 滤镜**；弹幕方向修正=drawtext 挪到 transpose 前（几何：内容靠转置正立，
  字必须走同一条变换链）。
- **真实弹幕已接入 ✓（2026-09-25）**：`x/v2/dm/web/seg.so`（collect `docs/danmaku/danmaku_proto.md` 契约）：
  - **坑1：`oid`=视频 cid 不是 aid**（传 aid → 空段；12 次全空实测）；`pid`=avid；6min/包。
  - **坑2：jsapi http 拿二进制废**（1.2MB 被截成 2/193 字节）→ **native `httpjson.getBinary`**
    （curl GET + `JS_NewArrayBufferCopy`；证书错误同 postSync 策略回退）+ `net.httpGetBinary`
    + `client.getBinary`（传输可注入）。
  - **坑3：protobuf key 是 varint**（field≥16 的新字段 key 多字节，单字节读 → 错位到非法 wt7；
    老视频 elem 含 field20/21/26+，外层含 field4）→ key/len 全 varint + 未知字段按 wire 跳过。
  - **坑4：魔数不能死板取 0x0A**（段1 elems 可空、以 field4 状态包开头 `22 04…`）→ 宽松 wire
    判定（`first>=8 && wt∈{0,1,2,5}`，`</{` 头自动排除）+ 解析失败双保险 + **空段自动续拉下一段**。
  - 金标准 fixture：`dm_seg.bin`（炮姐 cid=1176840 包1，1.2MB/5863 条，mode 1/4/5 显示、7 过滤）。
  - 真机判决：`danmaku real n=497 pkgs=3 span=0..1056316`（真实时间轴接管；3万条内存护栏；
    超稿件时长的尾包条目永不触发，无害）。device curl 对 api.bilibili.com 返回 000（疑 IPv6
    即败无回落）——仅影响 shell 探针，native/jsapi 不受影响。
- **未登录提示文案（收官）**：我的页扫码行下 `未登录时视频可能无法正常显示，登录后体验更完整`；
  评论面板写评论旁 `未登录 · 评论可能显示不全，登录后可发表评论`（hasLogin=nav 档案为准）。
- 版本链终态：**2.3.0**（native player v1.7.0 + **httpjson getBinary** + AMR 解包核对/断言全过）。
