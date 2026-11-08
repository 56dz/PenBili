/*
 * session.h - player2 播放会话（worker 线程持有全部 GStreamer 对象）
 *
 * ============================ 线程模型 ============================
 *
 *   JSAPI 线程（QuickJS 单线程）          worker 线程（本模块）
 *   ------------------------------        ------------------------------
 *   open()    -> 拷参数进 req，push      <-- 取 OPEN，构建管线，preroll
 *   pause()   -> push PAUSE                 set_state(PAUSED)
 *   resume()  -> push RESUME                set_state(PLAYING)
 *   seek(ms)  -> push SEEK                  debounce + 重启源或 seek_simple
 *   status()  -> 只读原子量，不加锁         （每 100ms 刷新快照）
 *   stop()    -> push STOP                  清管线
 *   release() -> push RELEASE + 有界等待    退出循环
 *
 * 所有 GStreamer 调用都只在 worker 线程上发生。JS 线程碰不到任何 Gst 对象，
 * 这样就不需要任何锁来保护管线，也就不可能阻塞 JS 线程。
 *
 * ============================ seek debounce ============================
 * 实测（ADR §6.3）：背靠背 seek 会互相干扰，第一个 seek 的 firstsinkbuf 高达 2367ms。
 * 所以 worker 收到 SEEK 后不立刻执行，而是：
 *   1) 记下 target，置 seekPending=1；
 *   2) 继续取命令，把后续 SEEK 全部覆盖到 target（丢弃中间值）；
 *   3) 队列里没有更多 SEEK 且距最后一次 seek 已过 PB2_SEEK_DEBOUNCE_MS 才真正执行。
 *
 * 「算完成」的判据按源类型不同（**都不是**等 GST_MESSAGE_ASYNC_DONE ——
 * flush seek 之后 get_state() 立即返回 SUCCESS、管线根本不进 ASYNC 态，
 * GStreamer 不会发 ASYNC_DONE，等它只会每次等满 3s 超时，实测见 session.c 中
 * seek 完那段注释）：
 *   - 网络源（B 站 m4s）：重启 ffmpeg + 重建管线，判据是 preroll 成功；
 *   - 本地文件：seek_simple 后用 position 落到 clamped-1500 ~ clamped+3000 判定，
 *     并顺带 poll bus 捕获 ERROR。
 */
#ifndef PENBILI_PB2_SESSION_H
#define PENBILI_PB2_SESSION_H

#include "core/queue.h"

/* 一次 open 请求的完整参数（JS 线程拷好后交给 worker，worker 负责 free） */
typedef struct {
    char *input;      /* 主源：本地路径或 http(s) URL */
    char *input2;     /* 可选：第二路源（B 站 DASH 音频），空串表示没有 */
    char *userAgent;
    char *referer;
    char *audioDevice;
    long long startMs;
    long long durationMs;
    int fps;
    int fpsCap;
    int audio;        /* 1 = 要音频 */
    int transpose;    /* 1/2/3，2 = 270 度（竖屏笔） */
    int x;
    int y;
    int w;
    int h;
    /*
     * 【P0-3】显式直播意图：1 = 直播，0 = 点播，-1 = 调用方没传（老版本 JS）。
     *
     * 以前靠 is_live_request() 猜：「durationMs == 0 且是 http」就判直播。
     * 这个判据过宽 —— B 站 view 接口对互动视频/部分港澳台条目会返回
     * duration: 0，JS 侧 `parsed.durationMs || durationSec*1000` 就落 0，
     * 于是**点播被接成直播**：走 souphttpsrc ! flvdemux 而 DASH 分片不是 FLV
     * → flvdemux 建不出 pad → 无画面，且永久失去 seek（do_seek 直播早退）。
     * 现在 JS 显式传意图，native 只信它；-1 才回落到启发式并打 WARN。
     */
    int isLive;

    /*
     * 【X7 Pro 适配】设备相关的显示参数，由 JS 侧 device_profile.js 下发。
     *
     * X5/X6 Pro 时代这两个值写死在 native 里（面板列偏移 107、DRM plane 76），
     * 换到 X7 Pro（视口 936x280 / 面板带 280 宽）后这两个值都变了：
     *   - 列偏移 = (480 - 逻辑带高) / 2，X7 Pro 是 100 而不是 107；
     *   - kmssink 申请 plane 76 可能直接失败（该 id 在别的固件上属于 UI 平面
     *     或根本不存在），视频分支静默建不起来 → 黑屏。
     *
     * 语义（保证老版本 JS 不传时行为完全不变）：
     *   panelOffset < 0  → 用 X6 Pro 默认 107
     *   panelOffset >= 0 → 显式面板列偏移
     *   planeId == 0     → 用 X6 Pro 默认 76
     *   planeId < 0      → 不写 plane-id，交给 kmssink 自动挑空闲 overlay 平面
     *   planeId > 0      → 显式 plane id（真机取证后可用 storage 覆盖）
     */
    int panelOffset;
    int planeId;

    /*
     * 【X7 Pro 3.6.3】软件缩放开关（0/1）。
     *
     * 背景：kmssink 的 plane-id 是 DRM 的 plane **对象号**，由内核按 DT 分配，
     * 换固件就可能变。X6 Pro 上 76 = Esmart1-win0（带缩放器），显式指定它
     * 能让 1080p buffer 直接缩到 253x450；但 X7 Pro 真机上 76 对应的平面
     * 不可用 —— 视频分支建起来后 sink 抢不到平面，一帧 buffer 都不来，
     * 最终 set_state(PLAYING) 返回 FAILURE（真机日志见 docs/x7pro-adaptation.md §3.6）。
     *
     * 不猜平面号的稳妥做法：让 kmssink 自选（planeId=-1），并在解码后插
     * videoscale 把 buffer 直接缩到窗口大小。这样 sink 侧不需要任何缩放能力，
     * 挑到 Smart 平面也能 1:1 上屏，几何完全由 render-rectangle 决定。
     *
     * 代价是一次软件缩放（640x360 → 253x450 量级，RK3562 上可忽略）。
     * 语义：0/缺省 = 硬件缩放（保持 X6 Pro 现状）；1 = 软件缩放。
     */
    int swScale;

    /*
     * 【3.5.6 第四轮】videoscale 输出相对 render-rectangle 的尺寸百分比。
     *
     * 真机四档截图里 `out=253x450` 一模一样，画面始终只占挖洞区的一小块：
     * X7 Pro 自选的 overlay 平面很可能只做 **1:1 贴 buffer**，既不按
     * render-rectangle 缩放，也不理会矩形变大。这种情况下唯一能改变画面
     * 实际尺寸的旋钮是「解码后送进 sink 的 buffer 有多少像素」。
     *
     * 100 = 与矩形等大（原行为，X6 Pro 必须保持）；190 = 宽高等比放大到
     * 1.9 倍。只放大 buffer，不动 render-rectangle，所以矩形永远不会越出
     * 面板、kmssink 不会因为矩形非法而拒绝上屏。放大是等比的，不引入变形。
     * 缺省 100，范围由 native 钳到 50..400。
     */
    int bufScale;
} pb2_open_req_t;

void pb2_open_req_free(pb2_open_req_t *req);

/*
 * 启动 worker 线程并完成 gst_init（gst_init 在 worker 上做，避免 25~345ms
 * 的初始化阻塞 JS 线程）。
 * 返回 0 成功。重复调用是幂等的。
 */
int pb2_session_start(void);

/* 投递命令（全部非阻塞）。返回 0 成功。 */
int pb2_session_cmd(int type, long long i64, void *ptr);

/* 状态快照读取（无锁，可在 JS 线程调用） */
void pb2_session_stats(pb2_stats_t *out);
void pb2_session_probe(pb2_probe_t **out);

/*
 * 停止 worker 线程并释放全部资源。
 *
 * 【P0-2】**有界等待，不是 join**：先 quit 命令队列唤醒 worker，再用
 * PB2_SHUTDOWN_WAIT_MS（300ms）的 pthread_cond_timedwait 等 worker 自己
 * 宣告退出；超时则 pthread_detach 掉它并置 worker_detached=1，立刻返回。
 * 这样 release() 绝不可能被 worker 的阻塞调用（preroll 12s、seek 轮询 3s、
 * reap_pair、amixer spawn）拖住 JS 线程。
 * 被 detach 的 worker 仍可能在用 g_w，所以 pb2_session_start() 会先用
 * PB2_REOPEN_WAIT_MS（1500ms）等它真正退出再重置状态。
 * 返回值：0 成功。
 */
int pb2_session_shutdown(void);

/* gst 是否初始化成功 / registry 是否就绪 */
int pb2_session_gst_ready(void);

/* 【X7 Pro 适配】当前会话生效的面板列偏移 / kmssink plane-id（-1 = 自动） */
int pb2_session_panel_offset(void);
int pb2_session_plane_id(void);
/* 本次会话是否在用软件缩放（1/2 = 解码后 videoscale 到窗口大小） */
int pb2_session_sw_scale(void);
/* 本次会话 videoscale 输出倍率（100 = 与矩形等大） */
int pb2_session_buf_scale(void);
/* 1 = 本次会话因音频设备被占而主动降级为无声（画面正常） */
int pb2_session_audio_off(void);

/*
 * gst 版本字符串（如 "GStreamer 1.22.0"）。gst_init 之前调用返回 NULL。
 * 返回的指针指向 GStreamer 内部常量，**不要 free**。
 */
const char *pb2_session_gst_version(void);
/* 最后一次下发的 render-rectangle（"<x,y,w,h>"），排障用 */
const char *pb2_session_rect(void);

/* 已渲染帧数（原子读，status 用） */
unsigned long long pb2_session_frames(void);

/*
 * 距上一帧的毫秒数 —— 视频卡死看门狗的唯一输入。
 *
 * 【P0-1】永远返回 >= 0，**绝不返回 -1**。
 * 曾经在「一帧都还没到」时返回 -1，而 JS 侧是 `Number(st.videoStallMs) || 0`，
 * `Number(-1) || 0 === -1`（-1 为真值），于是 `-1 > 8000` 恒假 —— 无首帧的
 * 永久挂起永远不会被看门狗发现。现在无帧时返回「距本次 open/seek 起点
 * (frame_epoch_ms) 的毫秒数」，起播卡住也会随时间增长并最终触发超时。
 */
long long pb2_session_stall_ms(void);

/*
 * 音频健康（status().audioDead / audioHeals 的来源）。
 *
 * audio_dead = 「期望有声，但首帧后 4.5s 仍一个音频字节都没到，且已自愈过一次」。
 * 覆盖两类真机静默故障：DASH 音频 URL 偶发失效（ffmpeg 可选映射直接丢掉音轨）、
 * alsasink 打开失败。check_audio_stall() 会先做最多 2 次原地重建自愈。
 */
int pb2_session_audio_dead(void);
int pb2_session_audio_heals(void);

#endif /* PENBILI_PB2_SESSION_H */
