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
/* seek 端到端目标（下发 -> 第一帧上屏），仅用于日志判定 */
#define PB2_SEEK_TARGET_MS 800

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
    GstElement *asink;      /* 音频分支的 alsasink */
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
                     "queue max-size-buffers=8 max-size-bytes=0 max-size-time=0 ! "
                     "h264parse ! mppvideodec rotation=%d ! "
                     "%s"
                     "videorate ! video/x-raw,framerate=%s ! "
                     "kmssink name=vsink plane-id=%d driver-name=rockchip sync=true "
                     "max-lateness=%d qos=true force-aspect-ratio=false skip-vsync=true "
                     "render-rectangle=%s",
                     g_w.rotate, fpscap, PB2_PANEL_RATE, PB2_KMS_PLANE_ID, PB2_MAX_LATENESS,
                     g_w.rect);
        } else if (strstr(cn, "h265") || strstr(cn, "hevc")) {
            snprintf(desc, sizeof(desc),
                     "queue max-size-buffers=8 max-size-bytes=0 max-size-time=0 ! "
                     "h265parse ! mppvideodec rotation=%d ! "
                     "%s"
                     "videorate ! video/x-raw,framerate=%s ! "
                     "kmssink name=vsink plane-id=%d driver-name=rockchip sync=true "
                     "max-lateness=%d qos=true force-aspect-ratio=false skip-vsync=true "
                     "render-rectangle=%s",
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
        if ((strncmp(cn, "audio/mpeg", 10) == 0 && mpegversion == 4) ||
            strstr(cn, "aac") || strstr(cn, "AAC")) {
            snprintf(desc, sizeof(desc),
                     "queue max-size-buffers=32 max-size-bytes=0 max-size-time=0 ! "
                     "aacparse ! faad ! audioconvert ! audioresample ! "
                     "audio/x-raw,rate=48000,channels=2 ! "
                     "alsasink name=asink buffer-time=30000 latency-time=10000");
        } else if (strstr(cn, "x-ac3") || strstr(cn, "x-eac3") || strstr(cn, "x-a52")) {
            snprintf(desc, sizeof(desc),
                     "queue max-size-buffers=32 ! ac3parse ! a52dec ! audioconvert ! "
                     "audioresample ! audio/x-raw,rate=48000,channels=2 ! "
                     "alsasink name=asink buffer-time=30000 latency-time=10000");
        } else {
            /* 未知音频编码：只要有 parser 就走通用链路，失败也只是没声音 */
            pb2_logf("pad-added: audio caps %s, trying generic chain", cn);
            snprintf(desc, sizeof(desc),
                     "queue max-size-buffers=32 ! decodebin ! audioconvert ! "
                     "audioresample ! audio/x-raw,rate=48000,channels=2 ! "
                     "alsasink name=asink buffer-time=30000 latency-time=10000");
        }
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
        /* 【P0-4】同上 */
        g_w.abranch = 1;
        pb2_stats_patch_audio(&g_w.stats, 1, 0);
        pb2_logf("audio branch ready (probe attached)");
    }
    gst_object_unref(sink);
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
    g_w.asink = NULL;
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
    /* 【P1-1】新会话：清掉上一次的源死亡标记与独立节流器 */
    g_w.src_died = 0;
    g_w.src_died_at_ms = 0;
    g_w.last_src_check2_ms = 0;
    g_w.last_src_check_ms = 0;
    g_w.first_frame_logged = 0;
    g_w.first_frame_retries = 0; /* 每个新会话重新给满自愈配额 */
    /* 【P1-18】帧率上限：0 = 不限制，>0 时在 mppvideodec 之后插 videorate max-rate */
    g_w.fps_cap = req->fpsCap;
    pb2_stats_patch_resync(&g_w.stats, 0, 0);

    pb2_probe_reset(&g_w.probe);
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
        g_w.first_frame_logged = 0;
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
        g_w.seek_frame_reported = 0; /* 首帧到达时由 on_video_buffer 报端到端耗时 */
        /* 【P0-1】seek 后重新等首帧（落点第一帧），超时/源先死同样判 error */
        g_w.frame_epoch_ms = pb2_now_ms();
        g_w.first_frame_logged = 0;
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
    g_w.fps_frames_base = 0;
    g_w.last_fps_ms = pb2_now_ms();
    g_w.fps_calc = 0;
    /*
     * 【P0-1】seek 之后同样要重新等首帧：这里的「首帧」是落点之后的第一帧。
     * 顺带把 resyncing 立起来，JS 那边会显示「加载中」并让位，
     * 首帧一到（on_video_buffer）就自动清掉。
     */
    g_w.frame_epoch_ms = pb2_now_ms();
    g_w.first_frame_logged = 0;
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
             * 帧率不受影响。w/h 为 0 时等效隐藏平面。
             * 只在管线活着且 vsink 存在时执行；JS 侧在 open 前/后任意时刻
             * 都可以发，worker 空闲时处理，零阻塞。
             */
            if (g_w.pipe && g_w.pipeline_alive && g_w.vsink_el) {
                int rx = (int)(short)((c->i64 >> 0) & 0xFFFF);
                int ry = (int)(short)((c->i64 >> 16) & 0xFFFF);
                int rw = (int)(unsigned short)((c->i64 >> 32) & 0xFFFF);
                int rh = (int)(unsigned short)((c->i64 >> 48) & 0xFFFF);
                char rect[48];
                snprintf(rect, sizeof(rect), "<%d,%d,%d,%d>", rx, ry, rw, rh);
                gst_util_set_object_arg(g_w.vsink_el, "render-rectangle", rect);
                pb2_logf("SET_RECT %s", rect);
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
