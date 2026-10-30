/*
 * player —— 有道词典笔 X5 (Cvitek CV1826) 原生视频播放 JSAPI（纯 C）
 *
 * 设备事实（profiles/youdao-x5.md 真机探测）：
 *   - 无 GStreamer、无 /dev/dri（无 DRM/KMS）、无硬件视频解码器
 *   - 有 /usr/bin/ffmpeg (4.4，软解 h264)，但编译时 --disable-outdevs/--disable-indevs
 *     → 不能直接输出到 fbdev/alsa，必须由本模块读 rawvideo 自己贴 /dev/fb0
 *   - 显示只有 /dev/fb0（cvifb，32bpp，物理 254x800，双缓冲 yres_virtual=1600，pan 切换）
 *   - 音频走 /usr/bin/aplay（ALSA）
 *   - 单核 Cortex-A53：360p 软解 + 缩放约 1.1~1.9x 实时，720p 不可用
 *
 * 管线（v3.0.0 起：ffmpeg 只 demux + copy，解码交给 GStreamer/MPP）：
 *   ffmpeg -ss <start> -i <input> -map 0:v:0 -c:v copy -bsf:v h264_mp4toannexb -f h264 pipe:3
 *          -map 0:a:0? -ac 2 -ar 44100 -f s16le pipe:4
 *   → 视频线程按 Annex-B 起始码切出 access unit，按音频钟节拍逐个写进 FIFO
 *   → gst-launch filesrc(fifo) ! h264parse ! mppvideodec ! kmssink(plane-id=76)
 *   → 音频线程把 pipe:4 的数据写进 aplay stdin（阻塞写＝用音频时钟给整条管线限速）
 *
 * 无音轨时 ffmpeg 会因 "Output file #1 does not contain any stream" 失败，
 * 此时视频线程在第 0 帧失败后自动以"仅视频 + 时钟节拍"方式重启一次。
 *
 * JS 用法：
 *   import { open, pause, resume, seek, status, stop, redraw, release } from 'player'
 *   open(input, startMs, durationMs, fps, audio, transpose, x, y, w, h
 *        [, audioDevice[, userAgent, referer[, audioInput[, fpsCap]]]]) -> { ok, state, ... }
 *
 * 第 15 个位置参数 fpsCap（v3.2.0）＝ 渲染帧率上限，0/缺省 = 不限。>0 时在
 * mppvideodec 之后插 `videorate max-rate=<fpsCap>`，并把节拍器的 s->fps 夹到同一上限
 * （否则节拍器按源帧率排程，videorate 降帧后每帧都要空等）。为什么限帧不能只靠
 * 选流档位：有些内容服务端只给 60fps 档，请求 30fps 会直接落空。见 render_start()。
 *
 * 尾部两个可选参数（v1.1.0，bilibili_x5 项目加入）：
 *   userAgent — 传给 ffmpeg -user_agent（B 站 CDN 对默认 Lavf UA 返回 403，见 profile）
 *   referer   — 传给 ffmpeg -headers "Referer: <referer>\r\n"（DASH 流需要；html5 不校验）
 *   两者都做长度/字符集校验（valid_user_agent / valid_referer），不合法返回 PLAYER_BAD_HEADERS。
 *
 * v1.8.0 音画对齐重写（2026-09-30，修复"越播声音越超前"）：
 *   旧行为视频"只等不追"——帧到点才贴屏，但从不丢弃落后帧。软解+滤镜链（drawtext×4 +
 *   scale + transpose）吞吐一旦瞬时跌破实时（复杂场景/UI 抢占），每次掉帧都永久累积，
 *   画面越来越落后而音频（ALSA 硬实时）照走 → 声音越来越先出（用户实测：开播同步、
 *   越到后面声音越超前）。新规则：帧落后音频钟 >AV_SKIP_MS 即丢帧快进追钟（每
 *   AV_SKIP_BLIT_EVERY 帧贴一屏保留快进观感），漂移有界不再累积；音画同步统一覆盖
 *   durl 单文件路径（原来完全无节拍，画面可领先音频数秒）；音频供给停滞时画面冻结
 *   等声（rw_timeout 15s 兜底），移除旧的"停滞 8s 转墙钟"——那是漂移后无法回吸的根源。
 *
 * v1.9.0 音频锚修正（2026-10-01，修复"画面恒定超前声音一个缓冲容量"）：
 *   真机 soak 实证（恒速 1x、u=0、零 hold）writer 写出量领先可闻声一个缓冲存量
 *   （aplay 管道 64KB + ALSA -B 600ms，蓝牙 ≈0.9s）——写出≠发声。锚改为
 *   可闻位置 = 写出量 − 管道容量 − ALSA 缓冲（aplay -v setup 实测解析，失败保守回退）。
 *   起播的假超前丢帧风暴（~160 帧）随之消失；停滞期间误差有界（≤一个缓冲），恢复自愈。
 *
 * v2.1.0/2.1.3 音画同步分层（2026-10-02，用户诉求："轻微不同步平滑加速追上；过大不同步直接
 *   提示加载中并强制加载为同步画面"）：
 *   旧行为的分层只有一档——帧落后音频钟 >AV_SKIP_MS 就丢帧，且为"活性保障"每 0.6s 才贴
 *   一屏（≈1.6fps），于是网络波动后进入一段可见的低帧率顿挫期，靠解码产速>1x 慢慢排空积压
 *   （实测"过好一会才追上"）。新行为分两档：
 *     ① 轻微滞后（AV_SKIP_MS < drift ≤ AV_RESYNC_MS）→ **平滑快进**：每帧都贴屏、不等钟，
 *        画面以当前解码产速连续快进（不再 0.6s 节流），观感顺滑而非顿挫；产速>1x 时积压自然
 *        排空，仍不收敛则升级到②。
 *     ② 过大滞后（drift > AV_RESYNC_MS）→ **强制重同步**：跳过积压内容，把**视频流**重启到
 *        "当前可闻音频位置 + 预估启动时延"（音频进程/aplay 完全不动，声音不断）。启动时延
 *        （-ss 定位 + probesize + 首帧解码 ≈1~2s）由每次重启实测自适应，使首帧落屏时刻的音频
 *        位置 ≈ 首帧内容位置 → **落屏即同步**（否则会白落一个启动时延、又得靠①慢慢追）。
 *        重同步期间 status.resyncing=1，JS 显示"加载中…"；仅 DASH 拆分模式可安全重启
 *        （durl 音视频同进程，重启会连带音频 → 退回①平滑快进兜底）。
 *   断流重启（原 video_retries 路径）复用同一落点逻辑，顺带修掉它"重启后恒定落后一个启动时延"
 *   的旧缺陷。
 *   另一处补漏：漂移检查原本只在"读到帧"时执行，于是**纯视频停帧**（帧流断、音频链还健康）时
 *   native 完全无感，画面只能冻到 JS 巡检（8s）做双进程重启（实测连带打断音频 + 恢复期再走
 *   起播门 3.5s）。现利用 poll 的 200ms 空转做同一套巡检：音频链仍在供数（audio_bytes 增长）
 *   且落后 >AV_RESYNC_MS 即强制重同步（只重启视频）；音频链也断供（双侧硬断档）时不抢，交给
 *   JS 看门狗双重启。连续 3 次空转重同步仍无帧则放弃并清 resyncing（防坏节点上无限重启）。
 *
 * v2.2.0 网络预取（2026-10-02，用户诉求："像 B 站 App 那样：起播缓冲 ~0.5s，播放中用多余带宽
 *   尽量多缓冲，缓冲池保持 ~15s 音视频；不足 15s 就缓冲到片尾"）：
 *   动机：v2.1.x 的"加载中"偏多——根因是解码链路几乎无蓄水（视频 pipe 8MB≈0.7s 解码后画面、
 *   音频环 2.9s），网络一抖动立刻变成画面停帧 → 音画漂移 → 强制重同步。
 *   做法：给每个输入加 `-thread_queue_size`——ffmpeg 的输入线程把**压缩包**排进内存队列；
 *   主线程被输出管道反压时队列继续被灌满，于是下载与解码解耦，抖动先被队列吸收。
 *   只缓冲压缩流是硬约束：解码后画面 459KB/帧×24fps=11MB/s，15s 要 165MB（MemTotal 352MB）→ 不可行；
 *   压缩流 15s 仅 ~780KB。详见 AV_PREFETCH_PACKETS_VIDEO / _AUDIO 处注释。
 *   顺带修掉"重同步打转"（真机实测 v2.1.3：弱网下 rs=9/56s，画面累计被跳过 ~80s）：
 *   根因是重启视频流要 ~2~6s 才出首帧，而这期间漂移继续涨 → 冷却一过又触发 → 每次又跳一段内容。
 *   两处收口：① 队列按流精确到 ~15s（原 1500 包 ≈62s，重启时白重下 ~2.7MB 把链路打满 → 首帧
 *   从 2s 拖到 6s → 正反馈）；② 冷却 6s → 15s。配合 v2.1.3 的"残余漂移反馈"（lead 收敛到实测
 *   重启耗时），落点误差 = Δ 的抖动（稳定网络下 →0）→ 重同步由"每 6s 一次"变为"收敛后不再触发"。
 *
 * v3.0.0 硬解重写（2026-10-29，X6 Pro / RK3562，用户诉求："别再软解了，用 MPP 硬解"）：
 *   动机：RK3562 有 MPP 视频解码单元，实测 `mppvideodec` 解 40s 720p 只要 1.21s（**33x 实时**），
 *   而软解在 640x360 上只有 1.1~1.9x —— 整条链路的余量、功耗、掉帧全部来自软解这一环。
 *   为什么不能走 ffmpeg 硬解：ffmpeg 4.4 只有 h264_v4l2m2m/hevc_v4l2m2m，而 RK3562 的 MPP
 *   **不走 V4L2 mem2mem**（/dev/video0..17 全是 rkcif/rkisp 摄像头驱动，`-c:v h264_v4l2m2m`
 *   报 `Could not find a valid device`）⇒ 硬解只能交给 GStreamer。
 *   做法（关键：**音画同步逻辑一行不改**，只把"像素搬运"换成"压缩包转发"）：
 *     ffmpeg -ss S -i <input> -map 0:v:0 -c:v copy -bsf:v h264_mp4toannexb -f h264 pipe:3
 *       → 本模块按 Annex-B 起始码切 AU → 按原节拍（音频锚/墙钟/漂移/重同步）逐 AU 写入 FIFO
 *       → gst-launch filesrc ! h264parse ! mppvideodec rotation=R ! kmssink(plane 76)
 *   为什么"只转发压缩包"反而更准：解码不再占用本进程 CPU，节拍逻辑（v1.8.0~v2.9.4 一路调出来的
 *   音频锚/平滑快进/强制重同步）原样保留，`s->frames` 语义不变（= 已送显 AU 数）→ status 字段全保。
 *   三个必须处理的新问题（都在下面代码里落了注释）：
 *     ① **帧率**：copy 模式没有 `-r` 重采样，流的真实帧率 ≠ JS 传的 24（B 站 DASH 多为 30）→
 *        必须从 ffmpeg stderr 的 `Input #0 ... 30 fps` 解析出来覆盖 s->fps，否则节拍慢 25%、
 *        漂移一路涨到强制重同步。stderr 因此改走 pipe（同时解决 `-debug_ts` 洪水）。
 *     ② **seek 落点**：`-ss` + copy 只能落在**前一个关键帧**（实测 -ss 10.5 → 首包 pts 10.0），
 *        不像软解能解码丢帧到精确点。不修正的话 seek 后画面恒定落后声音 0~1 个 GOP。
 *        用 `-debug_ts` 的第一条 `demuxer+ffmpeg ... pkt_pts_time:X` 拿到相对落点 X（<0），
 *        把 video_base_ms 改成 S+X（= 关键帧真实内容时间）→ 前置 GOP 走①平滑快进瞬间吃掉，
 *        之后与音频严格对齐。
 *     ③ **背压**：FIFO 写是阻塞的，gst 消费慢时压住 video_thread 再压住 ffmpeg —— 这正是想要的
 *        蓄水；但停播时不能永久阻塞，故 FIFO 写端 O_NONBLOCK + poll 带 stop_flag 检查。
 */

#define _GNU_SOURCE 1 /* F_GETPIPE_SZ 等 GNU 扩展（zig cc/glibc 默认不定义） */

#include <jquick_config.h>
#include <jsmodules/JSCModuleExtension.h>
#include <quickjs/quickjs.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>        /* SCHED_FIFO：音频线程防抢占 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>   /* FIONREAD：读 aplay 管道实际占用量（v2.9.4 音频锚精确化） */
#include <sys/mman.h>
#include <sys/resource.h> /* setpriority：aplay 提优先级 */
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>      /* v2.2.8：回收残留 gst-launch（孤儿进程占着 overlay 平面） */

#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031 /* linux fcntl.h；老 sysroot 兜底 */
#endif

static void raise_audio_thread_prio(void); /* 定义在 RING 水位区（feeder 调用点更早） */

#define PLAYER_VERSION "2.3.2"

/* ---- v2.2.0 网络预取（用户诉求："像 B 站 App 那样：起播缓冲 ~0.5s，播放中用多余带宽尽量多缓冲，
 *      缓冲池保持 ~15s 音视频；不足 15s 就缓冲到片尾"）----
 * 为什么不能缓冲"解码后的画面"：输出是 254×452×4B = 459KB/帧，24fps = 11MB/s，15s 要 165MB
 * （本机 MemTotal 仅 352MB）→ 不可行。所以只缓冲**压缩源码流**：15s ≈ 视频 43KB/s×15=653KB +
 * 音频 8KB/s×15=123KB ≈ 780KB，完全可忽略。
 * 实现：`-thread_queue_size N` —— ffmpeg 的输入线程把解包后的**压缩包**排进内存队列；主线程被
 * 输出管道反压（视频 pipe 8MB 满 / 音频环满）时队列继续被灌满，于是"下载"与"解码"解耦：
 * 网络抖动先被队列吸收，不再立刻变成画面停帧 → 音画漂移 → 强制重同步（"加载中"）。
 * N 的取值：包数 = 目标秒数 × 每秒包数。视频 1 包/帧（24fps→24/s，60fps 源→60/s）；
 * AAC 1 包/1024 样本（44.1k→43/s）。故 600 包 ≈ 15~25s 视频、800 包 ≈ 18s 音频；
 * 内存 = 包数 × 平均包长 ≈ 1.1MB(视频 1.8KB/包) + 0.15MB(音频 0.2KB/包)，实测可忽略。
 * 取值偏保守是刻意的：**重启视频流时队列会被丢弃重下**，队列越深、弱网下重同步一次浪费越大
 * （实测 1500 包时单次重启重下 ~2.7MB，把链路打满 → 下一次重启的首帧从 2s 拖到 6s → 触发
 * 更多重同步，形成正反馈风暴）。
 * "不足 15s 缓冲到片尾"天然成立：输入线程读到 EOF 即停，队列里剩余的包照常排空播放。
 * 起播 ~0.5s 缓冲也天然成立：既有"起播门"要等视频首帧（实测 0.6~2.5s，探流+解码耗时），
 * 这段时间输入线程已把队列灌到远大于 0.5s。
 * 注意：队列在 ffmpeg 进程内，native 侧拿不到其填充量 → 暂无法做"缓冲进度条"UI；
 * 但正因为队列把下载与解码解耦，网络抖动不再穿透为"加载中"，且解码不再被网络限速
 * → ①平滑快进真正有了余量（产速可跑到 CPU 上限 1.1~1.9x，而不是被链路卡在 1x）。
 * 副作用（实测）：单次重同步后队列要重灌 → 弱网下每次重同步多下 ~1MB。 */
#define AV_PREFETCH_PACKETS_VIDEO 300 /* ≈15~30s 视频（实测 ≈3KB/包 → ≈20s）；须 > rw_timeout 8s + 重启耗时 */
#define AV_PREFETCH_PACKETS_AUDIO 700 /* ≈16s 音频（+环 2.9s + ALSA 0.6s ≈ 19.5s 抗断） */
#define AV_STR_(x) #x
#define AV_STR(x) AV_STR_(x)
#define AV_PREFETCH_PACKETS_VIDEO_STR AV_STR(AV_PREFETCH_PACKETS_VIDEO)
#define AV_PREFETCH_PACKETS_AUDIO_STR AV_STR(AV_PREFETCH_PACKETS_AUDIO)
#define FB_PATH "/dev/fb0"
#define FB_PAN_PATH "/sys/class/graphics/fb0/pan"
#define FB_MODES_PATH "/sys/class/graphics/fb0/modes"
#define FB_STRIDE_PATH "/sys/class/graphics/fb0/stride"
#define FB_BPP_PATH "/sys/class/graphics/fb0/bits_per_pixel"
#define FB_VSIZE_PATH "/sys/class/graphics/fb0/virtual_size"

/* ---- X6 Pro (RK3562) KMS overlay 后端 ----
 * 该机型**完全没有 /dev/fb0**：Falcon/UI 由 weston 用 DRM plane 54 (Esmart0-win0) 合成，
 * 视频必须交给另一个 overlay plane。真机实测（2026-10-29）：
 *   - 空闲 plane 76 (Esmart1-win0) / 90 / 104 可供 kmssink 使用，zpos 高于 UI → 硬件叠加；
 *   - 裸 DRM SETPLANE 自建 dumb buffer 一律 ENOSPC（Rockchip 需带 FB 才占得住），
 *     **只有 kmssink 能真正占用该平面** ⇒ 帧必须交给 gst-launch；
 *   - 帧链：ffmpeg --rawvideo pipe:3--> 本模块 --FIFO--> gst-launch filesrc blocksize=fsz
 *     ! video/x-raw,format=BGRA ! videoconvert ! kmssink。用 FIFO+filesrc 而非 fdsrc
 *     （本机 fdsrc 短读即 streaming error），也不链 libgstreamer（保持模块零外部依赖）；
 *   - render-rectangle 是**面板绝对坐标 480x960**：Falcon 合成面实测占面板列 107..372，
 *     故面板 x = 107 + 物理矩形 x；面内 y/宽/高与物理矩形一致（1:1，无缩放）；
 *   - 必须显式 force-aspect-ratio=false，否则 kmssink 会在矩形内做信箱缩放；
 *   - 注意：miniapp_cli capture 只截 Falcon 画布，**看不见 overlay 平面**；
 *     真像素取证用 `gst-launch kmssrc plane-id=76 num-buffers=1 ! videoconvert ! pngenc`。
 */
#define PANEL_X_OFFSET 107
#define KMS_PLANE_ID 76
#define KMS_ID_STR "76"
#define GST_LAUNCH_PATH "/usr/bin/gst-launch-1.0"
#define KMS_FIFO_PATH "/tmp/vp_vid.fifo"
#define FFMPEG_PATH "/usr/bin/ffmpeg"
#define LOG_PATH "/tmp/vp_player.log"      /* 视频进程 stderr */
#define GST_LOG_PATH "/tmp/vp_gst.log"     /* v3.0.0：gst-launch 的 stderr（硬解/协商失败取证） */
#define LOG_AUDIO_PATH "/tmp/vp_audio.log" /* 音频独立进程 stderr（分文件防 O_TRUNC 互毁取证） */
#define APLAY_PATH "/usr/bin/aplay"
#define BT_LIST_CMD "/usr/bin/bluealsa-aplay -L 2>/dev/null"
#define APLAY_LOG_PATH "/tmp/vp_aplay.log"
#define TREND_LOG_PATH "/tmp/vp_trend.log" /* v2.9.4 直播长跑趋势落盘（约 1 行/秒；不依赖 JS/屏幕状态） */

/* v3.0.1：判断「视频平面是否真的在扫帧」，给音频起播门用（修「先声后画」）。
 * 定义放在常量区之后、render_* 之前；实现见 fb_write_stream 上方。 */
static double now_seconds(void);
static int kms_video_active(void);
static int wait_kms_video(double max_ms);

#define ST_IDLE 0
#define ST_PLAYING 1
#define ST_PAUSED 2
#define ST_ENDED 3
#define ST_ERROR 4

static const char *state_name(int st) {
    switch (st) {
        case ST_PLAYING: return "playing";
        case ST_PAUSED: return "paused";
        case ST_ENDED: return "ended";
        case ST_ERROR: return "error";
        default: return "idle";
    }
}

/* ---------------- framebuffer ---------------- */

struct fb_info {
    int fd;
    int line_length;
    int bpp;
    int xres_virtual;
    int yres_virtual;
    int visible_h;   /* 单块缓冲高度（从 modes 解析） */
    int buffers;     /* 缓冲块数 = yres_virtual / visible_h（本机双缓冲=2） */
    size_t map_len;
    unsigned char *map;
};

static struct fb_info g_fb = { -1, 0, 0, 0, 0, 0, 1, 0, NULL };

/* modes 形如 "U:254x800p-0,U:480x800p-0" → 取第一个模式的可见高度 */
static int parse_mode_height(const char *modes) {
    const char *p = strchr(modes, ':');
    int w = 0;
    int h = 0;
    if (!p) return 0;
    if (sscanf(p + 1, "%dx%d", &w, &h) != 2) return 0;
    return h;
}

static int read_text_file(const char *path, char *buf, int n) {
    int fd = open(path, O_RDONLY);
    ssize_t r;
    if (fd < 0) return -1;
    r = read(fd, buf, (size_t)(n - 1));
    close(fd);
    if (r <= 0) return -1;
    buf[r] = '\0';
    return (int)r;
}

static long read_long_file(const char *path, long fallback) {
    char buf[64];
    if (read_text_file(path, buf, sizeof(buf)) < 0) return fallback;
    return strtol(buf, NULL, 10);
}

/* "1024,1600" / "0,800" → (a,b) */
static int read_pair_file(const char *path, long *a, long *b) {
    char buf[96];
    char *comma = NULL;
    if (read_text_file(path, buf, sizeof(buf)) < 0) return -1;
    comma = strchr(buf, ',');
    if (!comma) return -1;
    *comma = '\0';
    *a = strtol(buf, NULL, 10);
    *b = strtol(comma + 1, NULL, 10);
    return 0;
}

static int fbdev_open(void) {
    long vsize[2];
    char modes[128];
    if (g_fb.map) return 0;
    g_fb.line_length = (int)read_long_file(FB_STRIDE_PATH, 0);
    g_fb.bpp = (int)read_long_file(FB_BPP_PATH, 0);
    if (read_pair_file(FB_VSIZE_PATH, &vsize[0], &vsize[1]) != 0) return -1;
    g_fb.xres_virtual = (int)vsize[0];
    g_fb.yres_virtual = (int)vsize[1];
    if (g_fb.line_length <= 0 || g_fb.bpp != 32 || g_fb.yres_virtual <= 0) return -1;
    g_fb.visible_h = 0;
    if (read_text_file(FB_MODES_PATH, modes, sizeof(modes)) > 0) {
        g_fb.visible_h = parse_mode_height(modes);
    }
    if (g_fb.visible_h > 0 && g_fb.yres_virtual >= g_fb.visible_h) {
        g_fb.buffers = g_fb.yres_virtual / g_fb.visible_h;
    } else {
        g_fb.buffers = 1;
    }
    if (g_fb.buffers < 1) g_fb.buffers = 1;
    if (g_fb.buffers > 4) g_fb.buffers = 4;
    g_fb.fd = open(FB_PATH, O_RDWR);
    if (g_fb.fd < 0) return -1;
    g_fb.map_len = (size_t)g_fb.line_length * (size_t)g_fb.yres_virtual;
    g_fb.map = (unsigned char *)mmap(NULL, g_fb.map_len, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb.fd, 0);
    if (g_fb.map == MAP_FAILED) {
        g_fb.map = NULL;
        close(g_fb.fd);
        g_fb.fd = -1;
        return -1;
    }
    return 0;
}

static void fbdev_close(void) {
    if (g_fb.map) {
        munmap(g_fb.map, g_fb.map_len);
        g_fb.map = NULL;
    }
    if (g_fb.fd >= 0) {
        close(g_fb.fd);
        g_fb.fd = -1;
    }
}

/* 贴一帧：**写入所有缓冲块**，而不是只写当前 pan 显示的那一块。
 *
 * 为什么：框架是双缓冲 + pan 切换。只写当前显示块的话，框架一 pan，
 * 用户就会看到另一块缓冲里的旧内容 —— 表现为"闪上一帧旧画面"。
 * 两块都写之后，无论 pan 到哪一块，视频都是一样的新帧。
 * 代价是每帧两次 memcpy（254x452x4 ≈ 458KB → 916KB，24fps 约 22MB/s），可忽略。 */
static int fbdev_blit(const unsigned char *frame, int x, int y, int w, int h) {
    int buf;
    int row;
    if (!g_fb.map || !frame || w <= 0 || h <= 0) return -1;
    if (g_fb.buffers < 1) return -1;
    for (buf = 0; buf < g_fb.buffers; buf++) {
        size_t base = (size_t)buf * (size_t)(g_fb.visible_h > 0 ? g_fb.visible_h : g_fb.yres_virtual) *
                      (size_t)g_fb.line_length;
        for (row = 0; row < h; row++) {
            size_t off = base + (size_t)(y + row) * (size_t)g_fb.line_length + (size_t)x * 4u;
            if (off + (size_t)w * 4u > g_fb.map_len) return -1;
            memcpy(g_fb.map + off, frame + (size_t)row * (size_t)w * 4u, (size_t)w * 4u);
        }
    }
    return 0;
}

/* 当前正在显示的缓冲块序号（仅用于 status 上报，不影响写入） */
static int fbdev_current_buffer(void) {
    long pan[2] = { 0, 0 };
    int visible = g_fb.visible_h > 0 ? g_fb.visible_h : g_fb.yres_virtual;
    if (read_pair_file(FB_PAN_PATH, &pan[0], &pan[1]) != 0) return 0;
    if (visible <= 0) return 0;
    return (int)(pan[1] / visible);
}

/* ---------------- KMS overlay 输出（X6 Pro，无 fbdev） ---------------- */

static int wait_child(pid_t pid, int timeout_ms); /* 定义在下方 pipe 工具区 */

struct render_info {
    int kms;      /* 1 = 该机型无 /dev/fb0，走 KMS overlay */
    pid_t gst_pid;
    int fifo_fd;
    int started;
    int w, h;     /* 已建链的目标显示矩形（旋转后的"物理方向"，v3.0.0 起 = render-rectangle 尺寸） */
    int rx, ry;   /* 已建链的物理矩形 */
    int fps;      /* 保留（诊断用；v3.0.0 起不再进 caps） */
    int transpose;/* 1=顺时针 90° / 2=逆时针 90°（ffmpeg 语义）→ 映射到 mppvideodec rotation */
    int fps_cap;  /* v3.2.0：渲染帧率上限（0 = 不限）；>0 时在解码器后插 videorate max-rate */
    int dump_fd;  /* 诊断用：PENBILI_DUMP_FIFO 落盘 fd，-1 = 未启用 */
    char path[64];
};

static struct render_info g_rd = { 0, 0, -1, 0, 0, 0, 0, 0, 30, 1, 0, -1, "" };

/* 非阻塞轮询打开 FIFO 写端：gst 起不来时不要永久阻塞视频线程。
 * v3.0.0：打开后**保留 O_NONBLOCK** —— 压缩包转发是长写（一路写到播完），必须能在
 * gst 消费慢时用 poll 等待并随时响应 stop_flag，不能卡在阻塞 write 里出不来。
 * 需要"写满"语义的地方统一走 fifo_write_all()。 */
static int fifo_open_write(const char *path, int timeout_ms) {
    int waited = 0;
    for (;;) {
        int fd = open(path, O_WRONLY | O_NONBLOCK);
        if (fd >= 0) return fd;
        if (errno != ENXIO && errno != ENOENT) return -1;
        if (waited >= timeout_ms) return -1;
        usleep(20000);
        waited += 20;
    }
}

/* 阻塞写满（v3.0.0 起 FIFO 侧改用 fifo_write_all：需要能被 stop_flag 打断）。
 * 保留给内部小块写用；当前无调用点，加 unused 属性避免 -Wunused-function。 */
static int write_full(int fd, const unsigned char *buf, size_t n) __attribute__((unused));
static int write_full(int fd, const unsigned char *buf, size_t n) {
    size_t put = 0;
    while (put < n) {
        ssize_t w = write(fd, buf + put, n - put);
        if (w > 0) { put += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

/* ---- v2.2.8: 回收"上一代"残留的 gst-launch ----
 * gst-launch 是 setsid 出来的孤儿进程：模块被 JS 重启 / 页面重进 / app 被系统杀掉时，
 * 父进程死了它还在跑，并且**一直占着 overlay plane 76**。于是新实例的 kmssink
 * 抢不到平面（plane 保持 ACTIVE 但不再更新，画面冻在最后一帧），真机实测就是这个症状。
 * 做法：按 cmdline 精确匹配本模块的 pipeline（含 FIFO 路径，不会误杀别的 gst 用户），
 * 排除自己刚 fork 的子进程 pid，其余一律 SIGKILL。 */
static void render_reap_orphans(void) {
    DIR *d = opendir("/proc");
    struct dirent *de;
    if (!d) return;
    while ((de = readdir(d)) != NULL) {
        char path[64];
        char cmd[512];
        int fd;
        ssize_t n;
        long pid;
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        pid = strtol(de->d_name, NULL, 10);
        if (pid <= 0 || (pid_t)pid == g_rd.gst_pid) continue;
        snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
        fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        n = read(fd, cmd, sizeof(cmd) - 1);
        close(fd);
        if (n <= 0) continue;
        cmd[n] = '\0';
        {
            size_t k;
            for (k = 0; k < (size_t)n; k++) {
                if (cmd[k] == '\0') cmd[k] = ' ';
            }
        }
        if (strstr(cmd, "gst-launch-1.0") && strstr(cmd, KMS_FIFO_PATH)) {
            kill((pid_t)pid, SIGKILL);
        }
    }
    closedir(d);
}

static void render_stop(void) {
    /* 取证：谁在关 FIFO。关闭写端会让 gst 读到 EOF 并**正常退出（exit=0）**，
     * 单看 /tmp/vp_gst_exit.log 的 exit=0 无法区分「gst 自己崩」与「我们关的」。
     * 这条日志配合 gst_exit 的时间戳即可定位（t 为 uptime 秒）。 */
    if (g_rd.started || g_rd.fifo_fd >= 0) {
        int dfd = open("/tmp/vp_gst_exit.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (dfd >= 0) {
            char sl[128];
            int sl_n = snprintf(sl, sizeof(sl), "t=%.1f STOP pid=%ld fd=%d w=%d h=%d rx=%d ry=%d\n",
                    now_seconds(), (long)g_rd.gst_pid, g_rd.fifo_fd, g_rd.w, g_rd.h, g_rd.rx, g_rd.ry);
            if (sl_n > 0) (void)!write(dfd, sl, (size_t)sl_n);
            close(dfd);
        }
    }
    if (g_rd.fifo_fd >= 0) {
        close(g_rd.fifo_fd);
        g_rd.fifo_fd = -1;
    }
    if (g_rd.gst_pid > 0) {
        kill(g_rd.gst_pid, SIGTERM);
        wait_child(g_rd.gst_pid, 800);
        g_rd.gst_pid = 0;
    }
    if (g_rd.path[0]) {
        unlink(g_rd.path);
        g_rd.path[0] = '\0';
    }
    g_rd.started = 0;
    g_rd.w = 0;
    g_rd.h = 0;
    g_rd.rx = -1;
    g_rd.ry = -1;
}

/* g_rd.path 是上下文里唯一的 FIFO 路径，建链前后都要存在。 */
static void render_make_path(void) {
    if (!g_rd.path[0]) snprintf(g_rd.path, sizeof(g_rd.path), "%s", KMS_FIFO_PATH);
}

/* ---- v3.0.0：FIFO 写 = 压缩字节流（非阻塞 + poll，可被 stop_flag 打断）----
 * 为什么不用原来的 write_full：那时每帧 w*h*4 是定长小写（≤0.5MB）且写满即返回；
 * 现在一路写到播完，gst 消费慢时 write 会阻塞——停播时不能永久卡在这里（stop_children
 * 要 join 本线程）。故 FIFO 保持 O_NONBLOCK，用 poll 等可写，每次最多等 100ms 再看 stop_flag。
 * 返回 0 = 写完；-1 = 写失败（EPIPE/对端关闭 → 调用方重建链）；1 = 被 stop_flag 中止。 */
static int fifo_write_all(int fd, const unsigned char *buf, size_t n, const volatile int *stop) {
    size_t put = 0;
    while (put < n) {
        ssize_t w = write(fd, buf + put, n - put);
        if (w > 0) {
            put += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p;
            int pr;
            if (stop && *stop) return 1;
            p.fd = fd;
            p.events = POLLOUT;
            p.revents = 0;
            pr = poll(&p, 1, 100);
            if (pr < 0 && errno != EINTR) return -1;
            continue;
        }
        return -1; /* EPIPE：gst 侧没了 */
    }
    return 0;
}

/* ---- v3.0.0：建链 = H.264 字节流 → h264parse → mppvideodec（MPP 硬解）→ kmssink ----
 * 与旧链路（BGRA rawvideo → videoconvert → kmssink）的区别：
 *   · FIFO 里不再是"整帧像素"，而是**变长 Annex-B 压缩字节流**（blocksize 只是 filesrc 读粒度）；
 *   · 旋转由解码器做（mppvideodec rotation），不再走 ffmpeg transpose 滤镜；
 *   · 缩放交给 KMS/RGA（kmssink render-rectangle 的硬件缩放），不再走 ffmpeg scale；
 *   · 因此**不需要 videoconvert**：mppvideodec 输出 NV12，kmssink 直接吃。
 *     若某些固件上 NV12 上屏颜色/尺寸不对，设环境变量 PENBILI_GST_CONVERT=1 插一层
 *     videoconvert 即可（免重编排查用）。
 * rotation 映射：s->transpose 是 ffmpeg 语义（1=顺时针 90°，2=逆时针 90°，3=180°），
 *   mppvideodec rotation 是**顺时针角度**（0/90/180/270）。
 *
 * 【2026-10-30 决定性复验，结论：transpose=2 → rotation=270】
 *   方法：把真实直播录制流 /tmp/dump.h264 分别以 rotation=90 和 270 上屏，
 *   用 kmssrc 抓平面 PNG，再按用户视角（面板图顺时针转 90°）比对画面里的文字方向。
 *   实测 rotation=90 的画面**整体上下颠倒**（"WORLDS 26" 和选手脸都是倒的），
 *   rotation=270 完全正常。此前 v3.0.0→v3.0.1 把 270 改成 90 是**改错了**：
 *   那次是靠"把平面 PNG 单独转正后比对"推断的，转正方向本身就搞反了，
 *   于是把正确值改成了错误值，用户戴机后一直反馈"倒置"。
 *     transpose=1（顺时针 90°）≡ rotation=90
 *     transpose=2（逆时针 90°）≡ rotation=270
 *   JS 侧（play_session.js / probe.js）恒传 transpose=2 → rotation=270。
 *   注意（v3.0.1）：本函数只读 g_rd.transpose，而 g_rd.transpose 曾经**从未被赋值**
 *   （只有 s->transpose 被写），导致映射表改动毫无效果 —— 那一处修复是对的，
 *   现在 open() 里两处同步赋值，真机 ps 应看到 rotation=270。 */
static int rotation_for_transpose(int t) {
    if (t == 1) return 90;
    if (t == 3) return 180;
    return 270; /* t == 2（JS 恒传） */
}

static int render_start(int x, int y, int w, int h) {
    char rect[80];
    char location[96];
    char rotation[24];
    char *argv[40];
    const char *extra = NULL;
    int use_fdsrc;
    pid_t pid;
    int px = PANEL_X_OFFSET + x;
    int i = 0;
    int rot = rotation_for_transpose(g_rd.transpose);

    /* 先把路径定下来，render_stop() 才能 unlink 掉上一代 FIFO。
     * render_stop() 会把 g_rd.path[0] 置空，所以之后必须重新赋值。 */
    render_make_path();
    render_stop();
    render_reap_orphans(); /* v2.2.8：先清掉上一代孤儿，否则平面抢不到 */
    render_make_path();
    unlink(g_rd.path);
    if (mkfifo(g_rd.path, 0666) != 0) return -1;

    snprintf(location, sizeof(location), "location=%s", g_rd.path);
    /* v3.1.2【「约一秒卡一次」的真正根因与修法】
     *
     * 症状：点播/直播都能播、音频完全正常，但画面每 0.5~1s 顿一下。
     * 早期误判过三条路（都已排除）：
     *   · 网络抖动 —— 用本地 h264 文件跑同一条链路，卡顿一模一样；
     *   · 解码慢 —— mppvideodec 解 60s 720p 只要 1.5s（~40x 实时），余量充足；
     *   · Falcon UI 每秒重绘抢平面 —— 彻底不写响应式状态后大空档从 36 降到 30，
     *     只是噪声级差异；debugfs 里 ui_changes 恒为 0。
     *
     * 真正原因：**filesrc 会攒满 blocksize 才把数据推给下游**。
     *   app 每 33ms 往 FIFO 写一个 ~3KB 的 AU，而 filesrc blocksize=65536，
     *   等于要攒 ~20 帧（≈0.66s）才下推一批 → h264parse/解码器一次收到一大簇
     *   → 瞬间解完几帧连画、然后干等下一簇 ⇒ 用户看到的就是「冲一段、停一下」。
     *   这也解释了为什么「平均帧率是对的（24fps）、但帧间隔直方图是双峰的」。
     *   （曾经在注释里写过「pipe 上的 read 会返回已有字节、不会凑满」——那是错的，
     *     filesrc 用的是 gst_base_src 的填充语义，会一直读到 blocksize 满足为止。）
     *
     * 修法：filesrc → **fdsrc**。fdsrc 专为管道设计，有数据就推，不等凑满块。
     *   真机 A/B（16s、本地 24fps、无网络、其余参数完全相同）：
     *     filesrc + sync=false ：vid_fps=22.88，>80ms 大空档 **78 次**（成簇）
     *     filesrc + sync=true  ：vid_fps=23.44，大空档 3 次（sync 靠丢帧摊平）
     *     fdsrc   + sync=false ：vid_fps=22.88，大空档 78 次（不解决根因）
     *     fdsrc   + sync=true  ：**vid_fps=24.00、大空档 0 次、帧间隔全在 30-45ms** ← 采用
     *   fdsrc 必须配 sync=true：fdsrc 逐帧推送后，sink 仍需按 PTS 对齐 vblank 才能
     *   匀速出帧（否则跟着上游写的瞬时节奏走）。
     *
     * 实现要点：fdsrc 从 **fd 3** 读。子进程在 exec 前把 FIFO 读端 open 成 fd 3；
     *   必须带 O_NONBLOCK——否则 open(O_RDONLY) 会阻塞等写端，而父进程的
     *   open(O_WRONLY|O_NONBLOCK) 又会因"没有读者"返回 ENXIO，两边互等死锁。
     *   fdsrc 自己会把 fd 设成非阻塞并用 poll 等待，所以传进去时非阻塞是安全的。
     * 排查开关：/tmp/vp_filesrc.on → 退回 filesrc（老行为），便于对照。 */
    snprintf(rotation, sizeof(rotation), "rotation=%d", rot);
    snprintf(rect, sizeof(rect), "render-rectangle=<%d,%d,%d,%d>", px, y, w, h);
    if (getenv("PENBILI_GST_CONVERT")) extra = "videoconvert";
    use_fdsrc = (access("/tmp/vp_filesrc.on", F_OK) != 0);

    pid = fork();
    if (pid < 0) {
        unlink(g_rd.path);
        g_rd.path[0] = '\0';
        return -1;
    }
    if (pid == 0) {
        int logfd;
        setsid();
        /* O_APPEND 而非 O_TRUNC：重建链（EPIPE 后重试 / seek / 断流重启）时不能把
         * 上一代 gst 的死因清掉——真机排查「视频显示链断开」时日志恒为 0 字节，
         * 正是因为第二次 render_start 把第一次的错误信息截断了。 */
        logfd = open(GST_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (logfd >= 0) {
            dup2(logfd, 2);
            /* v3.0.1 取证：librga 的 "rga_api version ..." 走 **stdout** 而非 stderr，
             * 只重定向 fd 2 时日志里看不到它，容易误判"gst 根本没跑起来"。 */
            dup2(logfd, 1);
            close(logfd);
        }
        /* 排查开关：PENBILI_GST_DEBUG=<GST_DEBUG 值> 给子进程设 GST_DEBUG。
         * 真机遇到「gst 自己 exit=0（EOS）而写端仍开着」时，靠它看清是哪一级报的 EOS。 */
        {
            const char *gdbg = getenv("PENBILI_GST_DEBUG");
            setenv("GST_DEBUG", (gdbg && gdbg[0]) ? gdbg : "*:3", 1);
            setenv("GST_DEBUG_NO_COLOR", "1", 1);
        }
        /* v3.1.2：fdsrc 方案 —— 在 exec 前把 FIFO 读端 open 成 fd 3。
         * 必须 O_NONBLOCK：阻塞式 open(O_RDONLY) 会等写端，而父进程的
         * open(O_WRONLY|O_NONBLOCK) 在没有读者时返回 ENXIO，两边互等死锁。
         * fdsrc 自己会设非阻塞 + poll，所以传进去时非阻塞是安全的。 */
        if (use_fdsrc) {
            int rfd = open(g_rd.path, O_RDONLY | O_NONBLOCK);
            if (rfd < 0) {
                int efd = open("/tmp/vp_gst_exit.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (efd >= 0) {
                    const char *m = "CHILD open fifo rd FAILED\n";
                    (void)!write(efd, m, strlen(m));
                    close(efd);
                }
                _exit(126);
            }
            if (rfd != 3) {
                dup2(rfd, 3);
                close(rfd);
            }
        }
        argv[i++] = (char *)GST_LAUNCH_PATH;
        /* PENBILI_GST_NOQ=1 → 去掉 -q，把 EOS/协商等全量消息打进日志（排查专用）。 */
        /* 排查期：默认不加 -q（看到 EOS/协商消息）；设 PENBILI_GST_QUIET=1 恢复安静模式。 */
        if (getenv("PENBILI_GST_QUIET")) argv[i++] = (char *)"-q";
        /* fpsdisplaysink 的 `rendered: N, dropped: M` 是 **element 的 last-message**，
         * 只有 `-v` 才会被 gst-launch 打到日志；不加 -v 它一行都不输出（实测踩过）。
         * 该开关只在取证期打开，正常播放不加，避免日志膨胀。 */
        if (access("/tmp/vp_fps.on", F_OK) == 0) argv[i++] = (char *)"-v";
        if (use_fdsrc) {
            argv[i++] = (char *)"fdsrc";
            argv[i++] = (char *)"fd=3";
            /* 3000 ≈ 一个 640x360 帧的 AU 大小。fdsrc 有数据就推，这个值只影响
             * 单次 read 上限，不再有「攒满才下推」的问题。 */
            argv[i++] = (char *)"blocksize=3000";
        } else {
            argv[i++] = (char *)"filesrc";
            argv[i++] = location;
            argv[i++] = (char *)"blocksize=65536";
        }
        argv[i++] = (char *)"!";
        argv[i++] = (char *)"h264parse";
        argv[i++] = (char *)"!";
        argv[i++] = (char *)"mppvideodec";
        argv[i++] = rotation;
        argv[i++] = (char *)"!";
        /* ---- v3.2.0 帧率上限（「我的 → 视频调节 / 直播」的帧率设置）----
         *
         * 为什么限帧要放在**解码器之后**而不是靠选流档位：
         *   有些稿件/直播间服务端**只提供 60fps 档**，没有 30fps 变体。
         *   只靠「请求 30fps 档」在那种内容上会直接落空（要么播 60fps，
         *   要么干脆没得播）；只靠 videorate 又会白拉两倍码率再丢一半。
         *   所以 JS 侧两件事都做：优先请求 30fps 档，同时把上限传进来兜底。
         *
         * 真机实测（2026-10-30，fdsrc fd=3 → h264parse → mppvideodec rotation=270
         * → videorate max-rate=30 → fpsdisplaysink → kmssink，源 /tmp/t720_60.h264
         * 即 1280x720@60）：
         *   不加 videorate → current 57.39fps / dropped 0
         *   加 max-rate=30  → current 29.73fps / dropped 0，帧间隔均匀
         * 30fps 源上加 max-rate=30 是直通（实测 24fps 源 + max-rate=48 仍是 23.91fps，
         * videorate 不做插帧，只做上限）——所以低帧率内容不会被这个开关伤到。 */
        if (g_rd.fps_cap > 0) {
            char cap[32];
            snprintf(cap, sizeof(cap), "max-rate=%d", g_rd.fps_cap);
            argv[i++] = (char *)"videorate";
            argv[i++] = strdup(cap);
            argv[i++] = (char *)"!";
        }
        if (extra) {
            argv[i++] = (char *)extra;
            argv[i++] = (char *)"!";
        }
        /* v3.1.2：queue 默认**去掉**。
         *
         * v3.1.1 曾为了救 sync=true 的掉帧在解码器后插 60 帧 queue，那是基于 filesrc
         * 块读聚合的前提。改成 fdsrc 后数据是逐帧均匀推送的，queue 只会增加起播延迟
         * 和内存占用，实测 fdsrc + sync=true 不带 queue 就是 24.00fps / 大空档 0。
         * 保留为排查开关：/tmp/vp_queue.on → 插回 queue（对照旧行为）。
         * /tmp/vp_noqueue.on 已无意义（默认就没有 queue），保留兼容。 */
        if (access("/tmp/vp_queue.on", F_OK) == 0) {
            argv[i++] = (char *)"queue";
            argv[i++] = (char *)"max-size-buffers=60";
            argv[i++] = (char *)"max-size-bytes=0";
            argv[i++] = (char *)"max-size-time=0";
            argv[i++] = (char *)"!";
        }
        /* v3.1.4 取证开关：/tmp/vp_fps.on 存在时，在 sink 外面套一层 fpsdisplaysink，
         * 让它每秒往日志打一行 `rendered: N, dropped: M, current: X, average: Y`。
         * 这是**唯一可信的渲染帧率判据** —— debugfs 采样器（dev/x6plane2）读数自相矛盾
         * （同一份库连测两轮，一轮 58fps/0 大空档、一轮 27fps/86 次），因为它自己每秒
         * 读 150 次 summary，本身就在干扰 page-flip。fpsdisplaysink 直接在 sink 的
         * chain 里计数，不受采样开销影响。
         *
         * gst-launch 把 argv 用空格拼成管线，属性值里带空格必须**整段塞进一个 argv**
         * 并用双引号包住（video-sink="kmssink ..."），否则会被当成多个元素。 */
        /* ---- v3.1.3 sink 参数定案：sync=true + max-lateness=-1 + qos=false ----
         *
         * 这是「约一秒卡一次」的最终修复。三条参数必须**成组**使用，缺一条就退化：
         *
         *   sync=true      让 sink 按 PTS 对齐 vblank 做 page-flip，把网络抖动的
         *                  ±100ms 到达抖动挡在显示层之外。缺它 → 出帧跟着上游写的
         *                  瞬时节奏走，帧间隔忽快忽慢（真机 74 次大空档/20s）。
         *   qos=false      从源头关掉 QoS 事件上报。
         *   max-lateness=-1 让 sink 永不因"迟到"丢帧（-1 = unlimited）。
         *
         * 为什么后两条是必需的：kmssink 默认 `qos=true` 且 max-lateness 默认 20ms，
         * 而 mppvideodec 在本机的 PTS 是坏的（/tmp/vp_gst.log 里明确写着
         * `mppdec gstmppdec.c:562 MPP is not able to generate pts`）。于是每一帧
         * 都被判"迟到" → sink 回 QoS 事件 → videodecoder 主动丢帧 →
         * 真机实测只剩 **0.90fps**，日志里 18872 条 `Dropping frame due to QoS`。
         * 一度误判成"sync 有问题"而改成 sync=false（28.8fps 但大空档 74 次），
         * 加上后两条再测 sync=true 才得到 29.95fps / 大空档 1 次 —— 坏的不是 sync，
         * 是"迟到就丢"这条默认策略。
         *
         * 排查开关：/tmp/vp_nosync.on → sync=false（回退对照）。 */
        {
            /* ⚠️ 坑（真机踩过，代价是整屏没画面）：gst-launch 会把**含空格的 argv
             * 元素整段加引号**。曾把 `plane-id=76 driver-name=rockchip sync=true ...`
             * 拼成一个 argv 传进去，结果 gst 报
             *   could not set property "plane-id" ... to "76 driver-name=rockchip ..."
             * 即所有属性被当成 plane-id 的值 → 管线构建失败 → 只剩音频在响。
             * 所以：**普通属性必须一个 argv 一个**；只有 video-sink 这种"值本身带
             * 空格"的属性才故意拼成带引号的单段（那是 gst 语法要求）。 */
            char sync[24];
            snprintf(sync, sizeof(sync), "sync=%s",
                    access("/tmp/vp_nosync.on", F_OK) == 0 ? "false" : "true");
            if (access("/tmp/vp_fps.on", F_OK) == 0) {
                char vs[400];
                snprintf(vs, sizeof(vs),
                        "video-sink=\"kmssink plane-id=" KMS_ID_STR " driver-name=rockchip %s "
                        "max-lateness=-1 qos=false force-aspect-ratio=false %s\"", sync, rect);
                argv[i++] = (char *)"fpsdisplaysink";
                argv[i++] = (char *)"signal-fps-measurements=true";
                argv[i++] = (char *)"fps-update-interval=1000";
                argv[i++] = (char *)"silent=false";
                argv[i++] = strdup(vs);
            } else {
                argv[i++] = (char *)"kmssink";
                argv[i++] = (char *)"plane-id=" KMS_ID_STR;
                argv[i++] = (char *)"driver-name=rockchip";
                argv[i++] = strdup(sync);
                argv[i++] = (char *)"max-lateness=-1";
                argv[i++] = (char *)"qos=false";
                argv[i++] = (char *)"force-aspect-ratio=false";
                argv[i++] = rect;
            }
        }
        argv[i] = NULL;
        {
            /* 取证：子进程确实走到 execv（真机出现「gst exit=0 但日志 0 字节」时，
             * 需要区分「exec 没跑」与「gst 跑了但一句话都没输出」）。 */
            int mfd = open("/tmp/vp_gst_exit.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (mfd >= 0) {
                const char *m = "CHILD execv gst-launch\n";
                (void)!write(mfd, m, strlen(m));
                close(mfd);
            }
        }
        execv(GST_LAUNCH_PATH, argv);
        {
            int mfd = open("/tmp/vp_gst_exit.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (mfd >= 0) {
                const char *m = "CHILD execv FAILED\n";
                (void)!write(mfd, m, strlen(m));
                close(mfd);
            }
        }
        _exit(127);
    }
    g_rd.gst_pid = pid;
    {
        int dfd = open("/tmp/vp_gst_exit.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (dfd >= 0) {
            char sl[128];
            struct stat pst;
            memset(&pst, 0, sizeof(pst));
            (void)stat(g_rd.path, &pst);
            int sl_n = snprintf(sl, sizeof(sl), "t=%.1f START pid=%ld ino=%llu rect=%d,%d,%d,%d rot=%d\n",
                    now_seconds(), (long)pid, (unsigned long long)pst.st_ino, px, y, w, h, rot);
            if (sl_n > 0) (void)!write(dfd, sl, (size_t)sl_n);
            close(dfd);
        }
    }
    g_rd.fifo_fd = fifo_open_write(g_rd.path, 4000);
    if (g_rd.fifo_fd < 0) {
        render_stop();
        return -1;
    }
    /* v3.0.2：给 gst 一点时间真正进入 read 循环。
     * 真机现象：fifo_open_write（O_NONBLOCK）在 gst 刚 open 完 FIFO 就立刻返回，
     * 播放器随即一次性灌入首个 AU（~140KB）；而 gst 此时还在 filesrc 的
     * "opening file / pad not activated yet" 阶段，实测随后就 exit=0（日志停在
     * "Pipeline is PREROLLING"）。手动用 shell 阻塞式 open 喂同样的数据从不复现。
     * 这里补 250ms 让 filesrc 的 start/激活完成，代价只有起播多 0.25s。 */
    usleep(250000);
    {
        struct stat fst;
        memset(&fst, 0, sizeof(fst));
        (void)fstat(g_rd.fifo_fd, &fst);
        int dfd = open("/tmp/vp_gst_exit.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (dfd >= 0) {
            char sl[128];
            int sl_n = snprintf(sl, sizeof(sl), "t=%.1f FIFO fd=%d ino=%llu\n",
                    now_seconds(), g_rd.fifo_fd, (unsigned long long)fst.st_ino);
            if (sl_n > 0) (void)!write(dfd, sl, (size_t)sl_n);
            close(dfd);
        }
    }
    g_rd.w = w;
    g_rd.h = h;
    g_rd.rx = x;
    g_rd.ry = y;
    g_rd.started = 1;
    return 0;
}

/* 统一入口：有 fbdev 走 memcpy，没 fbdev 走 KMS overlay */
static int fb_open(void) {
    if (g_fb.map) return 0;
#ifndef PENBILI_KMS_ONLY /* -DPENBILI_KMS_ONLY：X6 Pro 无 /dev/fb0，构建期直接跳过 fbdev 探测 */
    if (fbdev_open() == 0) return 0;
#endif
    /* X6 Pro 分支：没有 /dev/fb0，改为 KMS，首帧时按实际矩形建链 */
    g_rd.kms = 1;
    g_rd.rx = -1;
    g_rd.ry = -1;
    g_rd.started = 0;
    return 0;
}

/* v3.0.0：把一段压缩字节写进 FIFO（建链按需、矩形变了就重建）。
 * 返回 0 = 已写出；-1 = 失败（调用方按"显示链断开"处理）；1 = stop_flag 中止。
 * 注意：这里**不再有帧尺寸概念**，w/h 只用于算 render-rectangle。 */
/* 视频平面是否真的在扫帧（v3.0.1）。
 * 为什么要这个：`video_first_frame` 以前在 `fb_write_stream()` **之前**就置 1，于是音频起播门
 * 立刻放行开写；而此时首帧才刚进 FIFO，gst-launch 还在 PREROLL + 首帧解码，KMS 平面要几百毫秒
 * 后才有画面 → 用户听到的就是「先出声、后出画」。
 * 直接读 rockchip 的 DRM summary 判 `Esmart1-win0`（plane 76）是否 ACTIVE 且 buf[0].addr 非 0，
 * 这是「画面真的在扫」的权威信号，比拍脑袋 sleep 一个固定时长可靠。 */
#define KMS_SUMMARY_PATH "/sys/kernel/debug/dri/0/summary"
#define KMS_VIDEO_WIN "Esmart1-win0"

static int kms_video_active(void) {
    char buf[8192];
    char *p;
    int fd;
    ssize_t n;
    int active = 0;
    fd = open(KMS_SUMMARY_PATH, O_RDONLY);
    if (fd < 0) return 1; /* 读不到就当已上屏，别把起播卡死 */
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 1;
    buf[n] = '\0';
    p = strstr(buf, KMS_VIDEO_WIN);
    if (!p) return 0;
    /* 该窗口的块到下一个 "win_id:" 或文件尾为止 */
    {
        char *end = strstr(p + 1, "win_id:");
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char blk[1024];
        if (len >= sizeof(blk)) len = sizeof(blk) - 1;
        memcpy(blk, p, len);
        blk[len] = '\0';
        if (!strstr(blk, "ACTIVE")) return 0;
        if (!strstr(blk, "buf[0]: addr: 0x0000000000000000")) active = 1;
    }
    return active;
}

/* 等首帧真正落到 KMS 平面（最多 max_ms）。返回 1 = 已上屏，0 = 超时。 */
static int wait_kms_video(double max_ms) {
    double t0 = now_seconds();
    while (now_seconds() - t0 < max_ms / 1000.0) {
        if (kms_video_active()) return 1;
        usleep(10000);
    }
    return kms_video_active();
}

static int fb_write_stream(const unsigned char *data, size_t n, int x, int y, int w, int h,
                           const volatile int *stop) {
    int attempt;
    if (!g_rd.kms || !data || n == 0 || w <= 0 || h <= 0) return -1;
    for (attempt = 0; attempt < 2; attempt++) {
        if (!g_rd.started || g_rd.w != w || g_rd.h != h || g_rd.rx != x || g_rd.ry != y) {
            if (render_start(x, y, w, h) != 0) return -1;
        }
        {
            int r;
            /* 诊断（v3.0.2）：区分「gst 在我们写之前就死了」与「写完才死」。
             * 真机现象是 gst 停在 "Pipeline is PREROLLING ..." 就 exit=0，
             * 日志里看不到任何 EOS/协商错误 → 必须知道它死在写入前还是后。 */
            if (g_rd.gst_pid > 0) {
                int wst = 0;
                pid_t wr = waitpid(g_rd.gst_pid, &wst, WNOHANG);
                if (wr == g_rd.gst_pid) {
                    int mfd = open("/tmp/vp_gst_exit.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
                    if (mfd >= 0) {
                        char ml[128];
                        int mn = snprintf(ml, sizeof(ml), "t=%.1f DEAD_BEFORE_WRITE status=0x%x exit=%d n=%zu\n",
                                now_seconds(), wst, WIFEXITED(wst) ? WEXITSTATUS(wst) : -1, n);
                        if (mn > 0) (void)!write(mfd, ml, (size_t)mn);
                        close(mfd);
                    }
                    g_rd.gst_pid = 0;
                }
            }
            /* 诊断：PENBILI_DUMP_FIFO=<path> 把真正写进 FIFO 的压缩字节原样落盘，
             * 用于「播放器里 gst 秒退、手动喂同样 gst 却正常」时做字节级回放比对。 */
            if (g_rd.dump_fd < 0) g_rd.dump_fd = open("/tmp/dump.h264", O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (g_rd.dump_fd >= 0 && n > 0) {
                size_t dn = 0;
                while (dn < n) {
                    ssize_t dw = write(g_rd.dump_fd, data + dn, n - dn);
                    if (dw <= 0) break;
                    dn += (size_t)dw;
                }
            }
            r = fifo_write_all(g_rd.fifo_fd, data, n, stop);
            if (r == 0) return 0;
            if (r == 1) return 1;
        }
        /* gst 侧断了（EPIPE）：先把死因落到 /tmp/vp_gst_exit.log —— 真机排「视频显示链断开」
         * 时，光看 /tmp/vp_gst.log 分不清是 gst 自己退出（解码/协商错误）还是写端时序问题。 */
        if (g_rd.gst_pid > 0) {
            int st = wait_child(g_rd.gst_pid, 300);
            int dfd = open("/tmp/vp_gst_exit.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (dfd >= 0) {
                char el[128];
                int el_n = snprintf(el, sizeof(el),
                        "t=%.1f pid=%ld status=0x%x exit=%d sig=%d n=%zu\n",
                        now_seconds(), (long)g_rd.gst_pid, st,
                        WIFEXITED(st) ? WEXITSTATUS(st) : -1,
                        WIFSIGNALED(st) ? WTERMSIG(st) : -1, n);
                if (el_n > 0) (void)!write(dfd, el, (size_t)el_n);
                close(dfd);
            }
            g_rd.gst_pid = 0;
        }
        /* 重建链再试。重建后 h264parse 需要 SPS/PPS —— 由调用方在下一批数据的 AU 开头
         * 自带（Annex-B 流的 IDR 前必有 SPS/PPS，源流如此）。 */
        render_stop();
    }
    return -1;
}

/* v3.0.0：KMS overlay 下"重绘"是空操作 —— 平面自己持着最后一帧（旧链路要靠重贴 RGB
 * 才能恢复，因为 plane 内容会被别的东西冲掉；现在 gst 自己占着 plane，无需外部重贴）。
 * fbdev 后端（X5）行为完全不变。 */
static int fb_blit(const unsigned char *frame, int x, int y, int w, int h) {
    if (g_fb.map) return fbdev_blit(frame, x, y, w, h);
    if (g_rd.kms) return 0;
    return -1;
}

static void fb_close(void) {
    render_stop();
    g_rd.kms = 0;
    fbdev_close();
}

static int fb_current_buffer(void) {
    if (g_rd.kms) return 0;
    return fbdev_current_buffer();
}

static int fb_buffers_reported(void) {
    if (g_rd.kms) return 1;
    return g_fb.buffers;
}

/* ---------------- session ---------------- */

struct session {
    pthread_mutex_t mu;
    int state;
    int has_session;
    pid_t ff_pid;
    pid_t aplay_pid;
    int video_fd;
    int audio_fd;
    int aplay_fd;
    int out_w;
    int out_h;
    int rect_x;
    int rect_y;
    int rect_w;
    int rect_h;
    int fps;
    int transpose;
    long start_ms;
    long duration_ms;
    long position_ms;
    long frames;
    int audio_enabled;   /* 请求带音频 */
    int audio_is_bt;     /* 本次会话是否把音频送到了蓝牙 */
    int audio_rate;      /* 音频采样率：44100 内置 / 48000 蓝牙；0=尚未探测（open 时置0） */
    char audio_dev[128]; /* 实际使用的 aplay -D 设备（空=ALSA 默认） */
    int has_audio;
    long audio_bytes;
    long audio_dropped;
    int paced;           /* 无音频时用时钟节拍 */
    char error[192];
    char input[1024];
    char audio_input[1024]; /* DASH 第二输入（音频轨 URL）；空 = 单文件；seek/重启复用 */
    char user_agent[256]; /* 空 = 不给 ffmpeg 传 -user_agent */
    char referer[520];    /* 空 = 不给 ffmpeg 传 -headers */
    unsigned char *frame;
    int frame_valid;
    pthread_t vth;
    pthread_t ath;
    int vth_started;
    int ath_started;
    volatile int stop_flag;
    struct timespec started_at;
    int retried_no_audio;

    /* ---- audio push（v1.3.0 完全重写：feeder 入环 + writer 按水位整形）----
     * 旧架构 audio_thread 直写 aplay：ffmpeg 产率与蓝牙消费率刚性耦合，
     * 任一侧抖动直接变成"闪断/单边/失真"（v1 起即存在，2026-09-24 用户授权重写）。
     * 新架构：feeder 只管入环（满则阻塞保背压），writer 以起播预蓄+水位滞回整形出环。
     * v1.5.0：环扩到 512KB（≈2.7s @48k16b2ch）——1.6s 防御下仍被 >1.6s 的网络/CPU
     * 场景波动穿透成"零星卡顿"（实测 underrun 650-1200ms 与 826ms 防御吻合），再抬一档。 */
    unsigned char ring[524288]; /* 512KB ≈ 2.74s */
    size_t rpos;                /* 读偏移 */
    size_t rlen;                /* 环内可读字节 */
    pthread_t wth;
    int wth_started;
    pid_t ff_a_pid;    /* v1.6.0 音频独立 ffmpeg 进程（DASH 双流时） */
    long writer_bytes; /* writer 实际写出量 = 真实播放时钟（拆进程后音画同步的锚） */
    int audio_alive;   /* 音频产出存活（音频进程死后=0 → 视频转墙钟节拍降级） */
    volatile int render_paused; /* 暂停 fb 输出（评论面板/系统UI覆盖时交出显示权；解码与音频照常） */
    volatile int video_first_frame; /* A/V 起播门（v1.7.1）：视频首帧已产出 → writer 放行开写 */
    volatile int gate_passed;    /* 门等待已结束（正常开/15s超时放行都置1）→ gateActive=!passed */
    double last_blit_at;        /* 最近一帧产出的墙钟秒（巡检基准 → status.videoStallMs） */
    double gate_wait_ms;        /* 起播门实际等待时长（诊断 → status.gateWaitMs） */
    /* v3.0.1 画面节拍器：以「固定帧间隔 + PLL 慢速纠偏」代替「硬等音频锚」。
     * 理由见 video_thread 里的注释（音频锚是 46ms 台阶量，硬等会把台阶传导成画面卡顿）。 */
    double last_pace_at;        /* 上一次节拍放行时刻（秒，单调钟） */
    long underruns;             /* writer 等水位/空环轮次（≥1 即发生过欠载等待） */
    long wr_errors;             /* aplay 写失败次数（设备崩溃/断流） */
    long ring_drops;            /* 环满被丢弃的字节数（蓝牙消费慢的直接证据） */
    long video_skips;           /* v1.8.0 丢帧快进计数（追音频钟时丢弃的帧数，status 诊断） */
    /* v2.1.0 音画同步分层（平滑快进 / 强制重同步） */
    int resyncing;              /* 视频流重启中（首帧未产出）→ status.resyncing → JS "加载中…" */
    long resync_count;          /* 漂移触发的强制重同步次数（诊断；不含断流重启） */
    long resync_lead_ms;        /* 重启落点前移量：预估启动时延（每次重启实测自适应） */
    double resync_at;           /* 最近一次视频流重启的墙钟秒（漂移重同步冷却基准） */
    long last_drift_ms;         /* 最近一次计算的音画漂移 apos−due（诊断） */
    int resync_burst;           /* v2.2.0 滚动窗口内已触发重同步次数（限流用） */
    int resync_timeout;         /* v2.2.5 因首帧超时被判失败（用于失败分类：应报错误而非"已播完"，
                                 * 并允许"起播阶段"也走重试路径） */
    double video_spawn_at;      /* v2.2.5 最近一次启动/重启视频流的墙钟（首帧超时判定基准） */
    double resync_burst_at;     /* v2.2.0 滚动窗口起点墙钟 */
    int ever_played;            /* v2.1.0 本会话是否产出过帧（重启后 frames 归零，据此区分"中途断流"与"从未出画"） */
    long probe_audio_bytes;     /* v2.1.0 空转期巡检用：上次看到的音频供给量（判定音频链是否在供数） */
    int stall_resyncs;          /* v2.1.0 连续"空转期重同步"次数（读到帧即清零；≥3 放弃让 JS 巡检接管） */
    struct timespec paused_at;  /* v1.8.0 暂停起点（resume 时补偿墙钟基准，防 paced 快进闪跳） */
    long pipe_cap_bytes;        /* v1.9.0 aplay 管道容量（F_GETPIPE_SZ 实测，缺省按 64KB） */
    long video_pipe_bytes;      /* v2.9.3 视频 pipe 实际容量（诊断） */
    long pace_align_ms;         /* v2.9.3 直播墙钟节拍：画面目标的固定后移量（自适应对齐 A/V） */
    long alsa_buf_bytes;        /* v1.9.0 ALSA 缓冲字节（aplay -v setup 解析；0=未解析到不修正） */
    long audio_base_ms;         /* v1.9.3 音频时钟基准（writer_bytes==0 时的内容位置，= 起播 start_ms） */
    long video_base_ms;         /* v1.9.3 视频时钟基准（断流恢复后独立重置，通常=audio_base） */
    int video_retries;          /* v1.9.3 视频流中途断流重启次数（上限 5，防无限循环） */

    /* ---- v3.0.0 硬解：stderr 泵（解析真实帧率 / seek 落点）+ Annex-B AU 累积 ---- */
    int stderr_fd;              /* ffmpeg stderr 读端（泵线程独占） */
    pthread_t sth;              /* stderr 泵线程 */
    int sth_started;
    int fps_known;              /* 1 = 已从流信息里解析出真实帧率（覆盖 JS 传值） */
    int seek_pending;           /* 1 = 本次 spawn 带 -ss，等 -debug_ts 报出真实落点 */
    int seek_land_valid;        /* 1 = seek_land_ms 已解析 */
    long seek_land_ms;          /* 落点相对 -ss 的偏移（实测 -ss 10.5 → -500ms，即关键帧 10.0s） */
    int seek_land_applied;      /* 1 = 已修正 video_base_ms（只修正一次） */
    volatile int pump_quit;     /* stderr 泵线程退出标志（stop_flag 不适用：重启时不能置） */
    unsigned char *au_buf;      /* Annex-B 累积缓冲（变长压缩字节，按 AU 边界切分后转发） */
    size_t au_cap;
    size_t au_len;
    long au_bytes;              /* 累计转发的压缩字节数（诊断） */
    int video_eof;              /* 1 = 视频 pipe 已 EOF（自然结束，不再走断流重试） */
};

static struct session g_s;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static long long ms_since(const struct timespec *t0) {
    struct timespec ts;
    long long ms;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ms = (long long)(ts.tv_sec - t0->tv_sec) * 1000LL + (long long)(ts.tv_nsec - t0->tv_nsec) / 1000000LL;
    return ms < 0 ? 0 : ms;
}

static void set_error(struct session *s, const char *msg) {
    size_t n;
    if (!msg) msg = "";
    n = strlen(msg);
    if (n >= sizeof(s->error)) n = sizeof(s->error) - 1;
    memcpy(s->error, msg, n);
    s->error[n] = '\0';
}

/* 读日志尾部作为错误提示（ffmpeg stderr 重定向到这里） */
static void capture_log_tail(struct session *s) {
    char buf[1024];
    int fd = open(LOG_PATH, O_RDONLY);
    ssize_t r;
    size_t i;
    if (fd < 0) return;
    r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0) return;
    buf[r] = '\0';
    /* 取最后一行非空内容 */
    for (i = (size_t)r; i > 1; i--) {
        if (buf[i - 1] == '\n' && i - 2 < (size_t)r && buf[i - 2] != '\n') {
            size_t end = i - 1;
            size_t start = end;
            while (start > 0 && buf[start - 1] != '\n') start--;
            buf[end] = '\0';
            if (end > start) set_error(s, buf + start);
            return;
        }
    }
    set_error(s, buf);
}

/* v3.0.0 硬解链路下 video pipe 里不再是定长像素帧，read_full 已无调用点（保留备查）。 */
static int read_full(int fd, unsigned char *buf, size_t n) __attribute__((unused));
static int read_full(int fd, unsigned char *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r > 0) {
            got += (size_t)r;
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static int wait_child(pid_t pid, int timeout_ms) {
    int waited = 0;
    int status = 0;
    if (pid <= 0) return 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return status;
        if (r < 0) return -1;
        if (waited >= timeout_ms) break;
        usleep(50000);
        waited += 50;
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return status;
}

static void kill_child(pid_t *pid) {
    if (*pid <= 0) return;
    kill(*pid, SIGTERM);
    wait_child(*pid, 1500);
    *pid = 0;
}

static void close_fd(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/* ---------------- 子进程 ---------------- */

/* 提升 /proc/sys/fs/pipe-max-size（best-effort，进程内只做一次）。
 * 笔端默认 1MB，而视频 pipe 想扩到 8MB（≈17 帧 ≈0.57s 蓄水）—— 2026-10-07 真机查明：
 * 8MB 申请一直被 sysctl 上限顶回去（F_SETPIPE_SZ 超过 pipe-max-size 需要 CAP_SYS_RESOURCE，
 * 本 app 虽有 root 却拿不到），结果视频 pipe 只有 1MB（≈2 帧），**几乎没有抗抖动余量**。
 * 直接把 sysctl 放宽最省事（app 以 root 跑，实测可写）。失败无害：仍是 1MB 兜底。 */
/* v2.9.3 诊断：视频 pipe 扩容结果（跨会话保留，便于排查 F_SETPIPE_SZ 是否真的生效） */
static long g_vpipe_bytes = -1;
static int g_vpipe_errno = 0;

/* ---- v2.2.8: -reconnect 只能用于网络输入 ----
 * `-reconnect*` 是 **http/https 协议的私有 AVOption**，不是通用格式选项。喂本地文件
 * （/tmp/x.mp4、本地转码缓存、file:// 源）时 ffmpeg 4.4 会在协议选项解析阶段直接退出：
 *     Option reconnect not found.
 * 症状：open() 返回 ok，但随后一帧都收不到，status 里 error="Option reconnect not found"。
 * X5 上本地播放同样受影响，故三处 argv（视频带音轨 / 视频无音轨 / 独立音频）都要按协议门控。 */
static int is_net_input(const char *url) {
    if (!url || !url[0]) return 0;
    return strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0 ||
           strncmp(url, "rtmp://", 7) == 0 || strncmp(url, "rtsp://", 7) == 0 ||
           strncmp(url, "tcp://", 6) == 0;
}

static void ensure_pipe_max_raised(void) {
    static int done = 0;
    int fd;
    if (done) return;
    done = 1;
    fd = open("/proc/sys/fs/pipe-max-size", O_WRONLY);
    if (fd >= 0) {
        const char *v = "67108864"; /* 64MB */
        if (write(fd, v, strlen(v)) < 0) {
            /* 只读挂载 / 无权限：忽略 */
        }
        close(fd);
    }
}

/* ---- v3.0.0：ffmpeg stderr 泵 ----
 * 为什么需要：copy 模式下有两件事**只能从 ffmpeg 日志里拿到**，而且都不能等进程结束：
 *   ① **真实帧率**：`-c:v copy` 没有 `-r` 重采样，输出帧率 = 源帧率。JS 侧恒传 24
 *      （src/services/bili/playurl.js 的 HTML5_FPS），但 B 站 DASH/直播多为 30fps →
 *      节拍按 24 算会慢 25%，漂移一路涨到强制重同步（每 12s 跳一次画面）。
 *      从 `Stream #0:0: Video: ... 30 fps,` 解析出真值覆盖 s->fps。
 *   ② **seek 落点**：`-ss` + copy 只能落在**前一个关键帧**（实测 -ss 10.5 → 首包 pts 10.0），
 *      不像软解能解码丢帧到精确点。`-debug_ts` 会打出 `demuxer+ffmpeg ... pkt_pts_time:-0.5`
 *      （= 首包相对 -ss 的偏移）→ 用它把 video_base_ms 修正成关键帧的真实内容时间。
 * 顺带把 stderr 转存到 LOG_PATH（保持 capture_log_tail 的取证能力），并**持续抽干**
 * 管道——否则 64KB stderr 管道写满会把 ffmpeg 堵死（这是改成管道后必须配套做的事）。 */
static void ff_stderr_pump_stop(struct session *s);
static void *ff_stderr_thread(void *arg);

/* 从 "… 30 fps," / "… 24 fps," 里取帧率。只在含 "Video:" 的 Stream 行上找，
 * 避免命中 "tbr"/"tbn" 那些同样带数字的字段。 */
static int parse_fps_line(const char *line) {
    const char *p = line;
    if (!strstr(line, "Video:")) return 0;
    while ((p = strstr(p, " fps")) != NULL) {
        const char *q = p;
        int v = 0;
        int nd = 0;
        while (q > line && q[-1] >= '0' && q[-1] <= '9') {
            q--;
            nd++;
        }
        if (nd > 0 && nd <= 3) {
            v = atoi(q);
            if (v >= 1 && v <= 120) return v;
        }
        p += 4;
    }
    return 0;
}

/* 从 "demuxer+ffmpeg ... pkt_pts_time:-0.5" 取首包相对落点（秒，带小数）。
 * 返回 1 = 解析成功。 */
static int parse_land_line(const char *line, double *out_sec) {
    const char *p;
    if (!strstr(line, "demuxer+ffmpeg")) return 0;
    p = strstr(line, "pkt_pts_time:");
    if (!p) return 0;
    p += strlen("pkt_pts_time:");
    if (!((*p >= '0' && *p <= '9') || *p == '-' || *p == '+')) return 0;
    *out_sec = atof(p);
    return 1;
}

static void ff_stderr_pump_stop(struct session *s) {
    if (s->sth_started) {
        s->pump_quit = 1;
        pthread_join(s->sth, NULL);
        s->sth_started = 0;
    }
    s->pump_quit = 0;
    close_fd(&s->stderr_fd);
}

static void *ff_stderr_thread(void *arg) {
    struct session *s = (struct session *)arg;
    char buf[8192];
    char line[1024];
    size_t llen = 0;
    int logfd = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int fd = s->stderr_fd;

    while (fd >= 0 && !s->pump_quit) {
        struct pollfd p;
        ssize_t r;
        size_t i;
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, 200) <= 0) continue; /* 200ms 轮询：ffmpeg 死/会话停都能及时退出 */
        r = read(fd, buf, sizeof(buf));
        if (r <= 0) break; /* EOF：ffmpeg 退出 */
        for (i = 0; i < (size_t)r; i++) {
            char ch = buf[i];
            if (ch == '\n' || ch == '\r' || llen + 1 >= sizeof(line)) {
                line[llen] = '\0';
                if (llen > 0) {
                    int f;
                    double land;
                    if (!s->fps_known && (f = parse_fps_line(line)) > 0) {
                        /* v3.2.0：用户设了帧率上限时，节拍器必须按**出帧**帧率走，
                         * 不能按源帧率 —— videorate 把 60fps 源降到 30fps 后，
                         * 节拍器若仍按 60fps 排程（16ms/帧）就会每帧都等不到新画面。 */
                        if (g_rd.fps_cap > 0 && f > g_rd.fps_cap) f = g_rd.fps_cap;
                        s->fps = f;
                        g_rd.fps = f;
                        s->fps_known = 1;
                    }
                    if (s->seek_pending && !s->seek_land_valid &&
                        parse_land_line(line, &land) == 1) {
                        /* 落点只可能 ≤ 0（-ss 之后的首包必在 -ss 点之前或正好在它上面）。
                         * 夹在 [-10s, 0] 防脏值把时钟推飞。 */
                        if (land > 0.0) land = 0.0;
                        if (land < -10.0) land = -10.0;
                        s->seek_land_ms = (long)(land * 1000.0);
                        s->seek_land_valid = 1;
                    }
                    /* -debug_ts 是**每包两行**的洪水（2h 片 ≈ 100MB），而 /tmp 是 tmpfs
                     * （吃 RAM）。取证只留真正的诊断/错误行 → 丢弃含 "demuxer" 的包级行。 */
                    if (logfd >= 0 && !strstr(line, "demuxer")) {
                        if (write(logfd, line, llen) < 0) { /* ignore */ }
                        if (write(logfd, "\n", 1) < 0) { /* ignore */ }
                    }
                }
                llen = 0;
                continue;
            }
            line[llen++] = ch;
        }
    }
    if (logfd >= 0) close(logfd);
    return NULL;
}

static int spawn_ffmpeg(struct session *s, long start_ms, int with_audio) {
    int vpipe[2] = { -1, -1 };
    int apipe[2] = { -1, -1 };
    int spipe[2] = { -1, -1 }; /* v3.0.0：stderr 改走管道 → 解析真实帧率 / seek 落点 */
    pid_t pid;
    char ss[32];
    char arbuf[16]; /* 音频采样率（蓝牙 48000 / 内置 44100） */
    char hdr[544]; /* "Referer: <url>\r\n"，仅当设置了 referer 才用 */
    char logfd_path_check[8];

    if (pipe(vpipe) != 0) return -1;
    if (with_audio && pipe(apipe) != 0) {
        close(vpipe[0]);
        close(vpipe[1]);
        return -1;
    }
    if (pipe(spipe) != 0) {
        close(vpipe[0]);
        close(vpipe[1]);
        if (apipe[0] >= 0) close(apipe[0]);
        if (apipe[1] >= 0) close(apipe[1]);
        return -1;
    }
    /* 视频 pipe 扩容（v1.9.2：8MB）/ 音频 256KB：
     * 1MB≈2.2 帧 → ffmpeg 无蓄水空间，弱网突发停顿直接击穿实时（2026-10-01 soak 实测：
     * drift 3s 内 +700~1000ms 的下载停顿波形，解码吞吐本身在 fps24 下够用）。
     * 8MB≈17 帧≈0.57s 解码前置存量，停顿期由存量顶上（与音频侧管道+环形预蓄同思想）。
     * v2.9.3：先 ensure_pipe_max_raised() 把 sysctl 放宽，否则这一扩容**一直没生效**（实测仍 1MB）。
     * 8MB 这个值不是随手取的：它≈音频侧固有延迟（aplay 管道 64KB + ALSA ≈0.56s），
     * 两边同量级 → 画面与可闻声天然对齐（见 video_thread 的 align 补偿）。 */
    ensure_pipe_max_raised();
    {
        /* v2.9.3：8MB → 20MB。8MB≈0.57s 太浅（真机"卡一下→突然快放"的循环里停顿常 >0.5s）。
         * 20MB≈45 帧≈**1.5s** 解码前置存量：停顿由存量顶上、画面不冻 → 不需要追赶 →
         * 从根上消掉"卡-追"循环。深度**须略小于**音频侧延迟（直播 aplay 管道 256KB + ALSA
         * ≈1.68s），差额交给 video_thread 的自适应 align 补平。 */
        int cap = 20 * 1024 * 1024;
        int got = -1;
        while (cap >= 1048576) {
            got = fcntl(vpipe[1], F_SETPIPE_SZ, cap);
            if (got != -1) break;
            cap /= 2;
        }
        if (got <= 0) {
            g_vpipe_errno = errno;                            /* 失败原因（EPERM/EINVAL/...） */
            got = fcntl(vpipe[1], F_GETPIPE_SZ);              /* 退回实测值（默认 64KB） */
        }
        g_vpipe_bytes = got > 0 ? got : 0;
        s->video_pipe_bytes = g_vpipe_bytes;
        /* v2.9.4：趋势文件随会话起始清空（避免多次会话累积） */
        truncate(TREND_LOG_PATH, 0);
        /* v2.9.3 诊断落盘（/tmp/vp_pipe.log）：F_SETPIPE_SZ 是否真的把视频 pipe 扩容成功。 */
        {
            int dfd = open("/tmp/vp_pipe.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (dfd >= 0) {
                char lb[160];
                int n = snprintf(lb, sizeof(lb), "video pipe want=20971520 got=%d bytes=%ld errno=%d\n",
                                 got, g_vpipe_bytes, g_vpipe_errno);
                if (n > 0) {
                    if (write(dfd, lb, (size_t)n) < 0) { /* ignore */ }
                }
                close(dfd);
            }
        }
    }
    if (with_audio) fcntl(apipe[1], F_SETPIPE_SZ, 262144);
    /* v3.0.0：视频 pipe 不再搬像素，而是搬**压缩包**（copy 模式）。
     * 20MB 压缩流 ≈ 数百秒（实测 640x360 24fps 码率 ~730kbps → 20MB ≈ 220s），
     * 远深于旧的 1.5s 像素蓄水 → 网络抖动几乎不再穿透到画面。
     * 保留原扩容逻辑（值不必变，深一点无害：pipe 是内核环形缓冲，不占用户态内存）。 */
    snprintf(ss, sizeof(ss), "%ld.%03ld", (long)(start_ms / 1000), (long)(start_ms % 1000));
    snprintf(arbuf, sizeof(arbuf), "%d", s->audio_rate > 0 ? s->audio_rate : 44100);
    (void)logfd_path_check;

    pid = fork();
    if (pid < 0) {
        close(vpipe[0]);
        close(vpipe[1]);
        if (apipe[0] >= 0) close(apipe[0]);
        if (apipe[1] >= 0) close(apipe[1]);
        return -1;
    }
    if (pid == 0) {
        int logfd;
        /* 子进程：视频 → fd3，音频 → fd4，stderr → spipe[1]（父进程泵线程解析帧率/落点） */
        setpgid(0, 0);
        if (dup2(vpipe[1], 3) < 0) _exit(126);
        if (with_audio) {
            if (dup2(apipe[1], 4) < 0) _exit(126);
        }
        close(vpipe[0]);
        close(vpipe[1]);
        if (apipe[0] >= 0) close(apipe[0]);
        if (apipe[1] >= 0) close(apipe[1]);
        if (dup2(spipe[1], 2) < 0) _exit(126);
        close(spipe[0]);
        close(spipe[1]);
        logfd = -1; /* v3.0.0：stderr 不再直接落文件（改由父进程泵线程转存 LOG_PATH） */
        (void)logfd;
        if (with_audio) {
            char *argv[80]; /* 双输入 × (probesize/rw_timeout/reconnect/thread_queue_size/ss/ua/headers)
                             * 后最多 70 项，留 NULL 余量 */
            int i = 0;
            argv[i++] = "ffmpeg";
            argv[i++] = "-nostdin";
            argv[i++] = "-hide_banner";
            /* v3.0.0：copy 模式仍需 info 级日志 —— 要从中解析**真实帧率**（Input #0 的
             * "24 fps"）与 **seek 落点**（-debug_ts 的 pkt_pts_time）。日志由父进程泵线程
             * 实时抽干（写 LOG_PATH + 解析），不会把 64KB stderr 管道堵死 ffmpeg。 */
            argv[i++] = "-loglevel";
            argv[i++] = "info";
            if (start_ms > 0) {
                argv[i++] = "-debug_ts"; /* 仅 seek 时开：定位 copy 模式的真实落点（见文件头 ②） */
            }
            /* v3.0.0 移除：-threads / -skip_loop_filter / -sws_flags —— 全是**解码器/滤镜**
             * 选项，copy 模式下不参与（-skip_loop_filter 还会因找不到解码器而报错）。 */
            if (s->user_agent[0]) {
                argv[i++] = "-user_agent";
                argv[i++] = s->user_agent;
            }
            if (s->referer[0]) {
                snprintf(hdr, sizeof(hdr), "Referer: %s\r\n", s->referer);
                argv[i++] = "-headers";
                argv[i++] = hdr;
            }
            /* 收缩探测窗口：DASH 双 http 输入并发探测在窄带宽（实测 646kbps）下拖死首帧；
             * mp4 moov+首帧 64KB 足够定流（每输入前各放一份，为 input 选项） */
            argv[i++] = "-probesize";
            argv[i++] = "65536";
            argv[i++] = "-analyzeduration";
            argv[i++] = "5000000";
            argv[i++] = "-rw_timeout";
            /* v2.2.5：保持 15s。曾试 8s（想"在缓冲窗口内就发现断档"），但实测证明**重启是破坏性的**：
             * 重启会丢掉已下好的 ~20s 缓冲，若断档仍在（涓流），新 ffmpeg 反而在探流阶段饿死。
             * 满断档本来就由 20s 缓冲吸收（缓冲有数据时画面照常播、漂移不涨），rw_timeout 只是
             * "断档确实持续"的确认 → 越晚确认越好（给网络恢复留时间）→ 与缓冲同量级 15s 最合适。 */
            argv[i++] = "15000000";
            if (is_net_input(s->input)) { /* v2.2.8: 仅网络协议支持 -reconnect */
                argv[i++] = "-reconnect";
                argv[i++] = "1"; /* v1.9.3：CDN 断流协议级重连（Range 续传），修"视频/音频进程中途退出" */
                argv[i++] = "-reconnect_streamed";
                argv[i++] = "1";
                argv[i++] = "-reconnect_delay_max";
                argv[i++] = "10";
            }
            argv[i++] = "-thread_queue_size";
            argv[i++] = AV_PREFETCH_PACKETS_VIDEO_STR; /* v2.2.0 输入线程预取压缩包（≈15~25s） */
            argv[i++] = "-ss";
            argv[i++] = ss;
            argv[i++] = "-i";
            argv[i++] = s->input;
            if (with_audio && s->audio_input[0]) {
                /* DASH 第二输入：音频轨独立下载（实测 ~66kbps，带宽富余 10 倍 → 声音供给恒稳）。
                 * -ss/UA/headers 逐输入配齐：seek 同起点、CDN 反爬同凭据。 */
                argv[i++] = "-probesize";
                argv[i++] = "65536";
                argv[i++] = "-analyzeduration";
                argv[i++] = "5000000";
                argv[i++] = "-rw_timeout";
                argv[i++] = "15000000";
                if (is_net_input(s->audio_input)) { /* v2.2.8: 仅网络协议支持 -reconnect */
                    argv[i++] = "-reconnect";
                    argv[i++] = "1";
                    argv[i++] = "-reconnect_streamed";
                    argv[i++] = "1";
                    argv[i++] = "-reconnect_delay_max";
                    argv[i++] = "10";
                }
                argv[i++] = "-thread_queue_size";
            argv[i++] = AV_PREFETCH_PACKETS_AUDIO_STR; /* v2.2.0 音轨独立预取（≈18s） */
                argv[i++] = "-ss";
                argv[i++] = ss;
                if (s->user_agent[0]) {
                    argv[i++] = "-user_agent";
                    argv[i++] = s->user_agent;
                }
                if (s->referer[0]) {
                    argv[i++] = "-headers";
                    argv[i++] = hdr;
                }
                argv[i++] = "-i";
                argv[i++] = s->audio_input;
            }
            /* ---- v3.0.0 视频输出：**只 demux + copy**，解码交给 gst 的 mppvideodec ----
             * 原来这里是 `-vf scale=..,transpose=..,format=rgb32 -r <fps> -f rawvideo`：
             * 软解 + 每帧 sws 转换 + 每帧 0.5MB memcpy 进 pipe，X6 Pro 上实测只有 1.1~1.9x
             * 实时；改成 copy 后本进程只搬压缩包（~90KB/s），CPU 几乎归零，且**省掉整条
             * 像素链**。旋转（transpose）与缩放（scale）都下移到 gst：前者是 mppvideodec 的
             * rotation 属性（解码器内完成，实测等价），后者由 kmssink 的 render-rectangle
             * 交给 KMS/RGA 硬件做。
             * `-bsf:v h264_mp4toannexb`：把 AVCC（长度前缀）转成 Annex-B（起始码）——
             * 下游 h264parse 与本模块的 AU 切分都按起始码识别。`-f h264` 是裸流 muxer。 */
            argv[i++] = "-map";
            argv[i++] = "0:v:0";
            argv[i++] = "-c:v";
            argv[i++] = "copy";
            argv[i++] = "-bsf:v";
            argv[i++] = "h264_mp4toannexb";
            argv[i++] = "-f";
            argv[i++] = "h264";
            argv[i++] = "pipe:3";
            /* v2.9.4：音频输出必须受 with_audio 门控 —— 拆分模式（DASH / 直播双进程）下
             * 视频进程只出视频；旧代码无条件加音频输出，会去写**未被 dup2 的 fd4**
             * （with_audio=0 时不创建音频 pipe）→ ffmpeg 立即失败。 */
            if (with_audio) {
            argv[i++] = "-map";
            argv[i++] = s->audio_input[0] ? "1:a:0?" : "0:a:0?";
            /* async 已移除（第五档回滚）：aresample=async 是变速重采样，会拉伸/压缩音频
             * 填补时间戳空洞 → "声音变粗/变调"；且其变速输出扰乱 SBC 编码同步 → 左右耳单边。
             * 症状时间线：两者均自引入 async 的 v3 起出现，v2（无 async）从未有过。 */
            argv[i++] = "-ac";
            argv[i++] = "2";
            argv[i++] = "-ar";
            argv[i++] = arbuf;
            argv[i++] = "-f";
            argv[i++] = "s16le";
            argv[i++] = "pipe:4";
            } /* end if (with_audio) */
            argv[i] = NULL;
            /* 视频 ffmpeg 提优先级（v1.9.4，nice -5 与音频解码同档）。v3.0.0 起本进程只做
             * demux+copy，CPU 占用极小，但保留优先级无害（解包线程仍在搬运网络数据）。 */
            setpriority(PRIO_PROCESS, 0, -5);
            execv(FFMPEG_PATH, argv);
        } else {
            char *argv[48];
            int i = 0;
            argv[i++] = "ffmpeg";
            argv[i++] = "-nostdin";
            argv[i++] = "-hide_banner";
            argv[i++] = "-loglevel";
            argv[i++] = "info"; /* v3.0.0：同双输出分支，父进程泵线程解析帧率/落点 */
            if (start_ms > 0) {
                argv[i++] = "-debug_ts";
            }
            /* 拆进程后 dash 视频走本分支 —— 探测上限必须与音频分支一致：
             * 缺失时默认 probesize(5MB) 在烂网络上 = 首帧 10s+/seek 12s 零帧（1.6.0 实测） */
            argv[i++] = "-probesize";
            argv[i++] = "65536";
            argv[i++] = "-analyzeduration";
            argv[i++] = "5000000";
            argv[i++] = "-rw_timeout";
            argv[i++] = "15000000"; /* v2.2.5：同上（与 20s 缓冲同量级；早退会白丢缓冲） */
            if (is_net_input(s->input)) { /* v2.2.8: 仅网络协议支持 -reconnect */
                argv[i++] = "-reconnect";
                argv[i++] = "1"; /* v1.9.3：CDN 断流协议级重连（Range 续传） */
                argv[i++] = "-reconnect_streamed";
                argv[i++] = "1";
                argv[i++] = "-reconnect_delay_max";
                argv[i++] = "10";
            }
            if (s->user_agent[0]) {
                argv[i++] = "-user_agent";
                argv[i++] = s->user_agent;
            }
            if (s->referer[0]) {
                snprintf(hdr, sizeof(hdr), "Referer: %s\r\n", s->referer);
                argv[i++] = "-headers";
                argv[i++] = hdr;
            }
            argv[i++] = "-thread_queue_size";
            argv[i++] = AV_PREFETCH_PACKETS_VIDEO_STR; /* v2.2.0 输入线程预取压缩包（≈15~25s） */
            argv[i++] = "-ss";
            argv[i++] = ss;
            argv[i++] = "-i";
            argv[i++] = s->input;
            argv[i++] = "-an";
            argv[i++] = "-map";
            argv[i++] = "0:v:0";
            argv[i++] = "-c:v";
            argv[i++] = "copy";
            argv[i++] = "-bsf:v";
            argv[i++] = "h264_mp4toannexb";
            argv[i++] = "-f";
            argv[i++] = "h264";
            argv[i++] = "pipe:3";
            argv[i] = NULL;
            setpriority(PRIO_PROCESS, 0, -5); /* 同上（v1.9.4）：DASH 视频分支 */
            execv(FFMPEG_PATH, argv);
        }
        _exit(127);
    }
    /* 父进程 */
    close(vpipe[1]);
    if (apipe[1] >= 0) close(apipe[1]);
    close(spipe[1]);
    s->ff_pid = pid;
    /* v3.0.0：起 stderr 泵线程（抽干 + 转存 LOG_PATH + 解析帧率/落点）。
     * 必须先 join 上一代（否则多次 seek 会累积线程），join 前 ffmpeg 已 kill → 管道 EOF。 */
    ff_stderr_pump_stop(s);
    s->stderr_fd = spipe[0];
    s->fps_known = 0;
    s->seek_pending = (start_ms > 0) ? 1 : 0;
    if (pthread_create(&s->sth, NULL, ff_stderr_thread, s) == 0) s->sth_started = 1;
    else {
        close_fd(&s->stderr_fd);
        s->seek_pending = 0;
    }
    s->video_fd = vpipe[0];
    /* v2.1.1 修复：仅当本次进程带音频输出时才接管 audio_fd。拆分模式（with_audio=0，DASH）
     * 下音频由 spawn_audio_ffmpeg 的独立进程经自己的 pipe 供给；旧代码无条件写 apipe[0]
     * （此时恒为 -1）→ "重启视频流"会把音频管道 fd 清成 -1，feeder 随即 EBADF 退出 →
     * 声音断供（真机实测：ab 冻结 + underrun，而音频 ffmpeg 其实还活着）。断流重连
     * （v1.9.3）与强制重同步都走本函数，故两者此前都会误伤音频链。 */
    if (with_audio) s->audio_fd = apipe[0];
    return 0;
}

/* 自动挑选音频输出设备：
 *   本机音频走 ALSA；内置扬声器是默认设备，蓝牙耳机则要显式指定 bluealsa 的 PCM。
 *   `bluealsa-aplay -L` 的输出形如（真机实测）：
 *       bluealsa:SRV=org.bluealsa,DEV=<BT_MAC>,PROFILE=a2dp  （真机地址不入库）
 *           Redmi Buds 6, trusted audio-card, playback
 *   注意 DEV 前面还有 SRV=...，所以必须匹配 "bluealsa:" 而不是 "bluealsa:DEV="，
 *   否则永远匹配不到、音频就一直在内置扬声器上（已踩过）。
 * 返回 0 表示找到了蓝牙设备。
 *
 * v2.3.1（X6 Pro 真机）：bluealsa-aplay -L 在本机版本上**直接崩溃**——
 *     $ /usr/bin/bluealsa-aplay -L
 *     *** stack smashing detected ***: terminated
 *     Aborted
 *   （v3.0.0, 43344 B, Mar 18 2024；耳机已 Connected 时同样崩。）
 *   由于 abort 前 stdout 未 flush，popen 永远读到空 ⇒ 蓝牙永远选不中。
 *   故加 BlueZ 回退：bluetoothctl devices → 逐台 info 判 Connected/Audio Sink。 */
#define BTCTL_PATH "/usr/bin/bluetoothctl"
#define BT_MAX_DEVS 8

/* 单台设备是否「已连接 + 有 Audio Sink」→ 1/0。一次 popen 一次 info。 */
static int bt_dev_is_connected_sink(const char *mac) {
    char cmd[192];
    char line[256];
    FILE *fp;
    int connected = 0;
    int sink = 0;
    if (!mac || !mac[0]) return 0;
    snprintf(cmd, sizeof(cmd), "%s info '%s' 2>/dev/null", BTCTL_PATH, mac);
    fp = popen(cmd, "r");
    if (!fp) return 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Connected: yes")) connected = 1;
        /* A2DP Sink = 0000110b（耳机作为我们推流的接收端）。AVRCP-only 的
         * 设备能连但放不出声，必须排除，否则会静默无声。 */
        else if (strstr(line, "Audio Sink")) sink = 1;
    }
    pclose(fp);
    return (connected && sink) ? 1 : 0;
}

/* 回退路径：扫已配对设备，挑第一台连接的 A2DP Sink。0 = 找到。 */
static int detect_bt_pcm_bluez(char *out, int n) {
    char macs[BT_MAX_DEVS][24];
    int n_mac = 0;
    int i;
    char cmd[192];
    char line[256];
    FILE *fp;

    snprintf(cmd, sizeof(cmd), "%s devices 2>/dev/null", BTCTL_PATH);
    fp = popen(cmd, "r");
    if (!fp) return -1;
    while (fgets(line, sizeof(line), fp) && n_mac < BT_MAX_DEVS) {
        /* "Device B0:A3:F2:AD:7E:FA Redmi Buds 6" */
        char mac[24];
        if (strncmp(line, "Device ", 7) != 0) continue;
        if (sscanf(line + 7, "%23s", mac) != 1) continue;
        if (strlen(mac) != 17 || mac[2] != ':' || mac[5] != ':') continue;
        snprintf(macs[n_mac], sizeof(macs[n_mac]), "%s", mac);
        n_mac++;
    }
    pclose(fp); /* 先关掉外层 pipe 再逐台 info，避免管道缠绕 */

    for (i = 0; i < n_mac; i++) {
        if (!bt_dev_is_connected_sink(macs[i])) continue;
        snprintf(out, n, "bluealsa:DEV=%s,PROFILE=a2dp", macs[i]);
        return 0;
    }
    return -1;
}

static int detect_bt_pcm(char *out, int n) {
    FILE *fp;
    char line[256];
    if (!out || n <= 0) return -1;
    out[0] = '\0';
    fp = popen(BT_LIST_CMD, "r");
    if (!fp) return -1;
    while (fgets(line, sizeof(line), fp)) {
        char *p = strstr(line, "bluealsa:");
        if (p) {
            char *end = p;
            while (*end && *end != ' ' && *end != '\n' && *end != '\r' && *end != '\t') end++;
            if (end > p && (int)(end - p) < n) {
                memcpy(out, p, (size_t)(end - p));
                out[end - p] = '\0';
                pclose(fp);
                return 0;
            }
        }
    }
    pclose(fp);
    /* v2.3.1：bluealsa-aplay -L 在本机崩溃 ⇒ 回退 BlueZ，否则蓝牙永远选不中。 */
    if (detect_bt_pcm_bluez(out, n) == 0) {
        fprintf(stderr, "[PLAYER] bt via bluetoothctl: %s\n", out);
        return 0;
    }
    return -1;
}

/* ---------------- v2.3.0: X6 Pro audio route (Playback Path) ----------------
 * Symptom (X6 Pro fw4.3.5, measured on device): aplay is running,
 * /proc/asound/card1/pcm0p/sub0/status = RUNNING, Master 88%, yet the speaker is
 * completely silent. Cause: the rk817 codec mixer control `Playback Path`
 * (numid=45) defaults to OFF at boot, so the amp path is not connected:
 *     amixer -c 0 sget 'Playback Path'   ->  Item0: 'OFF'
 *     amixer -c 0 sset 'Playback Path' 'SPK'  ->  sound immediately
 * On X5 a system audio daemon did this; on X6 Pro PulseAudio is unreachable
 * (`amixer: Mixer attach default error: Connection refused`) so nobody does.
 * The player therefore does it itself.
 *
 * Design: only write when the current value IS "OFF". If the user picked
 * headphones (HP) or bluetooth (BT) we leave it alone, so starting playback
 * never yanks audio back to the speaker. All failures are silent (X5 has no
 * such control / no amixer). */
#define AMIXER_PATH "/bin/amixer"
#define AMIXER_CARD "0"
#define AMIXER_CTL "Playback Path"
#define AMIXER_CTL_SPK "SPK"     /* bluetooth still goes out via bluealsa (-D bluealsa:...), codec route stays SPK */

/* Read the current enum name ("OFF"/"SPK"/"HP"/"BT"...). 0 = read ok. */
static int mixer_get_playback_path(char *out, int n) {
    char cmd[192];
    FILE *fp;
    int rc = -1;
    if (!out || n <= 0) return -1;
    out[0] = '\0';
    snprintf(cmd, sizeof(cmd), "%s -c %s cget name='%s' 2>/dev/null",
             AMIXER_PATH, AMIXER_CARD, AMIXER_CTL);
    fp = popen(cmd, "r");
    if (!fp) return -1;
    {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            /* 只认枚举值那一行：`  ; Item #0 'OFF'`
             * v2.3.2 修 bug：amixer cget 的**首行**是
             *   numid=45,iface=MIXER,name='Playback Path'
             * 也是单引号包着的，旧代码取「第一对单引号」会拿到控件名本身，
             * 于是 cur="Playback Path" 永远 != "OFF" ⇒ 自愈静默失效
             * （真机 mixtest get 实测输出 current = 'Playback Path' 即证据）。 */
            char *item = strstr(line, "Item #");
            char *q1;
            char *q2;
            if (!item) continue;
            q1 = strchr(item, '\'');
            if (!q1) continue;
            q2 = strchr(q1 + 1, '\'');
            if (q2 && q2 > q1 + 1 && (int)(q2 - q1 - 1) < n) {
                memcpy(out, q1 + 1, (size_t)(q2 - q1 - 1));
                out[q2 - q1 - 1] = '\0';
                rc = 0;
                break;
            }
        }
    }
    pclose(fp);
    return rc;
}

static int mixer_set_playback_path(const char *value) {
    char cmd[256];
    int st;
    if (!value || !value[0]) return -1;
    snprintf(cmd, sizeof(cmd), "%s -c %s sset '%s' '%s' >/dev/null 2>&1",
             AMIXER_PATH, AMIXER_CARD, AMIXER_CTL, value);
    st = system(cmd);
    return (st == 0) ? 0 : -1;
}

/* Call before spawning aplay. want_bt only affects the log line; the codec route
 * is always SPK because bluealsa does not go through rk817 routing. */
static void audio_route_ensure(int want_bt) {
    char cur[64];
    if (mixer_get_playback_path(cur, sizeof(cur)) != 0) return; /* no such control (X5 / other) */
    if (strcmp(cur, "OFF") != 0) return;                        /* already routed -> respect it */
    if (mixer_set_playback_path(AMIXER_CTL_SPK) == 0) {
        fprintf(stderr, "[PLAYER] Playback Path OFF -> %s (bt=%d)\n", AMIXER_CTL_SPK, want_bt);
    }
}

/* ---------------- v1.6.0：音频独立 ffmpeg 进程（网易云级待遇）----------------
 * 对照解剖（2026-09-24）：网易云=系统 SoundPlayer，纯 bluealsa D-BUS 路径（与我们同栈、
 * 同 --sbc-quality=0），不碰内核声卡 —— 输出路径无差异；它不卡的本质是
 * "本地文件供给 + 单音频解码近零负载"。而 v1.5.0 的盲区：ffmpeg 进程内音频解码/输出段
 * 与 3 个视频解码线程同进程共享调度 —— 高动态场景视频解码一涨，音频段被挤饿，
 * 环补不进 → 零星卡（与"前30秒好、复杂画面卡"的时间线吻合）。
 * 拆分后：音频=66kbps AAC 单线程独立进程（≈5% CPU），永不与视频共享调度域。 */
static int spawn_ffaudio(struct session *s) {
    int apipe[2] = { -1, -1 };
    pid_t pid;
    char ss[32];
    char arbuf[16];
    char hdr[544];

    if (pipe(apipe) != 0) return -1;
    fcntl(apipe[1], F_SETPIPE_SZ, 262144);
    snprintf(ss, sizeof(ss), "%ld.%03ld", (long)(s->start_ms / 1000), (long)(s->start_ms % 1000));
    snprintf(arbuf, sizeof(arbuf), "%d", s->audio_rate > 0 ? s->audio_rate : 44100);

    pid = fork();
    if (pid < 0) {
        close(apipe[0]);
        close(apipe[1]);
        return -1;
    }
    if (pid == 0) {
        int logfd;
        int i = 0;
        char *argv[48];
        setpgid(0, 0);
        if (dup2(apipe[1], 4) < 0) _exit(126);
        close(apipe[0]);
        close(apipe[1]);
        logfd = open(LOG_AUDIO_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644); /* 音频进程独立日志 */
        if (logfd >= 0) {
            dup2(logfd, 2);
            close(logfd);
        }
        /* 音频解码进程提优先级（相对视频进程 nice 0）：66k 解码极轻，-5 即够 */
        setpriority(PRIO_PROCESS, 0, -5);
        argv[i++] = "ffmpeg";
        argv[i++] = "-nostdin";
        argv[i++] = "-hide_banner";
        argv[i++] = "-loglevel";
        argv[i++] = "error";
        argv[i++] = "-threads";
        argv[i++] = "1"; /* 音频单线程：与视频解码零共享 */
        if (s->user_agent[0]) {
            argv[i++] = "-user_agent";
            argv[i++] = s->user_agent;
        }
        if (s->referer[0]) {
            snprintf(hdr, sizeof(hdr), "Referer: %s\r\n", s->referer);
            argv[i++] = "-headers";
            argv[i++] = hdr;
        }
        argv[i++] = "-probesize";
        argv[i++] = "65536";
        argv[i++] = "-analyzeduration";
        argv[i++] = "5000000";
        argv[i++] = "-rw_timeout";
        argv[i++] = "15000000";
        if (is_net_input(s->audio_input)) { /* v2.2.8: 仅网络协议支持 -reconnect */
            argv[i++] = "-reconnect";
            argv[i++] = "1"; /* v1.9.3：音频轨断流重连（soak 末段实测过 CDN 断流杀进程） */
            argv[i++] = "-reconnect_streamed";
            argv[i++] = "1";
            argv[i++] = "-reconnect_delay_max";
            argv[i++] = "10";
        }
        argv[i++] = "-thread_queue_size";
        argv[i++] = AV_PREFETCH_PACKETS_AUDIO_STR; /* v2.2.0 音轨独立预取（≈18s） */
        argv[i++] = "-ss";
        argv[i++] = ss;
        argv[i++] = "-i";
        argv[i++] = s->audio_input;
        argv[i++] = "-map";
        argv[i++] = "0:a:0";
        argv[i++] = "-ac";
        argv[i++] = "2";
        argv[i++] = "-ar";
        argv[i++] = arbuf;
        argv[i++] = "-f";
        argv[i++] = "s16le";
        argv[i++] = "pipe:4";
        argv[i] = NULL;
        execv(FFMPEG_PATH, argv);
        _exit(127);
    }
    close(apipe[1]);
    s->audio_fd = apipe[0];
    s->ff_a_pid = pid;
    return 0;
}

static int spawn_aplay(struct session *s, const char *device) {
    int ppipe[2] = { -1, -1 };
    pid_t pid;
    const char *use = device;
    char devbuf[128];

    devbuf[0] = '\0';
    s->audio_is_bt = 0;
    s->audio_dev[0] = '\0';
    if (!use || !use[0]) {
        /* 空 = 自动：已连接蓝牙耳机就走蓝牙（bluealsa），否则用内置扬声器 */
        if (detect_bt_pcm(devbuf, sizeof(devbuf)) == 0) {
            use = devbuf;
            s->audio_is_bt = 1;
        }
    } else if (strcmp(use, "speaker") == 0 || strcmp(use, "default") == 0) {
        use = NULL; /* 显式要求内置扬声器 */
    }
    if (use && use[0]) {
        strncpy(s->audio_dev, use, sizeof(s->audio_dev) - 1);
        s->audio_dev[sizeof(s->audio_dev) - 1] = '\0';
    }

    /* v2.3.0: power up the codec route (OFF -> SPK) before aplay starts writing,
     * otherwise the first chunk of audio is produced into a dead amp path. */
    audio_route_ensure(s->audio_is_bt);

    if (pipe(ppipe) != 0) return -1;
    pid = fork();
    if (pid < 0) {
        close(ppipe[0]);
        close(ppipe[1]);
        return -1;
    }
    if (pid == 0) {
        char *argv[24];
        int i = 0;
        char ratebuf[16];
        snprintf(ratebuf, sizeof(ratebuf), "%d", s->audio_rate > 0 ? s->audio_rate : 44100);
        setpgid(0, 0);
        if (dup2(ppipe[0], 0) < 0) _exit(126);
        close(ppipe[0]);
        close(ppipe[1]);
        {
            /* aplay 的 stdout+stderr 都落盘（v1.9.0 起加 -v：setup dump 含 ALSA 实际
             * buffer_size/start_threshold，是音频锚"未发声存量"的权威实测值） */
            int alog = open(APLAY_LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (alog >= 0) {
                dup2(alog, 1);
                dup2(alog, 2);
                close(alog);
            }
        }
        argv[i++] = "aplay";
        argv[i++] = "-v"; /* v1.9.0：setup dump → APLAY_LOG_PATH（probe_alsa_buffer 解析） */
        /* 诊断期不加 -q：ALSA underrun/overrun 告警进 /tmp/vp_aplay.log（音频卡顿关键证据） */
        if (use && use[0]) {
            argv[i++] = "-D";
            argv[i++] = (char *)use;
        }
        argv[i++] = "-t";
        argv[i++] = "raw";
        argv[i++] = "-f";
        argv[i++] = "S16_LE";
        argv[i++] = "-r";
        argv[i++] = ratebuf;
        if (s->audio_is_bt) {
            /* A2DP：v1.3.x 定档 600/100 —— 与环滞回(250/1200)组成 ~1.6s 抗断防御；
             * 历史实测：400ms(826ms总防御) 以下被 >1s 波动穿透 → underrun 650-1200ms 可闻 */
            argv[i++] = "-B";
            argv[i++] = "600000";
            argv[i++] = "-F";
            argv[i++] = "100000";
        }
        argv[i++] = "-c";
        argv[i++] = "2";
        argv[i++] = "-";
        argv[i] = NULL;
        /* aplay（音频消费端）提优先级：不被 ffmpeg 软解线程周期性抢占 → 供给抖动=零星卡顿。
         * child 已独立进程组（setpgid），process 级设置仅影响自身。root 下生效，失败无害。 */
        setpriority(PRIO_PROCESS, 0, -10);
        execv(APLAY_PATH, argv);
        _exit(127);
    }
    close(ppipe[0]);
    s->aplay_pid = pid;
    s->aplay_fd = ppipe[1];
    /* v2.9.3：直播把 writer→aplay 管道加深到 256KB（≈1.45s）。
     * 笔端的"缓冲深度"由视频/音频两条管道共同决定（容量 = 流水线延迟）——
     * 视频侧 24MB≈2.3s、音频侧 256KB+ALSA≈1.64s，同量级才能"既加缓冲又保持音画同步"
     * （video_thread 的 align 用实测 pipe_cap_bytes 自动把画面目标后移同量，见那里）。
     * 只对直播加：VOD 走音频锚，加深只平白增加起播/定位延迟。 */
    if (s->duration_ms <= 0) {
        fcntl(ppipe[1], F_SETPIPE_SZ, 262144);
    }
    /* v1.9.0：实测管道容量（未设 F_SETPIPE_SZ 通常是 64KB）——音频锚要扣的"未发声存量"之一 */
    {
        int cap = fcntl(ppipe[1], F_GETPIPE_SZ);
        s->pipe_cap_bytes = cap > 0 ? (long)cap : 65536;
    }
    return 0;
}

/* ---------------- 线程 ---------------- */

/* 线程内的状态更新故意不加锁：
 * stop_children() 是在持有 s->mu 的情况下 join 本线程的，
 * 若这里再去抢 s->mu 会直接死锁。stop_flag 先于 kill/join 置位，
 * 因此线程能安全地写完最后状态并退出。 */

/* ---- v1.8.0 音画对齐参数 ---- */
/* 帧落后音频钟超过此值 → 平滑快进。
 * v3.0.1：100 → 200（2026-10-30 真机钉死「约一秒一次卡顿」的根因）。
 *   稳态 drift 天然就停在 ~100ms —— 它不是真的音画不同步，而是 audio_anchor_ms() 里
 *   「pipe 残留 + ALSA 缓冲」估算的固有偏差（实测 audioBufMs 恒 871ms，误差量级 100ms）。
 *   旧的 100ms 阈值让**每一帧**都判定为"滞后"，于是永久走快进分支：以 PACE_CATCHUP_MAX
 *   (1.25x = 37.5fps @30fps) 出帧，读前视缓冲被抽干 → 空转等网络 → 节奏诊断实测
 *   maxgap=126ms 每秒一次，与用户「大概一秒一次的卡顿」完全对应。
 *   放宽到 200ms 后稳态落回「按音频钟匀速等待」分支，上屏节拍恢复 33ms 均匀；
 *   真正超过 200ms 的滞后仍然会被快进追回，观感无损。 */
#define AV_SKIP_MS 200
/* v2.9.3：墙钟节拍下落后时的**最大追赶倍率**（1.25 = 略快一点追上，不出现"突然快放"） */
#define PACE_CATCHUP_MAX 1.25
#define AV_WAIT_CAP_MS 200 /* durl 单进程模式每帧等待上限（video pipe 满互锁死锁的解扣） */
/* v2.1.0 音画同步分层：轻微滞后平滑快进追上；过大滞后强制重同步（跳到同步画面） */
#define AV_RESYNC_MS 1000          /* 帧落后音频钟超过此值 → 强制重同步（"过大"的定义 = 音频最多
                                    * 超前画面 1s）。v2.2.5 曾上调到 3s（想少打扰），但用户实测反馈
                                    * "音频又快于画面" —— 3s 的唇音失配不可接受。v2.2.7 收回 1s：
                                    * 提示频率问题已由 JS 侧"<2.5s 的快速重启不弹提示"解决，不必再用
                                    * 大阈值换安静。0.1~1s 的滞后仍优先交给①平滑快进自愈。 */
#define AV_RESYNC_COOLDOWN_MS 12000 /* 两次强制重同步最小间隔：越短则"跳内容"越少（漂移累积越少），
                                     * 频率问题交给 JS 侧"快速恢复不弹提示"解决 */
#define AV_FIRST_FRAME_MAX_MS 15000 /* v2.2.5 首帧硬上限：每次（重）启视频流后超过此值仍无首帧
                                     * = ffmpeg 在劣化链路上探流饿死（probesize 64KB 等不到，而涓流
                                     * 又不会触发 rw_timeout）→ 按"视频流失败"处理并重试（新连接往往
                                     * 能换到好节点）。**起播阶段同样适用**：否则会永远停在"缓冲中"
                                     * （真机实测：链路 4KB/s 时首帧永不出现，无任何兜底）。 */
#define AV_RESYNC_WINDOW_SEC 60     /* 限流滚动窗口 */
#define AV_RESYNC_BURST_MAX 3       /* 窗口内最多触发次数：超过即停触发（画面退回"落后但连续"） */
#define AV_RESYNC_LEAD_MIN_MS 300  /* 重启落点前移量 clamp 下界 */
#define AV_RESYNC_LEAD_MAX_MS 4000 /* 重启落点前移量 clamp 上界 */

/* 音频可闻锚（v1.9.0）：writer 写出量 − "已写出但尚未发声"的存量。
 * 存量 = aplay 管道容量 + ALSA 缓冲：aplay 消费速率恒等于播放速率
 * （2026-09-30 真机 soak 实测 writer 恒速 1x、u=0、零 hold → 缓冲恒满），
 * 写出量恒领先可闻声一个缓冲容量；不扣掉它，起播 800ms 预蓄会让视频
 * 丢帧追一个假超前，蓝牙(-B 600000)下画面全程领先声音 ~0.9s。
 * 两个用量在稳态下与启动阈值无关（填满即恒定）；供给停滞时真实存量下降、
 * 本估算偏高 → 画面滞后 ≤ 一个缓冲容量，恢复供给后经丢帧追赶自动回正。
 * 返回相对 start_ms 的毫秒（W 从会话起计）。 */
/* aplay 管道中"已写入但尚未被 aplay 读走"的字节数（FIONREAD 实测）。
 * v2.9.4：取代"管道容量"作为扣除量 —— 旧公式假设管道**总是满的**，一旦未满就会
 * **高估**可闻位置（认为声音比实际更靠前）→ 伺服把画面往前提 → 撞 align 0 地板后
 * 再也无法补偿累积滞后（真机现象：连续播 30 分钟后音频落后画面 ~1.5s）。
 * 内核 4.19 的 pipe_ioctl(FIONREAD) 不检查读写端，写端调用同样返回管道占用；失败回退容量。 */
static long pipe_pending_bytes(const struct session *s) {
    int n = -1;
    if (s->aplay_fd >= 0 && ioctl(s->aplay_fd, FIONREAD, &n) == 0 && n >= 0) {
        return (long)n;
    }
    return (long)s->pipe_cap_bytes; /* 回退：沿用旧的"按容量假设" */
}

static long long audio_anchor_ms(const struct session *s) {
    double bytes_per_ms = ((double)(s->audio_rate > 0 ? s->audio_rate : 44100)) * 4.0 / 1000.0;
    long long audible = (long long)s->writer_bytes - (long long)pipe_pending_bytes(s) - s->alsa_buf_bytes;
    if (audible < 0) audible = 0;
    return (long long)((double)audible / bytes_per_ms);
}

/* v2.9.4 直播长跑趋势落盘：约 1 行/秒（调用点在 video_thread 墙钟分支的每秒边界）。
 * 动机：笔息屏后 JS 层被系统挂起（console.warn 停写），长时间（30min+）音画漂移取证
 * 只能由 native 自己完成。字段与 JS 侧 poll tick 对齐，外加 pipe（FIONREAD 实测占用）。 */
static void dump_trend(const struct session *s) {
    char lb[440];
    long in_pipe = pipe_pending_bytes(s);
    double bpm = ((double)(s->audio_rate > 0 ? s->audio_rate : 44100)) * 4.0 / 1000.0;
    long long aud, apos, content;
    int n;
    if (bpm <= 0) return;
    aud = (long long)s->writer_bytes - (long long)in_pipe - s->alsa_buf_bytes;
    if (aud < 0) aud = 0;
    apos = (long long)((double)aud / bpm);
    content = s->video_base_ms + (long long)((double)s->frames * 1000.0 / (double)s->fps);
    n = snprintf(lb, sizeof(lb),
                 "t=%lld f=%ld pos=%lld ab=%ld wb=%ld rlen=%ld pipe=%ld cap=%ld alsa=%ld "
                 "apos=%lld d=%lld pal=%ld sk=%ld u=%ld rd=%ld rst=%d\n",
                 ms_since(&s->started_at), s->frames, content, s->audio_bytes, s->writer_bytes,
                 (long)s->rlen, in_pipe, s->pipe_cap_bytes, s->alsa_buf_bytes,
                 apos, apos - content, s->pace_align_ms, s->video_skips,
                 s->underruns, s->ring_drops, s->video_retries);
    if (n > 0) {
        int fd = open(TREND_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            if (write(fd, lb, (size_t)n) < 0) { /* ignore */ }
            close(fd);
        }
    }
}

/* 解析 aplay -v 的 setup dump（APLAY_LOG_PATH）取 ALSA 实际 buffer_size（帧）。
 * 调用时机 = writer 预蓄完成后、开写前（距 aplay 起动 ≥800ms，日志必已落盘）。
 * 解析失败保守回退：BT 用 -B 600ms 名义值；扬声器不修正（宁勿过度回退）。 */
static void probe_alsa_buffer(struct session *s) {
    char buf[4096];
    int fd = open(APLAY_LOG_PATH, O_RDONLY);
    if (fd < 0) return;
    {
        ssize_t r = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (r > 0) {
            const char *p;
            buf[r] = '\0';
            p = strstr(buf, "buffer_size");
            if (p) {
                long frames = -1;
                p = strchr(p, ':');
                if (p) frames = strtol(p + 1, NULL, 10);
                if (frames > 0 && frames < 4000000) {
                    s->alsa_buf_bytes = frames * 4; /* S16_LE × 2ch */
                    return;
                }
            }
        }
    }
    if (s->audio_is_bt) {
        s->alsa_buf_bytes = (long)(0.6 * (double)(s->audio_rate > 0 ? s->audio_rate : 44100) * 4.0);
    }
}

/* ---- v3.0.0 Annex-B 访问单元（AU）切分 ----
 * 硬解链路下 video pipe 里是**变长压缩字节流**（ffmpeg -c:v copy -f h264），不再有"定长像素帧"。
 * 但上层语义没变：frames 必须是"已送显的帧数"，position_ms = frames/fps，节拍/漂移/重同步
 * 全部按它算。所以必须把字节流重新切回一帧一帧（AU）再喂给 gst。
 *
 * 切分规则（H.264 Annex-B，够用且不需要 Exp-Golomb 解析）：
 *   顺序扫 NAL 起始码（00 00 01，4 字节起始码 00 00 00 01 会被识别为其后 3 字节）。
 *   当前 AU 内**已经出现过 VCL NAL（type 1..5）**，此时再遇到
 *     · 另一个 VCL NAL（下一帧的第一个 slice），或
 *     · AUD(9) / SPS(7) / PPS(8) / SEI(6)（它们只会出现在新 AU 的开头）
 *   就说明 AU 到此为止。
 * bilibili 的流是「一帧一 slice」，因此 1 AU == 1 帧，frames 与真实帧数一一对应。
 *
 * 返回第一个完整 AU 的字节数；0 = 数据还不够（调用方继续读 pipe）。
 * 流损坏/无起始码时不会无限累积：au_len 超过 AU_FLUSH_CAP 就整块当一帧吐出去。 */
#define AU_FLUSH_CAP (4u * 1024u * 1024u)

/* v3.0.1 读前视目标：把压缩流尽量读进 au_buf，让节拍等待期间缓冲里始终有后续帧。
 * 为什么需要（2026-10-30 真机钉死「约一秒一次卡顿」的根因）：
 *   旧循环是「读 1 个 AU → 等音频钟 → 写出 → 消费」的串行结构，**等待期间完全不读**。
 *   于是 CDN 一旦出现 ~127ms 的交付空档，缓冲里就没有下一帧 → 上屏出现 127ms 空洞
 *   （节奏诊断 maxgap=127 每秒一次，与用户观感完全一致）。
 * 192KB 的压缩流按 60~600kbps 算 = 2.5~25s 的前视，足以吸收任何网络分块抖动；
 * 内存代价 192KB（设备 352MB，可忽略），且 gst 侧不需要改。 */
#define AU_READ_AHEAD (192u * 1024u)

static void au_consume(struct session *s, size_t n);

static size_t au_first_len(struct session *s) {
    size_t i = 0;
    size_t au_start = 0;
    int seen_first = 0;
    int have_vcl = 0;

    if (s->au_len > AU_FLUSH_CAP) return s->au_len; /* 兜底：坏流强制放行 */

    while (i + 3 <= s->au_len) {
        if (s->au_buf[i] == 0 && s->au_buf[i + 1] == 0 && s->au_buf[i + 2] == 1) {
            size_t hdr = i + 3;
            int type;
            int is_vcl;
            if (hdr >= s->au_len) break; /* NAL 头还没到齐 */
            type = s->au_buf[hdr] & 0x1f;
            is_vcl = (type >= 1 && type <= 5);
            if (!seen_first) {
                seen_first = 1;
                au_start = i;
            } else if (have_vcl && (is_vcl || type == 6 || type == 7 || type == 8 || type == 9)) {
                /* 首个起始码之前的字节是垃圾（如 4 字节起始码多出的 1 个 0），先丢掉 */
                if (au_start > 0) au_consume(s, au_start);
                return i - au_start; /* 新 AU 从这里开始 */
            }
            if (is_vcl) have_vcl = 1;
            i = hdr;
            continue;
        }
        i++;
    }
    /* 还没切出完整 AU：顺手丢掉首个起始码之前的垃圾，避免缓冲区无限前移 */
    if (seen_first && au_start > 0) au_consume(s, au_start);
    return 0;
}

/* 追加压缩字节到 AU 累积缓冲（必要时扩容） */
static int au_append(struct session *s, const unsigned char *data, size_t n) {
    if (n == 0) return 0;
    if (s->au_len + n > s->au_cap) {
        size_t cap = s->au_cap ? s->au_cap : 65536;
        unsigned char *nb;
        while (cap < s->au_len + n) cap *= 2;
        nb = (unsigned char *)realloc(s->au_buf, cap);
        if (!nb) return -1;
        s->au_buf = nb;
        s->au_cap = cap;
    }
    memcpy(s->au_buf + s->au_len, data, n);
    s->au_len += n;
    return 0;
}

/* 消费掉前 n 字节（把余下的前移） */
static void au_consume(struct session *s, size_t n) {
    if (n >= s->au_len) {
        s->au_len = 0;
        return;
    }
    memmove(s->au_buf, s->au_buf + n, s->au_len - n);
    s->au_len -= n;
}

/* ---- v2.1.0 视频流重启（强制重同步 / 断流重连共用）---- */

/* 把**视频流**重启到指定内容位置：杀旧视频 ffmpeg → 以 -ss target 重开（音频进程/aplay 不动）。
 * 仅用于 DASH 拆分模式（音频独立进程）——durl 音视频同进程，重启会连带音频。
 * 返回 0 = 已重启（调用方应 `return video_thread(arg)` 重入主循环），-1 = 启动失败。
 * resyncing 置位至首帧产出（JS 显示"加载中…"）。 */
static int respawn_video_at(struct session *s, long long target_ms) {
    if (target_ms < s->start_ms) target_ms = s->start_ms;
    if (s->duration_ms > 0) {
        long long hi = s->start_ms + s->duration_ms - 1000;
        if (hi > s->start_ms && target_ms > hi) target_ms = hi;
    }
    close_fd(&s->video_fd);
    kill_child(&s->ff_pid);
    s->video_base_ms = (long)target_ms;
    s->frames = 0;
    /* v3.0.0：重启视频流 = 全新 Annex-B 序列（新 SPS/PPS + 新 IDR），累积缓冲必须丢弃，
     * 否则上一段流的半截 AU 会与新流首帧粘在一起 → gst 解码花屏/丢帧。 */
    s->au_len = 0;
    s->video_eof = 0;
    s->position_ms = (long)target_ms;
    s->video_first_frame = 1;        /* 起播门已过：重启不再等门 */
    s->last_blit_at = now_seconds(); /* 巡检基线重置，防恢复期被误判停帧 */
    clock_gettime(CLOCK_MONOTONIC, &s->started_at); /* paced 基线一并重置 */
    s->resync_at = now_seconds(); /* 冷却基准：重启后短期内不再触发漂移重同步 */
    s->resyncing = 1;
    s->resync_timeout = 0;
    s->video_spawn_at = now_seconds();
    if (spawn_ffmpeg(s, (long)target_ms, 0) != 0) {
        s->resyncing = 0;
        return -1;
    }
    return 0;
}

/* 触发闸门（v2.2.0）：冷却 + 滚动窗口限流。
 * 为什么需要限流：真机实测（youdao-x5）弱网/重稿件下视频管线吞吐 <1x（软解 + fb 双缓冲写
 * 33ms/帧 ≈ CPU 100%），漂移必然持续增长 → 只靠"冷却一过就重同步"会变成每 20s 跳一段内容的
 * 打转（实测 v2.1.3：rs=9/56s、累计跳过 ~80s 画面）。故窗口内最多 AV_RESYNC_BURST_MAX 次；
 * 超限后不再触发，画面退回"落后但连续"（配合①平滑快进尽量收窄），把加载提示的频次钉住。
 * 返回 1 = 允许触发（并已计入窗口），0 = 拒绝。有副作用，必须放在 && 链末位（前面条件先短路）。 */
static int av_resync_gate(struct session *s) {
    double now = now_seconds();
    if (now - s->resync_at < (double)AV_RESYNC_COOLDOWN_MS / 1000.0) return 0;
    if (now - s->resync_burst_at >= (double)AV_RESYNC_WINDOW_SEC) {
        s->resync_burst = 0;
        s->resync_burst_at = now;
    }
    if (s->resync_burst >= AV_RESYNC_BURST_MAX) return 0;
    s->resync_burst++;
    return 1;
}

/* 落到"当前可闻音频位置 + 预估启动时延"重启视频流：首帧落屏时刻的音频位置 ≈ 首帧内容位置
 * → 落屏即同步。lead 用**残余漂移反馈**自适应（见 video_thread 首帧处）：落点偏了就修多少，
 * 故首次用先验估值、其后一次即可收敛（比直接测"重启耗时"更准——那个量与落点误差不是一回事）。 */
static int restart_video_synced(struct session *s) {
    long long target = (long long)s->audio_base_ms + audio_anchor_ms(s) + (long long)s->resync_lead_ms;
    return respawn_video_at(s, target);
}

static void *video_thread(void *arg) {
    struct session *s = (struct session *)arg;
    unsigned char rd[65536];
    size_t au_n = 0;
    int au_ready = 0;
    int failed = 0;
    int eof_done = 0;
    /* v3.0.0 节奏诊断（排查「约一秒一次卡顿」）：每秒一行，回答"帧是匀速产出还是成串爆发"。
     *   n      = 本秒送出 AU（帧）数
     *   wait   = 本秒在音频钟上等帧的累计毫秒 / maxwait = 单帧最长等待
     *   maxgap = 相邻两次写 FIFO 的最大间隔（= 上屏节拍的最大空档，卡顿的直接证据）
     *   fifo   = 写 FIFO 的累计耗时 / maxfifo = 单次最长（>1 帧时间即为写侧阻塞）
     * 上限 300 行（5 分钟）后停止，避免长播把 /tmp 写满。 */
    double d_t0 = now_seconds();
    long d_n = 0, d_lines = 0;
    double d_wait = 0, d_maxwait = 0, d_maxgap = 0, d_last_au = 0;
    double d_fifo = 0, d_maxfifo = 0;
    double d_last_rd = 0, d_maxrdgap = 0, d_lastau2 = 0, d_maxaugap = 0;

    while (!s->stop_flag) {
        struct pollfd pfd;
        int pr;
        int do_blit = 1;
        ssize_t got;

        /* ---- v3.0.1 读前视：先把 pipe 里已有的压缩字节尽量吸进 au_buf ----
         * 非阻塞（poll timeout 0）：有多少拿多少，拿不到就去走下面的等待/节拍逻辑。
         * 这一步是「上屏不出现网络空档」的关键——见 AU_READ_AHEAD 的注释。 */
        if (s->au_len < AU_READ_AHEAD) {
            for (;;) {
                struct pollfd pf2;
                int pr2;
                ssize_t g2;
                if (s->au_len >= AU_READ_AHEAD) break;
                pf2.fd = s->video_fd;
                pf2.events = POLLIN;
                pf2.revents = 0;
                pr2 = poll(&pf2, 1, 0);
                if (pr2 <= 0) break; /* 没数据了 / 出错（出错交给下面的主 poll 报） */
                g2 = read(s->video_fd, rd, sizeof(rd));
                if (g2 < 0 && errno == EINTR) continue;
                if (g2 < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                if (g2 < 0) { failed = 1; break; }
                if (g2 == 0) { s->video_eof = 1; break; }
                if (au_append(s, rd, (size_t)g2) != 0) { failed = 1; break; }
                {
                    double rn = now_seconds();
                    if (d_last_rd > 0) {
                        double g = (rn - d_last_rd) * 1000.0;
                        if (g > d_maxrdgap) d_maxrdgap = g;
                    }
                    d_last_rd = rn;
                }
            }
            if (failed) break;
        }

        /* ---- v3.0.0：再看累积缓冲里是否已切出完整 AU；有就立刻走下面的节拍逻辑 ---- */
        if (!au_ready && s->au_len > 0) {
            au_n = au_first_len(s);
            if (au_n > 0) {
                au_ready = 1;
                {
                    double an = now_seconds();
                    if (d_lastau2 > 0) {
                        double g = (an - d_lastau2) * 1000.0;
                        if (g > d_maxaugap) d_maxaugap = g;
                    }
                    d_lastau2 = an;
                }
            }
        }

        if (!au_ready) {
            pfd.fd = s->video_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            pr = poll(&pfd, 1, 200);
            if (pr < 0) {
                if (errno == EINTR) continue;
                failed = 1;
                break;
            }
            if (pr == 0) {
            /* v2.1.0 空转期（200ms 无新帧）漂移巡检：纯视频停帧而音频链仍在供数时，漂移检查
             * 因"只在读到帧时才跑"而永不触发 → 画面只能冻到 JS 巡检（8s）做**双进程重启**
             * （实测连带打断音频、恢复期还要重新过起播门 3.5s）。此处直接强制重同步：只重启
             * 视频流，声音不断。
             * 判据 audio_bytes 仍在增长 = 音频链健康、问题在视频侧；音频链也断供（双侧硬断档）
             * 时不抢——否则反复重启视频既救不活音频，又会不断重置 videoStallMs 把 JS 巡检压死。
             * 连续 3 次仍无帧则放弃并清 resyncing，把处置权交回 JS 巡检（防坏节点上无限重启）。 */
            /* v2.2.5 首帧卡死保护（通用：起播与重启都适用）：spawn 后 15s 仍无首帧 = 链路劣化导致
             * ffmpeg 探流饿死（涓流不触发 rw_timeout）→ 判失败走失败路径重试（≤5 次），否则画面
             * 会永久冻结/永远停在"缓冲中"（真机实测过 66s+ 不恢复）。 */
            /* 直播（duration_ms<=0）首帧上限放宽到 60s：外网打洞端口 TCP connect 实测要 5s+，
             * 加上 HLS 探流 + 分片下载，15s 首帧上限必然超时 → 误触发断流重启 → -ss seek
             * 历史位置 → HLS 直播窗口早无此分片 → 卡死循环。VOD 保持 15s。 */
            long ffmax = (s->duration_ms <= 0) ? 60000 : AV_FIRST_FRAME_MAX_MS;
            if (s->frames == 0 && (now_seconds() - s->video_spawn_at) * 1000.0 > (double)ffmax) {
                s->resync_timeout = 1;
                failed = 1;
                break;
            }
            if (!s->paced && s->audio_enabled && s->audio_input[0] &&
                s->frames > 0 && s->audio_alive && s->audio_bytes > s->probe_audio_bytes) {
                if (s->stall_resyncs >= 3) {
                    s->resyncing = 0;
                } else {
                    long long last_pos = s->video_base_ms + (long long)((double)s->frames * 1000.0 / (double)s->fps);
                    long long apos = s->audio_base_ms + audio_anchor_ms(s);
                    if (apos - last_pos > AV_RESYNC_MS && av_resync_gate(s)) {
                        s->last_drift_ms = (long)(apos - last_pos);
                        s->resync_count++;
                        s->stall_resyncs++;
                        if (restart_video_synced(s) == 0) continue;
                    }
                }
            }
            s->probe_audio_bytes = s->audio_bytes;
            continue;
            }
        }
        if (!au_ready) {
            /* 读一段压缩字节追加到 AU 累积缓冲，再尝试切出一个完整 AU。
             * EOF（got==0）且缓冲里还有残留 → 尾帧也吐出去，不丢。 */
            got = read(s->video_fd, rd, sizeof(rd));
            if (got < 0 && errno == EINTR) continue;
            if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (got < 0) {
                failed = 1;
                break;
            }
            if (got == 0) {
                /* pipe 关闭 = ffmpeg 结束。缓冲里可能还有最后 1~2 帧 */
                au_n = s->au_len;
                if (au_n == 0) {
                    failed = 1;
                    break;
                }
                au_ready = 1;
                s->video_eof = 1;
            } else {
                if (au_append(s, rd, (size_t)got) != 0) {
                    failed = 1;
                    break;
                }
                {
                    double rn = now_seconds();
                    if (d_last_rd > 0) {
                        double g = (rn - d_last_rd) * 1000.0;
                        if (g > d_maxrdgap) d_maxrdgap = g;
                    }
                    d_last_rd = rn;
                }
                continue; /* 回去重新切 AU */
            }
        }
        /* 到这儿 au_ready=1 且 au_n>0：缓冲区前 au_n 字节就是一个完整 AU（== 一帧） */
        /* ---- 播出节拍（v1.8.0 重写，锚 = 音频时钟 = writer 写出量换算的播放位置）----
         * 帧到时（apos ≥ due）→ 立即贴屏；帧落后音频钟 → v2.1.0 分两档：轻微滞后平滑快进、
         * 过大滞后强制重同步（见下）；音频时钟死亡（audio_alive=0）→ 墙钟节拍接管（画面续播）。
         * v1.7.1 首帧免钟保留：frames==0 跳过等待直接落屏——起播门等首帧、首帧等 writer
         * 时钟会互锁（writer 不开写则 apos 恒 0，永远追不上 due）。
         * 旧"停滞 8s 转墙钟"移除：音频供给停滞（网络卡）时画面冻结等声才是同步正解——
         * 音频进程 rw_timeout=15s 必然 EOF/出错 → audio_alive=0 才转墙钟；旧的 8s 转墙钟
         * 会在恢复后留下"视频超前音频"的整段错位，且等待期间的超前漂移永远无法回吸。 */
        if (s->paced) {
            /* 墙钟基线对齐到**首帧落屏**时刻（v2.9.3）：spawn→首帧有 1~3s（探流+HLS 起播），
             * 若计入 elapsed，视频会为"追平墙钟"把 HLS 起始缓冲（~3s）快进放掉（起播瞬间快放）。
             * 直播（duration_ms<=0）走这条墙钟路径，故必须对齐。 */
            if (s->frames == 0) clock_gettime(CLOCK_MONOTONIC, &s->started_at);
            /* 直播有音轨：把墙钟目标后移"音频管道固有延迟"（aplay stdin 管道 + ALSA 缓冲换算成 ms）。
             * 直播走墙钟节拍（见 session_start），画面按产出即贴 → 天然领先**可闻**声音约
             * (pipe_cap+alsa)/rate ≈ 0.56s（真机实测 avDrift ≈ -0.7s，与该项吻合）。
             * 补偿后画面与可闻声音对齐（残差 ≈ feeder 管道存量，~0.1s）。VOD 不加（走音频锚）。 */
            long long align = s->pace_align_ms;
            /* 自适应音画对齐（v2.9.3）：直播走墙钟，画面与**可闻**声的固有差 = 各段管道深度之和，
             * 随设备/缓冲配置变化，靠静态公式推不准（真机实测反复对不上）。
             * 这里用慢速伺服：拿实测 avDrift = 可闻音频位置 − 已读帧内容位置，
             * 反向微调"画面目标的固定后移量"把它压到 0（画面超前就再多后移，落后就少后移）。
             * 每 ~1s 动一次、单次 ≤100ms、夹在 [0, 3000]，避免与追赶逻辑互相打架。 */
            if (s->duration_ms <= 0 && s->audio_enabled && s->audio_alive) {
                if (s->frames > 0 && (s->frames % (long)s->fps) == 0) {
                    long long content = s->video_base_ms + (long long)((double)s->frames * 1000.0 / (double)s->fps);
                    long long apos = s->audio_base_ms + audio_anchor_ms(s);
                    long long d = apos - content;      /* 负 = 画面超前（可闻声在后） */
                    /* 符号：align 越大 → 画面越晚贴 → content 越小 → d = apos-content 越大。
                     * 故要把 d 推向 0：d<0（画面超前）要**增大** align；d>0 要减小。
                     * （v2.9.3 首版写反了 → 一路撞 0 地板，偏移失控到 -5.2s。） */
                    if (d > 80 || d < -80) {
                        long long step = -d / 3;
                        if (step > 100) step = 100;
                        if (step < -100) step = -100;
                        s->pace_align_ms += step;
                        if (s->pace_align_ms < 0) s->pace_align_ms = 0;
                        if (s->pace_align_ms > 4000) s->pace_align_ms = 4000;
                    }
                    align = s->pace_align_ms;
                }
            }
            /* v2.9.4：直播每秒落盘一条趋势（息屏后 JS 停写，长跑取证靠这里） */
            if (s->duration_ms <= 0 && s->frames > 0 && s->fps > 0 &&
                (s->frames % (long)s->fps) == 0) {
                dump_trend(s);
            }
            long long target = align + (long long)((double)(s->frames + 1) * 1000.0 / (double)s->fps);
            long long elapsed = ms_since(&s->started_at);
            while (!s->stop_flag && elapsed < target) {
                usleep(5000);
                elapsed = ms_since(&s->started_at);
            }
            if (s->stop_flag) break;
            if (elapsed >= target + AV_SKIP_MS) {
                /* 有界追赶（v2.9.3）：落后时不无限快进，最多 PACE_CATCHUP_MAX 倍速。
                 * 用户反馈：卡一下之后"整个画面突然加快"，很难看。原本落后就逐帧即时贴屏
                 * = 以解码产速快放（可达 2~3 倍）→ 观感是"瞬移式快进"。
                 * 这里给每帧加一个最小间隔（一帧时间 / 倍率），把追赶限在 1.25 倍速：
                 * 观感是"略快一点点追上"，且仍能最终追平墙钟（延迟不永久累积）。 */
                double min_gap = 1000.0 / ((double)s->fps * PACE_CATCHUP_MAX);
                double since = (now_seconds() - s->last_blit_at) * 1000.0;
                s->video_skips++;
                if (since < min_gap) {
                    usleep((useconds_t)((min_gap - since) * 1000.0));
                }
                do_blit = 1;
            }
        } else if (s->audio_enabled) {
            long long due = s->video_base_ms + (long long)((double)(s->frames + 1) * 1000.0 / (double)s->fps);
            long long apos = s->audio_base_ms + audio_anchor_ms(s);
            long long drift;
            int waited = 0;
            /* 等待限幅（v3.0.1 扩大适用面）：
             * · durl（音视频同进程）：视频线程不读帧 → video pipe 满 → ffmpeg 阻塞 → 音频断供
             *   → 锚停走 → 继续等 = 死锁。限幅后贴屏放行 pipe 解锁 ffmpeg。
             * · DASH（音频独立进程）：旧注释说"无此耦合 → 不限幅"，但真机实测（2026-10-30）
             *   发现这样会让**音频锚的阶梯更新传导到画面节拍**：audio_anchor_ms() 由
             *   writer_bytes（每写 8192B = 46ms 跳一次）+ FIONREAD 算出，是阶梯量；
             *   `while (apos < due)` 硬等时，连续几帧会一起卡在这个台阶上，
             *   节奏诊断实测 maxgap=126ms 每秒一次 = 用户反馈的「大概一秒一次的卡顿」。
             *   限幅到 2 帧时间：单帧最多等 2 帧，上屏节拍由帧率主导而不是音频锚台阶；
             *   真正的滞后仍由下面的 drift 判据（快进 / 强制重同步）兜底，不会累积。 */
            /* ---- v3.0.1 画面节拍器（替换「硬等音频锚」）----
             * 旧实现：`while (apos < due) usleep(2ms)`。问题在于 apos 是**台阶量**——
             * audio_anchor_ms() = writer_bytes（每写 8192B 才跳一次 ≈ 46ms）− pipe 残留 − ALSA
             * 缓冲，于是连续几帧落在同一台阶上一起等，台阶一跳又一起放行。
             * 真机节奏诊断（2026-10-30）实测：wait≈600ms/秒、wgap=110ms 每秒一次、
             * augap=110ms，而 aulen 稳定 210~258KB（读前视缓冲满）——**数据根本不缺**，
             * 卡顿完全由这个台阶传导造成，正是用户反馈的「大概一秒一次的卡顿」。
             *
             * 新实现：以「每帧固定 1000/fps ms」为基准节拍，再用 PLL 方式**慢速**把相位拉向
             * 音频锚（每帧最多 ±3ms，时间常数 ~0.3s）。台阶被这个低通滤掉，上屏间隔均匀；
             * 音频仍是权威时钟（长期相位由 drift 纠正锁死），真正的大滞后照旧走
             * 快进 / 强制重同步两条既有兜底路径。 */
            int frame_ms = (s->fps > 0) ? (int)(1000.0 / (double)s->fps) : 40;
            if (frame_ms < 1) frame_ms = 1;
            if (s->frames > 0 && s->audio_alive) {
                /* PLL：drift>0 = 声音超前 → 本帧间隔缩短（加快）；<0 → 拉长（放慢）。
                 * ±3ms/帧 @30fps = 每秒最多纠 90ms，既够压住台阶又能跟上真实漂移。 */
                int adj = 0;
                long long d0 = apos - due;
                if (d0 > AV_SKIP_MS) adj = 3;
                else if (d0 < -AV_SKIP_MS) adj = -3;
                else if (d0 > 0) adj = 1;
                else if (d0 < 0) adj = -1;
                int gap = frame_ms - adj;
                if (gap < 1) gap = 1;
                {
                    double since = (now_seconds() - s->last_pace_at) * 1000.0;
                    if (since < (double)gap) {
                        int ms = (int)((double)gap - since);
                        usleep((useconds_t)(ms * 1000));
                        waited = ms;
                    }
                }
                s->last_pace_at = now_seconds();
            }
            d_wait += (double)waited;
            if ((double)waited > d_maxwait) d_maxwait = (double)waited;
            if (s->stop_flag) break;
            apos = s->audio_base_ms + audio_anchor_ms(s);
            drift = apos - due;
            s->last_drift_ms = (long)drift;
            if (s->frames > 0 && !s->audio_alive) {
                s->paced = 1; /* 音频时钟死亡 → 墙钟接管 */
            } else if (drift > AV_RESYNC_MS && s->frames > 0 && s->duration_ms <= 0 &&
                       s->audio_input[0] && s->audio_alive) {
                /* 直播（duration_ms<=0）不能走"重启到音频位置"这条路：respawn_video_at 会用
                 * `-ss target` 重开 ffmpeg，而直播源没有内容时间轴，-ss 会跳到流的开头/直接
                 * 拉不到数据。真机实测（2026-10-30）直播每遇网络停顿 drift 就 +3s 且永不回落
                 * （累计到 14s），正是这里误用重同步造成的。
                 * 直播的正确做法是**就地重锚**：把视频基线挪到"当前可闻音频位置 − 已出帧数对应
                 * 的时长"，积压的那段画面直接丢弃（直播本来就是"看现在"，丢帧比追帧正确），
                 * ffmpeg/gst 都不动。音画立刻重新对齐，且不会触发 gst 重建链。
                 * 这里刻意**不经过 av_resync_gate**：重锚只是改一个整数，没有重启成本，
                 * 不需要限流（限流是给"重启视频进程"那种昂贵操作准备的）。 */
                if (s->fps > 0) {
                    long long pos = (long long)((double)s->frames * 1000.0 / (double)s->fps);
                    s->video_base_ms = apos - pos;
                }
                s->last_drift_ms = 0;
                s->resync_count++;
                continue; /* 下一帧按新基线计算 due */
            } else if (drift > AV_RESYNC_MS && s->frames > 0 && s->audio_input[0] && av_resync_gate(s)) {
                /* ② 不同步过大 → 强制重同步（v2.1.0）：跳过积压内容，视频流重启到"当前可闻音频
                 * 位置 + 预估启动时延"，首帧落屏即同步（音频侧完全不动，声音不断）。重同步期间
                 * status.resyncing=1 → JS 显示"加载中…"。仅 DASH 拆分模式可安全重启；
                 * durl（audio_input 空）无独立音频进程 → 退回下面①的平滑快进兜底。 */
                s->resync_count++;
                if (restart_video_synced(s) == 0) continue; /* 重入主循环读新流首帧（不递归，防长会话栈增长） */
                s->video_skips++;
                do_blit = 1; /* 重启失败 → 退化为平滑快进 */
            } else if (drift > AV_SKIP_MS) {
                /* ① 轻微滞后 → 平滑快进追上（v2.1.0）：每帧都贴屏、不等钟。
                 * 仍不收敛则累计到 >AV_RESYNC_MS 升级为②强制重同步。
                 *
                 * v3.0.1 关键修复（用户反馈「大概一秒一次的卡顿」，2026-10-30 真机钉死）：
                 * 旧实现此分支只置 do_blit=1、**完全不限速**，于是帧以「网络分块到达」的节奏
                 * 集中贴出——节奏诊断实测 maxgap=127ms 每秒出现一次，正是用户看到的卡顿。
                 * 而且稳态 drift 恰好稳定在 ~100ms（= AV_SKIP_MS），意味着**每一帧**都落进这个
                 * 分支 → 等于全程没有节拍器，画面完全跟着 CDN 分块抖动。
                 * 修法：与上面 paced 分支同构的**有界追赶**——每帧至少间隔 1/(fps*PACE_CATCHUP_MAX)。
                 * 127ms 的空档正好由 4 帧 × 33ms 填平，且 +100ms 的既有滞后提供了充裕余量，
                 * 不会因为限速而让滞后越积越多。 */
                s->video_skips++;
                do_blit = 1;
                if (s->fps > 0) {
                    double min_gap = 1000.0 / ((double)s->fps * PACE_CATCHUP_MAX);
                    double since = (now_seconds() - s->last_blit_at) * 1000.0;
                    if (since < min_gap) {
                        usleep((useconds_t)((min_gap - since) * 1000.0));
                    }
                }
            }
        }
        /* 直播首帧对齐（v3.0.2）：
         * 直播（duration_ms<=0）没有内容时间轴，视频基线只能从 0 起算；而音频进程
         * 一开始就出声，等 gst 建链 + PREROLL + 首帧解码完，音频锚往往已经跑了十几秒。
         * 于是 due = 0 + frames/fps 永远落后 apos 一大截 → drift 恒为 ~6s，画面只能
         * 以 PACE_CATCHUP_MAX 倍速一路狂追（真机 skip 每秒涨 30，观感是"一直快放"），
         * 而且永远追不平（追赶同时音频还在前进）。
         * 直播无法 seek 回音频位置，正确做法是**反过来把视频基线挪到当前音频位置**：
         * 首帧到达时令 video_base_ms = 当前可闻音频位置，此后两者同起点、同速前进。
         * 只在首帧做一次（frames==0），且仅限直播；VOD 的 video_base_ms 是真实内容
         * 位置（-ss 落点），绝不能动。 */
        if (s->frames == 0 && s->duration_ms <= 0 && s->audio_enabled && s->audio_alive) {
            long long anchor0 = s->audio_base_ms + audio_anchor_ms(s);
            if (anchor0 > 0) {
                s->video_base_ms = anchor0;
                s->last_drift_ms = 0;
            }
        }
        s->frames++;
        s->ever_played = 1; /* v2.1.0：本会话已出过画（重启后 frames 归零，据此区分中途断流与从未出画） */
        s->stall_resyncs = 0; /* v2.1.0：有新帧 = 空转期重同步成功/视频链恢复 → 计数清零 */
        s->position_ms = s->video_base_ms + (long)((double)s->frames * 1000.0 / (double)s->fps);
        s->frame_valid = 1;
        if (s->resyncing) {
            /* 重启后首帧（v2.1.2）：用**残余漂移**反馈修正 lead —— 这才是正确观测量。
             * 重启耗时 Δ 本身不可预知，且落点误差 = Δ − lead_used；直接测 Δ 赋值会残留
             * "Δ−lead" 的固定偏差（实测残留 ~2s → 每过 6s 冷却又触发一次重同步，要连做
             * 3 次才收敛）。改为 lead ← lead + 残余（钳制），一次重同步后即落到同步附近，
             * 残余交给①平滑快进吸收。 */
            long long anchor = s->audio_base_ms + audio_anchor_ms(s);
            long long pos = s->video_base_ms + (long long)((double)s->frames * 1000.0 / (double)s->fps);
            long residual = (long)(anchor - pos); /* >0 = 重启后仍落后 → lead 偏小 */
            long lead = s->resync_lead_ms + residual;
            if (lead < AV_RESYNC_LEAD_MIN_MS) lead = AV_RESYNC_LEAD_MIN_MS;
            if (lead > AV_RESYNC_LEAD_MAX_MS) lead = AV_RESYNC_LEAD_MAX_MS;
            s->resync_lead_ms = lead;
            s->last_drift_ms = residual;
            s->resyncing = 0; /* 首帧已产出 → 清除"加载中…" */
        }
        if (do_blit) s->last_blit_at = now_seconds();
        /* render_paused：评论面板/系统UI覆盖时暂停 fb 输出（解码/位置/音频照常）。
         * 根因（2026-09-24 真机三现象钉死）：视频矩形像素归 blit（33ms 写两块）专属，
         * 任何 UI 覆盖都会与其交替抢帧（弹幕层/评论面板/下拉控制中心均闪；暂停后不闪=blit 停）。 */
        if (do_blit && !s->render_paused) {
            double f_t0 = now_seconds();
            int wr = fb_write_stream(s->au_buf, au_n,
                    s->rect_x, s->rect_y, s->rect_w, s->rect_h, &s->stop_flag);
            double f_dt = (now_seconds() - f_t0) * 1000.0;
            d_fifo += f_dt;
            if (f_dt > d_maxfifo) d_maxfifo = f_dt;
            if (wr != 0) {
                /* 注意：v3.0.0 起 KMS 分支下 fb_blit 是空操作，真正的出画在 fb_write_stream。 */
                if (s->stop_flag) break;
                set_error(s, "视频显示链断开");
                s->state = ST_ERROR;
                return NULL;
            }
            {
                double nowd = now_seconds();
                if (d_last_au > 0) {
                    double gap = (nowd - d_last_au) * 1000.0;
                    if (gap > d_maxgap) d_maxgap = gap;
                }
                d_last_au = nowd;
                d_n++;
            }
            /* v3.0.1：**首帧真正写进 FIFO 之后**才放行音频起播门。以前这句在写之前，
             * 音频立刻开写、而 gst-launch 还要 PREROLL + 解码首帧，用户听到「先声后画」。
             * v2.1.0 起滞后帧也逐帧贴屏（平滑快进），故 last_blit_at 每帧刷新、不会误报停帧。 */
            s->video_first_frame = 1;
        }
        /* 每秒落一行节奏诊断（上限 300 行）。默认关闭 —— 排查「上屏卡顿」时再打开，
         * 免得日常播放每秒一次 open/write/close。
         * 开关用**文件存在性**而不是环境变量：`miniapp_cli start` 不会把 adb shell 里
         * export 的变量传给被启动的 miniapp 进程（实测 PENBILI_VID_DIAG=1 完全无效，
         * /tmp/vp_vid.log 一直是空文件），而 `touch /tmp/vp_diag.on` 随时生效。 */
        if (d_lines < 300 && now_seconds() - d_t0 >= 1.0) {
            int dfd = (access("/tmp/vp_diag.on", F_OK) == 0) ? open("/tmp/vp_vid.log", O_WRONLY | O_CREAT | O_APPEND, 0644) : -1;
            if (dfd >= 0) {
                char dl[220];
                int dl_n = snprintf(dl, sizeof(dl),
                        "t=%.1f n=%ld fps=%d wait=%.0f maxwait=%.0f wgap=%.0f "
                        "rdgap=%.0f augap=%.0f fifo=%.0f maxfifo=%.0f sk=%ld drift=%ld "
                        "aulen=%lu st=%s\n",
                        now_seconds() - d_t0, d_n, s->fps, d_wait, d_maxwait, d_maxgap,
                        d_maxrdgap, d_maxaugap, d_fifo, d_maxfifo, s->video_skips,
                        s->last_drift_ms, (unsigned long)s->au_len, state_name(s->state));
                if (dl_n > 0) (void)!write(dfd, dl, (size_t)dl_n);
                close(dfd);
            }
            d_lines++;
            d_t0 = now_seconds();
            d_n = 0;
            d_wait = 0;
            d_maxwait = 0;
            d_maxgap = 0;
            d_fifo = 0;
            d_maxfifo = 0;
            d_maxrdgap = 0;
            d_maxaugap = 0;
        }
        /* 该 AU 已送出：从累积缓冲里消费掉，继续切下一帧 */
        au_consume(s, au_n);
        s->au_bytes += (long)au_n;
        au_n = 0;
        au_ready = 0;
        if (s->video_eof && s->au_len == 0) {
            /* 尾帧已送出且无残留 → 源正常结束（不是断流，不能走重连） */
            eof_done = 1;
            break;
        }
    }

    if (s->stop_flag) return NULL;

    if (eof_done) {
        s->state = ST_ENDED;
        if (s->duration_ms > 0) s->position_ms = s->duration_ms;
        return NULL;
    }

    if (failed && (s->ever_played || s->resync_timeout) && s->video_retries < 5 &&
        s->audio_input[0] && s->audio_enabled && s->audio_alive && s->duration_ms > 0 &&
        !(s->duration_ms > 0 && s->position_ms + 2000 >= s->start_ms + s->duration_ms)) {
        /* 视频流中途断流（CDN 断开/rw_timeout/进程被杀）≠ 播放结束（v1.9.3，修
         * "画面显示已播完但声音还在播"）。旧逻辑把任何视频进程退出都判 ENDED。
         * 现以"当前可闻位置 + 预估启动时延"重启视频流（v2.1.0）：音频侧（独立进程+aplay）
         * 完全不动，时钟连续，首帧落屏即同步。仅 DASH 拆分模式可安全重启（durl 同进程会
         * 连带音频，维持原判，靠 -reconnect 兜底）。上限 5 次防无限循环；
         * 已接近片尾（duration 已知）仍按正常结束。
         * v2.1.0：判据由 frames>0 改为 ever_played —— 重启后 frames 已归零，若仍用 frames>0
         * 会把"重启后的视频流又断"误判成"从未出画"→ 掉进无音轨重试从头重放。 */
        s->video_retries++;
        s->retried_no_audio = 1; /* 视频链已证可产出：后续零帧失败不再走"无音轨重试"（防从头重放） */
        /* v2.1.0：断流重启复用"音频位置 + 启动时延"落点（与强制重同步同构）——
         * 首帧落屏即同步；旧逻辑只按音频位置重开，白落一个启动时延又得慢慢追。
         * resyncing 由 respawn_video_at 置位 → JS 显示"加载中…"。 */
        if (restart_video_synced(s) == 0) {
            return video_thread(arg); /* 与无音轨重试同构：重启后重入主循环 */
        }
        capture_log_tail(s);
        if (!s->error[0]) set_error(s, "视频流断线重连失败");
        s->state = ST_ERROR;
        return NULL;
    }

    if (failed && s->frames == 0 && s->audio_input[0] && s->video_retries > 0 && !s->resync_timeout) {
        /* 断流恢复重启后仍零帧 = 源确实耗尽（未知时长的视频正常结尾）→ 按结束处理，
         * 不落入下面的无音轨重试（那会从头重放）也不报错误 */
        s->state = ST_ENDED;
        return NULL;
    }

    if (failed && !s->ever_played && s->frames == 0 && s->audio_enabled && !s->retried_no_audio && !s->resync_timeout) {
        /* 无音轨时 ffmpeg 会因第二个输出没有流而整体失败：去掉音频输出重来一次 */
        s->retried_no_audio = 1;
        s->audio_enabled = 0;
        s->paced = 1;
        close_fd(&s->video_fd);
        close_fd(&s->audio_fd);
        kill_child(&s->ff_pid);
        kill_child(&s->aplay_pid);
        close_fd(&s->aplay_fd);
        clock_gettime(CLOCK_MONOTONIC, &s->started_at);
        s->video_spawn_at = now_seconds();
        if (spawn_ffmpeg(s, s->start_ms, 0) == 0 && spawn_aplay(s, NULL) == 0) {
            return video_thread(arg);
        }
        set_error(s, "无音轨重试失败");
        s->state = ST_ERROR;
        return NULL;
    }

    if (failed && s->frames == 0) {
        capture_log_tail(s);
        if (!s->error[0]) set_error(s, "解码进程未产出画面");
        s->state = ST_ERROR;
    } else {
        s->state = ST_ENDED;
        if (s->duration_ms > 0) s->position_ms = s->duration_ms;
    }
    return NULL;
}

/* 环形缓冲（feeder 写 / writer 读，单产单销，len/rpos 更新顺序即天然屏障） */
static size_t ring_free(const struct session *s) { return sizeof(s->ring) - s->rlen; }

static size_t ring_put(struct session *s, const unsigned char *data, size_t n) {
    size_t cap = ring_free(s);
    if (n > cap) n = cap;
    if (n == 0) return 0;
    size_t tail = sizeof(s->ring) - s->rpos;
    if (n <= tail) {
        memcpy(s->ring + s->rpos, data, n);
    } else {
        memcpy(s->ring + s->rpos, data, tail);
        memcpy(s->ring, data + tail, n - tail);
    }
    s->rpos = (s->rpos + n) % sizeof(s->ring);
    s->rlen += n;
    return n;
}

/* 取出至多 max 字节到 out（不弹出；调用 ring_drop 消费） */
static size_t ring_peek(const struct session *s, unsigned char *out, size_t max) {
    size_t n = s->rlen < max ? s->rlen : max;
    if (n == 0) return 0;
    size_t start = (s->rpos + sizeof(s->ring) - s->rlen) % sizeof(s->ring);
    size_t tail = sizeof(s->ring) - start;
    if (n <= tail) {
        memcpy(out, s->ring + start, n);
    } else {
        memcpy(out, s->ring + start, tail);
        memcpy(out + tail, s->ring, n - tail);
    }
    return n;
}

static void ring_drop(struct session *s, size_t n) {
    if (n > s->rlen) n = s->rlen;
    s->rlen -= n;
}

static void ring_reset(struct session *s) {
    s->rpos = 0;
    s->rlen = 0;
}

/* feeder：ffmpeg audio pipe → 环（满则阻塞保背压；入口提 RT 优先级） */
static void *audio_thread(void *arg) {
    struct session *s = (struct session *)arg;
    unsigned char buf[8192];
    raise_audio_thread_prio();
    while (!s->stop_flag) {
        struct pollfd pfd;
        int pr;
        ssize_t n;
        pfd.fd = s->audio_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (s->audio_fd < 0) break;
        pr = poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        n = read(s->audio_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) {
            s->audio_alive = 0; /* 音频产出断了（独立进程死/EOF）→ 视频侧转墙钟降级 */
            break;
        }
        s->audio_bytes += (long)n;
        if (s->audio_bytes > 4096) s->has_audio = 1;
        /* 环满 = 持续慢于实时 → 阻塞等待（保留"ALSA 时钟节拍解码"的背压语义；
         * 弹性从旧的 64KB pipe 拉大到 256KB 环 + writer 预蓄，微抖动不再直接变成听感空隙） */
        while (!s->stop_flag && ring_free(s) < (size_t)n) usleep(5000);
        if (s->stop_flag) break;
        size_t put = ring_put(s, buf, (size_t)n);
        if (put < (size_t)n) s->ring_drops += (long)((size_t)n - put); /* 防御性计数，常态恒 0 */
    }
    return NULL;
}

/* writer：环 → aplay（起播预蓄 + 水位滞回 + 空环等待 + 阻塞写吸收突发）
 * 水位滞回（v1.3.1）：网络带宽贴码率（实测 bw≈646kbps）→ 环稳态水位天然偏低，
 * 普通抖动直接穿透 → 闪断。改为低于 LOW(400ms) 暂停消费、蓄回 HIGH(900ms) 放行，
 * 用 ~0.9s 受控延迟换连续性；视频时钟跟音频走，音画同步不受损。
 * 证据计数：u=欠载等待轮 w=aplay写失败 rd=环满丢弃 ad=写错误字节。 */
#define RING_START_MS 800  /* 起播预蓄 800ms（吸收解码起步+首波网络抖动） */
#define RING_LOW_MS 250    /* 低水位调低：网络正常时不误伤暂停（hold=可闻蓄水期） */
#define RING_HIGH_MS 2000  /* 高水位 2s（cap 于 491KB≈2.56s）；+alsa存量600ms ≈ 3.1s 抗断防御 */

/* 音频线程提实时优先级（feeder/writer 均为阻塞型：poll/sleep/阻塞写，必然让出 CPU，
 * FIFO 不会饿死普通线程）。root 下生效；失败（EPERM）无害退化为普通调度。
 * 动机：软解 ffmpeg 多线程解码会周期性抢占音频线程 → 供给抖动 → 零星卡顿。 */
static void raise_audio_thread_prio(void) {
    struct sched_param sp;
    sp.sched_priority = 1;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        /* 忽略：非 root 或内核无 RT 调度时保持普通调度 */
    }
}
static void *audio_writer_thread(void *arg) {
    struct session *s = (struct session *)arg;
    unsigned char buf[8192];
    raise_audio_thread_prio();
    size_t rate_bytes = (size_t)(s->audio_rate > 0 ? s->audio_rate : 44100) * 4; /* B/s @2ch16b */
    /* 直播（duration_ms<=0）：**关闭水位滞回** —— hold 会把可闻位置冻结在环里（2s 量级），
     * 与墙钟视频错位；关掉后 writer 一有数据就消费，可闻滞后收敛到 aplay 管道 + ALSA（~0.56s）。
     * （2026-10-07：直播改墙钟节拍后必须同步打开此项，否则音画差 ~2s。）VOD 不变。 */
    size_t low_level = (s->duration_ms > 0) ? rate_bytes * RING_LOW_MS / 1000 : 0;
    size_t high_level = rate_bytes * RING_HIGH_MS / 1000;
    if (high_level > sizeof(s->ring) - 32768) high_level = sizeof(s->ring) - 32768; /* 留 feeder 空间 */
    if (low_level >= high_level) low_level = high_level / 2;
    /* 起播预蓄：攒够 500ms 再放行 */
    size_t start_level = rate_bytes * RING_START_MS / 1000;
    if (start_level > high_level) start_level = high_level;
    while (!s->stop_flag && s->rlen < start_level) usleep(20000);
    probe_alsa_buffer(s); /* v1.9.0：开写前锁定 ALSA 缓冲实测值（音频锚扣减用） */
    /* A/V 起播门（v1.7.1）：视频首帧产出前不开写——弱网下音频(66kbps)常先缓冲完先出声，
     * 画面几秒后才来（先声后画）。门期内 ring 继续蓄（上限 2.7s 无损）、视频 pipe 堵塞
     * 在 1MB 缓冲（ffmpeg 反压等待）。15s 超时放行（网络极差先出声再由 JS 巡检 seek 对齐），
     * 放行时顺延 last_blit 基准，避免"刚放行就被巡检判停帧"的抖动。 */
    {
        double gate_t0 = now_seconds();
        while (!s->stop_flag && !s->video_first_frame && now_seconds() - gate_t0 < 15.0) {
            usleep(20000);
        }
        /* v3.0.1：首帧写进 FIFO ≠ 画面已经上屏。gst-launch 还要 PREROLL + 解首帧 +
         * kmssink 拿到 buffer，实测有几百毫秒空档，音频若此刻开写就是「先声后画」。
         * 这里再等 KMS 平面真的 ACTIVE 且有 buffer（最多 1.2s，超时照样放行——
         * 不能让读不到 debugfs 的设备永远没声音）。 */
        if (s->video_first_frame && !s->stop_flag) wait_kms_video(1200.0);
        s->gate_wait_ms = (now_seconds() - gate_t0) * 1000.0;
        if (!s->video_first_frame) s->last_blit_at = now_seconds();
        s->gate_passed = 1; /* 门结束（放行即判据切换）：巡检从此刻起才有裁决权 */
    }
    int held = 0; /* 滞回状态：0=放行中 1=蓄水暂停中 */
    while (!s->stop_flag) {
        size_t n;
        /* 水位滞回：跌破 LOW 暂停消费 → 蓄回 HIGH 放行（吸收网络供给抖动） */
        if (!held && s->rlen < low_level) {
            held = 1;
            s->underruns++; /* 每次进入暂停=发生过一次跌破（欠载事件计数） */
        } else if (held && s->rlen >= high_level) {
            held = 0;
        }
        if (held) {
            usleep(20000);
            continue;
        }
        if (s->rlen == 0) {
            usleep(10000);
            continue;
        }
        n = ring_peek(s, buf, sizeof(buf));
        if (n == 0) {
            usleep(5000);
            continue;
        }
        ssize_t off = 0;
        int failed = 0;
        while (off < (ssize_t)n && !s->stop_flag) {
            ssize_t w = write(s->aplay_fd, buf + off, (size_t)(n - (size_t)off));
            if (w > 0) {
                off += w;
                continue;
            }
            if (w < 0 && errno == EINTR) continue;
            s->wr_errors++;
            if (w > 0) s->audio_dropped += w; /* 不可达，防御 */
            s->audio_dropped += (long)((size_t)n - (size_t)off);
            if (w < 0 && (errno == EPIPE || errno == EBADF)) failed = 1;
            break;
        }
        if (off > 0) {
            s->writer_bytes += (long)off; /* 真实播放时钟（音频时钟锚） */
            ring_drop(s, (size_t)off);
        }
        if (failed) {
            /* aplay 写失败（EPIPE=设备被占用/早退，实测"bluealsa busy"场景）→
             * 立即宣告音频时钟死亡：video_thread 快速转墙钟，不空等 8s 探测窗 */
            s->audio_alive = 0;
            break; /* aplay 已死，feeder 继续收但无人消费（后续入环计入 ring_drops） */
        }
    }
    return NULL;
}

/* ---------------- 会话控制 ---------------- */

static void stop_children(struct session *s) {
    s->stop_flag = 1;
    if (s->ff_pid > 0) kill(s->ff_pid, SIGCONT);
    if (s->aplay_pid > 0) kill(s->aplay_pid, SIGCONT);
    kill_child(&s->ff_pid);
    kill_child(&s->ff_a_pid); /* v1.6.0 音频独立进程 */
    kill_child(&s->aplay_pid);
    if (s->vth_started) {
        pthread_join(s->vth, NULL);
        s->vth_started = 0;
    }
    if (s->ath_started) {
        pthread_join(s->ath, NULL);
        s->ath_started = 0;
    }
    if (s->wth_started) {
        pthread_join(s->wth, NULL);
        s->wth_started = 0;
    }
    close_fd(&s->video_fd);
    close_fd(&s->audio_fd);
    close_fd(&s->aplay_fd);
}

static void session_reset(struct session *s) {
    s->state = ST_IDLE;
    s->has_session = 0;
    s->frames = 0;
    /* v3.0.0：AU 累积缓冲跨会话必须清空，否则上一段流的残字节会被当成本段首帧前缀 */
    s->au_len = 0;
    s->au_bytes = 0;
    s->video_eof = 0;
    s->audio_bytes = 0;
    s->audio_dropped = 0;
    s->has_audio = 0;
    s->audio_is_bt = 0;
    s->audio_dev[0] = '\0';
    s->paced = 0;
    s->retried_no_audio = 0;
    s->frame_valid = 0;
    s->error[0] = '\0';
    s->underruns = 0;
    s->wr_errors = 0;
    s->ring_drops = 0;
    s->video_skips = 0;
    s->pipe_cap_bytes = 65536; /* F_GETPIPE_SZ 实测前按内核默认 */
    s->video_pipe_bytes = 0;
    s->pace_align_ms = 0;
    s->alsa_buf_bytes = 0;
    s->audio_base_ms = 0; /* v1.9.3 时钟基准（session_start 里对齐 start_ms） */
    s->video_base_ms = 0;
    s->video_retries = 0;
    /* v2.1.0 音画同步分层状态 */
    s->resyncing = 0;
    s->resync_count = 0;
    s->resync_lead_ms = 1200; /* 首启无实测值：先验估值（-ss 定位 + probesize + 首帧解码） */
    s->resync_at = 0;
    s->last_drift_ms = 0;
    s->ever_played = 0;
    s->probe_audio_bytes = 0;
    s->stall_resyncs = 0;
    s->resync_burst = 0;
    s->resync_burst_at = 0;
    s->resync_timeout = 0;
    s->video_spawn_at = 0;
    ring_reset(s);
}

/* 启动会话（调用方需保证已有会话已停止） */
static int session_start(struct session *s, long start_ms, int with_audio, const char *audio_device) {
    s->stop_flag = 0;
    s->frames = 0;
    s->position_ms = start_ms;
    s->start_ms = start_ms;
    s->has_audio = 0;
    s->audio_bytes = 0;
    s->audio_dropped = 0;
    s->frame_valid = 0;
    s->error[0] = '\0';
    s->audio_enabled = with_audio;
    s->retried_no_audio = 0;
    /* 直播（duration_ms<=0）且单进程（无独立音频输入）→ **必须墙钟节拍**，不能用音频锚。
     * 根因（2026-10-07 真机定位，帧率恒 4.2fps ≈ 1/236ms 且直播无声）：
     *   · 视频 pipe 上限只有 1MB（/proc/sys/fs/pipe-max-size=1048576，笔端无 CAP_SYS_RESOURCE）
     *     → 8MB 申请失败、逐级回退到 1MB（≈2 帧）；
     *   · 音频 writer 要等环形缓冲蓄满 800ms（RING_START_MS）才开写；
     *   · ffmpeg 是**同一进程**产出视频(fd3)+音频(fd4) → 视频 pipe 一满，它连音频也不产了。
     *   于是形成互锁：视频 writer 按音频锚等待（writer_bytes 恒 0 → 锚恒 0 → 每帧撞满
     *   AV_WAIT_CAP_MS=200ms）→ 视频 pipe 满 → ffmpeg 停摆 → 环永远蓄不满 → writer 永不开写
     *   → 锚永远 0。实测 drift 恰好 = -frames×33ms（证明 apos 恒 0）。
     * 直播源本身就是实时的 → 用墙钟（= 媒体时间）。配合 writer 侧**直播关闭水位滞回**
     * （见 audio_writer_thread），可闻音频滞后 ≈ aplay 管道 + ALSA 缓冲（~0.56s），与视频侧
     * 存量同量级 → 音画偏移可控且不漂移。VOD（duration_ms>0）行为完全不变。 */
    s->paced = (with_audio && s->duration_ms > 0) ? 0 : 1;
    s->underruns = 0;
    s->wr_errors = 0;
    s->ring_drops = 0;
    s->video_skips = 0;
    s->pipe_cap_bytes = 65536; /* F_GETPIPE_SZ 实测前按内核默认 */
    s->video_pipe_bytes = 0;
    s->pace_align_ms = 0;
    s->alsa_buf_bytes = 0;
    s->audio_base_ms = start_ms; /* v1.9.3 时钟基准：writer_bytes==0 ⇔ 内容位置 start_ms */
    s->video_base_ms = start_ms;
    s->video_retries = 0;
    /* v2.1.0：新会话/seek 复位同步分层状态（lead 自适应值随会话重置） */
    s->resyncing = 0;
    s->resync_count = 0;
    s->resync_lead_ms = 1200;
    s->resync_at = 0;
    s->last_drift_ms = 0;
    s->ever_played = 0;
    s->probe_audio_bytes = 0;
    s->stall_resyncs = 0;
    s->resync_burst = 0;
    s->resync_burst_at = 0;
    s->resync_timeout = 0;
    s->video_spawn_at = 0;
    ring_reset(s);
    /* 音频设备/采样率：每次 open（audio_rate==0 触发）探测一次；seek 重启复用同值保持一致。
     * 蓝牙(auto 或显式 bluealsa)→ 48000；内置/speaker → 44100。 */
    if (with_audio && s->audio_rate == 0) {
        s->audio_rate = 44100;
        s->audio_is_bt = 0;
        if (!audio_device || !audio_device[0]) {
            char bt[128];
            if (detect_bt_pcm(bt, sizeof(bt)) == 0) {
                s->audio_is_bt = 1;
                s->audio_rate = 48000;
            }
        } else if (strncmp(audio_device, "bluealsa", 8) == 0) {
            s->audio_is_bt = 1;
            s->audio_rate = 48000;
        }
    } else if (!with_audio && s->audio_rate == 0) {
        s->audio_rate = 44100;
    }
    clock_gettime(CLOCK_MONOTONIC, &s->started_at);
    /* A/V 起播门基准：每次 open/seek 重启复位（必须早于 video_thread 创建） */
    s->video_first_frame = 0;
    s->gate_passed = 0;
    s->last_pace_at = now_seconds();
    s->last_blit_at = now_seconds();
    s->gate_wait_ms = 0;

    /* DASH（有第二输入音频轨）→ 音频走独立进程，视频进程只需单输出；
     * durl 回退（单文件，音视频同文件）→ 保持原双输出同进程模式 */
    int split_audio = with_audio && s->audio_input[0];
    s->video_spawn_at = now_seconds();
    if (spawn_ffmpeg(s, start_ms, with_audio && !split_audio) != 0) {
        set_error(s, "无法启动 ffmpeg 进程");
        s->state = ST_ERROR;
        return -1;
    }
    if (with_audio) {
        int afail = 0;
        if (split_audio && spawn_ffaudio(s) != 0) afail = 1;
        if (!afail && spawn_aplay(s, audio_device) != 0) afail = 1;
        if (afail) {
            /* 音频不可用不致命：退化为仅视频 + 墙钟节拍。
             * split 模式的视频进程本就是单输出（无需重启）；durl 模式的进程带音频输出 → 重启。 */
            kill_child(&s->ff_a_pid);
            close_fd(&s->audio_fd);
            if (!split_audio) {
                kill_child(&s->ff_pid);
                close_fd(&s->video_fd);
                s->video_spawn_at = now_seconds();
                if (spawn_ffmpeg(s, start_ms, 0) != 0) {
                    set_error(s, "无法启动解码进程");
                    s->state = ST_ERROR;
                    return -1;
                }
            }
            s->audio_enabled = 0;
            s->paced = 1;
            clock_gettime(CLOCK_MONOTONIC, &s->started_at);
        }
    }
    s->audio_alive = s->audio_fd >= 0 ? 1 : 0;
    s->writer_bytes = 0;
    s->state = ST_PLAYING;
    s->has_session = 1;
    if (pthread_create(&s->vth, NULL, video_thread, s) != 0) {
        stop_children(s);
        set_error(s, "无法创建解码线程");
        s->state = ST_ERROR;
        return -1;
    }
    s->vth_started = 1;
    if (s->audio_fd >= 0) {
        if (pthread_create(&s->ath, NULL, audio_thread, s) == 0) s->ath_started = 1;
        /* writer 单独线程：预蓄+水位整形，与 feeder 解耦（v1.3.0 重写核心） */
        if (pthread_create(&s->wth, NULL, audio_writer_thread, s) == 0) s->wth_started = 1;
    }
    return 0;
}

/* ---------------- JS 绑定 ---------------- */

static JSValue mk_bool(JSContext *ctx, const char *k, int v, JSValue obj) {
    JS_SetPropertyStr(ctx, obj, k, JS_NewBool(ctx, v ? 1 : 0));
    return obj;
}

static JSValue mk_int(JSContext *ctx, const char *k, long v, JSValue obj) {
    JS_SetPropertyStr(ctx, obj, k, JS_NewInt32(ctx, (int)v));
    return obj;
}

static JSValue mk_str(JSContext *ctx, const char *k, const char *v, JSValue obj) {
    JS_SetPropertyStr(ctx, obj, k, v ? JS_NewString(ctx, v) : JS_NULL);
    return obj;
}

/* ---- 可选 HTTP 头参数校验（与 src/services/player.js 的 JS 侧规则一致）---- */
/* UA：非空时必须是 1..255 的可打印 ASCII（控制字符会破坏 argv/HTTP 语义） */
static int valid_user_agent(const char *s) {
    size_t n;
    size_t i;
    if (!s) return 1;
    n = strlen(s);
    if (n == 0) return 1;
    if (n > 255) return 0;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch > 0x7e) return 0;
    }
    return 1;
}
/* Referer：非空时必须是 http(s) URL，无空白/控制字符，≤512 */
static int valid_referer(const char *s) {
    size_t n;
    size_t i;
    if (!s) return 1;
    n = strlen(s);
    if (n == 0) return 1;
    if (n > 512) return 0;
    if (strncmp(s, "http://", 7) != 0 && strncmp(s, "https://", 8) != 0) return 0;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch <= 0x20 || ch == 0x7f) return 0;
    }
    return 1;
}

/* open(input, startMs, durationMs, fps, audio, transpose, x, y, w, h, audioDevice[, userAgent, referer]) */
static JSValue js_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *input = NULL;
    int startMs = 0;
    int durationMs = 0;
    int fps = 24;
    int audio = 1;
    int transpose = 1;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    const char *audioDevice = NULL;
    const char *userAgent = NULL;
    const char *referer = NULL;
    int fpsCap = 0;
    struct session *s = &g_s;
    JSValue res;
    int started;

    (void)this_val;
    if (argc < 10) {
        return JS_ThrowTypeError(ctx, "player.open(input, startMs, durationMs, fps, audio, transpose, x, y, w, h[, audioDevice])");
    }
    input = JS_ToCString(ctx, argv[0]);
    if (!input) return JS_EXCEPTION;
    JS_ToInt32(ctx, &startMs, argv[1]);
    JS_ToInt32(ctx, &durationMs, argv[2]);
    JS_ToInt32(ctx, &fps, argv[3]);
    JS_ToInt32(ctx, &audio, argv[4]);
    JS_ToInt32(ctx, &transpose, argv[5]);
    JS_ToInt32(ctx, &x, argv[6]);
    JS_ToInt32(ctx, &y, argv[7]);
    JS_ToInt32(ctx, &w, argv[8]);
    JS_ToInt32(ctx, &h, argv[9]);
    if (argc > 10 && !JS_IsUndefined(argv[10]) && !JS_IsNull(argv[10])) audioDevice = JS_ToCString(ctx, argv[10]);
    if (argc > 11 && !JS_IsUndefined(argv[11]) && !JS_IsNull(argv[11])) userAgent = JS_ToCString(ctx, argv[11]);
    if (argc > 12 && !JS_IsUndefined(argv[12]) && !JS_IsNull(argv[12])) referer = JS_ToCString(ctx, argv[12]);
    /* 第 15 个位置参数（下标 14）：渲染帧率上限。0/缺省 = 不限；超出 [1,120] 一律当不限，
     * 不让脏值把管线搞成"零帧"（videorate max-rate=0 会让所有帧被丢弃）。 */
    if (argc > 14 && !JS_IsUndefined(argv[14]) && !JS_IsNull(argv[14])) {
        JS_ToInt32(ctx, &fpsCap, argv[14]);
    }
    if (fpsCap < 1 || fpsCap > 120) fpsCap = 0;

    if (!input[0] || strlen(input) >= sizeof(s->input) || strstr(input, "..")) {
        JS_FreeCString(ctx, input);
        if (audioDevice) JS_FreeCString(ctx, audioDevice);
        if (userAgent) JS_FreeCString(ctx, userAgent);
        if (referer) JS_FreeCString(ctx, referer);
        return JS_ThrowTypeError(ctx, "player.open: 播放地址不合法");
    }
    if (fps < 1 || fps > 60) fps = 24;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        JS_FreeCString(ctx, input);
        if (audioDevice) JS_FreeCString(ctx, audioDevice);
        if (userAgent) JS_FreeCString(ctx, userAgent);
        if (referer) JS_FreeCString(ctx, referer);
        return JS_ThrowTypeError(ctx, "player.open: 视频矩形不合法");
    }
    if (!valid_user_agent(userAgent) || !valid_referer(referer)) {
        JS_FreeCString(ctx, input);
        if (audioDevice) JS_FreeCString(ctx, audioDevice);
        if (userAgent) JS_FreeCString(ctx, userAgent);
        if (referer) JS_FreeCString(ctx, referer);
        res = JS_NewObject(ctx);
        mk_bool(ctx, "ok", 0, res);
        mk_str(ctx, "code", "PLAYER_BAD_HEADERS", res);
        mk_str(ctx, "error", "User-Agent/Referer 参数不合法", res);
        mk_str(ctx, "state", state_name(s->state), res);
        return res;
    }

    pthread_mutex_lock(&s->mu);
    if (s->has_session) {
        pthread_mutex_unlock(&s->mu);
        JS_FreeCString(ctx, input);
        if (audioDevice) JS_FreeCString(ctx, audioDevice);
        if (userAgent) JS_FreeCString(ctx, userAgent);
        if (referer) JS_FreeCString(ctx, referer);
        res = JS_NewObject(ctx);
        mk_bool(ctx, "ok", 0, res);
        mk_str(ctx, "code", "PLAYER_ALREADY_RUNNING", res);
        mk_str(ctx, "error", "已有播放会话在运行", res);
        mk_str(ctx, "state", state_name(s->state), res);
        return res;
    }
    pthread_mutex_unlock(&s->mu);

    if (fb_open() != 0) {
        JS_FreeCString(ctx, input);
        if (audioDevice) JS_FreeCString(ctx, audioDevice);
        if (userAgent) JS_FreeCString(ctx, userAgent);
        if (referer) JS_FreeCString(ctx, referer);
        res = JS_NewObject(ctx);
        mk_bool(ctx, "ok", 0, res);
        mk_str(ctx, "code", "PLAYER_FB_FAILED", res);
        mk_str(ctx, "error", "/dev/fb0 打开失败（需要 32bpp 且可写）", res);
        return res;
    }

    pthread_mutex_lock(&s->mu);
    session_reset(s);
    strncpy(s->input, input, sizeof(s->input) - 1);
    s->input[sizeof(s->input) - 1] = '\0';
    /* 可选 HTTP 头：每次 open 都显式写入或清空（seek 走 session_start 复用现值） */
    if (userAgent && userAgent[0]) {
        strncpy(s->user_agent, userAgent, sizeof(s->user_agent) - 1);
        s->user_agent[sizeof(s->user_agent) - 1] = '\0';
    } else {
        s->user_agent[0] = '\0';
    }
    if (referer && referer[0]) {
        strncpy(s->referer, referer, sizeof(s->referer) - 1);
        s->referer[sizeof(s->referer) - 1] = '\0';
    } else {
        s->referer[0] = '\0';
    }
    /* DASH 第二输入（音频轨）：所有参数/资源校验通过后解析 —— 之后仅剩成功路径，
     * 失败 return 无需清理本字符串 */
    if (argc > 13 && !JS_IsUndefined(argv[13]) && !JS_IsNull(argv[13])) {
        const char *ai = JS_ToCString(ctx, argv[13]);
        if (ai && ai[0]) {
            if (strlen(ai) >= sizeof(s->audio_input) || strstr(ai, "..")) {
                JS_FreeCString(ctx, ai);
                JS_FreeCString(ctx, input);
                if (audioDevice) JS_FreeCString(ctx, audioDevice);
                if (userAgent) JS_FreeCString(ctx, userAgent);
                if (referer) JS_FreeCString(ctx, referer);
                return JS_ThrowTypeError(ctx, "player.open: 音频地址不合法");
            }
            strncpy(s->audio_input, ai, sizeof(s->audio_input) - 1);
            s->audio_input[sizeof(s->audio_input) - 1] = '\0';
        } else {
            s->audio_input[0] = '\0';
        }
        JS_FreeCString(ctx, ai);
    } else {
        s->audio_input[0] = '\0';
    }
    s->audio_rate = 0; /* 新会话：开播时重新探测蓝牙/采样率（seek 重启走 session_start 复用） */
    s->render_paused = 0;
    s->out_w = w;
    s->out_h = h;
    s->rect_x = x;
    s->rect_y = y;
    s->rect_w = w;
    s->rect_h = h;
    s->fps = fps;
    g_rd.fps = fps; /* KMS caps framerate（仅影响首帧建链，不参与节拍） */
    /* v3.2.0：帧率上限进 g_rd（render_start 读它决定插不插 videorate），
     * 同时把 s->fps 夹到上限 —— 否则节拍器仍按 60fps 排程（每帧 16ms），
     * 而 videorate 已把出帧降到 30fps，画面会被节拍器拖着走成"每两帧空等一次"。
     * 注意要在 parse_fps_line 覆盖 s->fps 的地方也夹一次（见 ff_stderr_thread）。 */
    g_rd.fps_cap = fpsCap;
    if (fpsCap > 0 && s->fps > fpsCap) s->fps = fpsCap;
    g_rd.rx = -1;   /* 新会话：矩形可能变了，首帧重建链 */
    g_rd.ry = -1;
    s->transpose = (transpose == 2) ? 2 : 1;
    /* v3.0.1 致命修复：render_start() 读的是 g_rd.transpose，而这里以前只写 s->transpose
     * → g_rd.transpose 恒为初值 0 → rotation_for_transpose(0) 恒走默认分支 = 270，
     * 于是「用户反馈画面上下倒置」时把映射表从 270 改 90 的动作**完全没生效**
     * （真机 ps 可见 gst 命令行仍是 rotation=270，实证）。两处必须同步。
     * 另外把 transpose=3（180°）也透传，别再被折叠成 1。 */
    g_rd.transpose = (transpose == 2) ? 2 : (transpose == 3 ? 3 : 1);
    s->duration_ms = durationMs > 0 ? durationMs : 0;
    s->frame_valid = 0;
    s->state = ST_PLAYING;
    s->has_session = 1;
    if (!s->frame) {
        s->frame = (unsigned char *)malloc((size_t)w * (size_t)h * 4u);
        if (!s->frame) {
            session_reset(s);
            pthread_mutex_unlock(&s->mu);
            JS_FreeCString(ctx, input);
            if (audioDevice) JS_FreeCString(ctx, audioDevice);
            if (userAgent) JS_FreeCString(ctx, userAgent);
            if (referer) JS_FreeCString(ctx, referer);
            res = JS_NewObject(ctx);
            mk_bool(ctx, "ok", 0, res);
            mk_str(ctx, "code", "PLAYER_CALL_FAILED", res);
            mk_str(ctx, "error", "内存不足（帧缓冲分配失败）", res);
            return res;
        }
    }
    started = session_start(s, startMs, audio ? 1 : 0, audioDevice);
    pthread_mutex_unlock(&s->mu);
    JS_FreeCString(ctx, input);
    if (audioDevice) JS_FreeCString(ctx, audioDevice);
    if (userAgent) JS_FreeCString(ctx, userAgent);
    if (referer) JS_FreeCString(ctx, referer);

    res = JS_NewObject(ctx);
    if (started != 0) {
        mk_bool(ctx, "ok", 0, res);
        mk_str(ctx, "code", "PLAYER_FFMPEG_FAILED", res);
        mk_str(ctx, "error", s->error[0] ? s->error : "解码进程启动失败", res);
        mk_str(ctx, "state", state_name(s->state), res);
        return res;
    }
    mk_bool(ctx, "ok", 1, res);
    mk_str(ctx, "state", state_name(ST_PLAYING), res);
    mk_int(ctx, "positionMs", startMs, res);
    mk_int(ctx, "durationMs", s->duration_ms, res);
    mk_int(ctx, "fps", s->fps, res);
    mk_int(ctx, "outWidth", s->out_w, res);
    mk_int(ctx, "outHeight", s->out_h, res);
    mk_bool(ctx, "audioRequested", audio ? 1 : 0, res);
    mk_int(ctx, "audioRate", s->audio_rate, res);
    mk_bool(ctx, "audioBt", s->audio_is_bt, res);
    return res;
}

/* ---- 显示权与弹幕泳道（v1.7.0）---- */

/* 暂停/恢复 fb 输出：评论面板、系统UI覆盖时交出显示权（解码/位置/音频照常） */
static JSValue js_pauseRender(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    g_s.render_paused = 1;
    return JS_UNDEFINED;
}

static JSValue js_resumeRender(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    g_s.render_paused = 0;
    return JS_UNDEFINED;
}

static JSValue js_status(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct session *s = &g_s;
    JSValue res = JS_NewObject(ctx);
    (void)this_val;
    (void)argc;
    (void)argv;
    pthread_mutex_lock(&s->mu);
    mk_bool(ctx, "ok", 1, res);
    mk_str(ctx, "state", state_name(s->state), res);
    mk_int(ctx, "positionMs", s->position_ms, res);
    mk_int(ctx, "durationMs", s->duration_ms, res);
    mk_int(ctx, "frames", s->frames, res);
    mk_int(ctx, "fps", s->fps, res);
    mk_bool(ctx, "hasAudio", s->has_audio, res);
    mk_bool(ctx, "audioEnabled", s->audio_enabled, res);
    mk_int(ctx, "audioBytes", s->audio_bytes, res);
    mk_int(ctx, "audioDropped", s->audio_dropped, res);
    mk_int(ctx, "audioUnderruns", s->underruns, res);
    mk_int(ctx, "audioWrErrors", s->wr_errors, res);
    mk_int(ctx, "audioRingDrops", s->ring_drops, res);
    /* 已请求音频但产出链已断（aplay 打开失败/写失败——实测主因：bluealsa 被其他应用
     * （网易云 SoundPlayer）独占 → "Device or resource busy"）→ JS 侧提示用户 */
    mk_bool(ctx, "audioDead", s->audio_enabled && s->audio_alive == 0, res);
    /* A/V 巡检（v1.7.1）：playing 态距最近一帧产出的毫秒数（JS 侧 >8000 且音频仍在走 →
     * seek 重开恢复，重开必经起播门保证音画成对重启）；gateWaitMs=起播门等待时长（诊断）。 */
    mk_int(ctx, "videoStallMs", s->state == ST_PLAYING ? (int)((now_seconds() - s->last_blit_at) * 1000.0) : 0, res);
    mk_int(ctx, "gateWaitMs", (int)s->gate_wait_ms, res);
    /* 门进行中（等首帧/未放行）→ JS 巡检让位，防弱网首帧 8~15s 区间被误判停帧打断门 */
    mk_bool(ctx, "gateActive", s->audio_enabled && !s->gate_passed, res);
    /* 音画漂移诊断（v1.8.0 出，v1.9.0 改用可闻锚）：可闻音频位置 − 已读帧内容位置，
     * 均相对本会话起点。正 = 声音超前画面（丢帧快进应把它压回 ±AV_SKIP_MS 内）；
     * 负 = 画面超前。丢帧快进计数 videoSkips 持续增长 = 软解吞吐贴实时线（复杂场景）。
     * audioBufMs = 锚修正量（管道+ALSA 缓冲 ≈ 写出量与可闻声的固有差），恒定即正常。 */
    if (s->audio_enabled) {
        double bytes_per_ms = ((double)(s->audio_rate > 0 ? s->audio_rate : 44100)) * 4.0 / 1000.0;
        long long content = s->video_base_ms + (long long)((double)s->frames * 1000.0 / (double)s->fps);
        long long apos = s->audio_base_ms + audio_anchor_ms(s);
        mk_int(ctx, "avDriftMs", (long)(apos - content), res);
        mk_int(ctx, "audioBufMs", (long)(((double)(s->pipe_cap_bytes + s->alsa_buf_bytes)) / bytes_per_ms), res);
        mk_int(ctx, "videoPipeKb", (long)(g_vpipe_bytes / 1024), res); /* v2.9.3 诊断：视频 pipe 实际 KB */
        mk_int(ctx, "videoPipeErr", (long)g_vpipe_errno, res);      /* v2.9.3 诊断：扩容失败 errno */
        mk_int(ctx, "paceAlignMs", s->pace_align_ms, res);            /* v2.9.3 诊断：自适应对齐量 */
    } else {
        mk_int(ctx, "avDriftMs", 0, res);
        mk_int(ctx, "audioBufMs", 0, res);
    }
    mk_int(ctx, "videoSkips", s->video_skips, res);
    mk_int(ctx, "videoRestarts", s->video_retries, res); /* v1.9.3：>0 即发生过视频断流重启 */
    /* v2.1.0 音画同步分层：resyncing=视频流正在强制重同步（首帧未产出）→ JS 显示"加载中…"；
     * resyncCount=漂移触发的强制重同步次数；avDriftNowMs=最近一次漂移（apos−due，诊断）。 */
    mk_bool(ctx, "resyncing", s->state == ST_PLAYING && s->resyncing, res);
    /* v2.2.4：本次重同步已持续毫秒数（未在重同步时为 0）——JS 据此只在"恢复确实慢"时才弹
     * 「加载中」，把 1~2s 的快速视频流重启做成静默自愈（用户只看到一次极短的画面停顿）。 */
    mk_int(ctx, "resyncingMs", s->resyncing ? (long)((now_seconds() - s->resync_at) * 1000.0) : 0, res);
    mk_int(ctx, "resyncCount", s->resync_count, res);
    mk_int(ctx, "avDriftNowMs", s->last_drift_ms, res);
    mk_int(ctx, "resyncLeadMs", s->resync_lead_ms, res); /* v2.2.0 诊断：落点前移量自适应值 */
    mk_bool(ctx, "audioIsBt", s->audio_is_bt, res);
    mk_str(ctx, "audioDevice", s->audio_dev[0] ? s->audio_dev : NULL, res);
    mk_bool(ctx, "frameValid", s->frame_valid, res);
    mk_int(ctx, "fbBuffers", fb_buffers_reported(), res);
    mk_int(ctx, "fbCurrentBuffer", fb_current_buffer(), res);
    mk_int(ctx, "pid", (long)s->ff_pid, res);
    mk_str(ctx, "error", s->error[0] ? s->error : NULL, res);
    pthread_mutex_unlock(&s->mu);
    return res;
}

static JSValue js_pause(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct session *s = &g_s;
    JSValue res = JS_NewObject(ctx);
    (void)this_val;
    (void)argc;
    (void)argv;
    pthread_mutex_lock(&s->mu);
    if (s->has_session && s->state == ST_PLAYING) {
        if (s->ff_pid > 0) kill(s->ff_pid, SIGSTOP);
        if (s->aplay_pid > 0) kill(s->aplay_pid, SIGSTOP);
        clock_gettime(CLOCK_MONOTONIC, &s->paused_at); /* v1.8.0：记暂停起点 */
        s->state = ST_PAUSED;
    }
    mk_bool(ctx, "ok", 1, res);
    mk_str(ctx, "state", state_name(s->state), res);
    mk_int(ctx, "positionMs", s->position_ms, res);
    pthread_mutex_unlock(&s->mu);
    return res;
}

static JSValue js_resume(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct session *s = &g_s;
    JSValue res = JS_NewObject(ctx);
    (void)this_val;
    (void)argc;
    (void)argv;
    pthread_mutex_lock(&s->mu);
    if (s->has_session && s->state == ST_PAUSED) {
        if (s->ff_pid > 0) kill(s->ff_pid, SIGCONT);
        if (s->aplay_pid > 0) kill(s->aplay_pid, SIGCONT);
        /* v1.8.0：把暂停时长补偿进墙钟基准 started_at——否则 paced（无音频/音频死亡）模式
         * 恢复后 target 远超 due，画面会以丢帧快进方式追赶整段暂停时长（闪跳）。
         * 音频时钟模式不受影响（锚 = writer 字节量，暂停期间 aplay 被 SIGSTOP → writer
         * 阻塞在管道写上 → 锚天然冻结，与画面一致）。 */
        {
            long paused_ms = (long)ms_since(&s->paused_at);
            struct timespec t = s->started_at;
            t.tv_sec += paused_ms / 1000;
            t.tv_nsec += (paused_ms % 1000) * 1000000L;
            if (t.tv_nsec >= 1000000000L) {
                t.tv_sec += 1;
                t.tv_nsec -= 1000000000L;
            }
            s->started_at = t;
        }
        s->state = ST_PLAYING;
    }
    mk_bool(ctx, "ok", 1, res);
    mk_str(ctx, "state", state_name(s->state), res);
    mk_int(ctx, "positionMs", s->position_ms, res);
    pthread_mutex_unlock(&s->mu);
    return res;
}

static JSValue js_seek(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct session *s = &g_s;
    int ms = 0;
    int rc = -1;
    JSValue res;
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "player.seek(positionMs)");
    JS_ToInt32(ctx, &ms, argv[0]);
    if (ms < 0) ms = 0;

    /* seek 只能同步完成（进程重启），调用方需容忍 100~500ms 阻塞 */
    pthread_mutex_lock(&s->mu);
    if (s->has_session) {
        int audio = s->audio_enabled;
        stop_children(s);
        session_reset(s);
        if (s->frame) {
            rc = session_start(s, ms, audio, NULL);
        }
    }
    res = JS_NewObject(ctx);
    mk_bool(ctx, "ok", rc == 0 ? 1 : 0, res);
    mk_str(ctx, "state", state_name(s->state), res);
    mk_int(ctx, "positionMs", s->position_ms, res);
    mk_int(ctx, "durationMs", s->duration_ms, res);
    if (rc != 0) mk_str(ctx, "error", s->error[0] ? s->error : "seek 失败", res);
    pthread_mutex_unlock(&s->mu);
    return res;
}

static JSValue js_stop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct session *s = &g_s;
    JSValue res = JS_NewObject(ctx);
    (void)this_val;
    (void)argc;
    (void)argv;
    pthread_mutex_lock(&s->mu);
    if (s->has_session) stop_children(s);
    session_reset(s);
    mk_bool(ctx, "ok", 1, res);
    mk_str(ctx, "state", state_name(s->state), res);
    pthread_mutex_unlock(&s->mu);
    return res;
}

static JSValue js_redraw(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct session *s = &g_s;
    JSValue res = JS_NewObject(ctx);
    int ok = 0;
    (void)this_val;
    (void)argc;
    (void)argv;
    pthread_mutex_lock(&s->mu);
    if (s->frame && s->frame_valid) {
        ok = fb_blit(s->frame, s->rect_x, s->rect_y, s->rect_w, s->rect_h) == 0;
    }
    mk_bool(ctx, "ok", ok, res);
    mk_bool(ctx, "frameValid", s->frame_valid, res);
    pthread_mutex_unlock(&s->mu);
    return res;
}

static JSValue js_release(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct session *s = &g_s;
    JSValue res = JS_NewObject(ctx);
    (void)this_val;
    (void)argc;
    (void)argv;
    pthread_mutex_lock(&s->mu);
    if (s->has_session) stop_children(s);
    session_reset(s);
    if (s->frame) {
        free(s->frame);
        s->frame = NULL;
    }
    mk_bool(ctx, "ok", 1, res);
    mk_str(ctx, "state", state_name(s->state), res);
    pthread_mutex_unlock(&s->mu);
    fb_close();
    return res;
}

static JSValue js_info(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue res = JS_NewObject(ctx);
    long vsize[2] = { 0, 0 };
    (void)this_val;
    (void)argc;
    (void)argv;
    mk_bool(ctx, "ok", 1, res);
    mk_str(ctx, "module", "player", res);
    mk_str(ctx, "version", PLAYER_VERSION, res);
    mk_int(ctx, "fbLineLength", (long)read_long_file(FB_STRIDE_PATH, 0), res);
    mk_int(ctx, "fbBpp", (long)read_long_file(FB_BPP_PATH, 0), res);
    read_pair_file(FB_VSIZE_PATH, &vsize[0], &vsize[1]);
    mk_int(ctx, "fbVirtualWidth", vsize[0], res);
    mk_int(ctx, "fbVirtualHeight", vsize[1], res);
    mk_int(ctx, "fbMapped", g_fb.map ? 1 : 0, res);
    mk_int(ctx, "renderBackend", g_rd.kms ? 1 : 0, res); /* 1 = KMS overlay plane */
    mk_int(ctx, "renderPlane", g_rd.kms ? KMS_PLANE_ID : 0, res);
    mk_str(ctx, "ffmpeg", FFMPEG_PATH, res);
    mk_str(ctx, "aplay", APLAY_PATH, res);
    return res;
}

/* ---------------- 模块注册 ---------------- */

static int player_module_init(JSContext *ctx, JSModuleDef *m) {
    JSValue def = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, def, "open", JS_NewCFunction(ctx, js_open, "open", 15));
    JS_SetPropertyStr(ctx, def, "pauseRender", JS_NewCFunction(ctx, js_pauseRender, "pauseRender", 0));
    JS_SetPropertyStr(ctx, def, "resumeRender", JS_NewCFunction(ctx, js_resumeRender, "resumeRender", 0));
    JS_SetPropertyStr(ctx, def, "pause", JS_NewCFunction(ctx, js_pause, "pause", 0));
    JS_SetPropertyStr(ctx, def, "resume", JS_NewCFunction(ctx, js_resume, "resume", 0));
    JS_SetPropertyStr(ctx, def, "seek", JS_NewCFunction(ctx, js_seek, "seek", 1));
    JS_SetPropertyStr(ctx, def, "status", JS_NewCFunction(ctx, js_status, "status", 0));
    JS_SetPropertyStr(ctx, def, "stop", JS_NewCFunction(ctx, js_stop, "stop", 0));
    JS_SetPropertyStr(ctx, def, "redraw", JS_NewCFunction(ctx, js_redraw, "redraw", 0));
    JS_SetPropertyStr(ctx, def, "release", JS_NewCFunction(ctx, js_release, "release", 0));
    JS_SetPropertyStr(ctx, def, "info", JS_NewCFunction(ctx, js_info, "info", 0));
    JS_SetModuleExport(ctx, m, "default", def);
    JS_SetModuleExport(ctx, m, "open", JS_NewCFunction(ctx, js_open, "open", 15));
    JS_SetModuleExport(ctx, m, "pause", JS_NewCFunction(ctx, js_pause, "pause", 0));
    JS_SetModuleExport(ctx, m, "resume", JS_NewCFunction(ctx, js_resume, "resume", 0));
    JS_SetModuleExport(ctx, m, "seek", JS_NewCFunction(ctx, js_seek, "seek", 1));
    JS_SetModuleExport(ctx, m, "status", JS_NewCFunction(ctx, js_status, "status", 0));
    JS_SetModuleExport(ctx, m, "pauseRender", JS_NewCFunction(ctx, js_pauseRender, "pauseRender", 0));
    JS_SetModuleExport(ctx, m, "resumeRender", JS_NewCFunction(ctx, js_resumeRender, "resumeRender", 0));
    JS_SetModuleExport(ctx, m, "stop", JS_NewCFunction(ctx, js_stop, "stop", 0));
    JS_SetModuleExport(ctx, m, "redraw", JS_NewCFunction(ctx, js_redraw, "redraw", 0));
    JS_SetModuleExport(ctx, m, "release", JS_NewCFunction(ctx, js_release, "release", 0));
    JS_SetModuleExport(ctx, m, "info", JS_NewCFunction(ctx, js_info, "info", 0));
    return 0;
}

static JSModuleDef *player_module_load(JSContext *ctx, const char *moduleName) {
    if (strcmp(moduleName, "player") == 0) {
        JSModuleDef *m = JS_NewCModule(ctx, moduleName, player_module_init);
        if (!m) return NULL;
        JS_AddModuleExport(ctx, m, "default");
        JS_AddModuleExport(ctx, m, "open");
        JS_AddModuleExport(ctx, m, "pause");
        JS_AddModuleExport(ctx, m, "resume");
        JS_AddModuleExport(ctx, m, "seek");
        JS_AddModuleExport(ctx, m, "status");
        JS_AddModuleExport(ctx, m, "pauseRender");
        JS_AddModuleExport(ctx, m, "resumeRender");
        JS_AddModuleExport(ctx, m, "stop");
        JS_AddModuleExport(ctx, m, "redraw");
        JS_AddModuleExport(ctx, m, "release");
        JS_AddModuleExport(ctx, m, "info");
        return m;
    }
    return NULL;
}

extern void registerCModuleLoader(const char *moduleName, LoadCModuleFunction loader);

JQUICK_EXPORT void custom_init_jsapis(void) {
    /* 子进程/管道写入对端消失时不要让进程被 SIGPIPE 杀掉 */
    signal(SIGPIPE, SIG_IGN);
    pthread_mutex_init(&g_s.mu, NULL);
    /* 文件描述符必须显式初始化为 -1：静态 0 会被当作 stdin 关闭掉 */
    g_s.video_fd = -1;
    g_s.audio_fd = -1;
    g_s.aplay_fd = -1;
    g_s.ff_pid = 0;
    g_s.aplay_pid = 0;
    session_reset(&g_s);
    registerCModuleLoader("player", &player_module_load);
}
