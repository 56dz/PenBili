/*
 * session.c - player2 播放会话实现（worker 线程独占 GStreamer）
 *
 * 本文件是整个模块的核心。设计原则见 session.h 顶部注释，这里只补充实现细节。
 */
#include "session.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio/audio_out.h"
#include "core/clock.h"
#include "core/log.h"
#include "session/cmd.h"
#include "source/src_http.h"
#include "third_party/gst_min.h"
#include "video/kms_plane.h"

/* ---------------- 面板几何（与 player.c 保持一致） ---------------- */
#define PB2_PANEL_X_OFFSET 107
#define PB2_KMS_PLANE_ID 76
#define PB2_PANEL_RATE "1497/25"
#define PB2_MAX_LATENESS 40

/* ---------------- 时间常量（毫秒） ---------------- */
#define PB2_SEEK_DEBOUNCE_MS 250   /* 背靠背 seek 合并窗口（实测需 200~300） */
#define PB2_BUS_TICK_MS 100        /* bus 轮询周期 */
#define PB2_PLANE_TICK_MS 2000     /* 平面检查周期 */
#define PB2_MARKER_TICK_MS 20000   /* /tmp/mp3playing 保活重写周期 */
#define PB2_SEEK_ASYNC_TIMEOUT_MS 3000
#define PB2_PREROLL_TIMEOUT_MS 12000
/*
 * 【P0-1】首帧超时：OPEN（或 seek 重建）之后这么久还没有任何一帧上屏，
 * 就判失败。B 站 m4s URL 带 deadline（约 2h 过期）、断网、ffmpeg 秒退
 * 都会落进这个窗口。以前这里是「永久挂起且不报错」。
 */
#define PB2_FIRST_FRAME_TIMEOUT_MS 15000
/* ffmpeg 存活探测周期（每 tick 都 waitpid 太浪费，1s 足够） */
#define PB2_SRC_CHECK_MS 1000
/*
 * 【P0-2】release() 的有界等待：JSAPI 线程最多等这么久让 worker 自己收尾。
 * worker 可能正卡在 12s get_state / 3s seek 轮询 / reap_pair / amixer spawn 里，
 * 绝不能无上界 join（页面切走时会把 QuickJS 主线程冻住十几秒）。
 */
#define PB2_SHUTDOWN_WAIT_MS 300
/*
 * 【P0-2】release() 超时把 worker 转 detached 之后，g_w 仍被那个线程使用。
 * 下一次 open 必须等它真的退出才能 memset(&g_w) —— 否则新会话把结构体清零、
 * 旧线程还在读写同一个地址，是纯内存竞争。这里给它一个有界的重开等待。
 */
#define PB2_REOPEN_WAIT_MS 1500
/* OPEN 之后平面需要一点时间才上屏，这期间丢失不算异常（P0-2） */
#define PB2_PLANE_GRACE_MS 2000
/* 连续丢失达到这个次数且屏幕亮着才报异常（P0-2） */
#define PB2_PLANE_LOST_REPORT 3
/*
 * seek 端到端目标（下发 -> 第一帧上屏），仅用于日志判定。
 *
 * 800 -> 1500：单 FIFO 双输入方案下，一个 ffmpeg 要串行建两路 HTTP 连接，
 * 实测首帧 926~1193ms（旧双 ffmpeg 并行是 490~650ms）。这是架构换来的
 * 代价，仍在人眼可接受范围。若继续用 800 判定，日志会一直 SLOW，
 * 真回归反而看不出来。
 *
 * 1500 -> 2600：为了修「seek 后开头静音 T-K 秒」，spawn 前多了一次 ffprobe
 * 关键帧探测（`-read_intervals T%+2`，实测 550~750ms）。这段是**纯串行**开销，
 * 直接加在 seek 路径上。实测端到端 1909ms（首帧本身仍是 1079ms，没变慢），
 * 所以预算按 2600 给。关键帧探测换来的是「静音窗口从最坏 5s 降到 0」，
 * 这个交换对用户体验是明确的正收益 —— 用户报的是静音，不是慢 1 秒。
 */
#define PB2_SEEK_TARGET_MS 2600

/*
 * 视频队列的限界方式 —— 这是一个踩了两天的死锁，务必别改回按 buffer 个数。
 *
 * 【现象】改成「单 ffmpeg 双输入 + 单 matroskademux」之后，seek 一过音频就
 * 再也不到（`av: first audio packet` 一行都没有、`ab=0`），frames 冻结在
 * 21~25，ffmpeg 进程停在 S 态（阻塞在写 FIFO）。起播正常，只有 seek 后死。
 *
 * 【根因】seek 后两路首包 PTS 不同：视频从关键帧 K 开始（-ss 之前最近的
 * 关键帧），音频从 T 开始。实测 T-K = 3.843s（视频 946.044 / 音频 949.867）。
 * matroskademux 在流式模式下必须「把落后的那一路推进到对齐点」才能开始
 * interleave，GST_DEBUG 里能看到：
 *     gst_matroska_demux_sync_streams:<d> Synchronizing stream 1 with other
 *     by advancing time from 0:00:00.000000000 to 0:15:43.577000000
 * 它要把音频从 0 推到 943.577s。这期间音频包被丢弃/跳过，但**视频包会被
 * 往前推**。视频队列若按「8 个 buffer」限界（8×33.37ms≈267ms），远装不下
 * 3.84s 的推进量 -> queue 满 -> demuxer 阻塞 -> 整条管线死锁。
 *
 * 【实测矩阵】（同一个文件、同一条命令，只改 queue 参数，gst-launch 直跑）
 *   视频 8 buffer        / 音频 32 buffer   -> 60s 超时（死锁）
 *   视频 8 buffer        / 音频无界         -> 超时（死锁）
 *   视频 无界            / 音频 32 buffer   -> 536ms 跑完  ✅
 *   视频 无界            / 音频无界         -> 537ms 跑完  ✅
 *   视频 time=2s         / 音频 32 buffer   -> 超时（死锁）
 *   视频 time=3s         / 音频 32 buffer   -> 543ms 跑完  ✅
 *   视频 bytes=2MB/6MB   / 音频 32 buffer   -> 545ms 跑完  ✅
 * 结论：卡死的是**视频**队列，且必须给到「能装下 T-K 那段推进量」的容量。
 * 按 buffer 个数限界是错的单位（buffer 时长随帧率变化，8 个在 30fps 只有
 * 267ms）；按时间或字节限界才能表达「至少装下几秒的视频」这个需求。
 *
 * 【为什么取 8s / 6MB 而不是刚好 4s】
 * 真机关键帧间隔实测 3.84s，但 B 站不同清晰度/不同视频的关键帧间隔可能更大
 * （3s 才刚好过关说明余量很小）。8s 给 2 倍余量；再叠一个 6MB 字节上限兜住
 * 「音频极少、视频极多」的极端情况（6MB 在 852x480 下约 10s 视频，够用且在
 * 词典笔上完全可接受）。两个上限是「或」关系，谁先到算谁。
 * 注意 audio 分支的 32 buffer 保持原样 —— 实测它不参与这个死锁。
 */
#define PB2_VIDEO_QUEUE_NS 8000000000LL
#define PB2_VIDEO_QUEUE_BYTES 6000000

/* ---------------- 全局状态（只在 worker 里改） ---------------- */
typedef struct {
    /* 线程 */
    pthread_t th;
    int th_started;
    /*
     * 【P0-2】worker 退出握手。
     *   worker_done：worker_main 的最后一句（在 return NULL 之前）置 1；
     *   worker_detached：release() 等超时后把线程 detach 了，这个 pthread_t
     *      已经不可 join，下一次 pb2_session_start 必须另起一个新线程。
     * 两者都由 g_start_mu 保护（跨线程写）。
     */
    int worker_done;
    int worker_detached;
    pb2_cmdq_t q;
    pb2_stats_t stats;
    pb2_probe_t probe;

    /* gst */
    int gst_ready;
    const char *gst_version;
    GstElement *pipe;
    GstElement *vsink;      /* 视频分支的 bin（ghost pad 挂 probe 用） */
    GstElement *vsink_el;   /* 分支内部名为 "vsink" 的 kmssink 元素（SET_RECT 用；bin 上没有 render-rectangle 属性） */
    /*
     * 平面是否被 SET_RECT 主动隐藏（键盘避让期间 1x1）。
     * check_plane() 必须据此跳过巡检：隐藏时 kmssink 把平面缩成 1x1，
     * 几何上看仍在 ACTIVE，但某些帧序下会短暂读到非 ACTIVE，
     * 于是每秒误报一次 `plane missing ... streak=N -- reported` 并
     * patch 出 PB2_E_PLANE_LOST，让 JS 侧弹「画面丢失」的假错误。
     * 隐藏是**我们自己主动做的**，不是故障，所以巡检要整个跳过。
     */
    int rect_hidden;
    GstElement *asink;      /* 音频分支的 alsasink */
    /*
     * 音频输出设备名。空串 = 走 ALSA 默认（pcm.!default -> speaker -> hw:1,0）。
     * 蓝牙耳机连上时是 "bluealsa:DEV=<MAC>,PROFILE=a2dp"。
     * 【为什么必须显式传】蓝牙 A2DP 走的是 bluealsa 的 D-Bus 通道，完全
     * 不经过 /etc/asound.conf，所以「连上蓝牙」不会改变 pcm.!default，
     * 不显式指定就永远从喇叭出声（实测确认）。
     */
    char adev[128];
    /*
     * 播放器自己的音量（0..100）。GStreamer 侧用 volume 元素，不碰 ALSA
     * 混音器 —— 实测 hw:1,0 直连时 hal-audio 写的 card1 numid54 根本不进
     * 播放链路（softvol 层没生效），所以系统控制中心调音量对播放无效。
     * 我们的做法：读 sys_config 的 spk_volume / bt_volume 映射到 volume 属性。
     */
    GstElement *vvol;       /* 音频分支里的 volume 元素 */
    int vol_pct;            /* 当前应用音量 0..100 */
    int vol_applied;        /* 已下发到 vvol 的值（-1 = 未下发） */
    long long vol_check_ms; /* 上次复查系统音量的时间（节流） */
    long long bt_check_ms;  /* 上次探测蓝牙路由的时间（独立节流，见下） */
    int bt_present;         /* 上次探测时蓝牙是否可用（用于检测插拔） */
    int bt_vol_applied;     /* 已下发给耳机的 AVRCP 音量（-1 = 未下发） */
    char bt_ctl_name[64];   /* 耳机的 mixer 控件名，形如 "Redmi Buds 6 - A2DP" */
    GstBus *bus;
    int vbranch;
    int abranch;
    int has_audio;          /* 是否真的建了音频分支 */
    int pipeline_alive;

    /* 媒体源（本地直读 / ffmpeg -> FIFO） */
    pb2_src_t src;
    int src_ready;
    long long base_ms;      /* 对 JS 报告位置 = base_ms + query_position() */

    /* 当前会话参数（OPEN 时写入） */
    pb2_open_req_t *req;
    int rotate;
    char rect[64];

    /* seek debounce */
    long long seek_target;
    int seek_pending;
    long long seek_last_ms;     /* 最后一次收到 seek 的时间 */
    long long seek_issue_ms;    /* 真正执行 seek 的时间 */
    int seek_coalesced;         /* 本次拖动被合并掉的次数（验证覆盖式 debounce） */
    int seek_frame_reported;    /* 本次 seek 的首帧耗时是否已上报（P1-3） */
    int mixer_pending; /* 1=mixer_setup 待做, 2=mixer_mute 待做, 0=无；见 do_open */

    /* 平面误报抑制 */
    long long plane_grace_until_ms; /* OPEN 后的宽限截止（此前的丢失不算异常） */

    /*
     * 【P0-1】首帧超时监控。
     *   frame_epoch_ms：本次 OPEN / seek 重建「开始等首帧」的时刻。
     *   没有任何一帧时不得把它当成 -1 哨兵（JS 的 `-1 > 8000` 恒 false，
     *   看门狗永远不触发 → 界面永久"加载中…"）。
     */
    long long frame_epoch_ms;
    /*
     * 【A/V 同步判据】本次 OPEN / seek 重建的下发时刻，专门给「音频首包延迟」
     * 当日志基准。不能复用 frame_epoch_ms —— 那个字段在首帧到达时就被清零
     * （它是给 check_first_frame 计时用的），而音频首包总是晚于视频首帧，
     * 清零后就只能退回 t_start（整会话起点），打出来的差值毫无意义。
     */
    long long audio_epoch_ms;
    long long last_src_check_ms; /* ffmpeg 存活探测的上次时间（首帧之前用） */
    /*
     * 【P1-1】首帧之后的源存活探测。
     * 用独立的节流时间戳：和 check_first_frame() 共用一个字段会互相饿死
     * （两个函数轮流把 last_src_check_ms 推到现在，谁都等不到 PB2_SRC_CHECK_MS）。
     *
     * src_died 是「写端已死、hold_fd 已放掉、正在等 EOS」的标记。
     * 故意不立刻 patch_alive(0)：hold_fd 一关 filesrc 就 EOF、demuxer 马上
     * 发 EOS，两者时间差很小；提前把 pipelineAlive 置 0 会让 JS 在 EOS
     * 到达前抢跑一次 recover，白白重建一遍管线。
     */
    long long last_src_check2_ms;
    int src_died;
    long long src_died_at_ms;
    int first_frame_logged;      /* 首帧到达日志只打一次 */
    int first_audio_logged;      /* 首个音频包到达日志只打一次 */
    /*
     * 【avSkewMs 判据（2026-11-01，lead 定的量化口径）】
     * 音画是否同步，不能看 JS 的 drift（那是 positionMs 与 seek 目标的差）。
     * 真正可算的是「demuxer 时间轴位置」与「已送显内容时长」的差：
     *     content_ms = (frames - seek_frames_base) / fps * 1000
     *     avSkewMs   = (pos - base_ms) - content_ms
     * 两路对齐时应稳定在 0 附近；音频比视频领先 (T-K) 时它停在 -(T-K)。
     *
     * 用标称 30fps（面板实际 29.67fps，1.1% 误差 ⇒ 每 5s 累积约 55ms），
     * 所以只在 seek 后前几秒读它最有意义，不要拿十几秒后的值当绝对值。
     */
    unsigned long long seek_frames_base; /* seek/open 起点时的 probe.frames */
    long long avskew_last_log_ms;
    long long avskew_last_ms;            /* 最近一次算出的 avSkewMs（诊断） */
    int fps_cap;                 /* JS 传下来的帧率上限（0 = 不限制，P1-18） */
    /*
     * 【真机 2026-10-31】三连 seek 的第三跳视频 ffmpeg 秒退（CDN 掐流，
     * 日志 `pid=2732 gone` → `first frame failed +1000ms`），用户直接看到
     * 播放错误。ffmpeg 秒退很多是瞬态（HTTP 2xx 后连接被重置），重开一次
     * 往往就好 —— 所以首帧之前源死允许自愈一次（整个会话限 2 次，
     * 防止 URL 真失效时白转 30s）。0=未用，>0=已用次数。
     */
    int first_frame_retries;

    /*
     * 【P1-3】seek 时「先起新源、再拆旧管线」，旧源的两个 ffmpeg 与 FIFO 先挂在这，
     * 由 worker 空闲时回收，不占 seek 的关键路径。
     */
    pb2_src_t dying_src;
    int dying_pending;

    /* 计数 */
    long long t_start;          /* 本次会话开始时间 */
    long long last_pos_ms;
    unsigned long long last_abytes;
    long long last_plane_ms;
    long long last_marker_ms;
    long long last_fps_ms;
    unsigned long long fps_frames_base;
    int fps_calc;
    int plane_lost_streak;
    int eos;
    int paused;
} pb2_worker_t;

static pb2_worker_t g_w;
static _Atomic int g_started = 0;
static pthread_mutex_t g_start_mu = PTHREAD_MUTEX_INITIALIZER;
/*
 * 【P0-2】worker 退出握手用的条件变量。
 * worker_main 的最后一件事（return NULL 之前）在 g_start_mu 下置
 * worker_done = 1 并 broadcast；pb2_session_shutdown 在同一把锁上做**有界**
 * timedwait，超时就把线程 detach 掉直接返回，绝不让 JSAPI 线程无限挂住。
 * 静态初始化：避免 init/destroy 生命周期与 detach 路径互相打架。
 */
static pthread_cond_t g_done_cv = PTHREAD_COND_INITIALIZER;

/* ============================ 工具 ============================ */

static char *xstrdup(const char *s) {
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s);
    p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

void pb2_open_req_free(pb2_open_req_t *req) {
    if (!req) return;
    free(req->input);
    free(req->input2);
    free(req->userAgent);
    free(req->referer);
    free(req->audioDevice);
    free(req);
}

static int is_http(const char *s) {
    return s && (strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0);
}

static int rotation_for_transpose(int t) {
    if (t == 1) return 90;
    if (t == 3) return 180;
    return 270; /* t == 2，JS 恒传 */
}

/* 把字符串里会破坏 gst desc 语法的字符转义（用于 location= 引号内） */
static void desc_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    size_t i;
    if (!in || cap == 0) {
        if (cap) out[0] = '\0';
        return;
    }
    for (i = 0; in[i] && o + 2 < cap; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = c;
        } else if ((unsigned char)c < 0x20) {
            out[o++] = ' ';
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

/* ============================ 视频/音频分支构建 ============================ */

/*
 * 取 pad 当前 caps 的 mime 名（"video/x-h264" / "audio/mpeg" ...）。
 *
 * mpegversion_out：音频编码版本（audio/mpeg 的 mpegversion=4 才是 AAC，
 * =1 是 MP3）。必须看这个字段，不能靠 mime 名区分 —— 踩过的坑：
 * 只看 "audio/mpeg" 就把 AAC 当成未知编码丢给 decodebin，
 * gst_pad_link 返回 -4 (NOFORMAT)，音频分支永远建不起来，
 * 连带 pipeline position 卡在 0（因为 audio sink 从没收到数据）。
 */
static const char *pad_caps_name(GstPad *pad, char *buf, size_t cap, int *mpegversion_out) {
    GstCaps *caps = gst_pad_get_current_caps(pad);
    GstStructure *st;
    const char *name = NULL;
    if (mpegversion_out) *mpegversion_out = 0;
    if (!caps) caps = gst_pad_query_caps(pad, NULL);
    if (!caps) return NULL;
    if (gst_caps_get_size(caps) < 1) {
        gst_caps_unref(caps);
        return NULL;
    }
    st = gst_caps_get_structure(caps, 0);
    if (st) {
        const char *n = gst_structure_get_name(st);
        if (n) {
            snprintf(buf, cap, "%s", n);
            name = buf;
        }
        if (mpegversion_out) {
            int mv = 0;
            if (gst_structure_get_int(st, "mpegversion", &mv)) *mpegversion_out = mv;
        }
    }
    gst_caps_unref(caps);
    return name;
}

static GstElement *parse_bin(const char *desc) {
    GError *err = NULL;
    GstElement *b = gst_parse_bin_from_description(desc, 1, &err);
    if (!b) {
        pb2_logf("parse_bin FAIL: %s | desc=%s", err ? err->message : "?", desc);
        if (err) g_error_free(err);
    }
    return b;
}

/*
 * pad-added 回调：GStreamer 流线程调用。
 *
 * 这里**不**能做任何耗时的同步操作（它会阻塞整个 demuxer），
 * 但 gst_bin_add + gst_pad_link + sync_state_with_parent 是标准做法，
 * 耗时在亚毫秒级，且必须在流线程里做（否则 pad 会消失）。
 *
 * 注意：不能用 gst_element_get_static_pad 之外的方式拿 bin 的 sink pad，
 * 因为我们用 gst_parse_bin_from_description(..., TRUE) 生成了 ghost pad。
 */
static int on_video_buffer(GstPad *pad, GstPadProbeInfo *info, void *ud);
static int on_audio_buffer(GstPad *pad, GstPadProbeInfo *info, void *ud);
static int on_audio_list(GstPad *pad, GstPadProbeInfo *info, void *ud);
static void apply_volume_now(void);
static void apply_bt_avrcp_volume(void);

static void on_pad_added(GstElement *el, GstPad *pad, void *data) {
    char capsbuf[128];
    const char *cn;
    GstElement *bin = NULL;
    GstPad *sink;
    int rc;
    int mpegversion = 0;
    char desc[1024];
    char fpscap[64];

    (void)el;
    (void)data;

    cn = pad_caps_name(pad, capsbuf, sizeof(capsbuf), &mpegversion);
    pb2_logf("pad-added caps=%s", cn ? cn : "(none)");
    if (!cn) return;

    /*
     * 【P1-18】帧率上限（JS 的 fpsCap / fps）。
     *
     * 旧 v1 实现（player.c:703-705）是 `videorate max-rate=<fpsCap>`，插在
     * mppvideodec 之后、面板率归一的 videorate 之前。真机实测：720p60 源不加
     * 这一级就是 57.39fps（面板 60Hz 全速解码，耗电/发热翻倍），加 max-rate=30
     * 后稳定 29.73fps、dropped=0；30fps 源加 max-rate=30 是直通、不插帧。
     * v2 重写时把这个参数丢了（jsapi 写了 req->fpsCap 但没人读），
     * 这里补回来：cap > 0 时插一级限速 videorate，= 0 时保持原样（不限）。
     */
    if (g_w.fps_cap > 0) {
        snprintf(fpscap, sizeof(fpscap), "videorate max-rate=%d ! ", g_w.fps_cap);
    } else {
        fpscap[0] = '\0';
    }

    if (strncmp(cn, "video/", 6) == 0) {
        if (g_w.vbranch) {
            pb2_logf("pad-added: video branch exists, ignore extra pad");
            return;
        }
        if (strstr(cn, "h264")) {
            snprintf(desc, sizeof(desc),
                     /* 视频队列必须按「时间/字节」限界，不能按 buffer 个数（见 PB2_VIDEO_QUEUE_*） */
                     "queue max-size-buffers=0 max-size-bytes=%d "
                     "max-size-time=%lld ! "
                     "h264parse ! mppvideodec rotation=%d ! "
                     "%s"
                     "videorate ! video/x-raw,framerate=%s ! "
                     "kmssink name=vsink plane-id=%d driver-name=rockchip sync=true "
                     "max-lateness=%d qos=true force-aspect-ratio=false skip-vsync=true "
                     "render-rectangle=%s",
                     PB2_VIDEO_QUEUE_BYTES, (long long)PB2_VIDEO_QUEUE_NS,
                     g_w.rotate, fpscap, PB2_PANEL_RATE, PB2_KMS_PLANE_ID, PB2_MAX_LATENESS,
                     g_w.rect);
        } else if (strstr(cn, "h265") || strstr(cn, "hevc")) {
            snprintf(desc, sizeof(desc),
                     "queue max-size-buffers=0 max-size-bytes=%d "
                     "max-size-time=%lld ! "
                     "h265parse ! mppvideodec rotation=%d ! "
                     "%s"
                     "videorate ! video/x-raw,framerate=%s ! "
                     "kmssink name=vsink plane-id=%d driver-name=rockchip sync=true "
                     "max-lateness=%d qos=true force-aspect-ratio=false skip-vsync=true "
                     "render-rectangle=%s",
                     PB2_VIDEO_QUEUE_BYTES, (long long)PB2_VIDEO_QUEUE_NS,
                     g_w.rotate, fpscap, PB2_PANEL_RATE, PB2_KMS_PLANE_ID, PB2_MAX_LATENESS,
                     g_w.rect);
        } else {
            pb2_logf("pad-added: unsupported video caps %s", cn);
            return;
        }
        bin = parse_bin(desc);
        if (!bin) {
            pb2_stats_patch_error(&g_w.stats, PB2_E_PIPELINE, "video branch build failed");
            return;
        }
        /*
         * 【P0-4】这里**绝不能**提前置 vbranch。
         * 以前的写法是在 gst_bin_add / get_static_pad / gst_pad_link 之前就置 1，
         * 而下面两条早退路径直接 return —— 于是 vbranch 永远为 1，后续 demuxer
         * 抛出的每一个 video pad 都被开头的 `if (g_w.vbranch) ignore extra pad`
         * 挡掉，永久黑屏且 pipelineAlive 仍为 1、lastError 仍是 0。
         * 现在改成「成功连上之后才置位」（见函数末尾）。
         */
    } else if (strncmp(cn, "audio/", 6) == 0) {
        if (!g_w.req || !g_w.req->audio) {
            pb2_logf("pad-added: audio disabled by request, skip");
            return;
        }
        if (g_w.abranch) {
            pb2_logf("pad-added: audio branch exists, ignore extra pad");
            return;
        }
        /*
         * AAC 判定：audio/mpeg 且 mpegversion=4。
         * 也接受 caps 里带 "aac" 字样（某些 demuxer 会给 audio/x-h264 之外的别名）。
         * mpegversion=1 是 MP3，走 decodebin 通用链路。
         */
        /*
         * alsasink 的 device= 参数。
         *   adev 为空 -> 不写（走 pcm.!default = speaker = hw:1,0）
         *   adev 非空 -> device="<adev>"（蓝牙 bluealsa PCM）
         * gst-launch 风格 desc 里带逗号/冒号的值必须用引号包住，
         * 否则 parse_bin 会在 ':' ',' 处截断。
         */
        {
            char devarg[192];
            if (g_w.adev[0]) {
                snprintf(devarg, sizeof(devarg), " device=\"%s\"", g_w.adev);
            } else {
                devarg[0] = '\0';
            }
        if ((strncmp(cn, "audio/mpeg", 10) == 0 && mpegversion == 4) ||
            strstr(cn, "aac") || strstr(cn, "AAC")) {
            snprintf(desc, sizeof(desc),
                     "queue max-size-buffers=32 max-size-bytes=0 max-size-time=0 ! "
                     "aacparse ! faad ! audioconvert ! audioresample ! "
                     "audio/x-raw,rate=48000,channels=2 ! volume name=vvol ! "
                     "alsasink name=asink buffer-time=30000 latency-time=10000%s",
                     devarg);
        } else if (strstr(cn, "x-ac3") || strstr(cn, "x-eac3") || strstr(cn, "x-a52")) {
            snprintf(desc, sizeof(desc),
                     "queue max-size-buffers=32 ! ac3parse ! a52dec ! audioconvert ! "
                     "audioresample ! audio/x-raw,rate=48000,channels=2 ! volume name=vvol ! "
                     "alsasink name=asink buffer-time=30000 latency-time=10000%s",
                     devarg);
        } else {
            /* 未知音频编码：只要有 parser 就走通用链路，失败也只是没声音 */
            pb2_logf("pad-added: audio caps %s, trying generic chain", cn);
            snprintf(desc, sizeof(desc),
                     "queue max-size-buffers=32 ! decodebin ! audioconvert ! "
                     "audioresample ! audio/x-raw,rate=48000,channels=2 ! volume name=vvol ! "
                     "alsasink name=asink buffer-time=30000 latency-time=10000%s",
                     devarg);
        }
        } /* end devarg scope */
        bin = parse_bin(desc);
        if (!bin) {
            pb2_logf("pad-added: audio branch build failed, continue video-only");
            return;
        }
        /* 【P0-4】同视频分支：不在这里置 abranch，成功 link 之后才置 */
    } else {
        return;
    }

    /* 关键顺序：先 add，再 link，再 sync_state_with_parent。
     * 反过来会拿到 NULL sink pad 或者状态不同步。 */
    if (!gst_bin_add((GstBin *)g_w.pipe, bin)) {
        pb2_logf("pad-added: gst_bin_add failed");
        gst_object_unref(bin);
        return;
    }

    sink = gst_element_get_static_pad(bin, "sink");
    if (!sink) {
        /*
         * 【P0-4】早退时必须把 bin 从 pipeline 摘掉再 unref，否则 bin 已进
         * pipeline（pipeline 持一份引用），我们这份 unref 之后它还挂着。
         * 同时明确 patch error —— 这条路径以前一个字都不打，JS 侧完全不知情。
         */
        pb2_logf("pad-added: bin has no ghost sink pad (caps=%s)", cn);
        pb2_stats_patch_error(&g_w.stats, PB2_E_BRANCH_LINK, "branch: no ghost sink pad");
        gst_bin_remove((GstBin *)g_w.pipe, bin);
        gst_object_unref(bin);
        return;
    }

    /*
     * probe 必须在**数据开始流之前**挂上，所以顺序是：
     *   get sink pad -> add probe -> link -> sync_state_with_parent
     * 挂在这个 bin 的 ghost sink pad 上（而不是里面的 kmssink），
     * 因为分支内部元素名是我们自己定的，而 ghost pad 一定存在。
     *
     * 踩过的坑：如果在 build_pipeline 之后由 worker 调 attach_probes()，
     * 那时 pad-added 还没触发、vsink 还是 NULL，probe 永远挂不上，
     * status().frames 恒 0（画面其实在播，position 正常前进）。
     */
    if (strncmp(cn, "video/", 6) == 0) {
        gst_pad_add_probe(sink, GST_PAD_PROBE_TYPE_BUFFER, on_video_buffer, NULL, NULL);
    } else {
        /*
         * 【音量统计冻结修复 v2】音频 probe 挂在 **demuxer 的 src pad**（pad），
         * 不是分支 bin 的 ghost sink pad（sink）。
         *
         * 实测证据：挂在 ghost sink pad 上时，起播阶段能收到一两个 buffer
         * （ab=6027 就冻在那儿），而 PCM 一直是 RUNNING、hw_ptr 稳定前进
         * （4.1M 帧 ≈ 85s 音频真的在播）—— 数据到了 alsasink，却没经过
         * 我们的 probe。挂到 src pad 上就没有这层不确定性：所有音频数据
         * 都从这里产生，BUFFER 和 BUFFER_LIST 都能看到，DROP 门同样生效。
         */
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, on_audio_buffer, NULL, NULL);
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER_LIST, on_audio_list, NULL, NULL);
    }

    rc = gst_pad_link(pad, sink);
    if (rc != 0) {
        /*
         * 【P0-4】link 失败：把 bin 从管线摘掉 + patch error。
         * 以前这里只打一行 log 就 return，vbranch 仍是 1 → 之后所有 video pad
         * 都被 `if (g_w.vbranch) ignore extra pad` 挡掉 → 永久黑屏、无错误上报。
         */
        pb2_logf("pad-added: gst_pad_link rc=%d (caps=%s) -> branch dropped", rc, cn);
        pb2_stats_patch_error(&g_w.stats, PB2_E_BRANCH_LINK, "branch: gst_pad_link failed");
        gst_object_unref(sink);
        gst_bin_remove((GstBin *)g_w.pipe, bin);
        gst_object_unref(bin);
        return;
    }

    gst_element_sync_state_with_parent(bin);

    if (strncmp(cn, "video/", 6) == 0) {
        g_w.vsink = bin;
        /*
         * 【键盘避让 SET_RECT】vsink 存的是 parse_bin 出来的**分支 bin**，
         * 不是 kmssink 元素本身。gst_util_set_object_arg 只对 GstObject 的
         * 可写属性生效，bin 上没有 render-rectangle，SET_RECT 会静默无效。
         * 所以这里立刻用 gst_bin_get_by_name 把内部名为 "vsink" 的 kmssink
         * 拿出来存进 g_w.vsink_el（GET_BY_NAME 返回新引用，teardown 随 pipe
         * 一起释放，与上面同规矩）。
         */
        g_w.vsink_el = gst_bin_get_by_name((GstBin *)bin, "vsink");
        /* 【P0-4】成功连上之后才置位 */
        g_w.vbranch = 1;
        pb2_logf("video branch ready (probe attached)");
    } else {
        g_w.asink = bin;
        g_w.has_audio = 1;
        /*
         * 取出音量元素。volume 是 linear 0..1 的属性（不是 dB），
         * 所以百分比要除以 100。拿不到就退化为「不控制音量」，
         * 不影响播放本身。
         */
        g_w.vvol = gst_bin_get_by_name((GstBin *)bin, "vvol");
        g_w.vol_applied = -1;
        if (g_w.vvol) {
            apply_volume_now();
            apply_bt_avrcp_volume();
        } else {
            pb2_logf("audio: no volume element (vvol) in branch, volume control disabled");
        }
        /* 【P0-4】同上 */
        g_w.abranch = 1;
        pb2_stats_patch_audio(&g_w.stats, 1, 0);
        pb2_logf("audio branch ready (probe attached) dev=%s vol=%d%%",
                 g_w.adev[0] ? g_w.adev : "(default speaker)", g_w.vol_pct);
    }
    gst_object_unref(sink);
}

/* ============================ 音量（应用级） ============================ */

/*
 * 把 g_w.vol_pct 下发到 volume 元素。
 *
 * 【为什么不用 ALSA 混音器】实测：alsasink 直开 hw:1,0 时，hal-audio
 * 写的 card1 numid54 完全不进播放链路（asound.conf 的 softvol 层没有
 * 真正生效），所以系统控制中心调音量对播放零影响。我们改成自己在
 * GStreamer 里做增益，读的仍是系统音量（sys_config 的 spk_volume /
 * bt_volume），这样控制中心和播放器保持一致，且不破坏用户设置。
 */
/*
 * 把音量同步给蓝牙耳机的 AVRCP 绝对音量（bluealsa 的 'Xxx - A2DP' 控件）。
 *
 * 【为什么还要这一手】GStreamer 的 volume 元素只在本地做软件增益，而蓝牙
 * 耳机的**硬件**音量是独立的（bluealsa 暴露的 pvolume，实测默认卡在 30%
 * 从不变化）。只做软件增益的话：用户把控制中心拉到 0 时耳机确实静音了
 * （软件增益 0），但拉到 100 时耳机仍按自己的 30% 播放，听感上限被锁死。
 * 同步过去让两边一致。失败不影响播放（耳机可能不支持绝对音量）。
 *
 * 只在路由是蓝牙时做；值没变就不重复下发（amixer 一次 ~30ms）。
 */
static void apply_bt_avrcp_volume(void) {
    char vol[16];
    char out[256];
    char *argv[8];
    int rc;

    if (!g_w.bt_present || !g_w.adev[0]) return;
    if (g_w.bt_vol_applied == g_w.vol_pct) return;
    snprintf(vol, sizeof(vol), "%d%%", g_w.vol_pct);
    argv[0] = (char *)"/usr/bin/amixer";
    argv[1] = (char *)"-D";
    argv[2] = (char *)"bluealsa";
    argv[3] = (char *)"sset";
    argv[4] = (char *)g_w.bt_ctl_name; /* 设备名 - A2DP，探测时填 */
    argv[5] = vol;
    argv[6] = NULL;
    if (!g_w.bt_ctl_name[0]) return;
    /*
     * 必须捕获输出：宿主 SIGCHLD=SIG_IGN 让 waitpid 恒 ECHILD，pb2_run 只能
     * 靠 /proc 判活，返回 0 表示「进程跑过了」而不是「成功了」。amixer 失败
     * 时退出码非 0 但拿不到，所以只能看它打印了什么。
     */
    rc = pb2_run(argv, out, (int)sizeof(out), 1500);
    if (out[0]) {
        char *nl = strchr(out, '\n');
        if (nl) *nl = '\0';
    }
    g_w.bt_vol_applied = g_w.vol_pct;
    pb2_logf("audio: bt avrcp volume -> %s (rc=%d) ctl='%s' out='%s'", vol, rc,
             g_w.bt_ctl_name, out);
}

static void apply_volume_now(void) {
    if (!g_w.vvol) return;
    if (g_w.vol_applied == g_w.vol_pct) return;
    g_object_set(g_w.vvol, "volume", (double)g_w.vol_pct / 100.0, NULL);
    g_w.vol_applied = g_w.vol_pct;
    pb2_logf("audio: volume %d%% -> %.3f", g_w.vol_pct, (double)g_w.vol_pct / 100.0);
}

/*
 * 读 /data/cfg/sys_config.conf 里的某个整数键（spk_volume / bt_volume）。
 *
 * 为什么不调 hal-audio 读：hal-audio 只有 -g（读）但输出的是 "file:0" 这种
 * 无意义内容（实测），而 sys_config.conf 是纯文本 key=value，直接读更可靠，
 * 也不需要起子进程（省 30ms+ 且不依赖 PATH）。
 * 返回 0..100；读不到返回 -1。
 */
static int read_sys_config_int(const char *key, int defval) {
    FILE *fp;
    char line[256];
    size_t klen = strlen(key);
    int val = -1;

    fp = fopen("/data/cfg/sys_config.conf", "r");
    if (!fp) return defval;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, key, klen) != 0) continue;
        if (line[klen] != '=') continue;
        val = atoi(line + klen + 1);
        break;
    }
    fclose(fp);
    if (val < 0) return defval;
    if (val > 100) val = 100;
    return val;
}

/*
 * 周期性同步「系统音量」和「音频路由」。
 *
 * 两条独立的事：
 *   ① 音量：控制中心改的是 sys_config.conf 的 spk_volume（实测 hal-audio -s N
 *      同时写 numid54 和这个文件）。播放器读它映射到 volume 元素，这样
 *      下拉控制中心才真正影响播放。
 *   ② 路由：蓝牙耳机插拔后必须重建音频分支才能换 device —— alsasink 的
 *      device 属性在 PLAYING 状态下改了也不会重开 PCM。这里检测到变化就
 *      投递一次原地 seek（复用已有的重建机制，代价 ~1.3s），比偷偷重建
 *      音频分支安全得多（后者要处理 pad 重新 link、时钟重锚一堆状态）。
 */
static void refresh_audio_route(void) {
    long long now = pb2_now_ms();
    char bt[128];

    if (now - g_w.vol_check_ms < 1000) return;
    g_w.vol_check_ms = now;

    /* ① 音量：spk_volume 是控制中心唯一写的键（见 do_open 里的实测说明） */
    {
        int pct = read_sys_config_int("spk_volume", -1);
        if (pct >= 0 && pct != g_w.vol_pct) {
            g_w.vol_pct = pct;
            apply_volume_now();
            apply_bt_avrcp_volume();
        }
    }

    /* ② 路由（只在有会话、已建音频分支时才关心） */
    if (!g_w.req || !g_w.pipeline_alive) return;
    /*
     * 蓝牙探测独立节流到 5s：pb2_bt_pcm_detect 内部要起 bluetoothctl
     * （devices + 每台 info，共 2~3 次 fork/exec，实测 60~200ms）。
     * 每秒跑一次会吃掉 worker 近 20% 的 CPU，而插拔耳机是秒级事件，
     * 5s 的检测延迟完全够用。
     */
    if (now - g_w.bt_check_ms < 5000) return;
    g_w.bt_check_ms = now;
    if (pb2_bt_pcm_detect(bt, (int)sizeof(bt)) == 0) {
        if (!g_w.bt_present || strcmp(bt, g_w.adev) != 0) {
            g_w.bt_present = 1;
            snprintf(g_w.adev, sizeof(g_w.adev), "%s", bt);
            g_w.bt_vol_applied = -1;
            {
                const char *mac = strstr(bt, "DEV=");
                g_w.bt_ctl_name[0] = '\0';
                if (mac) {
                    char macbuf[24];
                    const char *e = mac + 4;
                    size_t n2 = 0;
                    while (*e && *e != ',' && n2 + 1 < sizeof(macbuf)) {
                        macbuf[n2++] = *e++;
                    }
                    macbuf[n2] = '\0';
                    if (n2 == 17) {
                        char nm[48];
                        if (pb2_bt_name_detect(macbuf, nm, (int)sizeof(nm)) == 0) {
                            snprintf(g_w.bt_ctl_name, sizeof(g_w.bt_ctl_name),
                                     "%s - A2DP", nm);
                        }
                    }
                }
            }
            pb2_logf("audio: BT sink appeared (%s) -> rebuild pipeline to route audio", bt);
            g_w.vol_applied = -1; /* 新分支的 volume 元素还没设过 */
            goto rebuild;
        }
    } else {
        if (g_w.bt_present || g_w.adev[0]) {
            g_w.bt_present = 0;
            g_w.adev[0] = '\0';
            g_w.bt_ctl_name[0] = '\0';
            g_w.bt_vol_applied = -1;
            pb2_logf("audio: BT sink gone -> rebuild pipeline to use speaker");
            g_w.vol_applied = -1;
            goto rebuild;
        }
    }
    return;

rebuild:
    if (g_w.paused) {
        /*
         * 暂停态不重建（重建会把画面刷出来）。记一个待办，等 resume 后
         * 由下一次 tick 再触发 —— 那时 g_w.paused 已经是 0。
         */
        pb2_logf("audio: route change deferred (paused)");
        g_w.vol_check_ms = 0;
        return;
    }
    if (g_w.seek_pending) return; /* 已经在重建了 */
    g_w.seek_target = g_w.last_pos_ms;
    g_w.seek_pending = 1;
    g_w.seek_last_ms = pb2_now_ms() - PB2_SEEK_DEBOUNCE_MS - 1;
    pb2_stats_patch_seek(&g_w.stats, g_w.seek_target, 1);
    pb2_logf("audio: rebuild at %lldms for route change", g_w.seek_target);
}

/* ============================ 视频帧计数 probe ============================ */

static int on_video_buffer(GstPad *pad, GstPadProbeInfo *info, void *ud) {
    (void)pad;
    (void)info;
    (void)ud;
    atomic_fetch_add_explicit(&g_w.probe.frames, 1, memory_order_relaxed);
    atomic_store_explicit(&g_w.probe.lastFrameMs, pb2_now_ms(), memory_order_relaxed);

    /*
     * 【P0-1】首帧到达：停掉「无首帧」计时。
     *
     * frame_epoch_ms 归零表示已经有帧了，check_first_frame() 不再计时；
     * 同时把 resyncing 清掉，避免 JS 那边一直以为在重同步。
     */
    if (g_w.frame_epoch_ms > 0) {
        long long wait = pb2_now_ms() - g_w.frame_epoch_ms;
        g_w.frame_epoch_ms = 0;
        if (!g_w.first_frame_logged) {
            g_w.first_frame_logged = 1;
            /*
             * 【日志纠正】以前这里打的是 `pb2_now_ms() - g_w.t_start`，而 t_start 是
             * **open() 那一刻**，seek 之后并不重置。于是 seek 落点首帧会打出
             * `first frame on screen +48341ms (budget 15000ms) SLOW` 这种吓人的行，
             * 实际只等了 500ms。判据必须是「从开始等首帧那一刻算起」，
             * 也就是 frame_epoch_ms（open 时置 t0，seek 时重置）。
             * 注意：这只是日志，check_first_frame() 一直用的是 frame_epoch_ms，
             * 所以功能上从未误判，别被这行 SLOW 误导。
             */
            pb2_logf("first frame on screen +%lldms (budget %dms) %s", wait,
                     PB2_FIRST_FRAME_TIMEOUT_MS,
                     wait <= PB2_FIRST_FRAME_TIMEOUT_MS ? "OK" : "SLOW");
        }
        if (g_w.stats.resyncing) pb2_stats_patch_resync(&g_w.stats, 0, 0);
    }

    /*
     * 【P1-3】seek 端到端计时：do_seek 记下 seek_issue_ms，
     * 这里第一帧真正到达视频分支时打一次「下发 -> 上屏」耗时。
     * 只在 seek 后报一次（seek_frame_reported 复位在 do_seek 里）。
     */
    if (g_w.seek_issue_ms > 0 && !g_w.seek_frame_reported) {
        long long dt = pb2_now_ms() - g_w.seek_issue_ms;
        g_w.seek_frame_reported = 1;
        pb2_logf("SEEK first frame on screen +%lldms (budget %dms) %s", dt,
                 PB2_SEEK_TARGET_MS, dt <= PB2_SEEK_TARGET_MS ? "OK" : "SLOW");
    }
    return GST_PAD_PROBE_OK;
}

static int on_audio_buffer(GstPad *pad, GstPadProbeInfo *info, void *ud) {
    void *buf;
    size_t sz;
    (void)pad;
    (void)ud;
    /*
     * 【音量统计冻结 + 开头无声的真正根因（2026-10-31 深夜，实测定位）】
     *
     * 现象：ab（audioBytes）起播瞬间计到 ~6000 就永久冻结，但 PCM 是
     * RUNNING、hw_ptr 稳定前进 —— 音频其实一直在播，probe 却看不到。
     * 反过来还有更糟的一版：用户听到「开头 10 秒没声音」。
     *
     * 实测排除过程（真机日志）：
     *   1) probe 挂分支 bin 的 ghost sink pad：BUFFER 只收到 1 个
     *      （size=166）就再无声息，LIST 一次都没有 —— 而数据确实到了
     *      alsasink。说明 ghost pad 上的 probe 看不到后续数据流。
     *   2) 改挂 demuxer 的 src pad：BUFFER 疯狂触发（实测 #26113），
     *      证明数据在这里能全部看到。
     *   3) 但同一版里 `frames==0 → DROP` 的门一直关着，DROP 掉几千个
     *      音频 buffer 之后音频分支再也 preroll 不起来，PCM 变 closed，
     *      音频彻底没声 —— **「开头无声」就是这个 DROP 门造成的**，
     *      不是 alsasink 慢，也不是队列堵塞。
     *
     * 结论：**音频 probe 只做统计，绝不 DROP**。
     * A/V 同步由 `-copyts`（保留原始 PTS，见 src_http.c）保证，那才是
     * 唯一必要的机制；在 probe 层丢包会把音频分支搞死，副作用远大于收益。
     */
    buf = gst_pad_probe_info_get_buffer(info);
    if (!buf) return GST_PAD_PROBE_OK;
    sz = gst_buffer_get_size(buf);
    atomic_fetch_add_explicit(&g_w.probe.abytes, (unsigned long long)sz, memory_order_relaxed);
    /*
     * 【A/V 同步判据（2026-11-01）】记首个音频包到达时刻。
     *
     * 单 FIFO 单 demux 下两路是同一个 pipe 的 interleave，到达时刻本来就
     * 接近，所以这个差值只能证明"两路都真的在流动"，**不能**直接量化
     * 音画偏差。量化靠两个量的交叉验证：
     *   - pos（demuxer 时间轴，来自 query_position）
     *   - frames / fps（真正送显的内容时长）
     * 同步时 pos ≈ frames*1000/fps + baseMs。
     */
    if (!g_w.first_audio_logged) {
        g_w.first_audio_logged = 1;
        /*
         * 【硬判据：音频首包距 seek/open 下发多长】
         *
         * 单 demux（本方案）：demuxer 只有一个 segment，segment.start = K
         * （两路首帧 PTS 的最小值，即视频关键帧）。音频内容 T 的
         * running_time = T - K，alsasink 必须等时钟走到 T-K 才消费，
         * 于是 demuxer 的音频队列会先积压 -> 首包自然被推迟到 ~(T-K)。
         * 实测 T-K 约 700~1300ms。
         *
         * 旧双 demux：音频 demuxer 的 segment.start = T，首包 running_time
         * 从 0 起，几乎立刻被消费 -> 首包在 seek 后几十毫秒就到。
         *
         * 所以这个值落在 500ms 以上，就是「音频在等关键帧对齐」的物理证据，
         * 不依赖 fps 精度、不依赖 avSkewMs 的推算。
         */
        {
            /*
             * 基准点必须用「本次 seek/open 的下发时刻」。
             * seek 走的是重建管线，frame_epoch_ms 在 do_seek 里已被重置为本轮
             * seek 的时刻；起播（do_open）同理。用 g_w.t_start 是错的 —— 那是
             * 整个会话的启动时刻，seek 到第 3 次时差值会到 18000ms 以上，完全
             * 失去意义（实测曾打出 +18640ms）。
             */
            long long ep = g_w.audio_epoch_ms > 0 ? g_w.audio_epoch_ms : g_w.t_start;
            pb2_logf("av: first audio packet +%lldms after src start (size=%llu, "
                     "expect>=500ms if waiting for keyframe sync)",
                     pb2_now_ms() - ep, (unsigned long long)sz);
        }
    }
    return GST_PAD_PROBE_OK;
}

/*
 * 【音量统计冻结修复】BUFFER_LIST 入口：语义与 on_audio_buffer 相同，只是
 * 数据源是 GstBufferList（逐个累加 size；gate 关闭时整段 DROP）。
 * 注册点：on_pad_added / attach_probes，flag = GST_PAD_PROBE_TYPE_BUFFER_LIST。
 */
static int on_audio_list(GstPad *pad, GstPadProbeInfo *info, void *ud) {
    void *gl;
    unsigned long long total = 0;
    unsigned int n, i;
    (void)pad;
    (void)ud;
    /* 同 on_audio_buffer：只统计，绝不 DROP（DROP 会把音频分支搞死）。 */
    gl = gst_pad_probe_info_get_buffer_list(info);
    if (!gl) return GST_PAD_PROBE_OK;
    n = gst_buffer_list_length(gl);
    for (i = 0; i < n; i++) {
        void *b = gst_buffer_list_get(gl, i);
        if (b) total += (unsigned long long)gst_buffer_get_size(b);
    }
    atomic_fetch_add_explicit(&g_w.probe.abytes, total, memory_order_relaxed);
    return GST_PAD_PROBE_OK;
}

/*
 * 给 vsink/asink 挂 probe。
 *
 * 注意：正常路径下 probe 是在 on_pad_added 里挂的（见那里的注释），
 * 这里只在「分支已经建好但 probe 因故没挂上」时兜底。
 * 为避免重复计数，只在计数器从未动过时调用（do_open 里判断）。
 */
static void attach_probes(void) {
    if (g_w.vsink) {
        GstPad *p = gst_element_get_static_pad(g_w.vsink, "sink");
        if (p) {
            gst_pad_add_probe(p, GST_PAD_PROBE_TYPE_BUFFER, on_video_buffer, NULL, NULL);
            gst_object_unref(p);
            pb2_logf("video probe attached");
        }
    }
    if (g_w.asink) {
        GstPad *p = gst_element_get_static_pad(g_w.asink, "sink");
        if (p) {
            /* 双 probe：BUFFER + BUFFER_LIST（AAC 走 LIST，见 on_audio_buffer 注释） */
            gst_pad_add_probe(p, GST_PAD_PROBE_TYPE_BUFFER, on_audio_buffer, NULL, NULL);
            gst_pad_add_probe(p, GST_PAD_PROBE_TYPE_BUFFER_LIST, on_audio_list, NULL, NULL);
            gst_object_unref(p);
            pb2_logf("audio probe attached");
        }
    }
}

/* ============================ 管线生命周期 ============================ */

static void teardown_pipeline_ex(int close_src) {
    /*
     * 先拆管线再关媒体源。顺序很重要：
     * FIFO 的读端（filesrc）在 set_state(NULL) 时关闭，如果这时 ffmpeg 还活着，
     * 它的 write 会拿到 EPIPE/SIGPIPE；反过来先杀 ffmpeg 则 filesrc 读到 EOF。
     * 两种都能收场，但先拆管线更干净（没有半截数据进 demuxer）。
     *
     * close_src=0：【P1-3】seek 时新 ffmpeg 已经先起来了（下载与新管线并行），
     * 这里只拆旧管线，绝不能把新源一起关掉。
     */
    if (g_w.pipe) {
        pb2_logf("teardown: set_state(NULL)");
        gst_element_set_state(g_w.pipe, GST_STATE_NULL);
        gst_object_unref(g_w.pipe);
        g_w.pipe = NULL;
    }
    if (close_src && g_w.src_ready) {
        pb2_src_close(&g_w.src);
        g_w.src_ready = 0;
    }
    g_w.base_ms = 0;
    if (g_w.bus) {
        gst_object_unref(g_w.bus);
        g_w.bus = NULL;
    }
    g_w.vsink = NULL;
    g_w.vsink_el = NULL;
    g_w.rect_hidden = 0;
    g_w.asink = NULL;
    g_w.vvol = NULL;
    g_w.vol_applied = -1;
    g_w.vbranch = 0;
    g_w.abranch = 0;
    g_w.has_audio = 0;
    g_w.pipeline_alive = 0;
    g_w.eos = 0;
    /* 【P1-1】管线已拆，源死亡标记一并清掉，免得下一次会话开局就误升级 alive=0 */
    g_w.src_died = 0;
    g_w.src_died_at_ms = 0;
    g_w.last_src_check2_ms = 0;
    g_w.paused = 0;
    g_w.seek_pending = 0;
    g_w.seek_target = 0;
    pb2_probe_reset(&g_w.probe);
    g_w.seek_frames_base = 0;
    g_w.avskew_last_log_ms = 0;
    pb2_stats_patch_alive(&g_w.stats, 0);
    pb2_stats_patch_position(&g_w.stats, 0, 0);
    pb2_stats_patch_seek(&g_w.stats, 0, 0);
    pb2_stats_patch_audio(&g_w.stats, 0, 0);
    pb2_mp3_marker_clear();
}

/* 常规拆除：连带关闭媒体源 */
static void teardown_pipeline(void) { teardown_pipeline_ex(1); }

/*
 * 【P1-3】回收上一次 seek 留下的旧源（两个 ffmpeg + 两条 FIFO）。
 *
 * seek 时旧 ffmpeg 搬进 g_w.dying_src 后立刻回去建新管线，
 * 回收放在 worker 空闲路径（这里），reap 的 60ms 完全不进关键路径。
 * 只能在 worker 线程调用（会 kill/waitpid/unlink）。
 */
static void reap_dying(void) {
    if (!g_w.dying_pending) return;
    {
        long long t0 = pb2_now_ms();
        pb2_src_close(&g_w.dying_src);
        pb2_logf("seek: old src reaped in %lldms (off critical path)", pb2_now_ms() - t0);
    }
    memset(&g_w.dying_src, 0, sizeof(g_w.dying_src));
    g_w.dying_pending = 0;
}

/*
 * 构建管线。
 *
 * 关键设计：desc 里**只放 demuxer，不放任何 sink**，音视频分支全部由
 * pad-added 动态构建。原因（真机实测）：
 *   - 视频文件（无音频轨）+ 写死 audio 分支 => 管线里有一个永远没数据的 sink
 *     分支，preroll 永远完不成，get_state 卡满超时，position 恒 0，零换帧。
 *   - 写死单源 A/V 也处理不了「第二路输入」和 h265。
 *   - 完全动态后：只有视频 / 只有音频 / 音视频 / 双源 四种情况共用一套代码，
 *     而且 set_state(PLAYING) 立即返回 SUCCESS（因为当时管线里还没有 sink）。
 */
/*
 * 按路径猜容器类型，返回该用的 demuxer 元素名。
 *   返回 NULL 表示这是裸流（不需要 demuxer，直接进 parser）。
 *
 * 为什么不用 decodebin：decodebin 出的是解码后的 raw caps，会把我们
 * 按 caps 建 h264parse ! mppvideodec 的逻辑打乱，而且它挑的解码器不可控。
 * 这里只处理我们自己会遇到的三种形态：
 *   - HTTP FIFO       -> Matroska（ffmpeg -f matroska 的产物，唯一网络形态）
 *   - 本地 .mkv/.webm -> matroskademux
 *   - 本地 .h264/.264/.h265/.hevc -> 裸流，无 demuxer
 *   - 其余（.mp4/.m4s/.mov）-> qtdemux
 */
static const char *pick_demuxer(const char *path, int *raw_stream) {
    size_t n;
    const char *dot;
    *raw_stream = 0;
    if (!path) return "qtdemux";
    n = strlen(path);
    dot = strrchr(path, '.');
    if (dot && dot[1]) {
        if (strcasecmp(dot, ".mkv") == 0 || strcasecmp(dot, ".webm") == 0 ||
            strcasecmp(dot, ".mka") == 0) {
            return "matroskademux";
        }
        /*
         * .fifo 是我们的 ffmpeg 管道（source/src_http.c 建的命名管道），
         * 内容永远是 Matroska（-f matroska 的产物）。
         * 双 FIFO 模式下 location2 也是 .fifo，必须同样识别，
         * 否则会给音频路挂 qtdemux 直接解析失败。
         */
        if (strcasecmp(dot, ".fifo") == 0) {
            return "matroskademux";
        }
        if (strcasecmp(dot, ".h264") == 0 || strcasecmp(dot, ".264") == 0 ||
            strcasecmp(dot, ".h265") == 0 || strcasecmp(dot, ".hevc") == 0 ||
            strcasecmp(dot, ".265") == 0) {
            *raw_stream = 1;
            return NULL;
        }
    }
    (void)n;
    return "qtdemux";
}

static int build_pipeline(pb2_open_req_t *req, int src_already_open) {
    char desc[4096];
    char loc1[1200];
    char loc2[1200];
    GError *err = NULL;
    GstElement *vd;
    int n;
    size_t off;
    const char *dmx;
    int raw1 = 0;
    int raw_fixed = 0;

    /*
     * 媒体源：网络走 ffmpeg -> FIFO（Matroska），本地直读。
     * 见 source/src_http.h 顶部：GStreamer 直连 HTTP 无法 seek，
     * 必须由 ffmpeg 负责定位。
     *
     * 位置语义：网络源 base_ms 由 ffmpeg -ss 实现，query_position() 从 0 开始，
     * 所以对外位置 = base_ms + query_position()。本地文件 base_ms 恒 0。
     *
     * src_already_open = 1：调用者已经先建好 g_w.src（seek 时为并行下载用），
     * 这里只建 GStreamer 管线，不再 spawn ffmpeg。
     */
    if (!src_already_open) {
        if (pb2_src_open(&g_w.src, req->input, req->input2, req->userAgent, req->referer,
                         req->startMs, req->durationMs) != 0) {
            pb2_stats_patch_error(&g_w.stats, PB2_E_OPEN_FAILED, "media source open failed");
            return -1;
        }
        g_w.src_ready = 1;
    }
    g_w.base_ms = g_w.src.is_http ? g_w.src.baseMs : 0;

    desc_escape(g_w.src.location, loc1, sizeof(loc1));
    desc_escape(g_w.src.location2, loc2, sizeof(loc2));

    /*
     * desc 里**只放 demuxer，不放任何 sink**，音视频分支全部由 pad-added 动态构建。
     * 原因（真机实测）：
     *   - 视频文件（无音频轨）+ 写死 audio 分支 => 管线里有一个永远没数据的 sink
     *     分支，preroll 永远完不成，get_state 卡满超时，position 恒 0，零换帧。
     *   - 写死单源 A/V 也处理不了「第二路输入」和 h265。
     *   - 完全动态后：只有视频 / 只有音频 / 音视频 / 双源 四种情况共用一套代码，
     *     而且 set_state(PLAYING) 立即返回 SUCCESS（因为当时管线里还没有 sink）。
     *
     * blocksize=65536：FIFO 上必须给，否则 filesrc 短读会破坏流（实测）。
     */
    if (g_w.src.is_http) {
        n = snprintf(desc, sizeof(desc),
                     "filesrc location=\"%s\" blocksize=65536 ! matroskademux name=vd", loc1);
    } else {
        dmx = pick_demuxer(g_w.src.location, &raw1);
        if (raw1) {
            /* 裸流：没有 demuxer，parser 直接接在 filesrc 后面。
             * 这种情况分支是确定的（只有视频），不需要 pad-added。 */
            const char *parser = (strstr(g_w.src.location, "265") ||
                                  strstr(g_w.src.location, "hevc"))
                                     ? "h265parse"
                                     : "h264parse";
            n = snprintf(desc, sizeof(desc),
                         "filesrc location=\"%s\" ! %s ! mppvideodec rotation=%d ! "
                         "videorate ! video/x-raw,framerate=%s ! "
                         "kmssink name=vsink plane-id=%d driver-name=rockchip sync=true "
                         "max-lateness=%d qos=true force-aspect-ratio=false "
                         "skip-vsync=true render-rectangle=%s",
                         loc1, parser, g_w.rotate, PB2_PANEL_RATE, PB2_KMS_PLANE_ID,
                         PB2_MAX_LATENESS, g_w.rect);
            g_w.vbranch = 1; /* 固定分支，不走 pad-added */
            raw_fixed = 1;
            goto parse_it;
        }
        n = snprintf(desc, sizeof(desc), "filesrc location=\"%s\" ! %s name=vd", loc1, dmx);
    }
    off = (size_t)(n > 0 ? n : 0);

    if (loc2[0] && off + 200 < sizeof(desc)) {
        int raw2 = 0;
        const char *dmx2 = pick_demuxer(g_w.src.location2, &raw2);
        if (!raw2) {
            n = snprintf(desc + off, sizeof(desc) - off,
                         " filesrc location=\"%s\" ! %s name=ad", loc2, dmx2);
            if (n > 0) off += (size_t)n;
        }
    }

parse_it:
    pb2_logf("build desc: %s", desc);

    g_w.pipe = gst_parse_launch(desc, &err);
    if (!g_w.pipe) {
        char msg[PB2_ERRMSG_CAP];
        snprintf(msg, sizeof(msg), "gst_parse_launch: %s", err ? err->message : "?");
        pb2_logf("%s", msg);
        pb2_stats_patch_error(&g_w.stats, PB2_E_PIPELINE, msg);
        if (err) g_error_free(err);
        return -1;
    }

    /*
     * 裸流（.h264/.h265）：desc 里是固定分支，没有 demuxer、也不会触发 pad-added。
     * 这里直接拿 vsink 并挂 probe（顺序同 on_pad_added：先 probe 再放数据，
     * 但此时还没 set_state，所以来得及）。
     */
    if (raw_fixed) {
        GstElement *vs = gst_bin_get_by_name((GstBin *)g_w.pipe, "vsink");
        if (vs) {
            GstPad *vp = gst_element_get_static_pad(vs, "sink");
            if (vp) {
                gst_pad_add_probe(vp, GST_PAD_PROBE_TYPE_BUFFER, on_video_buffer, NULL, NULL);
                gst_object_unref(vp);
            }
            g_w.vsink = vs; /* 持引用，teardown 时随 pipe 一起释放 */
            g_w.vsink_el = vs; /* 裸流分支没有外层 bin，kmssink 本体就是它（SET_RECT 用） */
        }
        g_w.bus = gst_element_get_bus(g_w.pipe);
        if (!g_w.bus) {
            pb2_stats_patch_error(&g_w.stats, PB2_E_PIPELINE, "no bus");
            teardown_pipeline();
            return -1;
        }
        g_w.pipeline_alive = 1;
        pb2_stats_patch_alive(&g_w.stats, 1);
        return 0;
    }

    /* 连 pad-added（必须在 set_state 之前） */
    vd = gst_bin_get_by_name((GstBin *)g_w.pipe, "vd");
    if (!vd) {
        pb2_stats_patch_error(&g_w.stats, PB2_E_PIPELINE, "no demuxer 'vd'");
        teardown_pipeline();
        return -1;
    }
    g_signal_connect_data(vd, "pad-added", (void *)on_pad_added, NULL, NULL, 0);
    gst_object_unref(vd);

    if (loc2[0]) {
        GstElement *ad = gst_bin_get_by_name((GstBin *)g_w.pipe, "ad");
        if (ad) {
            g_signal_connect_data(ad, "pad-added", (void *)on_pad_added, NULL, NULL, 0);
            gst_object_unref(ad);
        }
    }

    g_w.bus = gst_element_get_bus(g_w.pipe);
    if (!g_w.bus) {
        pb2_stats_patch_error(&g_w.stats, PB2_E_PIPELINE, "no bus");
        teardown_pipeline();
        return -1;
    }

    g_w.pipeline_alive = 1;
    pb2_stats_patch_alive(&g_w.stats, 1);
    return 0;
}

/* ============================ open ============================ */

static void do_open(pb2_open_req_t *req) {
    long long t0 = pb2_now_ms();
    GstState cur = 0;
    GstState pend = 0;
    GstStateChangeReturn sr;

    pb2_logf("OPEN input=%s input2=%s start=%lld dur=%lld audio=%d trans=%d rect=%d,%d,%d,%d",
             req->input ? req->input : "(null)", req->input2 ? req->input2 : "",
             req->startMs, req->durationMs, req->audio, req->transpose, req->x, req->y,
             req->w, req->h);

    /* 上一次会话的残留先清掉 */
    teardown_pipeline();
    /*
     * 【P1-3】上一次 seek 留下的旧源还没收的话，这里一并收掉。
     * OPEN 本来就是冷启动路径（~50ms 里有时间预算），但它必须在
     * pb2_src_open 分配新 FIFO 之前跑，避免 pid/fifo 名字撞车。
     */
    reap_dying();
    if (g_w.req) {
        pb2_open_req_free(g_w.req);
        g_w.req = NULL;
    }

    /* 参数落库 */
    g_w.req = req;
    g_w.rotate = rotation_for_transpose(req->transpose);
    snprintf(g_w.rect, sizeof(g_w.rect), "<%d,%d,%d,%d>",
             PB2_PANEL_X_OFFSET + req->x, req->y, req->w, req->h);
    g_w.seek_target = req->startMs;
    g_w.seek_pending = 0;
    g_w.eos = 0;
    g_w.paused = 0;
    g_w.t_start = t0;
    g_w.last_pos_ms = req->startMs;
    g_w.last_plane_ms = t0;
    g_w.last_marker_ms = 0;
    g_w.last_fps_ms = t0;
    g_w.fps_frames_base = 0;
    g_w.fps_calc = 0;
    g_w.plane_lost_streak = 0;
    /* 【P0-2】起播宽限：OPEN 后 2s 内平面没上来是正常的，不算异常也不打日志 */
    g_w.plane_grace_until_ms = t0 + PB2_PLANE_GRACE_MS;
    g_w.seek_frame_reported = 1; /* OPEN 不是 seek，不报首帧耗时 */
    /*
     * 【P0-1】从这一刻开始等首帧。
     * frame_epoch_ms 同时是「无帧时 videoStallMs 的起点」（见
     * pb2_session_stall_ms），也是 check_first_frame() 的 15s 计时基准。
     */
    g_w.frame_epoch_ms = t0;
    g_w.audio_epoch_ms = t0;
    /* 【P1-1】新会话：清掉上一次的源死亡标记与独立节流器 */
    g_w.src_died = 0;
    g_w.src_died_at_ms = 0;
    g_w.last_src_check2_ms = 0;
    g_w.last_src_check_ms = 0;
    g_w.first_frame_logged = 0;
    g_w.first_audio_logged = 0;
    g_w.first_frame_retries = 0; /* 每个新会话重新给满自愈配额 */
    /* 【P1-18】帧率上限：0 = 不限制，>0 时在 mppvideodec 之后插 videorate max-rate */
    g_w.fps_cap = req->fpsCap;

    /*
     * 音频路由 + 音量：**必须在建管线之前定好**，因为 device= 是拼进
     * desc 里的（alsasink 的 device 在 PLAYING 下改了不会重开 PCM）。
     *
     * 蓝牙探测要起 2~3 个子进程（bluetoothctl devices + 每台 info），
     * 耗时 60~200ms，所以放在 OPEN 的关键路径上是有代价的。但只有
     * 「真的连了蓝牙」时才走 BlueZ 分支；没连时 bluetoothctl devices
     * 通常直接返回（实测 40ms 左右）。可接受。
     */
    if (pb2_bt_pcm_detect(g_w.adev, (int)sizeof(g_w.adev)) == 0) {
        const char *mac;
        g_w.bt_present = 1;
        g_w.bt_vol_applied = -1;
        /*
         * 从 "bluealsa:DEV=B0:A3:F2:AD:7E:FA,PROFILE=a2dp" 里抠出 MAC，
         * 再查别名拼 AVRCP 控件名（见 apply_bt_avrcp_volume 的注释）。
         * 查不到就留空 —— 那只是不同步耳机硬件音量，不影响出声。
         */
        g_w.bt_ctl_name[0] = '\0';
        mac = strstr(g_w.adev, "DEV=");
        if (mac) {
            char macbuf[24];
            const char *end = mac + 4;
            size_t n = 0;
            while (*end && *end != ',' && n + 1 < sizeof(macbuf)) {
                macbuf[n++] = *end++;
            }
            macbuf[n] = '\0';
            if (n == 17) {
                char name[48];
                if (pb2_bt_name_detect(macbuf, name, (int)sizeof(name)) == 0) {
                    snprintf(g_w.bt_ctl_name, sizeof(g_w.bt_ctl_name), "%s - A2DP",
                             name);
                }
            }
        }
        pb2_logf("audio: open with BT sink %s (mixer ctl='%s')", g_w.adev,
                 g_w.bt_ctl_name[0] ? g_w.bt_ctl_name : "?");
    } else {
        g_w.bt_present = 0;
        g_w.adev[0] = '\0';
        g_w.bt_ctl_name[0] = '\0';
        g_w.bt_vol_applied = -1;
        pb2_logf("audio: open with default pcm (speaker)");
    }
    /*
     * 音量来源：**永远读 spk_volume**。
     *
     * 实测（2026-11-01，蓝牙已连）：`hal-audio -s 40` 只写 spk_volume，
     * 既不改 bt_volume 也不改 bluealsa 的 AVRCP 音量 —— 而 hal-audio 正是
     * 下拉控制中心和设置页背后的那个 HAL 二进制。所以「用户调音量」这件事
     * 的唯一权威值就是 spk_volume，跟当前走喇叭还是耳机无关。
     * （bt_volume 是给蓝牙设置页用的独立档位，控制中心不碰它。）
     */
    g_w.vol_pct = read_sys_config_int("spk_volume",
                                      read_sys_config_int("bt_volume", 100));
    g_w.vol_applied = -1;
    g_w.vol_check_ms = pb2_now_ms();
    g_w.bt_check_ms = pb2_now_ms();

    pb2_stats_patch_resync(&g_w.stats, 0, 0);

    pb2_probe_reset(&g_w.probe);
    g_w.seek_frames_base = 0;
    g_w.avskew_last_log_ms = 0;
    pb2_stats_patch_state(&g_w.stats, PB2_STATE_OPENING);
    pb2_stats_patch_error(&g_w.stats, PB2_E_NONE, "");
    pb2_stats_patch_position(&g_w.stats, req->startMs, req->durationMs);
    pb2_stats_patch_seek(&g_w.stats, 0, req->startMs);
    pb2_stats_patch_audio(&g_w.stats, 0, 0);

    /*
     * 【同步兜底静音】必须在 build_pipeline 之前跑。
     *
     * 完整版 mixer（带回读校验，4 次 amixer ≈150ms）在 worker 空闲 tick 里，
     * 起播那一刻存在几十到几百毫秒的空窗；如果设备当时不是静音态（用户手工
     * 调过 / 上个 app 留下 Master=100%），这段空窗就会漏声。
     * 这里只花 ~17ms 把 card1 Master 压到 0，保证第一帧起就是静音。
     */
    if (pb2_mute_enabled()) pb2_mixer_pre_mute();

    if (build_pipeline(req, 0) != 0) {
        /*
         * 【P0-1】建管线失败也要走 teardown：build_pipeline 内部可能已经
         * 打开了 src（pb2_src_open 成功但 gst_parse_launch 失败），
         * 不拆会留下没人回收的 ffmpeg 进程和 FIFO。
         */
        pb2_logf("OPEN failed: build_pipeline -> error");
        pb2_stats_patch_error(&g_w.stats, PB2_E_OPEN_FAILED, "open: build pipeline failed");
        pb2_stats_patch_state(&g_w.stats, PB2_STATE_ERROR);
        teardown_pipeline();
        g_w.frame_epoch_ms = 0;
        return;
    }

    /*
     * 混音器配置延后到 worker 空闲路径执行（见 worker_main 里的 mixer_tick）。
     *
     * 为什么不在 OPEN 里同步做：一次 amixer sset + 一次 sget 回读实测各 ~37ms，
     * card0/card1 合计 4 次 => 约 150ms，直接把 `OPEN done in 53ms` 拖到 186ms。
     * 混音器不影响首帧上屏，延后做同样能满足「播放中 Master 必须是 100%」。
     *
     * 【全局静音】这里只记「这个视频有音轨/没音轨」，最终要不要出声由
     * pb2_audio_prepare() 结合静音开关决定。默认静音时是 mute，不是 setup。
     */
    g_w.mixer_pending = req->audio ? 1 : 2;
    if (pb2_mute_enabled()) {
        pb2_logf("mixer: muted=1 (source=%s) -> skipped (will force-mute, no amixer SPK)",
                 pb2_mute_reason());
    } else {
        pb2_logf("mixer: muted=0 (source=%s) -> will open speaker", pb2_mute_reason());
    }

    /* 起播 */
    sr = gst_element_set_state(g_w.pipe, GST_STATE_PLAYING);
    pb2_logf("set_state(PLAYING) -> %d (+%lldms)", sr, pb2_now_ms() - t0);

    if (sr == GST_STATE_CHANGE_FAILURE) {
        pb2_stats_patch_error(&g_w.stats, PB2_E_OPEN_FAILED, "set_state(PLAYING) failed");
        pb2_stats_patch_state(&g_w.stats, PB2_STATE_ERROR);
        teardown_pipeline();
        return;
    }

    /*
     * 不完全动态的管线在 set_state 时没有 sink，返回 SUCCESS；
     * 但如果 desc 恰好解析出了 sink（理论上不会），会返回 ASYNC，
     * 这里统一等一会儿让首个分支建起来。
     *
     * 【P0-1】**返回值必须判错**。以前 get_state 超时（12s！）只打一行日志，
     * 紧接着照样 patch(PLAYING)，于是「管线根本没 preroll 成功」被报成正在
     * 播放：JS 看到 state=playing 就退出起播门，画面却永远不来，lastError
     * 仍是 0，只能靠用户手动返回。现在超时/失败一律判 OPEN 失败，
     * 保证状态机能从 OPENING 走到 ERROR。
     */
    if (sr == GST_STATE_CHANGE_ASYNC) {
        sr = gst_element_get_state(g_w.pipe, &cur, &pend,
                                   (GstClockTime)PB2_PREROLL_TIMEOUT_MS * 1000000ULL);
        pb2_logf("get_state -> %d cur=%d pend=%d (+%lldms)", sr, cur, pend,
                 pb2_now_ms() - t0);
        if (sr != GST_STATE_CHANGE_SUCCESS) {
            pb2_logf("OPEN failed: preroll %s (sr=%d) -> error",
                     sr == GST_STATE_CHANGE_ASYNC ? "timeout" : "failure", sr);
            pb2_stats_patch_error(&g_w.stats, PB2_E_TIMEOUT, "open: preroll timeout");
            pb2_stats_patch_state(&g_w.stats, PB2_STATE_ERROR);
            teardown_pipeline();
            return;
        }
    }

    /* probe 由 on_pad_added 在分支建起来时挂（那时才能拿到 ghost sink pad）。
     * 这里不调用 attach_probes —— 此刻 vsink/asink 必然还是 NULL。 */

    /*
     * 起点不是 0 时立刻 seek。
     * **只在本地文件路径做**：网络源的起点已经由 ffmpeg -ss 实现，
     * 对 FIFO 再 seek 一次会失败（push 模式无法 seek），反而可能破坏流。
     */
    if (req->startMs > 0 && !g_w.src.is_http) {
        gst_element_seek_simple(g_w.pipe, GST_FORMAT_TIME,
                                GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                                (long long)req->startMs * 1000000LL);
        pb2_logf("initial seek to %lldms (+%lldms)", req->startMs, pb2_now_ms() - t0);
    }

    pb2_stats_patch_state(&g_w.stats, PB2_STATE_PLAYING);
    pb2_mp3_marker_set();
    g_w.last_marker_ms = pb2_now_ms();
    pb2_logf("OPEN done in %lldms", pb2_now_ms() - t0);
}

/* ============================ seek ============================ */

/* 前向声明：do_seek / take_error 都要判别 bus 消息类型，实现在 bus 段 */
static const char *msg_name(GstMessage *msg);
static int take_error(GstMessage *msg, const char *where);

/*
 * 真正执行 seek。
 *
 * 两条路径，取决于媒体源：
 *
 * A) 本地文件（is_http=0）：直接 gst_element_seek_simple，不重建管线。
 *    这是硬规则 3。实测阻塞 0.1~25ms、seek→首帧 0.1~13.3ms。
 *
 * B) 网络源（is_http=1）：**绝不 seek 本地/管道**，改为重启 ffmpeg 到新位置。
 *    原因：GStreamer 直连 HTTP 的 seek 被 qtdemux 在 push 模式下丢弃
 *    （`qtdemux: ignoring seek in push mode`），永远不生效；
 *    而 FIFO 里的数据是一次性的，也没法往回 seek。
 *    做法（【P1-3】重排后的顺序，**旧进程回收不在关键路径上**）：
 *      1. pb2_src_open(新 baseMs) 起新 ffmpeg + 新 FIFO（下载立刻开始）
 *      2. 把旧 pb2_src_t 挪进 g_w.dying_src，稍后 reap_dying() 回收
 *      3. teardown_pipeline_ex(0) 拆旧管线（不碰刚建的新源）
 *      4. build_pipeline(req, 1) 用新源重建（FIFO 需要新 fd，不能复用）
 *    旧写法是「先 close 旧源、串行 kill+waitpid 两条 ffmpeg、再起新源」，
 *    光回收就吃掉约 500ms 关键路径（实测 pid=1597/1598 各占 296/502ms）。
 *    现在实测 seek(restart) 60~170ms、seek→首帧 490~650ms（预算 800ms）。
 *
 * 三个必须处理的边界（ADR §6.3）：
 *   1) 越界：目标超过 duration 会导致读到 EOF，pos 归 0、画面冻结 => clamp
 *   2) 背靠背：由调用方 debounce（PB2_SEEK_DEBOUNCE_MS）
 *   3) 首帧延迟：seekPending 必须清掉，否则 JS 层会一直以为在 seek 中
 */
static void do_seek(long long target_ms) {
    long long dur = 0;
    long long clamped;
    long long t0;
    long long t0_pre;
    GstMessage *msg;
    int got_async = 0;

    t0_pre = pb2_now_ms();

    /*
     * 【P0-5】入口守卫不能只是「假装受理然后放弃」。
     *
     * 旧写法：管线不在就直接 patch_seek(0, target) 然后 return —— 用户拖动
     * 进度条毫无反应，日志里连一行都没有，而且**之后每一次** seek 都在这里
     * 被挡掉，永久静默失效。触发场景恰恰是最需要 seek 的场景：弱网 + 长时间
     * 播放导致 URL 过期、上一次 seek 重建失败。
     *
     * 新语义：只要还有 req（说明会话没结束），就先尝试自愈重建一次；
     * 重建成功就继续往下走正常 seek 流程，失败才真的报错返回。
     */
    if (!g_w.pipe || !g_w.pipeline_alive) {
        if (!g_w.req) {
            pb2_logf("SEEK ignored: no session (target=%lld)", target_ms);
            pb2_stats_patch_seek(&g_w.stats, 0, target_ms);
            return;
        }
        pb2_logf("SEEK self-heal: pipeline dead, trying rebuild before seek (target=%lld)",
                 target_ms);
        /* 恢复源：没有可用源就先开一个（上次失败可能连 src 都关了） */
        if (!g_w.src_ready) {
            if (pb2_src_open(&g_w.src, g_w.req->input, g_w.req->input2, g_w.req->userAgent,
                             g_w.req->referer, target_ms, g_w.req->durationMs) != 0) {
                pb2_logf("SEEK self-heal: src open failed -> error");
                pb2_stats_patch_error(&g_w.stats, PB2_E_SEEK, "seek: self-heal src open failed");
                pb2_stats_patch_state(&g_w.stats, PB2_STATE_ERROR);
                pb2_stats_patch_seek(&g_w.stats, 0, target_ms);
                return;
            }
            g_w.src_ready = 1;
        }
        g_w.req->startMs = (int)target_ms;
        if (build_pipeline(g_w.req, 1) != 0) {
            pb2_logf("SEEK self-heal: rebuild failed -> error");
            teardown_pipeline();
            pb2_stats_patch_error(&g_w.stats, PB2_E_SEEK, "seek: self-heal rebuild failed");
            pb2_stats_patch_state(&g_w.stats, PB2_STATE_ERROR);
            pb2_stats_patch_seek(&g_w.stats, 0, target_ms);
            return;
        }
        {
            GstState cur = 0, pend = 0;
            gst_element_set_state(g_w.pipe, g_w.paused ? GST_STATE_PAUSED : GST_STATE_PLAYING);
            gst_element_get_state(g_w.pipe, &cur, &pend,
                                  (GstClockTime)PB2_PREROLL_TIMEOUT_MS * 1000000ULL);
        }
        pb2_stats_patch_state(&g_w.stats,
                              g_w.paused ? PB2_STATE_PAUSED : PB2_STATE_PLAYING);
        pb2_stats_patch_error(&g_w.stats, PB2_E_NONE, "");
        pb2_logf("SEEK self-heal: rebuild ok, continue seek (+%lldms)", pb2_now_ms() - t0_pre);
        g_w.frame_epoch_ms = pb2_now_ms();
        g_w.audio_epoch_ms = g_w.frame_epoch_ms;
        g_w.first_frame_logged = 0;
        g_w.first_audio_logged = 0;
        g_w.last_src_check_ms = 0;
        g_w.last_src_check2_ms = 0; /* 【P1-1】重新探测新源 */
        g_w.src_died = 0;
        g_w.src_died_at_ms = 0;
    }

    if (gst_element_query_duration(g_w.pipe, GST_FORMAT_TIME, &dur) && dur > 0) {
        dur = dur / 1000000LL + g_w.base_ms;
        if (g_w.req && g_w.req->durationMs > 0 && dur > g_w.req->durationMs) {
            dur = g_w.req->durationMs;
        }
    } else {
        dur = g_w.req ? g_w.req->durationMs : 0;
    }

    clamped = target_ms;
    if (clamped < 0) clamped = 0;
    if (dur > 1000 && clamped > dur - 1000) clamped = dur - 1000;

    t0 = pb2_now_ms();
    g_w.seek_issue_ms = t0;
    pb2_logf("SEEK target=%lld clamped=%lld dur=%lld http=%d coalesced=%d", target_ms, clamped,
             dur, g_w.src.is_http, g_w.seek_coalesced);
    g_w.seek_coalesced = 0;

    if (g_w.src.is_http) {
        /*
         * ---------- 网络源：先起新 ffmpeg，再拆旧管线 ----------
         *
         * 【P1-3 修复】旧顺序是 teardown_pipeline()（内部串行 reap 两个 ffmpeg，
         * 200ms + 200ms）之后才起新 ffmpeg，白把 ~500ms 挂在关键路径上，
         * 真机日志据此出现 `pid=1597 closed +296ms` / `pid2=1598 closed +502ms`。
         *
         * 新顺序：
         *   1) 旧 src 结构体整个搬走（旧 ffmpeg 继续跑、旧 FIFO 继续存在）
         *   2) 立刻起新 ffmpeg（新 FIFO）—— 下载与拆旧并行
         *   3) 只拆旧 GStreamer 管线（teardown_pipeline_ex(0)，不碰新源）
         *   4) 用已打开的源建新管线并 preroll
         * 旧的两个 ffmpeg 与 FIFO 交给 worker 空闲时的 reap_dying() 回收，
         * 完全不占 seek 关键路径。
         */
        int was_paused = g_w.paused;
        pb2_src_t old;
        int had_old;

        reap_dying(); /* 上一次 seek 的残留先收掉，避免 pid/fd 堆积 */

        old = g_w.src;
        had_old = g_w.src_ready;
        g_w.src_ready = 0;

        if (g_w.req) g_w.req->startMs = (int)clamped;
        {
            long long ts = pb2_now_ms();
            if (!g_w.req || pb2_src_open(&g_w.src, g_w.req->input, g_w.req->input2,
                                         g_w.req->userAgent, g_w.req->referer, clamped,
                                         g_w.req->durationMs) != 0) {
                pb2_logf("seek: new src open failed +%lldms", pb2_now_ms() - ts);
                if (had_old) pb2_src_close(&old);
                teardown_pipeline();
                pb2_stats_patch_error(&g_w.stats, PB2_E_SEEK, "seek: source open failed");
                pb2_stats_patch_seek(&g_w.stats, 0, clamped);
                return;
            }
            g_w.src_ready = 1;
            pb2_logf("seek: new src ready +%lldms (old ffmpeg still alive, reaped later)",
                     pb2_now_ms() - ts);
        }

        /* 旧源挂到待回收槽 */
        if (had_old) {
            reap_dying();
            g_w.dying_src = old;
            g_w.dying_pending = 1;
        }

        /* 只拆旧管线，保留刚建好的新源 */
        teardown_pipeline_ex(0);
        if (build_pipeline(g_w.req, 1) != 0) {
            /*
             * 【P0-5】重建失败必须拆干净并置错，否则老代码会把
             * pipeline_alive 留在「半拆」状态，下一次 seek 直接被入口守卫
             * `if (!g_w.pipe || !g_w.pipeline_alive) return;` 挡掉，
             * 之后所有 seek 静默失效。
             */
            pb2_logf("seek: rebuild pipeline failed -> teardown + error");
            teardown_pipeline();
            pb2_stats_patch_error(&g_w.stats, PB2_E_SEEK, "seek: rebuild pipeline failed");
            pb2_stats_patch_state(&g_w.stats, PB2_STATE_ERROR);
            pb2_stats_patch_seek(&g_w.stats, 0, clamped);
            return;
        }
        {
            GstState cur = 0, pend = 0;
            gst_element_set_state(g_w.pipe, was_paused ? GST_STATE_PAUSED : GST_STATE_PLAYING);
            gst_element_get_state(g_w.pipe, &cur, &pend,
                                  (GstClockTime)PB2_PREROLL_TIMEOUT_MS * 1000000ULL);
        }
        /* probe 已由 pad-added 挂上，这里不重复挂（会双倍计数） */
        g_w.paused = was_paused;
        pb2_stats_patch_state(&g_w.stats,
                              was_paused ? PB2_STATE_PAUSED : PB2_STATE_PLAYING);
        pb2_logf("seek(restart) done total=%lldms base=%lld", pb2_now_ms() - t0, g_w.base_ms);
        g_w.last_pos_ms = clamped;
        pb2_stats_patch_position(&g_w.stats, clamped, dur);
        pb2_stats_patch_seek(&g_w.stats, 0, clamped);
        pb2_probe_reset(&g_w.probe);
        g_w.seek_frames_base = 0;
        g_w.avskew_last_log_ms = 0;
        g_w.seek_frame_reported = 0; /* 首帧到达时由 on_video_buffer 报端到端耗时 */
        /* 【P0-1】seek 后重新等首帧（落点第一帧），超时/源先死同样判 error */
        g_w.frame_epoch_ms = pb2_now_ms();
        g_w.audio_epoch_ms = g_w.frame_epoch_ms;
        g_w.first_frame_logged = 0;
        g_w.first_audio_logged = 0;
        g_w.last_src_check_ms = 0;
        g_w.last_src_check2_ms = 0; /* 【P1-1】新源重新探测，清掉上一轮的死亡标记 */
        g_w.src_died = 0;
        g_w.src_died_at_ms = 0;
        pb2_stats_patch_resync(&g_w.stats, 1, g_w.frame_epoch_ms);
        /* 【P0-2】seek 重启也重新给 2s 平面宽限，避免重启瞬间误报 */
        g_w.plane_grace_until_ms = pb2_now_ms() + PB2_PLANE_GRACE_MS;
        g_w.plane_lost_streak = 0;
        g_w.fps_frames_base = 0;
        g_w.last_fps_ms = pb2_now_ms();
        g_w.fps_calc = 0;
        return;
    }

    /* ---------- 本地文件：seek_simple，不重建 ---------- */
    if (!gst_element_seek_simple(g_w.pipe, GST_FORMAT_TIME,
                                 GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                                 clamped * 1000000LL)) {
        pb2_stats_patch_error(&g_w.stats, PB2_E_SEEK, "seek_simple returned FALSE");
        pb2_stats_patch_seek(&g_w.stats, 0, clamped);
        return;
    }
    pb2_logf("seek_simple blocking=%lldms", pb2_now_ms() - t0);

    /*
     * 等 seek 真正生效。
     *
     * 【踩过的坑】最初这里是等 bus 的 ASYNC_DONE 消息，结果每次都等满 3s 超时
     * （`seek done async=0 total=3050ms`），position 也没跳。
     * 用 spike5 对照实验发现：flush seek 之后 `gst_element_get_state()` 立即返回
     * SUCCESS（管线根本没进入 ASYNC 态），因此 GStreamer **不会**发 ASYNC_DONE。
     * 也就是说「等 ASYNC_DONE」这个前提本身是错的。
     *
     * 正确的判据是**position 落到目标附近**（这才是 JS 关心的），
     * 同时顺带 poll bus 捕获 ERROR。实测落点 100~110ms。
     */
    {
        long long pos = 0;
        long long deadline = t0 + PB2_SEEK_ASYNC_TIMEOUT_MS;
        long long settle = -1;
        while (pb2_now_ms() < deadline) {
            if (pb2_cmdq_quit_flag(&g_w.q)) break;
            msg = gst_bus_timed_pop_filtered(g_w.bus, (GstClockTime)10000000ULL,
                                             GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
            if (msg) {
                if (take_error(msg, "seek")) {
                    gst_message_unref(msg);
                    break;
                }
                if (strcmp(msg_name(msg), "eos") == 0) {
                    pb2_logf("seek: got eos");
                    g_w.eos = 1;
                }
                gst_message_unref(msg);
            }
            if (gst_element_query_position(g_w.pipe, GST_FORMAT_TIME, &pos) && pos >= 0) {
                long long ms = pos / 1000000LL + g_w.base_ms;
                if (ms >= clamped - 1500 && ms <= clamped + 3000) {
                    got_async = 1;
                    settle = pb2_now_ms() - t0;
                    break;
                }
            }
        }
        pb2_logf("seek done landed=%d total=%lldms", got_async,
                 settle >= 0 ? settle : pb2_now_ms() - t0);
    }
    /*
     * 【P0-3】落点校验失败**不能**把位置改到目标值。
     *
     * 以前无论 got_async 是 0 还是 1，下面三行都无条件执行：
     *   last_pos_ms = clamped; patch_position(clamped); patch_seek(0, clamped);
     * 而进入这里的失败路径有三条（3s 超时、bus ERROR、quit），画面根本没动，
     * 于是 status().positionMs 被谎报成目标值、seekPending 又被清 0，
     * JS 侧（index.vue 的 seekTo 把 r.positionMs 当真值写回 UI）就显示
     * 「进度条跳到目标、画面停在原处」，而且上层不会再等。
     * 这是把失败谎报成成功，比 seek 慢严重得多。
     *
     * 正确做法：
     *   - 落点成立（got_async=1）→ 按 clamped 写，语义不变；
     *   - 落点不成立 → 保留真实位置（能查就查 query_position），
     *     patch_error(PB2_E_SEEK) 让 JS 知道这次没成，并清 seekPending
     *     （否则 JS 会一直以为在 seek 中，永远不放行）。
     */
    if (got_async) {
        g_w.last_pos_ms = clamped;
        pb2_stats_patch_position(&g_w.stats, clamped, dur);
        pb2_stats_patch_seek(&g_w.stats, 0, clamped);
    } else {
        long long real = -1;
        long long pos_raw = 0;
        if (g_w.pipe && gst_element_query_position(g_w.pipe, GST_FORMAT_TIME, &pos_raw) &&
            pos_raw >= 0) {
            real = pos_raw / 1000000LL + g_w.base_ms;
        }
        if (real < 0) real = g_w.last_pos_ms; /* 连位置都查不到：保持上一次已知值，绝不用目标值冒充 */
        g_w.last_pos_ms = real;
        pb2_logf("seek: NOT landed, keep真实位置=%lldms (target=%lldms)", real, clamped);
        pb2_stats_patch_error(&g_w.stats, PB2_E_SEEK, "seek did not land");
        pb2_stats_patch_position(&g_w.stats, real, dur);
        pb2_stats_patch_seek(&g_w.stats, 0, real);
    }
    pb2_probe_reset(&g_w.probe);
    g_w.seek_frames_base = 0;
    g_w.avskew_last_log_ms = 0;
    g_w.fps_frames_base = 0;
    g_w.last_fps_ms = pb2_now_ms();
    g_w.fps_calc = 0;
    /*
     * 【P0-1】seek 之后同样要重新等首帧：这里的「首帧」是落点之后的第一帧。
     * 顺带把 resyncing 立起来，JS 那边会显示「加载中」并让位，
     * 首帧一到（on_video_buffer）就自动清掉。
     */
    g_w.frame_epoch_ms = pb2_now_ms();
    g_w.audio_epoch_ms = g_w.frame_epoch_ms;
    g_w.first_frame_logged = 0;
    g_w.first_audio_logged = 0;
    g_w.last_src_check_ms = 0;
    g_w.last_src_check2_ms = 0; /* 【P1-1】新源重新探测 */
    g_w.src_died = 0;
    g_w.src_died_at_ms = 0;
    pb2_stats_patch_resync(&g_w.stats, 1, g_w.frame_epoch_ms);
    if (g_w.paused) {
        /* 暂停中 seek：保持 PAUSED，让画面停在目标帧 */
        gst_element_get_state(g_w.pipe, NULL, NULL, (GstClockTime)2000000000ULL);
    }
}

/* ============================ bus / 周期任务 ============================ */

/*
 * 取消息类型名。返回 "error"/"eos"/"async-done"/"state-changed"/…
 * 这是唯一可靠判别方式（见 third_party/gst_min.h 的注释）。
 */
static const char *msg_name(GstMessage *msg) {
    GstStructure *st = gst_message_get_structure(msg);
    if (!st) return "";
    return gst_structure_get_name(st);
}

/*
 * 解析 ERROR 消息并落到 lastError。返回 1 表示这是 ERROR。
 *
 * 【踩过的坑】必须先确认这确实是 error 消息再调 gst_message_parse_error。
 * 对 EOS / 其它类型的消息调用它会在库内部踩空指针直接 SIGSEGV
 * （实测崩溃栈：gst_message_parse_error -> gst_structure_id_get ->
 *  gst_structure_id_get_valist -> gst_structure_id_get_value -> SIGSEGV）。
 */
static int take_error(GstMessage *msg, const char *where) {
    GError *ge = NULL;
    char *dbg = NULL;
    char buf[PB2_ERRMSG_CAP];

    if (strcmp(msg_name(msg), "error") != 0) return 0;

    gst_message_parse_error(msg, &ge, &dbg);
    if (!ge) {
        if (dbg) g_free(dbg);
        return 0;
    }
    snprintf(buf, sizeof(buf), "%s: %s", where, ge->message ? ge->message : "bus error");
    pb2_logf("%s | %s", buf, dbg ? dbg : "");
    pb2_stats_patch_error(&g_w.stats, PB2_E_PIPELINE, buf);
    pb2_stats_patch_state(&g_w.stats, PB2_STATE_ERROR);
    g_error_free(ge);
    if (dbg) g_free(dbg);
    return 1;
}

static void handle_bus_message(GstMessage *msg) {
    const char *nm;

    if (take_error(msg, "bus")) return;

    nm = msg_name(msg);
    if (strcmp(nm, "eos") == 0) {
        pb2_logf("BUS EOS");
        g_w.eos = 1;
        pb2_stats_patch_state(&g_w.stats, PB2_STATE_STOPPED);
        pb2_mp3_marker_clear();
    }
}

/*
 * 轮询 bus。timeout_ms 内没有消息就返回。
 * 只 filter ERROR | EOS：其它消息（含 ASYNC_DONE）一律不管。flush seek 之后
 * 管线根本不进 ASYNC 态、GStreamer 不会发 ASYNC_DONE（见 do_seek 里那段
 * 「等 ASYNC_DONE 这个前提本身是错的」），把别的消息混进来只会干扰判据。
 */
static void poll_bus(int timeout_ms) {
    GstMessage *msg;
    if (!g_w.bus) return;
    msg = gst_bus_timed_pop_filtered(g_w.bus, (GstClockTime)timeout_ms * 1000000ULL,
                                     GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
    if (!msg) return;
    handle_bus_message(msg);
    gst_message_unref(msg);
}

static void refresh_position(void) {
    long long pos = 0;
    long long dur = 0;
    if (!g_w.pipe || !g_w.pipeline_alive) return;
    /*
     * 单一权威媒体时钟（硬规则 4）：位置只来自 query_position，JS 侧不累加。
     *
     * base_ms 换算：网络源的 ffmpeg 用 -ss 定位，输出时间戳从 0 重基，
     * 所以对 JS 报告的位置 = base_ms + query_position()。本地文件 base_ms 恒 0。
     */
    if (gst_element_query_position(g_w.pipe, GST_FORMAT_TIME, &pos) && pos >= 0) {
        g_w.last_pos_ms = pos / 1000000LL + g_w.base_ms;
    }
    if (gst_element_query_duration(g_w.pipe, GST_FORMAT_TIME, &dur) && dur > 0) {
        dur = dur / 1000000LL + g_w.base_ms;
        /* 流式容器（Matroska over FIFO）duration 恒为未知/极大，
         * 所以 GStreamer 给的 duration 只在不超过 JS 声明值时才可信 */
        if (g_w.req && g_w.req->durationMs > 0 && dur > g_w.req->durationMs) {
            dur = g_w.req->durationMs;
        }
    } else {
        dur = g_w.req ? g_w.req->durationMs : 0;
    }
    pb2_stats_patch_position(&g_w.stats, g_w.last_pos_ms, dur);

    /*
     * 【avSkewMs】量化音画偏差（口径见 pb2_worker_t 里 seek_frames_base 注释）。
     *
     * 每 2 秒打一行，只在有帧之后算。判读方法：
     *   - 起播/seek 后立即读：应该收敛到 0 附近（±100ms）
     *   - 若稳定停在某个负值不动：视频落后音频，那个值 ≈ -(T-K)
     *   - 随时间缓慢单调漂移 = fps 标称误差（29.67 vs 30），不是音画不同步
     */
    if (g_w.first_frame_logged) {
        long long now2 = pb2_now_ms();
        if (now2 - g_w.avskew_last_log_ms >= 2000) {
            unsigned long long frames =
                atomic_load_explicit(&g_w.probe.frames, memory_order_relaxed);
            unsigned long long df = frames - g_w.seek_frames_base;
            long long content_ms = (long long)((double)df * 1000.0 / 30.0 + 0.5);
            long long avskew = (g_w.last_pos_ms - g_w.base_ms) - content_ms;
            g_w.avskew_last_log_ms = now2;
            g_w.avskew_last_ms = avskew;
            pb2_logf("av: avSkewMs=%lld pos=%lld base=%lld frames=%llu content=%lldms",
                     avskew, g_w.last_pos_ms, g_w.base_ms, frames, content_ms);
        }
    }

    /*
     * audioBytes：把 probe 的原子计数搬到 stats 快照里。
     * status() 只读快照（无锁），所以必须在这里同步，不能让 JS 去读 probe。
     */
    /*
     * 【P1-7】这里只更新字节数，绝不动 hasAudio。
     *
     * hasAudio 的权威来源只有两个：on_pad_added 真的把音频分支建起来了
     * （置 1），teardown_pipeline_ex 拆管线（清 0）。
     * 以前这里传 `g_w.has_audio ? 1 : 0` 一起写，而 seek 重建窗口里
     * has_audio 会被 teardown 清成 0、新分支还没接上，于是 JS 每个 tick
     * 都能看到 hasAudio 在 1/0 之间闪 —— 表现为「音频状态抖动」。
     */
    {
        unsigned long long ab =
            atomic_load_explicit(&g_w.probe.abytes, memory_order_relaxed);
        if (ab != g_w.last_abytes) {
            g_w.last_abytes = ab;
            pb2_stats_patch_audio_bytes(&g_w.stats, (long long)ab);
        }
    }
}

static void refresh_fps(void) {
    long long now = pb2_now_ms();
    long long dt = now - g_w.last_fps_ms;
    unsigned long long frames;
    unsigned long long delta;
    if (dt < 1000) return;
    frames = atomic_load_explicit(&g_w.probe.frames, memory_order_relaxed);
    delta = frames - g_w.fps_frames_base;
    g_w.fps_calc = (int)((double)delta * 1000.0 / (double)dt + 0.5);
    g_w.fps_frames_base = frames;
    g_w.last_fps_ms = now;
    pb2_stats_patch_fps(&g_w.stats, g_w.fps_calc);
}

/*
 * 平面检查。
 *
 * 屏幕关着时**直接跳过**（连日志都不打，见下面 P0-2 注释）；屏幕亮着时若平面
 * 连续丢失达到 PB2_PLANE_LOST_REPORT 次才记日志 + lastError。
 *
 * 已知偏差（未修，仅记录）：函数头原写「只在状态是 PLAYING 时才算异常」，
 * 实际下面的守卫只判 pipeline_alive / vbranch / eos，**没有检查
 * stats.state 是不是 PLAYING** —— 所以 PAUSED/STOPPED 期间若屏幕亮着、
 * 且平面确实掉了，streak 仍会累计（只在真正掉平面时才会发生，噪声可忽略）。
 *
 * 息屏期间平面消失是完全正常的（内核 disable crtc），而且实测只要进程活着，
 * 唤醒后平面会自动回到 ACTIVE，所以这里**绝不重建管线**，只记日志 + lastError。
 */
static void check_plane(void) {
    long long now = pb2_now_ms();
    int active;
    int screen_off;
    if (now - g_w.last_plane_ms < PB2_PLANE_TICK_MS) return;
    g_w.last_plane_ms = now;

    /*
     * 【P0-2】先判屏幕状态，关屏时**完全跳过**（连日志都不打）。
     *
     * 为什么必须放在 plane_query 之前：关屏时 bl_power=4，内核会 disable crtc，
     * 平面本来就不更新、summary 里根本没有 Esmart1-win0 段。以前先查平面再查
     * 屏幕，而且 pb2_screen_off() 优先信不可靠的 /tmp/.screen_state，导致关屏
     * 播放时每秒误报一次 `plane missing (active=0 screenOff=0)`。
     */
    screen_off = pb2_screen_off();
    if (screen_off == 1) {
        g_w.plane_lost_streak = 0;
        pb2_stats_patch_plane(&g_w.stats, -1, 0); /* -1 = 未检查（屏幕关） */
        return;
    }

    active = pb2_plane_query();
    pb2_stats_patch_plane(&g_w.stats, active, g_w.plane_lost_streak);

    if (active == 1) {
        if (g_w.plane_lost_streak > 0) {
            pb2_logf("plane recovered after %d misses", g_w.plane_lost_streak);
        }
        g_w.plane_lost_streak = 0;
        return;
    }

    /* 起播宽限：OPEN 后 2s 内平面还没上屏是正常的 */
    if (g_w.plane_grace_until_ms > 0 && now < g_w.plane_grace_until_ms) return;

    if (!g_w.pipeline_alive) return;
    if (!g_w.vbranch) return;
    if (g_w.eos) return;
    /* 平面是我们自己隐藏的（键盘避让 1x1），不是丢失，直接跳过 */
    if (g_w.rect_hidden) {
        g_w.plane_lost_streak = 0;
        pb2_stats_patch_plane(&g_w.stats, -1, 0); /* -1 = 未检查（主动隐藏） */
        return;
    }

    g_w.plane_lost_streak++;
    /* 连续丢失达到阈值才报异常；单次抖动只留一行 trace */
    if (g_w.plane_lost_streak >= PB2_PLANE_LOST_REPORT) {
        pb2_logf("plane missing (active=%d screenOff=%d) streak=%d -- reported", active,
                 screen_off, g_w.plane_lost_streak);
        /* 只记错误，绝不重建管线：实测只要进程活着，屏幕唤醒后平面自动恢复 */
        pb2_stats_patch_error(&g_w.stats, PB2_E_PLANE_LOST,
                              "kms plane not active while playing");
    } else {
        pb2_logf("plane not active yet (active=%d screenOff=%d) streak=%d/%d", active,
                 screen_off, g_w.plane_lost_streak, PB2_PLANE_LOST_REPORT);
    }
    pb2_stats_patch_plane(&g_w.stats, active, g_w.plane_lost_streak);
}

/*
 * 【P0-1】首帧看门狗。
 *
 * 背景：旧实现里「没有过帧」和「一切正常」对 JS 是无法区分的 ——
 * pb2_session_stall_ms() 在没帧时返回 -1，JS 的 `Number(-1) || 0` 仍然是 -1，
 * 于是 `videoStallMs > 8000` 恒为 false，无帧看门狗永远不会触发；
 * 而 do_open 又把 state 无条件 patch 成 PLAYING、lastError 保持 0。
 * 结果就是「起播失败但界面显示正在播放、没有任何错误」，只能手动返回。
 *
 * 这里补两个判据：
 *   1. ffmpeg 在首帧之前就退出（FIFO 是 O_RDWR 打开的，读端永远看不到 EOF，
 *      所以必须靠 /proc 轮询才能发现源已死）→ 立刻判失败，不用等满 15s。
 *   2. 超过 PB2_FIRST_FRAME_TIMEOUT_MS 还没有一帧 → 判失败。
 * 两条路都走 PB2_STATE_ERROR + lastError，并且拆管线，避免半死管线留在那儿。
 *
 * 只记日志/改状态，绝不在这里重建管线（重建是 do_seek/do_open 的事）。
 */
static void check_first_frame(void) {
    long long now;
    if (g_w.frame_epoch_ms <= 0) return; /* 已经有帧了 */
    if (!g_w.pipeline_alive) return;
    now = pb2_now_ms();

    if (g_w.src_ready && now - g_w.last_src_check_ms >= PB2_SRC_CHECK_MS) {
        g_w.last_src_check_ms = now;
        if (!pb2_src_alive(&g_w.src)) {
            /*
             * 【自愈一次】ffmpeg 首帧前秒退多半是瞬态 CDN 掐流（真机三连 seek
             * 第三跳实测），重开一次源通常就好。预算内（首帧 15s 看门狗共用
             * 这个 deadline，重开只需 ~0.6s）且会话总重试 <2 次才重试；
             * 否则按原路径报错（URL 真失效时不能白等）。
             * 重试 = 完整重走 do_open 的「起新源 + 重建管线」最小路径。
             */
            if (g_w.first_frame_retries < 2 && g_w.req &&
                now - g_w.frame_epoch_ms + 2000 <= PB2_FIRST_FRAME_TIMEOUT_MS) {
                long long t_restart = pb2_now_ms();
                pb2_logf("first frame: src died before any frame (+%lldms) -> retry %d",
                         now - g_w.frame_epoch_ms, g_w.first_frame_retries + 1);
                teardown_pipeline();
                pb2_src_close(&g_w.src); /* 关掉死源的 FIFO/句柄，日志见 reap */
                g_w.src_ready = 0;
                if (pb2_src_open(&g_w.src, g_w.req->input, g_w.req->input2,
                                 g_w.req->userAgent, g_w.req->referer, g_w.base_ms,
                                 g_w.req->durationMs) == 0) {
                    g_w.src_ready = 1;
                    if (build_pipeline(g_w.req, 1) == 0) {
                        gst_element_set_state(g_w.pipe, GST_STATE_PLAYING);
                        g_w.first_frame_retries++;
                        /* 计时基准重置：这次重试重新算 15s 看门狗 */
                        g_w.frame_epoch_ms = pb2_now_ms();
                        g_w.last_src_check_ms = g_w.frame_epoch_ms;
                        g_w.first_frame_logged = 0;
                        g_w.first_audio_logged = 0;
                        pb2_stats_patch_state(&g_w.stats, PB2_STATE_PLAYING);
                        pb2_stats_patch_error(&g_w.stats, PB2_E_NONE, "");
                        pb2_logf("first frame: retry restart done +%lldms (retries=%d)",
                                 pb2_now_ms() - t_restart, g_w.first_frame_retries);
                        return;
                    }
                    pb2_logf("first frame: retry build_pipeline failed");
                } else {
                    pb2_logf("first frame: retry src_open failed");
                }
                /* 重试失败 → 走下面的正常报错路径（teardown 已做过） */
            }
            pb2_logf("first frame failed: src exited before any frame (+%lldms)",
                     now - g_w.frame_epoch_ms);
            pb2_stats_patch_error(&g_w.stats, PB2_E_NO_FIRST_FRAME,
                                  "source exited before first frame");
            pb2_stats_patch_state(&g_w.stats, PB2_STATE_ERROR);
            teardown_pipeline();
            g_w.frame_epoch_ms = 0;
            return;
        }
    }

    if (now - g_w.frame_epoch_ms >= PB2_FIRST_FRAME_TIMEOUT_MS) {
        pb2_logf("first frame failed: no frame in %dms (alive=%d vbranch=%d) -> error",
                 PB2_FIRST_FRAME_TIMEOUT_MS, g_w.pipeline_alive, g_w.vbranch);
        pb2_stats_patch_error(&g_w.stats, PB2_E_NO_FIRST_FRAME,
                              "no first frame within 15s");
        pb2_stats_patch_state(&g_w.stats, PB2_STATE_ERROR);
        teardown_pipeline();
        g_w.frame_epoch_ms = 0;
    }
}

/*
 * 【P1-1】首帧之后的「源已死」探测。
 *
 * 背景：ffmpeg 是独立子进程，`-c copy` 到 FIFO。它中途退掉（网络断、URL
 * 过期、CDN 掐流）时，FIFO 的写端没了，但我们**自己还持着 O_RDWR 的
 * hold_fd**，所以 filesrc 永远等不到 EOF → 既不出帧也不发 EOS → 界面上
 * 就是「画面冻住、进度条不动、也不报错」。
 *
 * 这里做两件事：
 *   1. 按 worker 空闲 tick 的 PB2_SRC_CHECK_MS（1000ms）节奏探一次
 *      `pb2_src_reap_if_dead()`。它确认写端真死之后才 close(hold_fd)，于是
 *      filesrc 立刻 EOF、matroskademux 发 EOS，走 handle_bus_message() 的
 *      eos 分支正常收尾。
 *   2. 只打标记置 src_died，**不立刻 patch_alive(0)**（理由见结构体注释）。
 *      若 3 秒内 EOS 没来（demuxer 可能卡在半个 cluster 上），才补一个
 *      patch_alive(0) 让 JS 的看门狗能接手。
 *
 * 已经 eos 或用完的会话直接返回，不重复打日志。
 */
static void check_src_after_first_frame(void) {
    long long now;

    if (g_w.frame_epoch_ms > 0) return; /* 还没出首帧，交给 check_first_frame */
    if (!g_w.pipeline_alive) return;
    if (g_w.eos) return;

    now = pb2_now_ms();

    /* 已经判定过源死了：只等 EOS，超时才升级成 alive=0 */
    if (g_w.src_died) {
        if (now - g_w.src_died_at_ms >= 3000) {
            pb2_logf("source died +%lldms ago and no EOS arrived -> alive=0",
                     now - g_w.src_died_at_ms);
            g_w.src_died = 0;
            pb2_stats_patch_alive(&g_w.stats, 0);
        }
        return;
    }

    if (!g_w.src_ready) return;
    if (now - g_w.last_src_check2_ms < PB2_SRC_CHECK_MS) return;
    g_w.last_src_check2_ms = now;

    if (pb2_src_reap_if_dead(&g_w.src)) {
        pb2_logf("source died after first frame -> release fifo, expect EOS");
        g_w.src_died = 1;
        g_w.src_died_at_ms = now;
    }
}

/*
 * /tmp/mp3playing 保活重写。
 * system_sleep_wakeup 只看文件是否存在，但某些实现会按 mtime 判断，
 * 所以每 20s 重写一次（open+close 更新 mtime），代价可忽略。
 */
static void keep_marker(void) {
    long long now = pb2_now_ms();
    if (now - g_w.last_marker_ms < PB2_MARKER_TICK_MS) return;
    g_w.last_marker_ms = now;
    pb2_mp3_marker_set();
}

/* ============================ worker 主循环 ============================ */

static void process_cmd(pb2_cmd_t *c) {
    switch (c->type) {
        case PB2_CMD_OPEN:
            do_open((pb2_open_req_t *)c->ptr);
            break;
        case PB2_CMD_PAUSE:
            if (g_w.pipe && g_w.pipeline_alive) {
                long long t0 = pb2_now_ms();
                gst_element_set_state(g_w.pipe, GST_STATE_PAUSED);
                g_w.paused = 1;
                pb2_stats_patch_state(&g_w.stats, PB2_STATE_PAUSED);
                pb2_logf("PAUSE took %lldms", pb2_now_ms() - t0);
            }
            break;
        case PB2_CMD_RESUME:
            if (g_w.pipe && g_w.pipeline_alive) {
                long long t0 = pb2_now_ms();
                gst_element_set_state(g_w.pipe, GST_STATE_PLAYING);
                g_w.paused = 0;
                g_w.last_fps_ms = t0;
                g_w.fps_frames_base =
                    atomic_load_explicit(&g_w.probe.frames, memory_order_relaxed);
                pb2_stats_patch_state(&g_w.stats, PB2_STATE_PLAYING);
                pb2_logf("RESUME took %lldms", pb2_now_ms() - t0);
            }
            break;
        case PB2_CMD_SEEK:
            /*
             * 只记目标，不立刻执行（debounce）。
             *
             * 【P1-3】覆盖式语义：拖动过程中每来一个 SEEK 都只是**覆盖**
             * seek_target 并刷新 seek_last_ms，worker 等到「最后一个目标之后
             * 250ms 没有新目标」才真正重启源。seek_coalesced 统计被合并掉的
             * 次数，用来在真机上确认一次拖动只触发一次重启。
             */
            if (g_w.seek_pending) g_w.seek_coalesced++;
            g_w.seek_target = c->i64;
            g_w.seek_pending = 1;
            g_w.seek_last_ms = pb2_now_ms();
            pb2_stats_patch_seek(&g_w.stats, 1, c->i64);
            break;
        case PB2_CMD_SET_RECT:
            /*
             * 【键盘避让】运行时改 render-rectangle，不重建管线。
             * i64 = x | y<<16 | w<<32 | h<<48（见 queue.h）。
             * gst_util_set_object_arg 实测（spike-report §12）立即生效，
             * 帧率不受影响。w/h 传 0（隐藏意图）会被下面提升成 1x1 —— 真机
             * 实测 0x0 会被 kmssink 当成「未设置」而回退全屏，见下方注释。
             * 只在管线活着且 vsink 存在时执行；JS 侧在 open 前/后任意时刻
             * 都可以发，worker 空闲时处理，零阻塞。
             *
             * 【必须加面板偏移】JS 传的 x 与 open 下发的 req->x 同一坐标系
             * （都是「视频逻辑区左缘」），而 build_pipeline 拼 g_w.rect 时会加
             * PB2_PANEL_X_OFFSET（见上面那个 snprintf）。这里若直接用 rx，
             * 恢复原矩形时画面会左移 107 像素。真机实测过：不加偏移打出
             * `<0,299,240,427>`，而 open 时建的是 `<107,299,240,427>`。
             */
            if (g_w.pipe && g_w.pipeline_alive && g_w.vsink_el) {
                int rx = (int)(short)((c->i64 >> 0) & 0xFFFF);
                int ry = (int)(short)((c->i64 >> 16) & 0xFFFF);
                int rw = (int)(unsigned short)((c->i64 >> 32) & 0xFFFF);
                int rh = (int)(unsigned short)((c->i64 >> 48) & 0xFFFF);
                int want_hide;
                char rect[48];
                long long t0_rect = pb2_now_ms();

                /*
                 * 【隐藏平面必须用 1x1，绝不能用 0x0】真机实测
                 * （/tmp/rect3.sh，videotestsrc + kmssink plane-id=76，
                 *  每轮读 /sys/kernel/debug/dri/0/summary 的 Esmart1-win0 段）：
                 *   <107,0,240,427> → dst pos[107,0] rect[240 x 427]  正常
                 *   <0,0,0,0>       → dst pos[0,0]   rect[480 x 960]  **全屏！**
                 *   <0,0,1,1>       → dst pos[0,0]   rect[1 x 1]      真正不可见
                 * 四种情况全都 rc=0、无 error/warn —— kmssink 对零宽高既不报错
                 * 也不隐藏，而是回退成「未设置矩形 ⇒ 全屏」。键盘避让期间若
                 * 传 0x0，视频平面会盖住整个 falcon 键盘（960x266 全屏 miniapp），
                 * 用户连候选词都点不到。所以这里把 0（以及任何 <1 的值）
                 * 统一提升成 1x1：几何合法、单个像素在 480x960 面板上不可见。
                 */
                if (rw < 1) rw = 1;
                if (rh < 1) rh = 1;
                /* 隐藏时也不要加面板偏移，免得 1x1 被推到非零坐标 */
                if (rw > 1 || rh > 1) rx += PB2_PANEL_X_OFFSET;
                want_hide = (rw <= 1 && rh <= 1) ? 1 : 0;
                snprintf(rect, sizeof(rect), "<%d,%d,%d,%d>", rx, ry, rw, rh);

                /*
                 * ================== kmssink render-rectangle 是单向棘轮 ==================
                 *
                 * 真机实测（.tmp/mkv/rect35.c，**真实管线结构**：kmssink 在
                 * parse_bin 出来的分支 bin 内、sync=true、videorate 上游）：
                 *
                 *   240x427 --直接 set--> 1x1        生效（pos[0,0] rect[1x1]）
                 *   1x1 --直接 set--> 240x427        **无效，6 轮全部停在 1x1**
                 *   1x1 --pipe PAUSED 循环--> 240x427 **无效**（rect38，5 轮全失败）
                 *   1x1 --sink READY/PLAYING--> 240x427 **无效**（rect33，平面直接 RELEASED 不回来）
                 *   1x1 --sink NULL--> 240x427       **无效**（rect36(b)，RELEASED）
                 *   1x1 --pipe READY -> set rect -> pipe PLAYING--> 240x427  **有效**（rect36(a)）
                 *
                 * 即：**只能缩小，不能放大**；唯一可靠的「放大」途径是让整条管线
                 * 过一次 READY 重置。而 pipe READY 会关掉 filesrc 的 FIFO 读端、
                 * 流位置全部丢失 —— 对我们这种「ffmpeg → FIFO → filesrc」的边下
                 * 边播架构来说，等价于「按当前播放位置重新拉一路源」，也就是
                 * 走已经充分验证过的 do_seek 重启路径（三跳 drift=0ms、
                 * first frame 413~516ms）。
                 *
                 * 所以这里的分支策略是：
                 *   1) 请求隐藏  -> 直接 set 1x1（快，~0ms，实测有效），记 rect_hidden
                 *   2) 请求恢复且当前是隐藏态 -> 更新 g_w.rect 并**投递一次原地 seek**，
                 *      build_pipeline 会用新的 g_w.rect 建管线，平面按正确矩形重建
                 *   3) 请求改矩形且从未隐藏过 -> 直接 set（正常尺寸之间不会触发棘轮）
                 *
                 * 为什么 1x1 之后必须走 seek 而不能「重设 render-rectangle 再让
                 * kmssink 自己重配」：render-rectangle 是 **write-only** 属性
                 * （g_object_get 报 "property 'render-rectangle' of object class
                 * 'GstKMSSink' is not readable"），kmssink 只在 sink 重新
                 * configure（分配 plane）时读取它，而 1x1 之后 sink 不再重新
                 * configure，所以设什么值都不会被读。
                 */
                if (want_hide) {
                    gst_util_set_object_arg(g_w.vsink_el, "render-rectangle", rect);
                    g_w.rect_hidden = 1;
                    pb2_logf("SET_RECT %s (hide) took %lldms", rect, pb2_now_ms() - t0_rect);
                } else if (g_w.rect_hidden) {
                    /*
                     * 恢复：把目标矩形写进 g_w.rect（build_pipeline 用它），
                     * 然后投递一次原地 seek 让管线带着新矩形重建。
                     * 走 seek 的 debounce 通道（seek_last_ms 回拨到过去，
                     * 下一次 worker tick 立刻执行），复用全部已验证代码。
                     */
                    snprintf(g_w.rect, sizeof(g_w.rect), "%s", rect);
                    g_w.rect_hidden = 0;
                    g_w.seek_target = g_w.last_pos_ms > 0 ? g_w.last_pos_ms : 0;
                    g_w.seek_pending = 1;
                    g_w.seek_last_ms = pb2_now_ms() - PB2_SEEK_DEBOUNCE_MS - 1;
                    pb2_stats_patch_seek(&g_w.stats, 1, g_w.seek_target);
                    pb2_logf("SET_RECT %s (restore: 1x1 无法放大，投递原地 seek@%lldms 重建管线)",
                             rect, g_w.seek_target);
                } else {
                    /* 正常尺寸之间改矩形：直接 set 即可，不会踩棘轮 */
                    gst_util_set_object_arg(g_w.vsink_el, "render-rectangle", rect);
                    snprintf(g_w.rect, sizeof(g_w.rect), "%s", rect);
                    pb2_logf("SET_RECT %s took %lldms", rect, pb2_now_ms() - t0_rect);
                }
            }
            break;
        case PB2_CMD_STOP:
            pb2_logf("STOP");
            teardown_pipeline();
            reap_dying(); /* 【P1-3】旧源不会自己消失，停止时一并回收 */
            /*
             * 【P0-1】收尾音频状态。之前 pb2_audio_restore() 是死函数（零调用点），
             * 现在让 STOP/RELEASE 真正走一遍：静音模式 => 强制重新静音（幂等），
             * 非静音模式 => 恢复 Master 100% 留给下一个 app。
             */
            pb2_audio_restore();
            pb2_stats_patch_state(&g_w.stats, PB2_STATE_STOPPED);
            break;
        case PB2_CMD_RELEASE:
            pb2_logf("RELEASE");
            teardown_pipeline();
            reap_dying();
            pb2_audio_restore();
            pb2_stats_patch_state(&g_w.stats, PB2_STATE_IDLE);
            break;
        case PB2_CMD_QUIT:
            break;
        default:
            break;
    }
}

static void *worker_main(void *arg) {
    pb2_cmd_t c;
    long long next_bus = 0;

    (void)arg;
    pb2_logf("worker start");

    /* gst_init 在 worker 上做：冷启动 500ms / 热 25ms，绝不能阻塞 JS 线程 */
    {
        long long t0 = pb2_now_ms();
        setenv("GST_REGISTRY", "/tmp/pb2gstreg.bin", 1);
        /* 清掉上次进程被杀留下的 FIFO（只删属主进程已死的） */
        pb2_src_cleanup_stale();
        gst_init(NULL, NULL);
        g_w.gst_version = gst_version_string();
        g_w.gst_ready = 1;
        pb2_logf("gst_init done in %lldms, version=%s", pb2_now_ms() - t0,
                 g_w.gst_version ? g_w.gst_version : "?");
    }

    pb2_stats_patch_state(&g_w.stats, PB2_STATE_IDLE);

    while (!pb2_cmdq_quit_flag(&g_w.q)) {
        int got = pb2_cmdq_pop(&g_w.q, &c);

        if (got == 0) {
            if (c.type == PB2_CMD_QUIT) {
                if (c.ptr) pb2_open_req_free((pb2_open_req_t *)c.ptr);
                break;
            }
            process_cmd(&c);
            continue;
        }

        /* 队列空了：做周期任务，然后小睡 */
        {
            long long now = pb2_now_ms();

            /* seek debounce 到期 => 执行 */
            if (g_w.seek_pending && now - g_w.seek_last_ms >= PB2_SEEK_DEBOUNCE_MS) {
                long long target = g_w.seek_target;
                g_w.seek_pending = 0;
                do_seek(target);
                continue;
            }

            /*
             * 【已知空块，P2-14】这里原本想「分支可能在 pad-added 之后才出现，
             * 补挂 probe」，但 probe 现在完全由 on_pad_added() 在
             * gst_pad_link 之前挂好（见那里的注释），兜底函数 attach_probes()
             * 至今 0 调用点。所以这个分支**什么也不做**，只是留个位置。
             * 注意 worker 空闲 tick 是按 pb2_cmdq_wait 的节奏跑的，删掉它对
             * 行为没有影响；保留是为了提醒后来者「probe 为什么不需要在这里补」。
             */
            if (g_w.pipeline_alive && !g_w.vbranch) {
                /* 空块，理由见上 */
            }

            refresh_position();
            refresh_fps();
            refresh_audio_route();
            check_plane();
            /* 【P0-1】首帧看门狗：15s 无帧或源先死 → error（含拆管线） */
            check_first_frame();
            /* 【P1-1】首帧之后源中途死掉 → 放掉 hold_fd 让 demuxer 发 EOS */
            check_src_after_first_frame();
            keep_marker();
            /*
             * 【P1-3】回收上一次 seek 留下的旧 ffmpeg。
             * 放在这里（队列空、关键路径之外）而不是 seek 里，这样 seek 的
             * 端到端耗时完全不含 kill+waitpid，旧进程的 200ms 回收被藏到
             * 新管线正在下载/预滚的这段时间里。
             */
            reap_dying();

            /*
             * 【P0-1】混音器延迟配置。
             * 放进关键路径会把 OPEN 从 53ms 拖到 186ms（一次 amixer ~37ms，
             * 两个卡各一次 sset + 一次 sget 回读 = 4 次），而混音器跟首帧
             * 上屏无关，所以挪到首批帧已经开始播之后的空闲 tick 里做。
             *
             * 默认静音：走 pb2_audio_prepare()，它会在静音模式下强制 mute，
             * **绝不**打开扬声器。只有 touched /tmp/pb2_sound 或 PB2_MUTE=0
             * 才会真的设 SPK + 100%。
             */
            if (g_w.mixer_pending) {
                int want = (g_w.mixer_pending == 1);
                g_w.mixer_pending = 0;
                if (pb2_mute_enabled()) {
                    pb2_logf("mixer: tick setup skipped (muted) source=%s", pb2_mute_reason());
                } else {
                    pb2_logf("mixer: tick setup start (want=%d)", want);
                }
                pb2_audio_prepare(want);
            }

            if (now >= next_bus) {
                poll_bus(PB2_BUS_TICK_MS);
                next_bus = pb2_now_ms() + PB2_BUS_TICK_MS;
            }

            /* 睡到下一个事件。最长 50ms，保证 debounce 精度和响应性。 */
            pb2_cmdq_wait(&g_w.q, 50);
        }
    }

    pb2_logf("worker loop exit");
    teardown_pipeline();
    reap_dying();
    pb2_audio_restore();
    if (g_w.req) {
        pb2_open_req_free(g_w.req);
        g_w.req = NULL;
    }
    pb2_stats_patch_state(&g_w.stats, PB2_STATE_IDLE);
    pb2_logf("worker end");

    /*
     * 【P0-2】退出握手：置 worker_done 并唤醒可能在 shutdown 里等待的线程。
     * 放在 return NULL 之前的**最后一句**，此时管线/ffmpeg/标记文件全部
     * 已清理干净，g_w 不再被本线程读写。
     */
    pthread_mutex_lock(&g_start_mu);
    g_w.worker_done = 1;
    pthread_cond_broadcast(&g_done_cv);
    pthread_mutex_unlock(&g_start_mu);

    return NULL;
}

/* ============================ 对外接口 ============================ */

int pb2_session_start(void) {
    int rc = 0;
    pthread_mutex_lock(&g_start_mu);
    if (atomic_load_explicit(&g_started, memory_order_acquire)) {
        pthread_mutex_unlock(&g_start_mu);
        return 0;
    }
    /*
     * 【P0-2】上一次 release() 超时把 worker detach 掉之后，那个线程可能
     * 还在跑收尾（拆管线 / reap ffmpeg）。它读写的是同一个 g_w，所以这里
     * **必须**等它真的置了 worker_done 才能 memset(&g_w)，否则就是把结构体
     * 从旧线程脚下抽走 —— 纯内存竞争，表现为随机崩溃或随机 memset 掉别人的
     * 管线指针。等待有上界，最多 PB2_REOPEN_WAIT_MS，超时只能硬清零（已在
     * 日志里留证据，方便真机定位）。
     */
    if (g_w.worker_detached) {
        struct timespec ts;
        long long deadline = pb2_now_ms() + PB2_REOPEN_WAIT_MS;
        int waited = 0;
        pb2_logf("session start: waiting detached worker (up to %dms)", PB2_REOPEN_WAIT_MS);
        while (!g_w.worker_done) {
            long long left = deadline - pb2_now_ms();
            if (left <= 0) break;
            clock_gettime(CLOCK_REALTIME, &ts);
            long long nsec = (long long)ts.tv_nsec + (left % 1000) * 1000000LL;
            ts.tv_sec += (time_t)(left / 1000 + nsec / 1000000000LL);
            ts.tv_nsec = (long)(nsec % 1000000000LL);
            pthread_cond_timedwait(&g_done_cv, &g_start_mu, &ts);
            waited++;
        }
        if (g_w.worker_done) {
            pb2_logf("session start: detached worker finished after %d wakeups", waited);
        } else {
            pb2_logf("session start: WARNING detached worker still alive after %dms, "
                     "clearing state anyway (old thread may touch g_w)", PB2_REOPEN_WAIT_MS);
        }
    }
    memset(&g_w, 0, sizeof(g_w));
    pb2_cmdq_init(&g_w.q);
    pb2_stats_init(&g_w.stats);
    pb2_probe_init(&g_w.probe);
    snprintf(g_w.rect, sizeof(g_w.rect), "<%d,0,240,427>", PB2_PANEL_X_OFFSET);

    if (pthread_create(&g_w.th, NULL, worker_main, NULL) != 0) {
        rc = -1;
    } else {
        g_w.th_started = 1;
        atomic_store_explicit(&g_started, 1, memory_order_release);
    }
    pthread_mutex_unlock(&g_start_mu);
    return rc;
}

int pb2_session_cmd(int type, long long i64, void *ptr) {
    pb2_cmd_t c;
    if (!atomic_load_explicit(&g_started, memory_order_acquire)) return -1;
    c.type = type;
    c.i64 = i64;
    c.ptr = ptr;
    if (pb2_cmdq_push(&g_w.q, &c) != 0) return -1;
    return 0;
}

void pb2_session_stats(pb2_stats_t *out) {
    if (!out) return;
    if (pb2_stats_read(&g_w.stats, out) != 0) {
        /* 读撕裂：退化成单字段读，保证不返回垃圾 */
        memset(out, 0, sizeof(*out));
        out->state = g_w.stats.state;
    }
}

void pb2_session_probe(pb2_probe_t **out) {
    if (out) *out = &g_w.probe;
}

int pb2_session_shutdown(void) {
    int joined = 0;
    int detached = 0;

    if (!atomic_load_explicit(&g_started, memory_order_acquire)) return 0;

    /* 先让 worker 自己看到 quit 标志，它会走正常的收尾路径 */
    pb2_cmdq_quit(&g_w.q);
    atomic_store_explicit(&g_started, 0, memory_order_release);

    pthread_mutex_lock(&g_start_mu);
    if (g_w.th_started && !g_w.worker_detached) {
        struct timespec ts;
        long long deadline = pb2_now_ms() + PB2_SHUTDOWN_WAIT_MS;

        /*
         * 【P0-2】这里**不能** pthread_join。worker 可能正卡在
         *   12s 的 gst_element_get_state（OPEN/seek 的 ASYNC 等待）
         *   3s 的本地 seek 轮询
         *   reap_pair 的 waitpid
         *   若干次 amixer spawn（open 前 pre_mute 1 次、worker tick 里
         *   setup 每卡 sset+sget 各一次共 4 次、mute 每卡 1 次共 2 次）
         * 任何一个都能把 JSAPI 线程（= QuickJS 主线程 = UI 线程）冻住十几秒。
         * 旧 v3.5.2 的 release 阻塞 4.9~5s 就是同类问题。
         *
         * 改为有界 timedwait：worker_main 收尾后自己置 worker_done。
         * 等到了就正常 join（此时 join 立即返回）；等不到就 detach 并记账，
         * 让下一次 pb2_session_start 去等它。
         */
        while (!g_w.worker_done) {
            long long left = deadline - pb2_now_ms();
            if (left <= 0) break;
            clock_gettime(CLOCK_REALTIME, &ts);
            {
                long long nsec = (long long)ts.tv_nsec + (left % 1000) * 1000000LL;
                ts.tv_sec += (time_t)(left / 1000 + nsec / 1000000000LL);
                ts.tv_nsec = (long)(nsec % 1000000000LL);
            }
            pthread_cond_timedwait(&g_done_cv, &g_start_mu, &ts);
        }

        if (g_w.worker_done) {
            /* 已退出：join 只是回收 pthread 资源，立即返回 */
            pthread_join(g_w.th, NULL);
            g_w.th_started = 0;
            joined = 1;
        } else {
            /*
             * 超时：worker 还在收尾。detach 之后 pthread_t 不可 join，
             * 下一次 start 另起新线程；旧线程继续安静地跑完自己的收尾
             * （它只碰 g_w 和已经属于它自己的 fd/子进程）。
             */
            pthread_detach(g_w.th);
            g_w.th_started = 0;
            g_w.worker_detached = 1;
            g_w.worker_done = 0; /* 新旧线程共用这一个标志，等它真正退出时置 1 */
            detached = 1;
        }
    }
    pthread_mutex_unlock(&g_start_mu);

    /*
     * 只有确认 worker 真的退出（joined）才能销毁命令队列的 mutex/cond。
     * detached 的情况下那个线程可能正卡在 pb2_cmdq_wait 里，销毁正在被使用
     * 的同步原语是 UB。这种情形下故意不销毁：下一次 pb2_session_start 会
     * memset(&g_w) 并重新 pb2_cmdq_init，旧对象随 g_w 一起被覆盖，代价是泄漏
     * 一个 mutex/cond（几百字节，且只在 release 超时这种罕见路径上发生）。
     */
    if (!detached) {
        pb2_cmdq_destroy(&g_w.q);
    }
    pb2_logf("session shutdown done (joined=%d detached=%d wait=%dms)",
             joined, detached, PB2_SHUTDOWN_WAIT_MS);
    return 0;
}

int pb2_session_gst_ready(void) { return g_w.gst_ready; }

const char *pb2_session_gst_version(void) { return g_w.gst_version; }

unsigned long long pb2_session_frames(void) {
    return atomic_load_explicit(&g_w.probe.frames, memory_order_relaxed);
}

long long pb2_session_stall_ms(void) {
    long long last = atomic_load_explicit(&g_w.probe.lastFrameMs, memory_order_relaxed);
    /*
     * 【P0-1】没有过任何一帧时**绝对不能返回 -1**。
     *
     * 旧的 `return -1` 一路走到 JS：play_session.js:438 是
     * `Number(st.videoStallMs) || 0` —— `Number(-1)` 是 -1，而 -1 是 truthy，
     * 所以兜底 0 不生效，拿到的仍是 -1；两个看门狗的条件是
     * `st.videoStallMs > 8000` / `> 25000`，-1 恒 false ⇒ **永不触发**。
     * 于是 URL 过期、断网、ffmpeg 秒退这些情况下界面永久"加载中…"，
     * native 侧 lastError 也是 0，用户只能返回重进。
     *
     * 现在返回「距本次 OPEN/seek 开始等首帧过了多久」：这个值单调增长，
     * 15s 后 JS 看门狗一定能判定失败（native 侧另有 check_first_frame()
     * 主动置 error，见 PB2_E_NO_FIRST_FRAME）。
     */
    if (last <= 0) {
        long long epoch = g_w.frame_epoch_ms;
        if (epoch <= 0) return 0;
        return pb2_now_ms() - epoch;
    }
    return pb2_now_ms() - last;
}
