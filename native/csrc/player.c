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
 */

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

#define PLAYER_VERSION "1.7.0"
#define FB_PATH "/dev/fb0"
#define FB_PAN_PATH "/sys/class/graphics/fb0/pan"
#define FB_MODES_PATH "/sys/class/graphics/fb0/modes"
#define FB_STRIDE_PATH "/sys/class/graphics/fb0/stride"
#define FB_BPP_PATH "/sys/class/graphics/fb0/bits_per_pixel"
#define FB_VSIZE_PATH "/sys/class/graphics/fb0/virtual_size"
#define FFMPEG_PATH "/usr/bin/ffmpeg"
#define LOG_PATH "/tmp/vp_player.log"      /* 视频进程 stderr */
#define LOG_AUDIO_PATH "/tmp/vp_audio.log" /* 音频独立进程 stderr（分文件防 O_TRUNC 互毁取证） */
#define DM_FONT "/etc/miniapp/resources/fonts/HarmonyOS_Sans_SC_Regular.ttf" /* 中文（fontconfig 被禁 → drawtext 必须显式 fontfile，真机 CN_OK 验证） */
#define DM_LANE_COUNT 4

/* 预建 4 个弹幕泳道文本文件（drawtext 对 missing file 会报错拖垮滤镜链） */
static void init_dm_files(void) {
    int i;
    for (i = 0; i < DM_LANE_COUNT; i++) {
        char p[40];
        int fd;
        snprintf(p, sizeof(p), "/tmp/bili_dm%d.txt", i);
        fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
    }
}
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
    int audio_active;    /* ffmpeg 当前确实在有音轨输出 */
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
    long underruns;             /* writer 等水位/空环轮次（≥1 即发生过欠载等待） */
    long wr_errors;             /* aplay 写失败次数（设备崩溃/断流） */
    long ring_drops;            /* 环满被丢弃的字节数（蓝牙消费慢的直接证据） */
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

static int spawn_ffmpeg(struct session *s, long start_ms, int with_audio) {
    int vpipe[2] = { -1, -1 };
    int apipe[2] = { -1, -1 };
    pid_t pid;
    char ss[32];
    char rbuf[32];
    char vf[1200]; /* 4泳道弹幕 drawtext 链（fontfile + reload 每帧重读 textfile） */
    char arbuf[16]; /* 音频采样率（蓝牙 48000 / 内置 44100） */
    char hdr[544]; /* "Referer: <url>\r\n"，仅当设置了 referer 才用 */
    char logfd_path_check[8];

    if (pipe(vpipe) != 0) return -1;
    if (with_audio && pipe(apipe) != 0) {
        close(vpipe[0]);
        close(vpipe[1]);
        return -1;
    }
    /* 视频 pipe 扩 1MB / 音频 256KB：视频 blit 的瞬时波动不再堵住 ffmpeg 的 mux 输出，
     * 防止"视频 pipe 满 → 音频输出被牵连停顿"（零星卡顿的候选来源之一）。
     * fcntl 失败保持默认 64KB（无害）。 */
    fcntl(vpipe[1], F_SETPIPE_SZ, 1048576);
    if (with_audio) fcntl(apipe[1], F_SETPIPE_SZ, 262144);
    /* 输出帧是"物理方向"的矩形（已转置），因此 scale 用 (out_h, out_w)。
     * 弹幕：4 泳道 drawtext 烧进视频帧（与 blit 同源 → 与 UI 层永不互踩 = 零闪）。
     * **drawtext 必须在 transpose 之前**：内容靠转置才在竖屏上正立，字与内容走同一条
     * 变换链方向才一致（画在转置后=字方向错，真机实测竖排 bug）；转置前是 452x254 横帧，
     * x=w-mod(t*SPEED\,w+text_w) 滚动、泳道 y 在 254 高度内，速度/相位/颜色四泳道错开。 */
    snprintf(vf, sizeof(vf),
             "scale=%d:%d"
             ",drawtext=fontfile=" DM_FONT ":textfile=/tmp/bili_dm0.txt:reload=1:fontcolor=white:fontsize=17:x=w-mod(t*110\\,w+text_w):y=8"
             ",drawtext=fontfile=" DM_FONT ":textfile=/tmp/bili_dm1.txt:reload=1:fontcolor=0xFFFFE0:fontsize=17:x=w-mod(t*142\\,w+text_w):y=72"
             ",drawtext=fontfile=" DM_FONT ":textfile=/tmp/bili_dm2.txt:reload=1:fontcolor=0xA0E8FF:fontsize=17:x=w-mod(t*90\\,w+text_w):y=136"
             ",drawtext=fontfile=" DM_FONT ":textfile=/tmp/bili_dm3.txt:reload=1:fontcolor=0xFFC8D8:fontsize=17:x=w-mod(t*166\\,w+text_w):y=200"
             ",transpose=%d,format=rgb32",
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
            char *argv[56]; /* 双输入 × (probesize/rw_timeout/ss/ua/headers) 后最多 50 项，留 NULL 余量 */
            int i = 0;
            argv[i++] = "ffmpeg";
            argv[i++] = "-nostdin";
            argv[i++] = "-hide_banner";
            argv[i++] = "-loglevel";
            argv[i++] = "error";
            argv[i++] = "-threads";
            argv[i++] = "3"; /* 4×A53 留一核给音频链/系统，防解码占满引发周期抢占 */
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
            argv[i++] = "15000000"; /* 15s 读写超时：烂节点挂起 → ffmpeg 显式失败可诊断，永不无期等待 */
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
            execv(FFMPEG_PATH, argv);
        } else {
            char *argv[40];
            int i = 0;
            argv[i++] = "ffmpeg";
            argv[i++] = "-nostdin";
            argv[i++] = "-hide_banner";
            argv[i++] = "-loglevel";
            argv[i++] = "error";
            argv[i++] = "-threads";
            argv[i++] = "3"; /* 同音频分支：留一核 */
            /* 拆进程后 dash 视频走本分支 —— 探测上限必须与音频分支一致：
             * 缺失时默认 probesize(5MB) 在烂网络上 = 首帧 10s+/seek 12s 零帧（1.6.0 实测） */
            argv[i++] = "-probesize";
            argv[i++] = "65536";
            argv[i++] = "-analyzeduration";
            argv[i++] = "5000000";
            argv[i++] = "-rw_timeout";
            argv[i++] = "15000000";
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
            execv(FFMPEG_PATH, argv);
        }
        _exit(127);
    }
    /* 父进程 */
    close(vpipe[1]);
    if (apipe[1] >= 0) close(apipe[1]);
    s->ff_pid = pid;
    s->video_fd = vpipe[0];
    s->audio_fd = apipe[0];
    s->audio_active = with_audio;
    return 0;
}

/* 自动挑选音频输出设备：
 *   本机音频走 ALSA；内置扬声器是默认设备，蓝牙耳机则要显式指定 bluealsa 的 PCM。
 *   `bluealsa-aplay -L` 的输出形如（真机实测）：
 *       bluealsa:SRV=org.bluealsa,DEV=B0:A3:F2:AD:7E:FA,PROFILE=a2dp
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
        char *argv[40];
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
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, 1);
                close(devnull);
            }
            /* aplay 的 stderr 落盘：蓝牙 PCM 打不开/参数不对时能直接看到原因 */
            {
                int alog = open(APLAY_LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (alog >= 0) {
                    dup2(alog, 2);
                    close(alog);
                }
            }
        }
        argv[i++] = "aplay";
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
    return 0;
}

/* ---------------- 线程 ---------------- */

/* 线程内的状态更新故意不加锁：
 * stop_children() 是在持有 s->mu 的情况下 join 本线程的，
 * 若这里再去抢 s->mu 会直接死锁。stop_flag 先于 kill/join 置位，
 * 因此线程能安全地写完最后状态并退出。 */

static void *video_thread(void *arg) {
    struct session *s = (struct session *)arg;
    size_t fsz = (size_t)s->out_w * (size_t)s->out_h * 4u;
    unsigned char *buf = s->frame;
    int failed = 0;

    while (!s->stop_flag) {
        struct pollfd pfd;
        int pr;
        pfd.fd = s->video_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pr = poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            failed = 1;
            break;
        }
        if (pr == 0) continue;
        if (read_full(s->video_fd, buf, fsz) != 0) {
            failed = 1;
            break;
        }
        if (s->paced) {
            long long elapsed = ms_since(&s->started_at);
            long long target = (long long)((double)(s->frames + 1) * 1000.0 / (double)s->fps);
            while (!s->stop_flag && elapsed < target) {
                usleep(5000);
                elapsed = ms_since(&s->started_at);
            }
            if (s->stop_flag) break;
        } else if (s->audio_input[0] && s->audio_enabled) {
            /* v1.6.0 拆进程后的音画同步：以真实播放位置（writer 写出量）为时钟。
             * 视频超前 → 等到帧到时；音频进程死/时钟停滞 8s → 转墙钟节拍（画面继续，声音降级）。 */
            long long due = s->start_ms + (long long)((double)(s->frames + 1) * 1000.0 / (double)s->fps);
            int waited = 0;
            while (!s->stop_flag && s->audio_alive) {
                double bytes_per_ms = ((double)(s->audio_rate > 0 ? s->audio_rate : 44100)) * 4.0 / 1000.0;
                long long apos = s->start_ms + (long long)((double)s->writer_bytes / bytes_per_ms);
                if (apos >= due) break;
                usleep(2000);
                waited += 2;
                if (waited > 8000) break;
            }
            if (s->stop_flag) break;
            if (!s->audio_alive || waited > 8000) s->paced = 1; /* 音频时钟不可用 → 墙钟接管 */
        }
        s->frames++;
        s->position_ms = s->start_ms + (long)((double)s->frames * 1000.0 / (double)s->fps);
        s->frame_valid = 1;
        /* render_paused：评论面板/系统UI覆盖时暂停 fb 输出（解码/位置/音频照常）。
         * 根因（2026-09-24 真机三现象钉死）：视频矩形像素归 blit（33ms 写两块）专属，
         * 任何 UI 覆盖都会与其交替抢帧（弹幕层/评论面板/下拉控制中心均闪；暂停后不闪=blit 停）。 */
        if (!s->render_paused && fb_blit(buf, s->rect_x, s->rect_y, s->rect_w, s->rect_h) != 0) {
            set_error(s, "framebuffer 写入失败");
            s->state = ST_ERROR;
            return NULL;
        }
    }

    if (s->stop_flag) return NULL;

    if (failed && s->frames == 0 && s->audio_enabled && !s->retried_no_audio) {
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
    size_t low_level = rate_bytes * RING_LOW_MS / 1000;
    size_t high_level = rate_bytes * RING_HIGH_MS / 1000;
    if (high_level > sizeof(s->ring) - 32768) high_level = sizeof(s->ring) - 32768; /* 留 feeder 空间 */
    if (low_level >= high_level) low_level = high_level / 2;
    /* 起播预蓄：攒够 500ms 再放行 */
    size_t start_level = rate_bytes * RING_START_MS / 1000;
    if (start_level > high_level) start_level = high_level;
    while (!s->stop_flag && s->rlen < start_level) usleep(20000);
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
    s->audio_active = 0;
    s->audio_is_bt = 0;
    s->audio_dev[0] = '\0';
    s->paced = 0;
    s->retried_no_audio = 0;
    s->frame_valid = 0;
    s->error[0] = '\0';
    s->underruns = 0;
    s->wr_errors = 0;
    s->ring_drops = 0;
    ring_reset(s);
}

/* 启动会话（调用方需保证已有会话已停止） */
static int session_start(struct session *s, long start_ms, int with_audio, const char *audio_device) {
    s->stop_flag = 0;
    init_dm_files(); /* 弹幕泳道文件清空（每开播重置） */
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
    s->paced = with_audio ? 0 : 1;
    s->underruns = 0;
    s->wr_errors = 0;
    s->ring_drops = 0;
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

    /* DASH（有第二输入音频轨）→ 音频走独立进程，视频进程只需单输出；
     * durl 回退（单文件，音视频同文件）→ 保持原双输出同进程模式 */
    int split_audio = with_audio && s->audio_input[0];
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

/* writeDm(idx 0..3, text)：写弹幕泳道文本文件（drawtext reload=1 每帧重读 → 即时生效）。
 * text 空 = 清空该泳道；UTF-8 原样写入（中文经 fontfile 渲染）。 */
static JSValue js_writeDm(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int idx = 0;
    size_t len = 0;
    const char *text;
    char path[40];
    int fd;
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "writeDm(idx, text)");
    JS_ToInt32(ctx, &idx, argv[0]);
    if (idx < 0 || idx >= DM_LANE_COUNT) return JS_ThrowRangeError(ctx, "idx 0..3");
    text = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!text) return JS_EXCEPTION;
    if (len > 240) {
        JS_FreeCString(ctx, text);
        return JS_ThrowRangeError(ctx, "text too long");
    }
    snprintf(path, sizeof(path), "/tmp/bili_dm%d.txt", idx);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        JS_FreeCString(ctx, text);
        return JS_ThrowInternalError(ctx, "open dm file failed");
    }
    if (len > 0) {
        if (write(fd, text, len) < 0) {
            close(fd);
            JS_FreeCString(ctx, text);
            return JS_ThrowInternalError(ctx, "write failed");
        }
    }
    close(fd);
    JS_FreeCString(ctx, text);
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
    JS_SetPropertyStr(ctx, def, "writeDm", JS_NewCFunction(ctx, js_writeDm, "writeDm", 2));
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
    JS_SetModuleExport(ctx, m, "writeDm", JS_NewCFunction(ctx, js_writeDm, "writeDm", 2));
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
        JS_AddModuleExport(ctx, m, "writeDm");
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
