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
 * 管线：
 *   ffmpeg -ss <start> -i <input> -map 0:v:0 -vf scale=..,transpose=.. -r <fps>
 *          -pix_fmt rgb32 -f rawvideo pipe:3
 *          -map 0:a:0? -ac 2 -ar 44100 -f s16le pipe:4
 *   → 视频线程按帧读入并贴到 /dev/fb0 的物理矩形（当前 pan 缓冲）
 *   → 音频线程把 pipe:4 的数据写进 aplay stdin（阻塞写＝用音频时钟给整条管线限速）
 *
 * 无音轨时 ffmpeg 会因 "Output file #1 does not contain any stream" 失败，
 * 此时视频线程在第 0 帧失败后自动以"仅视频 + 时钟节拍"方式重启一次。
 *
 * JS 用法：
 *   import { open, pause, resume, seek, status, stop, redraw, release } from 'player'
 *   open(input, startMs, durationMs, fps, audio, transpose, x, y, w, h
 *        [, audioDevice[, userAgent, referer]]) -> { ok, state, ... }
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
#include <sys/mman.h>
#include <sys/resource.h> /* setpriority：aplay 提优先级 */
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031 /* linux fcntl.h；老 sysroot 兜底 */
#endif

static void raise_audio_thread_prio(void); /* 定义在 RING 水位区（feeder 调用点更早） */

#define PLAYER_VERSION "2.2.7"

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
#define FFMPEG_PATH "/usr/bin/ffmpeg"
#define LOG_PATH "/tmp/vp_player.log"      /* 视频进程 stderr */
#define LOG_AUDIO_PATH "/tmp/vp_audio.log" /* 音频独立进程 stderr（分文件防 O_TRUNC 互毁取证） */
#define APLAY_PATH "/usr/bin/aplay"
#define BT_LIST_CMD "/usr/bin/bluealsa-aplay -L 2>/dev/null"
#define APLAY_LOG_PATH "/tmp/vp_aplay.log"

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

static int fb_open(void) {
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

static void fb_close(void) {
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
static int fb_blit(const unsigned char *frame, int x, int y, int w, int h) {
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
static int fb_current_buffer(void) {
    long pan[2] = { 0, 0 };
    int visible = g_fb.visible_h > 0 ? g_fb.visible_h : g_fb.yres_virtual;
    if (read_pair_file(FB_PAN_PATH, &pan[0], &pan[1]) != 0) return 0;
    if (visible <= 0) return 0;
    return (int)(pan[1] / visible);
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

static int spawn_ffmpeg(struct session *s, long start_ms, int with_audio) {
    int vpipe[2] = { -1, -1 };
    int apipe[2] = { -1, -1 };
    pid_t pid;
    char ss[32];
    char rbuf[32];
    char vf[128]; /* scale+transpose（弹幕烧帧已移除：drawtext×4 逐帧渲染占解码预算 ~20%，
                   * 是弱视频卡顿主因——2026-10-01 用户实测关弹幕即流畅） */
    char arbuf[16]; /* 音频采样率（蓝牙 48000 / 内置 44100） */
    char hdr[544]; /* "Referer: <url>\r\n"，仅当设置了 referer 才用 */
    char logfd_path_check[8];

    if (pipe(vpipe) != 0) return -1;
    if (with_audio && pipe(apipe) != 0) {
        close(vpipe[0]);
        close(vpipe[1]);
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
    /* 输出帧是"物理方向"的矩形（已转置），因此 scale 用 (out_h, out_w)。
     * 弹幕烧帧已移除（v2.7.0）：drawtext×4 reload=1 逐帧重渲染文字占解码预算 ~20%，
     * 弱稿件上把软解压到 1x 以下（CPU 100% 实证）→ 用户决策整体移除弹幕功能。 */
    snprintf(vf, sizeof(vf),
             "scale=%d:%d,transpose=%d,format=rgb32",
             s->out_h, s->out_w, s->transpose);
    snprintf(ss, sizeof(ss), "%ld.%03ld", (long)(start_ms / 1000), (long)(start_ms % 1000));
    snprintf(rbuf, sizeof(rbuf), "%d", s->fps);
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
        /* 子进程：视频 → fd3，音频 → fd4 */
        setpgid(0, 0);
        if (dup2(vpipe[1], 3) < 0) _exit(126);
        if (with_audio) {
            if (dup2(apipe[1], 4) < 0) _exit(126);
        }
        close(vpipe[0]);
        close(vpipe[1]);
        if (apipe[0] >= 0) close(apipe[0]);
        if (apipe[1] >= 0) close(apipe[1]);
        logfd = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd >= 0) {
            dup2(logfd, 2);
            close(logfd);
        }
        if (with_audio) {
            char *argv[80]; /* 双输入 × (probesize/rw_timeout/reconnect/thread_queue_size/ss/ua/headers)
                             * 后最多 70 项，留 NULL 余量 */
            int i = 0;
            argv[i++] = "ffmpeg";
            argv[i++] = "-nostdin";
            argv[i++] = "-hide_banner";
            argv[i++] = "-loglevel";
            argv[i++] = "error";
            argv[i++] = "-threads";
            argv[i++] = "4"; /* v1.9.5：4 线程吃满 4×A53（v2.6.0 起源分辨率受 310K 像素预算约束，
                              * 内存 ~10MB 可控）；音频链多为阻塞等待，实测不需要独占核 */
            argv[i++] = "-skip_loop_filter";
            argv[i++] = "all"; /* v1.9.5：跳过 h264 环路滤波（解码成本 ~25%）——254px 屏上
                                * 去块纹路不可见，换来的吞吐余量直接决定是否掉帧 */
            /* v2.9.1 实测记录：曾试 `-flags2 +fast`（解码器非规范优化路径）→ 真机 40~50s 窗口
             * 采样显示视频解码反而 60.3→69.6 jiffies/s（更贵），且属"非规范"标志有画质风险 →
             * 已回退。结论：本机 CPU 不是瓶颈（整机仅约 25% 忙），解码降本旋钮收益为负，不加。 */
            if (s->user_agent[0]) {
                argv[i++] = "-user_agent";
                argv[i++] = s->user_agent;
            }
            if (s->referer[0]) {
                snprintf(hdr, sizeof(hdr), "Referer: %s\r\n", s->referer);
                argv[i++] = "-headers";
                argv[i++] = hdr;
            }
            argv[i++] = "-sws_flags";
            argv[i++] = "fast_bilinear";
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
            argv[i++] = "-reconnect";
            argv[i++] = "1"; /* v1.9.3：CDN 断流协议级重连（Range 续传），修"视频/音频进程中途退出" */
            argv[i++] = "-reconnect_streamed";
            argv[i++] = "1";
            argv[i++] = "-reconnect_delay_max";
            argv[i++] = "10";
            argv[i++] = "-thread_queue_size";
            argv[i++] = AV_PREFETCH_PACKETS_VIDEO_STR; /* v2.2.0 输入线程预取压缩包（≈15~25s） */
            argv[i++] = "-ss";
            argv[i++] = ss;
            argv[i++] = "-i";
            argv[i++] = s->input;
            if (s->audio_input[0]) {
                /* DASH 第二输入：音频轨独立下载（实测 ~66kbps，带宽富余 10 倍 → 声音供给恒稳）。
                 * -ss/UA/headers 逐输入配齐：seek 同起点、CDN 反爬同凭据。 */
                argv[i++] = "-probesize";
                argv[i++] = "65536";
                argv[i++] = "-analyzeduration";
                argv[i++] = "5000000";
                argv[i++] = "-rw_timeout";
                argv[i++] = "15000000";
                argv[i++] = "-reconnect";
                argv[i++] = "1";
                argv[i++] = "-reconnect_streamed";
                argv[i++] = "1";
                argv[i++] = "-reconnect_delay_max";
                argv[i++] = "10";
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
            argv[i++] = "-map";
            argv[i++] = "0:v:0";
            argv[i++] = "-vf";
            argv[i++] = vf;
            argv[i++] = "-r";
            argv[i++] = rbuf;
            argv[i++] = "-f";
            argv[i++] = "rawvideo";
            argv[i++] = "pipe:3";
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
            argv[i] = NULL;
            /* 视频 ffmpeg 提优先级（v1.9.4，nice -5 与音频解码同档）：软解+滤镜链吞吐
             * 贴实时线时，JS/UI 的周期性突发会瞬时挤占解码线程 → 掉帧积压。音画连续
             * 优先于 UI 丝滑（UI 突发短，A53×4 下仍有余量）。root 下生效，失败无害。 */
            setpriority(PRIO_PROCESS, 0, -5);
            execv(FFMPEG_PATH, argv);
        } else {
            char *argv[48];
            int i = 0;
            argv[i++] = "ffmpeg";
            argv[i++] = "-nostdin";
            argv[i++] = "-hide_banner";
            argv[i++] = "-loglevel";
            argv[i++] = "error";
            argv[i++] = "-threads";
            argv[i++] = "4"; /* v1.9.5：同上（源分辨率受像素预算约束） */
            argv[i++] = "-skip_loop_filter";
            argv[i++] = "all"; /* v1.9.5：同上（解码降本 ~25%） */
            /* 拆进程后 dash 视频走本分支 —— 探测上限必须与音频分支一致：
             * 缺失时默认 probesize(5MB) 在烂网络上 = 首帧 10s+/seek 12s 零帧（1.6.0 实测） */
            argv[i++] = "-probesize";
            argv[i++] = "65536";
            argv[i++] = "-analyzeduration";
            argv[i++] = "5000000";
            argv[i++] = "-rw_timeout";
            argv[i++] = "15000000"; /* v2.2.5：同上（与 20s 缓冲同量级；早退会白丢缓冲） */
            argv[i++] = "-reconnect";
            argv[i++] = "1"; /* v1.9.3：CDN 断流协议级重连（Range 续传） */
            argv[i++] = "-reconnect_streamed";
            argv[i++] = "1";
            argv[i++] = "-reconnect_delay_max";
            argv[i++] = "10";
            if (s->user_agent[0]) {
                argv[i++] = "-user_agent";
                argv[i++] = s->user_agent;
            }
            if (s->referer[0]) {
                snprintf(hdr, sizeof(hdr), "Referer: %s\r\n", s->referer);
                argv[i++] = "-headers";
                argv[i++] = hdr;
            }
            argv[i++] = "-sws_flags";
            argv[i++] = "fast_bilinear";
            argv[i++] = "-thread_queue_size";
            argv[i++] = AV_PREFETCH_PACKETS_VIDEO_STR; /* v2.2.0 输入线程预取压缩包（≈15~25s） */
            argv[i++] = "-ss";
            argv[i++] = ss;
            argv[i++] = "-i";
            argv[i++] = s->input;
            argv[i++] = "-an";
            argv[i++] = "-vf";
            argv[i++] = vf;
            argv[i++] = "-r";
            argv[i++] = rbuf;
            argv[i++] = "-f";
            argv[i++] = "rawvideo";
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
    s->ff_pid = pid;
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
 * 返回 0 表示找到了蓝牙设备。 */
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
    return -1;
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
        argv[i++] = "-reconnect";
        argv[i++] = "1"; /* v1.9.3：音频轨断流重连（soak 末段实测过 CDN 断流杀进程） */
        argv[i++] = "-reconnect_streamed";
        argv[i++] = "1";
        argv[i++] = "-reconnect_delay_max";
        argv[i++] = "10";
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
#define AV_SKIP_MS 100     /* 帧落后音频钟超过此值 → 平滑快进（≈3-4 帧 @24fps；漂移有界的关键） */
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
static long long audio_anchor_ms(const struct session *s) {
    double bytes_per_ms = ((double)(s->audio_rate > 0 ? s->audio_rate : 44100)) * 4.0 / 1000.0;
    long long audible = (long long)s->writer_bytes - s->pipe_cap_bytes - s->alsa_buf_bytes;
    if (audible < 0) audible = 0;
    return (long long)((double)audible / bytes_per_ms);
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
    size_t fsz = (size_t)s->out_w * (size_t)s->out_h * 4u;
    unsigned char *buf = s->frame;
    int failed = 0;

    while (!s->stop_flag) {
        struct pollfd pfd;
        int pr;
        int do_blit = 1;
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
            if (s->frames == 0 && (now_seconds() - s->video_spawn_at) * 1000.0 > (double)AV_FIRST_FRAME_MAX_MS) {
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
        if (read_full(s->video_fd, buf, fsz) != 0) {
            failed = 1;
            break;
        }
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
            /* durl（音视频同进程）等待限幅：视频线程不读帧 → video pipe 满 → ffmpeg 阻塞
             * → 音频断供 → 锚停走 → 继续等 = 死锁。限幅后贴屏放行 pipe 解锁 ffmpeg。
             * DASH 音频独立进程，无此耦合 → 不限幅（供给停滞时画面冻结等声）。 */
            int wait_cap = s->audio_input[0] ? 0 : AV_WAIT_CAP_MS;
            while (!s->stop_flag && s->audio_alive && s->frames > 0 && apos < due) {
                usleep(2000);
                waited += 2;
                if (wait_cap && waited >= wait_cap) break;
                apos = s->start_ms + audio_anchor_ms(s);
            }
            if (s->stop_flag) break;
            drift = apos - due;
            s->last_drift_ms = (long)drift;
            if (s->frames > 0 && !s->audio_alive) {
                s->paced = 1; /* 音频时钟死亡 → 墙钟接管 */
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
                /* ① 轻微滞后 → 平滑快进追上（v2.1.0）：每帧都贴屏、不等钟，画面以当前解码产速
                 * 连续快进（不再旧的 0.6s 节流 → 消除"网络波动后一段 1.6fps 顿挫"）。产速>1x
                 * 时积压自然排空；仍不收敛则累计到 >AV_RESYNC_MS 升级为②强制重同步。 */
                s->video_skips++;
                do_blit = 1;
            }
        }
        s->frames++;
        s->ever_played = 1; /* v2.1.0：本会话已出过画（重启后 frames 归零，据此区分中途断流与从未出画） */
        s->stall_resyncs = 0; /* v2.1.0：有新帧 = 空转期重同步成功/视频链恢复 → 计数清零 */
        s->position_ms = s->video_base_ms + (long)((double)s->frames * 1000.0 / (double)s->fps);
        s->frame_valid = 1;
        /* 帧已产出：放行音频起播门（重启后的首帧也算产出——门只关心视频链路活着）。
         * v2.1.0 起滞后帧也逐帧贴屏（平滑快进），故 last_blit_at 每帧刷新、不会误报停帧。 */
        s->video_first_frame = 1;
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
        if (do_blit && !s->render_paused && fb_blit(buf, s->rect_x, s->rect_y, s->rect_w, s->rect_h) != 0) {
            set_error(s, "framebuffer 写入失败");
            s->state = ST_ERROR;
            return NULL;
        }
    }

    if (s->stop_flag) return NULL;

    if (failed && (s->ever_played || s->resync_timeout) && s->video_retries < 5 &&
        s->audio_input[0] && s->audio_enabled && s->audio_alive &&
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
    s->transpose = (transpose == 2) ? 2 : 1;
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
    mk_int(ctx, "fbBuffers", g_fb.buffers, res);
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
    mk_str(ctx, "ffmpeg", FFMPEG_PATH, res);
    mk_str(ctx, "aplay", APLAY_PATH, res);
    return res;
}

/* ---------------- 模块注册 ---------------- */

static int player_module_init(JSContext *ctx, JSModuleDef *m) {
    JSValue def = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, def, "open", JS_NewCFunction(ctx, js_open, "open", 13));
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
    JS_SetModuleExport(ctx, m, "open", JS_NewCFunction(ctx, js_open, "open", 13));
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
