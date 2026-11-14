/*
 * session.c - player2 播放会话实现（worker 线程独占 GStreamer）
 *
 * 本文件是整个模块的核心。设计原则见 session.h 顶部注释，这里只补充实现细节。
 */
#include "session.h"

#include <errno.h>
#include <dlfcn.h>
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

/* ---------------- 面板几何（X6 Pro 默认值） ----------------
 * 【X7 Pro 适配】这两个宏只是"老版本 JS 不传设备参数时"的兜底默认值。
 * 实际值在 do_open() 里由 req->panelOffset / req->planeId 覆盖，并写进
 * g_w.panel_x_offset / g_w.kms_plane_id；所有建链与 SET_RECT 都读 g_w。
 * X7 Pro 的面板带是 280 宽（列偏移 100），且 plane 76 不一定可用，
 * device_profile.js 会下发 panelOffset=100 / planeId=-1（kmssink 自动选平面）。
 */
#define PB2_PANEL_X_OFFSET 107
#define PB2_KMS_PLANE_ID 76
#define PB2_PANEL_RATE "1497/25"
#define PB2_MAX_LATENESS 40
/*
 * ============ 【起播/seek 后迟到宽限】============
 *
 * 真机取证（GST_DEBUG=basesink:7，2026-11-01）：
 *   279 条 `basesink gstbasesink.c:4028:<vsink> buffer late, dropping`
 *   （另有 230 条 asink 的 `frame dropping disabled`，音频不丢，正常）
 * 时间集中在 running_time 0:00:04.905 和 0:00:08.843~08.937，
 * 后者 94ms 内连续丢 276 帧（≈9 秒内容）。
 *
 * 机制：sink 的 preroll 在音频分支就完成了，管线时钟随即起跑；
 * 而视频分支（mppvideodec 首帧 + queue 排空）要晚 900~2100ms 才出帧。
 * 这批帧的 PTS 相对已经跑起来的时钟全部「迟到 > 40ms」，
 * 被 max-lateness=40 + qos=true 成批丢弃 → 用户看到的是
 * 「起播不是从头开始，大概从第 1 秒开始」（用户反馈）。
 *
 * 修法：**起播和 seek 之后的一小段窗口内不判迟到**（max-lateness=-1,
 * qos=false），让积压的帧按 PTS 正常翻页；窗口过后再切回严格的 40/true。
 * 暂停恢复**不给宽限** —— 那是另一个已知问题（恢复瞬间 400 帧爆发），
 * 必须继续靠 40/true 丢积压，所以 RESUME 里会立刻取消宽限。
 *
 * 排查开关：/tmp/pb2_nolate.on 存在时永远不切回 40/true（A/B 对照）。
 * 宽限时长可用 /tmp/pb2_lategrace 覆盖（毫秒，默认 3000）。
 */
#define PB2_LATE_GRACE_MS 3000

/* ---------------- 时间常量（毫秒） ---------------- */
#define PB2_SEEK_DEBOUNCE_MS 250   /* 背靠背 seek 合并窗口（实测需 200~300） */
#define PB2_BUS_TICK_MS 100        /* bus 轮询周期 */
#define PB2_PLANE_TICK_MS 2000     /* 平面检查周期 */
#define PB2_MARKER_TICK_MS 20000   /* /tmp/mp3playing 保活重写周期 */
#define PB2_SEEK_ASYNC_TIMEOUT_MS 3000
#define PB2_PREROLL_TIMEOUT_MS 12000
/*
 * 起播缓冲门超时（见 do_open 里的长注释）。
 * 4s 足够覆盖实测最慢的 2100ms 首帧，超时就退化放行，绝不让用户卡住。
 */
/*
 * 起播缓冲门的超时。蓝牙时单独放宽到 6s：bluealsa 打开 PCM 要额外
 * 走一次 D-Bus + 编码器初始化，实测 BT 的 gate 耗时 1.7~2.5s，比扬声器
 * 的 1.7~2.4s 只多一点，但边缘情况（耳机刚唤醒/编码器重配）会明显更久。
 * 门一旦超时就会「不等音频直接开播」，而音频分支稍后起来会把时钟起点
 * 拉到它自己那里，视频前几秒全被判迟到丢弃 —— 用户看到的就是
 * 「起播直接卡到第 3 秒」。所以蓝牙多给 2s 余量，宁可多等也不跳帧。
 */
#define PB2_START_GATE_TIMEOUT_MS 4000
#define PB2_START_GATE_TIMEOUT_BT_MS 6000
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
/*
 * 【首次 seek 必失败】点播视频队列改成按 buffer 个数限界，不用 max-size-time。
 * 见 on_pad_added 里的长注释：queue 的 max-size-time 是**时间戳跨度**语义，
 * seek 后 PTS 跨度跳变会让队列瞬间超限、反压 demuxer、堵死视频。
 * 240 个 buffer ≈ 8s @30fps，覆盖 demuxer 推进落后一路所需的 T-K（最大 3.843s）。
 */
#define PB2_VIDEO_QUEUE_BUFS 240
#define PB2_VIDEO_QUEUE_NS 8000000000LL
#define PB2_VIDEO_QUEUE_BYTES 6000000

/*
 * 【P1-2 直播用更小的视频队列】
 *
 * 上面那组 8s/6MB 是给**点播 seek** 定的：matroskademux 在流式模式下
 * 必须把落后的一路「推进」到对齐点（真机 T-K=3.843s），按 buffer 个数
 * 限界（8 个 ≈267ms）装不下这段推进量就会永久死锁，所以给到 8s 的余量。
 *
 * 直播（souphttpsrc ! flvdemux）没有这个问题：FLV 是单流实时源，音视频
 * tag 天然交织，demuxer 不做「推进落后一路」这件事。恰恰相反 —— 队列
 * 上限就是**播放延迟的下限**：上限 8s 意味着最坏情况下内存里压着 8 秒
 * 视频，画面恒定落后直播边缘最多 8s（用户感知就是「直播比弹幕慢好几秒」），
 * 而且弱网时积压的 8s 数据会在恢复瞬间一次性灌给解码器。
 *
 * 直播改用 2s/3MB：
 *   - 2s 足够吸收正常网络抖动（B 站直播 GOP 1~2s，一个 GOP 一定能装下）；
 *   - 又不至于把延迟堆到不可接受。实测直播 90s 长跑 vid_fps=30.08、
 *     big_gaps=0，与 8s 无差别，但延迟上限从 8s 降到 2s。
 *
 * 万一将来在某些直播源上出现「队列满 → soup 背压 → 卡顿」，把这个值
 * 调大即可 —— 它只是延迟/抗抖动之间的取捨，不涉及正确性。
 */
/*
 * 【直播缓冲】从 2s 提到 4s。
 *
 * 用户反馈直播每 2~5 秒闪一次屏，怀疑是数据包传输不稳定 —— 方向对。
 * 2s 的队列在 B 站直播（GOP 1~2s + CDN 抖动）下余量太薄：一次稍长的
 * 抖动就把队列抽干，souphttpsrc 背压 → 恢复时一批帧集中涌入 → 要么被
 * max-lateness 丢掉（闪黑），要么积压。4s 能吸收两倍抖动，延迟代价
 * 可接受（直播本来就比现场慢）。
 */
#define PB2_LIVE_VIDEO_QUEUE_NS 4000000000LL
#define PB2_LIVE_VIDEO_QUEUE_BYTES 3000000

/*
 * 【蓝牙耳机】ALSA 缓冲：**保持 30ms/10ms，与扬声器一致**。
 *
 * 曾经试过放大到 200ms/40ms（以为 A2DP 需要更大抖动缓冲），真机实测
 * 反而更差：20 秒平面采样 vid_fps 从 29.95 掉到 **19.70**，帧间隔稳定
 * 47ms（≈21fps）—— 说明 alsasink 的 buffer-time 不只是「抖动缓冲」，
 * 它同时决定 sink 报给 pipeline 的 latency，进而影响时钟与 videorate
 * 的节奏。放大 buffer 等于把整条管线的时钟拖慢，视频跟着降帧。
 *
 * 所以蓝牙也维持 30ms。真正的卡顿根因不在缓冲大小（见下面 ts-offset
 * 与直播队列两处注释）。保留这两个宏只是为了让「蓝牙要不要不同缓冲」
 * 这个决定有据可查，将来若真遇到 underrun 可以改这里做 A/B。
 */
#define PB2_BT_AUDIO_BUFFER_US 30000LL
#define PB2_BT_AUDIO_LATENCY_US 10000LL

/*
 * 【蓝牙耳机】音画偏移补偿：把**视频提前**多少毫秒。
 *
 * 用户实测：连蓝牙耳机后音频比视频快约 150ms，要求压到 100ms 以内。
 *
 * 机制：A2DP 的端到端延迟（bluealsa 编码 + 无线 + 耳机解码缓冲）约
 * 150~250ms，这段延迟发生在 bluealsa 之后，ALSA/GStreamer 完全看不见。
 * pipeline 时钟认为「内容 T 的声音在 T 时刻已播出」，实际要到 T+L 才
 * 到耳朵；视频按这个时钟渲染，于是声音先到、画面后到。
 *
 * 补偿必须落在 **kmssink**（不提供时钟的那一端），用负 ts-offset 让
 * 视频时间戳整体提前 L，画面就早 L 渲染，与晚到 L 的声音对齐。
 *
 * 【为什么不能放 alsasink】alsasink 是 pipeline 的时钟提供者
 * （provide-clock 默认 true，实测不能关，见 spike-report：关掉后视频
 * 飙到 88~119fps）。给时钟提供者自己的输入加 ts-offset，等于把整条
 * 管线的时间轴一起平移 —— 视频是 slave 跟着这个时钟走的，相对关系
 * 完全不变。真机实测证实：alsasink ts-offset 0/150/300 三档下
 * `av: avSkewMs` 毫无变化（-1494/-1786 同一量级），且 pauseProbe 的
 * 恢复后帧率从 29.94fps 飙到 99.30fps（时钟被推快，视频疯狂追帧）。
 *
 * 默认 150ms。真机标定：echo <ms> > /tmp/pb2_avoffset 后重开播放生效，
 * 正值 = 视频提前更多（画面显得更早）。若调大反而更差就写负数。
 */
#define PB2_BT_VIDEO_TS_OFFSET_MS 150

/*
 * 【P1-4】直播「可恢复断流」的错误码。
 *
 * 直播是长连接：CDN 掐流、WiFi 抖动、B 站边缘节点切换都会让 souphttpsrc
 * 报 bus error，但流本身十几秒后还能连回来。以前这种错误一律走
 * take_error() → PB2_E_PIPELINE + PB2_STATE_ERROR，JS 侧一看到 state=error
 * 就判起播失败、停轮询、弹「播放失败」，用户必须退出重进。
 *
 * 现在改用独立的 17：**patch_error 但不改 state**（保持 PLAYING），
 * JS 收到 17 时只显示「正在重连…」而不会拆掉播放页；重连成功后再把
 * lastError 清零。
 *
 * 17 这个数值是与 js2 约定死的（他那边 `LIVE_RECONNECT_ERR = 17`），
 * 语义必须严格对齐：17 只表示「直播正在自动重连，不要放弃」。
 * core/queue.h 的枚举不在本任务写域，所以常量定义在这里（见下），
 * 数值与 core 里 0..16 不冲突（PB2_E_BRANCH_LINK 16 是最后一个）。
 */
#define PB2_E_LIVE_RECONNECT 17

/* 直播连续这么久没有视频帧，就判定为断流并触发重连 */
#define PB2_LIVE_STALL_MS 6000
/* 最多连续重连这么多次，之后才认输报 error */
#define PB2_LIVE_RECONNECT_MAX 5

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
    int vsink_el_owned;          /* 1 = vsink_el 是我们拿到的新引用，teardown 必须 unref */
    /*
     * 平面是否被 SET_RECT 主动隐藏（键盘避让期间 1x1）。
     * check_plane() 必须据此跳过巡检：隐藏时 kmssink 把平面缩成 1x1，
     * 几何上看仍在 ACTIVE，但某些帧序下会短暂读到非 ACTIVE，
     * 于是每秒误报一次 `plane missing ... streak=N -- reported` 并
     * patch 出 PB2_E_PLANE_LOST，让 JS 侧弹「画面丢失」的假错误。
     * 隐藏是**我们自己主动做的**，不是故障，所以巡检要整个跳过。
     */
    int rect_hidden;
    /*
     * 【起播迟到宽限】宽限截止时刻（墙钟 ms）。> 0 表示仍处于宽限窗口，
     * worker tick 到点后会把 kmssink 的 max-lateness 从 -1 切回 40 并
     * qos 从 false 切回 true，然后清零。见 PB2_LATE_GRACE_MS 的长注释。
     */
    long long late_grace_until_ms;
    /*
     * 1 = 本次「等首帧」窗口允许迟到宽限（只有 OPEN 会置 1）。
     * seek 之后 frame_epoch_ms 同样 > 0，但**不能**给宽限：实测给宽限会把
     * seek 首帧从 516ms 恶化到 2480ms（sink 把 K..T 的积压逐帧播完才到 T）。
     * 所以用这个标志把「OPEN 等首帧」和「seek 等首帧」区分开。
     */
    int late_grace_armed;
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
    /*
     * 【P1-8】暂停期间检测到路由变化时置 1。暂停态不重建管线（重建会把
     * 停住的那一帧重新刷出来，用户会看到「暂停着却动了」），但也不能像
     * 以前那样只把 vol_check_ms 清零碰运气 —— 那样 resume 之后要等下一次
     * tick 撞上蓝牙探测窗口（以前 5s 节流，最坏再等 5s）才生效。
     * 现在 resume 后由 refresh_audio_route 第一段直接消费这个标记。
     */
    int route_pending;
    /*
     * 【P1-4】直播断流重连状态机。
     *   live_reconnect_pending：已排期一次重连，等 live_reconnect_at_ms 到点执行
     *   live_reconnect_count  ：本轮连续失败次数（成功即清 0）
     *   live_reconnect_at_ms  ：下一次重连的时间点（退避用）
     */
    int live_reconnect_pending;
    int live_reconnect_count;
    long long live_reconnect_at_ms;
    int bt_present;         /* 上次探测时蓝牙是否可用（用于检测插拔） */
    int bt_vol_applied;     /* 已下发给耳机的 AVRCP 音量（-1 = 未下发） */
    /*
     * 上一次采样到的 card1 Master（系统滑杆 / 扬声器功放音量）。-1 = 尚未采样。
     *
     * 【为什么需要它】蓝牙路由下的音量权威必须是**耳机自己的 AVRCP**：
     * 用户在耳机上调音量、或者上次用扬声器时把 Master 留成 0，都会让
     * Master 和 AVRCP 长期不一致（真机抓到 Master=0% / AVRCP=50%）。
     * 如果起播时拿 Master 去覆盖耳机，就会「点开视频音量突然变 0%」。
     * 所以只有当 Master **相对上次采样发生了变化**（= 用户真的动了系统
     * 滑杆）时，才把 Master 推给耳机；否则一律以 AVRCP 为准。
     */
    int master_last;
    char bt_ctl_name[64];   /* 耳机的 mixer 控件名，形如 "Redmi Buds 6 - A2DP" */
    /* 蓝牙音画偏移补偿（ns，正值 = 视频提前），见 PB2_BT_VIDEO_TS_OFFSET_MS */
    long long bt_ts_offset_ns;
    GstBus *bus;
    int vbranch;
    int abranch;
    int has_audio;          /* 是否真的建了音频分支 */
    int pipeline_alive;

    /* 媒体源（本地直读 / ffmpeg -> FIFO / 直播直连 GStreamer） */
    pb2_src_t src;
    int src_ready;
    long long base_ms;      /* 对 JS 报告位置 = base_ms + query_position() */
    /*
     * 【直播直连】1 = 本次会话走 souphttpsrc ! flvdemux，**没有 ffmpeg、没有 FIFO**。
     *
     * 为什么必须绕开 ffmpeg（真机实测，不可重复验证的结论）：
     *   B 站直播 FLV 的 PTS 是绝对值（实测 ≈2854014 秒，ffprobe 报
     *   `start: 2853204.901`、`ClusterTimeCode: 2854183333`）。ffmpeg 转封装
     *   写 FIFO 后，流式 matroskademux 面对「首 PTS 巨大」的流会永久停在
     *   PREROLLING（实测 rc=124 超时、buffers=0）；`-copyts` 与否、
     *   `-f mpegts`+tsdemux、`-re` 慢喂，全部一样卡。而 ffmpeg 先落到**普通
     *   文件**再 filesrc 读就正常 —— 说明是「FIFO 上的流式 demux 无法处理
     *   巨大起始 PTS」，不是 URL 或网络问题。
     *   点播 m4s 的 PTS 从 0 附近开始，所以点播完全正常，这就是分界线。
     *   实测直连 `souphttpsrc ! flvdemux` 双分支 45 秒 3018 帧 / 69.4fps / 0 drop。
     *
     * 带来的语义差异（都已在下面各函数里按 is_live 分支处理）：
     *   - 没有 pb2_src_open()，src_ready 恒 0，所有「源存活探测 / 自愈重试」自动跳过。
     *   - base_ms 恒 0；位置改用「本次会话已播时长」，因为 FLV 的绝对 PTS 不能直接报给 JS。
     *   - durationMs 恒 0（JS 侧 isStreamEnded() 对 durationMs<=0 恒返回 false）。
     *   - seek 无意义，直接忽略。
     */
    int is_live;

    /* 当前会话参数（OPEN 时写入） */
    pb2_open_req_t *req;
    int rotate;
    char rect[64];
    /* 设备自适应：面板列偏移与 kmssink plane-id（见 session.h 的字段注释） */
    int panel_x_offset;
    int kms_plane_id;
    /* 显式 plane-id 抢不到平面时，是否已经退回到「让 kmssink 自选」（每会话一次） */
    int plane_fallback_done;
    /* 1 = 解码后插 videoscale，把 buffer 缩到窗口大小（平面无需缩放能力） */
    int sw_scale;
    /* videoscale 输出相对 rect 的百分比（100 = 等大）；见 session.h */
    int buf_scale;
    /* 全屏 cover 的裁切量（物理像素，左右/上下各裁这么多）；见 session.h */
    int crop_x;
    int crop_y;
    /* 【第二十九轮 v6】首帧时打一次 sink 实际协商到的 caps。
     * 排查绿边只需要一行：buffer 尺寸与 render-rect 是否相等。
     * 每次 OPEN 清零。 */
    int vcaps_logged;
    /*
     * 音频设备被占（alsasink 打不开）时的降级状态：
     *   audio_off        = 本会话**主动**关了音频（不是片源本身没音轨）
     *   audio_retry_stage = 0 未重试 / 1 已带音频重试一次 / 2 已降级为无声
     * 见 open_audio_retry()。
     */
    int audio_off;
    int audio_retry_stage;
    /* 音频兜底需要重开一次（与 want_plane_retry 同机制，但带延时） */
    int want_audio_retry;
    int reopen_delay_ms;
    /*
     * OPEN 阶段就失败（抢不到平面 / set_state(PLAYING) FAILURE）时的重试意图。
     * 由 do_open 置位、process_cmd 读取：进程内**不能**直接递归 do_open，
     * 因为它开头会 free(g_w.req) —— 那正是我们要复用的请求对象。
     */
    int want_plane_retry;

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
    int vsrc_logged;             /* 诊断：demuxer 视频 src pad 首 buffer 只打一次 */
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
    int seek_frame_retries;      /* seek 后首帧不出的自愈重试次数（每次 seek 限 1 次） */
    int seek_retry_issued;      /* 1 = 当前这次 seek 是自愈重试发出的（do_seek 不得再清零计数器） */
    long long vbranch_ready_ms;  /* 视频分支建好的时刻（自愈重试计时起点） */

    /*
     * 【P1-3】seek 时「先起新源、再拆旧管线」，旧源的两个 ffmpeg 与 FIFO 先挂在这，
     * 由 worker 空闲时回收，不占 seek 的关键路径。
     */
    pb2_src_t dying_src;
    int dying_pending;

    /* 计数 */
    long long t_start;          /* 本次会话开始时间 */
    /*
     * 【P0-2】直播位置基准的暂停冻结。
     *
     * 直播的 positionMs 是「本次会话已播时长」= now - t_start（直播 PTS 是
     * 绝对值 ≈792 小时，不能直接报给 JS）。但 t_start 只在 do_open 赋值一次，
     * PAUSE/RESUME 都不碰它 → 暂停 10 分钟再恢复，positionMs 直接跳
     * +600000ms，进度条/计时完全失真，且违反「单一权威时钟」。
     *
     * 修法：暂停时记下 pause_started_ms；恢复时把 t_start 往后推同样的时长，
     * 等价于「暂停期间时钟不走」。live_rebuild_pipeline 里补回位置时也要用
     * 同一个基准，否则重建会掩盖掉错误值。
     */
    long long pause_started_ms;
    long long last_pos_ms;
    unsigned long long last_abytes;
    /* 【音频停摆看门狗】见 check_audio_stall() */
    unsigned long long audio_stall_ab;      /* 上次看到的 abytes */
    long long audio_stall_ms;                /* abytes 最后一次变化的时刻 */
    unsigned long long audio_stall_frames0;  /* abytes 最后一次变化时的视频帧数 */
    int audio_stall_heals;                   /* 本会话已自愈次数 */
    /*
     * 【无声自愈】见 check_audio_stall() 的 ① ②。
     *   first_frame_ms：本会话首帧上屏的时刻（frame_epoch_ms 清零那一刻写入）。
     *                   「多久没声音」只能从这里算 —— frame_epoch_ms 归零后就没有
     *                   "起算点"了，用 t_start 会把 seek/重建的时间也算进去。
     *   audio_heals    ：为「有音轨却完全没声」重建管线的次数（每会话上限 2）。
     */
    long long first_frame_ms;
    int audio_heals;
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

/*
 * 【音频停摆看门狗】阈值与配额。
 *
 * 8 秒：正常播放时 abytes 每秒涨 ~16KB（128kbps AAC），8 秒不动已经远超
 * 任何正常的调度抖动；而一次误判的代价是「原地 seek」——本身是可接受的
 * 轻微卡顿，所以宁可早一点救。实测故障时音频停了 45 秒都没人管。
 */
#define PB2_AUDIO_STALL_MS     8000
#define PB2_AUDIO_STALL_FRAMES 30   /* 同期视频至少涨这么多帧才算「只有音频停」 */
#define PB2_AUDIO_STALL_HEALS  2    /* 每会话最多自愈次数 */

/*
 * 【沉默自愈】「有音轨却完全没声音」的两条判据与配额。
 *
 * 用户反馈（2026-11-07）：「有概率没有声音」。真机上音频链有两条静默失败路径，
 * 都不会报错、视频照播：
 *   ① ffmpeg 的 `-map 1:a?`（第二路 DASH 音频）是**可选映射**：audio URL 403 /
 *      域名抽风 / 首字节超时，ffmpeg 不报错、直接只输出视频轨 → matroskademux
 *      永远不 emit audio pad → g_w.abranch 恒 0（hasAudio=0）。
 *   ② audio pad 到了、分支也建起来了，但 alsasink 打不开（device 被别的进程占着、
 *      bluealsa PCM 还没就绪）→ 分支报错后自毁，abytes 恒 0。
 * 两种都是「重开一次就好」的瞬时故障，所以这里做有限次数的原地重建（走 seek 路径，
 * 位置不变），而不是把无声状态一直甩给用户。
 *
 * 时间窗 4.5s / 24 帧：起播门已经等过 branch（最多 15s），首帧之后 4.5 秒
 * （≈24 帧 @30fps 的余量）还没有任何音频数据，就不可能是"正常但慢"了。
 */
#define PB2_AUDIO_START_GRACE_MS 4500
#define PB2_AUDIO_START_FRAMES   24
#define PB2_AUDIO_HEALS          2

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

/*
 * 深拷贝一份 open 请求（平铺字段 + 字符串）。
 *
 * 【为什么需要】OPEN 失败要重试一次（平面兜底），但 do_open 的第一件事之一
 * 就是 `pb2_open_req_free(g_w.req)` —— 那正是调用方传进来的同一个对象。
 * 直接 do_open(req) 第二次会在第一句把 req 释放掉，随后读到已释放内存。
 * 所以在重试前先复制一份：新对象交给 do_open，旧对象由它按原逻辑回收。
 */
static pb2_open_req_t *open_req_dup(const pb2_open_req_t *src) {
    pb2_open_req_t *r;
    if (!src) return NULL;
    r = (pb2_open_req_t *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    *r = *src;
    r->input = src->input ? strdup(src->input) : NULL;
    r->input2 = src->input2 ? strdup(src->input2) : NULL;
    r->userAgent = src->userAgent ? strdup(src->userAgent) : NULL;
    r->referer = src->referer ? strdup(src->referer) : NULL;
    r->audioDevice = src->audioDevice ? strdup(src->audioDevice) : NULL;
    return r;
}

static int is_http(const char *s) {
    return s && (strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0);
}

static int rotation_for_transpose(int t) {
    if (t == 1) return 90;
    if (t == 3) return 180;
    return 270; /* t == 2，JS 恒传 */
}

/*
 * 【3.5.6 第十八轮：为什么 rotation 仍然交给 decoder】
 *
 * 真机反馈「视频被压成一条、周围全是绿幕」，并给出关键对照：
 * 自检页的 A/B/C/D 四档里 **B 和 D 正常，A 和 C 是绿幕**。
 * 四档的差别只有 swScale：A/C = 1（横帧，videoscale 输出 rect 互换后的
 * 448x252），B/D = 2（竖帧，videoscale 输出 rect 本身 252x448）。
 *
 * 结论：这台机器的 mppvideodec **真的会把 buffer 转成竖帧**，
 * 所以 swScale=1 那条「互换」路径等于把已经转正的竖帧再按横帧缩一次，
 * kmssink 拿到 448x252 的横 buffer 去贴 252x448 的竖窗口 —— 只有一部分行
 * 被写入，其余是 plane 未初始化内存（YUV 全 0 = 正是用户看到的绿）。
 *
 * 因此：
 *   · 默认档改成 swScale=2（B 档，竖帧），这是真机验证过能用的那条；
 *   · 这里不再把 rotation 置 0、也不插 videoflip —— 曾尝试用显式
 *     videoflip 替代 decoder 的旋转意图，但 B 档已经证明现有链路是对的，
 *     贸然改管线只会让 A 档从"绿幕"变成"黑屏"（videoflip 缺插件时
 *     parse_bin 直接失败），得不偿失。
 *   · swScale=0（X6 Pro）照旧：rotation 交给 mppvideodec + kmssink 硬件旋转。
 */
static int decoder_rotation(void) {
    return g_w.rotate;
}

/*
 * 【X7 Pro 适配】kmssink 的 plane-id 片段。
 *
 * X6 Pro 传 "plane-id=76 "，X7 Pro 传空串（planeId=-1）：不写这个属性时
 * kmssink 会自己从 CRTC 的空闲 overlay plane 里挑一个，不会因为写死的 id
 * 在该机型上不存在而让整条视频分支 parse_bin 失败 / 抢不到平面 → 黑屏。
 *
 * 返回值指向 worker 独占的静态缓冲（g_w 只在 worker 线程访问），调用方
 * 必须在同一次 desc 拼接里用完，不要跨 tick 保存。
 */
static const char *kms_plane_arg(void) {
    static char arg[48];
    if (g_w.kms_plane_id < 0) {
        arg[0] = '\0';
    } else {
        snprintf(arg, sizeof(arg), "plane-id=%d ", g_w.kms_plane_id);
    }
    return arg;
}

/*
 * 把字符串里会破坏 gst desc 语法的字符转义（用于 location= 引号内）。
 *
 * 返回值：【P1-5】**0 = 完整转换；-1 = 被 cap 截断了**。
 *
 * 为什么必须返回截断信号：以前这个函数是 void，循环条件 `o + 2 < cap`
 * 悄悄停手，`out[o] = 0` 之后调用方完全看不出少了一半。直播 URL 带
 * expires/sign/token 等参数，实测 600~900 字符，jsapi 允许 input 最长
 * 1024；一旦 URL 里出现需要转义的 `"` 或反斜杠（CDN 签名偶尔会有），
 * 长度会膨胀并被切断 —— 症状是 souphttpsrc 拿半截 location 去请求，
 * CDN 回 403/404，用户看到「直播永远转圈」，而日志里只有 soup 的错误，
 * 完全看不出是 URL 被截断。调用方必须拿到这个信号并 patch_error。
 */
static int desc_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    size_t i;
    if (!in || cap == 0) {
        if (cap) out[0] = '\0';
        return -1;
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
    /* in[i] 还有内容 => 循环是被 cap 卡停的，不是正常走完 */
    return in[i] ? -1 : 0;
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

/*
 * 【3.5.6 排障】读 demuxer pad 上的**编码尺寸**（SPS 解析后的 width/height）。
 * 只读，不参与任何判断 —— 目的是让日志页能直接看到「源帧到底是横还是竖」，
 * 不用再靠用户描述画面有没有被压扁。
 */
static void pad_caps_size(GstPad *pad, int *w_out, int *h_out) {
    GstCaps *caps = gst_pad_get_current_caps(pad);
    GstStructure *st;
    int w = 0, h = 0;
    if (w_out) *w_out = 0;
    if (h_out) *h_out = 0;
    if (!caps) caps = gst_pad_query_caps(pad, NULL);
    if (!caps) return;
    if (gst_caps_get_size(caps) >= 1) {
        st = gst_caps_get_structure(caps, 0);
        if (st) {
            if (!gst_structure_get_int(st, "width", &w)) w = 0;
            if (!gst_structure_get_int(st, "height", &h)) h = 0;
        }
    }
    gst_caps_unref(caps);
    if (w_out) *w_out = w;
    if (h_out) *h_out = h;
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
static int on_video_src_probe(GstPad *pad, GstPadProbeInfo *info, void *ud);
static int on_audio_buffer(GstPad *pad, GstPadProbeInfo *info, void *ud);
static int on_audio_list(GstPad *pad, GstPadProbeInfo *info, void *ud);
/* 起播/seek 后的迟到宽限（实现见 check_plane 之前） */
static void arm_late_grace(void);

/*
 * 读取蓝牙音画偏移覆盖值（毫秒，可负）。
 * 真机标定用：echo 150 > /tmp/pb2_avoffset 后重启播放生效。
 * 无文件时用编译期默认 PB2_BT_TS_OFFSET_MS。
 */
static long long read_bt_ts_offset_ns(void) {
    FILE *fp = fopen("/tmp/pb2_avoffset", "r");
    long v = 0;
    if (fp) {
        if (fscanf(fp, "%ld", &v) != 1) v = 0;
        fclose(fp);
        if (v < -2000) v = -2000;
        if (v > 2000) v = 2000;
        pb2_logf("audio: bt ts-offset from /tmp/pb2_avoffset = %ldms", v);
        return (long long)v * 1000000LL;
    }
    return (long long)PB2_BT_VIDEO_TS_OFFSET_MS * 1000000LL;
}
static void apply_volume_now(void);
/*
 * 【直播】refresh_audio_route() 在 teardown/build 之前就定义了，
 * 而它的直播分支要直接重建管线（不能走 seek 路径），所以这里补前向声明。
 */
static void teardown_pipeline(void);
static int build_pipeline(pb2_open_req_t *req, int src_already_open);
static void apply_bt_avrcp_volume(void);

/*
 * 【直播】重建直播直连管线的唯一入口。
 *
 * 直播没有 ffmpeg、没有 FIFO、没有「位置」概念，所以所有「需要重建管线」
 * 的场景（蓝牙/耳机路由变更、键盘收起后恢复视频矩形）都不能走 do_seek()
 * ——那条路径会先 pb2_src_open(target_ms) 起 ffmpeg 拉 FLV，直播下必然
 * 卡在流式 demuxer 的永久 PREROLLING（见 pb2_worker_t.is_live 注释）。
 *
 * 这里直接 teardown + build_pipeline(req, 0)，build_pipeline 内部会重新
 * 走 is_live 分支（souphttpsrc ! flvdemux），并带上 g_w.rect / g_w.adev
 * 的当前值，所以「换 device」和「换矩形」都能生效。
 *
 * 代价：直播要重新拉一次流（约 0.5~1.5s）。这两类事件都是低频的
 * （插拔耳机、收起键盘），可以接受。
 *
 * 只能在 worker 线程调用（会 set_state(NULL) / 释放 GstElement）。
 */
/*
 * 【P0-2】直播位置的唯一计算入口。
 *
 * 直播没有「总时长」，也拿不到可信的媒体时钟（FLV 的 PTS 是自 1970 起的
 * 绝对值 ≈792 小时，直接报给 JS 会让进度条/计时爆掉）。所以位置用
 * 「本次会话已播时长」这个墙钟量。
 *
 * 但墙钟必须在暂停期间冻结，否则暂停 10 分钟恢复后 positionMs 跳 +600000ms：
 *   - 暂停中：位置停在 pause_started_ms - t_start
 *   - 播放中：位置 = now - t_start
 * RESUME 时把 t_start 往后推「暂停时长」，所以这两个式子共用同一个 t_start。
 *
 * 所有需要直播位置的地方（refresh_position / live_rebuild_pipeline）都必须
 * 走这里，避免两处各算一遍导致口径分叉。
 */
static long long live_position_ms(void) {
    long long base = g_w.paused && g_w.pause_started_ms > 0
                         ? g_w.pause_started_ms
                         : pb2_now_ms();
    long long pos = base - g_w.t_start;
    return pos > 0 ? pos : 0;
}

/*
 * 【P1-4】重建失败的统一出口。
 *
 * 直播重连期间的失败**不能**报 ERROR：js2 的 readStatus() 看到 state=error
 * 就判起播失败、停轮询、弹「播放失败」，用户只能退出重进 —— 而直播断流
 * 本来就是「等几秒自己会好」的场景。
 *
 * 所以配额内（live_reconnect_count 在 1..PB2_LIVE_RECONNECT_MAX）只刷新
 * 17 的文案、state 保持不动；排下一次重连由 check_live_reconnect() 的
 * !pipeline_alive 分支统一处理（唯一排期点，不会与这里叠加）。
 *
 * 超出配额 / 非直播才走正常的 ERROR 上报。
 */
static void live_rebuild_failed(int code, const char *msg) {
    if (g_w.is_live && g_w.live_reconnect_count > 0 &&
        g_w.live_reconnect_count <= PB2_LIVE_RECONNECT_MAX) {
        pb2_logf("live: rebuild failed while reconnecting (%s) -> keep state, will retry",
                 msg ? msg : "?");
        pb2_stats_patch_error(&g_w.stats, PB2_E_LIVE_RECONNECT,
                              "live reconnect: rebuild failed");
        teardown_pipeline();
        return;
    }
    pb2_stats_patch_error(&g_w.stats, code, msg);
    pb2_stats_patch_state(&g_w.stats, PB2_STATE_ERROR);
    teardown_pipeline();
}

static void live_rebuild_pipeline(const char *why) {
    long long t0 = pb2_now_ms();
    /*
     * 【P0-2 配套】teardown_pipeline() 会把 g_w.paused 清 0。如果直播是在
     * 暂停态被重建（蓝牙路由变更 / SET_RECT 恢复 / recover），清掉 paused
     * 会让 live_position_ms() 改用 now，位置立刻跳掉「已暂停的时长」。
     * 所以这里先快照暂停状态与位置，重建完再原样还回去。
     */
    int was_paused = g_w.paused;
    long long saved_pause_started = g_w.pause_started_ms;
    long long pos_before = live_position_ms();

    if (!g_w.req) {
        pb2_logf("live: rebuild(%s) skipped -> no req", why ? why : "?");
        return;
    }
    pb2_logf("live: rebuild(%s) dev=%s rect=%s", why ? why : "?",
             g_w.adev[0] ? g_w.adev : "speaker", g_w.rect);
    teardown_pipeline();
    if (build_pipeline(g_w.req, 0) == 0) {
        /*
         * 【P1-2】必须判 set_state 的返回值并等 preroll，与 do_open 里
         * （见那边的【P0-1】注释）保持一致。
         *
         * 以前这里直接 `gst_element_set_state(PLAYING)` 丢掉返回值，紧接着
         * 就 patch_error(NONE) + patch_state(PLAYING)。如果直播 URL 已过期
         * （B 站 deadline 约 2h）、CDN 403/404、或者 sink 分支根本建不起来，
         * JS 会看到「正在播放」而画面永远不来，且 lastError 是 0 —— 只能靠
         * 用户手动返回重进。
         *
         * 注意直播管线在分支建起来之前没有 sink，set_state 通常直接 SUCCESS；
         * 但 CDN 有问题时 souphttpsrc 会先失败，这里就靠返回值/Failure 兜住。
         */
        GstState cur = GST_STATE_NULL;
        GstState pend = GST_STATE_NULL;
        GstStateChangeReturn sr = gst_element_set_state(g_w.pipe, GST_STATE_PLAYING);
        g_w.late_grace_until_ms = 0; /* 直播重建后要快速追上直播点，不丢帧反而拖慢 */
        g_w.late_grace_armed = 0;
        pb2_logf("live: rebuild(%s) set_state(PLAYING) -> %d (+%lldms)", why ? why : "?", sr,
                 pb2_now_ms() - t0);
        if (sr == GST_STATE_CHANGE_FAILURE) {
            pb2_logf("live: rebuild(%s) FAILED -> error (set_state returned FAILURE)",
                     why ? why : "?");
            live_rebuild_failed(PB2_E_OPEN_FAILED, "live rebuild: set_state failed");
            return;
        }
        if (sr == GST_STATE_CHANGE_ASYNC) {
            sr = gst_element_get_state(g_w.pipe, &cur, &pend,
                                       (GstClockTime)PB2_PREROLL_TIMEOUT_MS * 1000000ULL);
            pb2_logf("live: rebuild(%s) preroll -> %d cur=%d pend=%d (+%lldms)",
                     why ? why : "?", sr, cur, pend, pb2_now_ms() - t0);
            if (sr != GST_STATE_CHANGE_SUCCESS) {
                pb2_logf("live: rebuild(%s) FAILED -> error (preroll %s)", why ? why : "?",
                         sr == GST_STATE_CHANGE_ASYNC ? "timeout" : "failure");
                live_rebuild_failed(PB2_E_TIMEOUT, "live rebuild: preroll timeout");
                return;
            }
        }
        /*
         * 与 do_open 对齐：重新起算首帧/首包基准，否则
         * first frame on screen 的耗时会把上一次会话的时间也算进去。
         */
        g_w.frame_epoch_ms = pb2_now_ms();
        g_w.audio_epoch_ms = g_w.frame_epoch_ms;
        g_w.first_frame_logged = 0;
        g_w.first_audio_logged = 0;
        g_w.plane_lost_streak = 0;
        g_w.plane_grace_until_ms = pb2_now_ms() + PB2_PLANE_GRACE_MS;
        pb2_probe_reset(&g_w.probe);
        g_w.seek_frames_base = 0;
        g_w.avskew_last_log_ms = 0;
        pb2_stats_patch_error(&g_w.stats, PB2_E_NONE, "");
        /*
         * 位置不能在重建后掉回 0：teardown_pipeline() 会 patch_position(0,0)。
         * 用 live_position_ms()（而不是裸 now - t_start）保证暂停态重建时
         * 位置仍然冻结，与 refresh_position 口径一致。
         */
        {
            long long pos = live_position_ms();
            g_w.last_pos_ms = pos;
            pb2_stats_patch_position(&g_w.stats, pos, 0);
        }
        /*
         * 恢复暂停态：teardown 清了 paused，这里还回去，并把 t_start 后移
         * 「暂停已持续时长」——等价于「暂停期间时钟不走」，且保证
         * live_position_ms() 立刻等于 pos_before（重建本身不推进位置）。
         */
        if (was_paused) {
            long long now2 = pb2_now_ms();
            long long paused_for = (saved_pause_started > 0)
                                       ? (now2 - saved_pause_started)
                                       : 0;
            if (paused_for < 0) paused_for = 0;
            g_w.t_start = now2 - pos_before;
            g_w.pause_started_ms = now2;
            g_w.paused = 1;
            gst_element_set_state(g_w.pipe, GST_STATE_PAUSED);
            pb2_stats_patch_state(&g_w.stats, PB2_STATE_PAUSED);
            g_w.last_pos_ms = pos_before;
            pb2_stats_patch_position(&g_w.stats, pos_before, 0);
            pb2_logf("live: rebuild(%s) restored paused (pos=%lldms paused_for=%lldms)",
                     why ? why : "?", pos_before, paused_for);
        }
        pb2_stats_patch_seek(&g_w.stats, 0, 0);
        /*
         * 【review P0-1】这里以前无条件 patch(PLAYING)，把上面 was_paused
         * 分支刚设的 PAUSED 覆盖掉：管线其实停在 PAUSED，status() 却报
         * playing。JS 的 A/V 与 hard 两个看门狗的闸门都是
         * `st.state === 'playing'`，会被重新武装 —— 暂停画面本来就不换帧，
         * videoStallMs 单调增长，8s/25s 后各自触发 recover()，每次重拉
         * 黑屏 0.5~1.5s，3 次配额在 30~90 秒内烧光，最后只能提示「卡死」。
         * 暂停着看直播被反复重拉就是这么来的。
         */
        pb2_stats_patch_state(&g_w.stats,
                              was_paused ? PB2_STATE_PAUSED : PB2_STATE_PLAYING);
        pb2_logf("live: rebuild(%s) ok in %lldms (dev=%s)",
                 why ? why : "?", pb2_now_ms() - t0,
                 g_w.adev[0] ? g_w.adev : "speaker");
    } else {
        /*
         * 【P1-2】build_pipeline 失败时也必须 teardown。
         * 以前只 patch error + ERROR，g_w.pipe 留成半死管线、pipeline_alive
         * 仍是 1，worker tick 继续对着它跑 refresh_position()/query_position()，
         * 而且下一次 recover 还是在半死状态上重建。
         */
        pb2_logf("live: rebuild(%s) FAILED (build_pipeline failed)", why ? why : "?");
        live_rebuild_failed(PB2_E_OPEN_FAILED, "live rebuild failed");
    }
}

/*
 * 【3.5.6 比例修复】把 render-rectangle 的宽高对齐到偶数。
 *
 * g_w.rect 是面板坐标下的**旋转后**矩形（例如 X7 Pro 16:9 -> <100,274,253,450>），
 * 253 是奇数。NV12 的 UV 平面按半宽采样，video/x-raw,width=253 是非法 caps：
 * videoscale 要么协商失败、要么悄悄输出别的尺寸，kmssink 再按 rect 拉伸一次，
 * 画面就被二次变形。对齐到偶数后 buffer 尺寸与 render-rectangle 严格相等，
 * 平面完全不需要缩放能力（1:1 上屏）。
 */
static void normalize_video_rect(const char *tag) {
    int x = 0, y = 0, w = 0, h = 0, nx, ny, nw, nh;
    if (!g_w.sw_scale) return;
    if (sscanf(g_w.rect, "<%d,%d,%d,%d>", &x, &y, &w, &h) != 4) return;
    /* 【3.5.6 第二十九轮】起点也要对齐偶数。
     * 只对齐宽高不够：y=271（奇）时 UV 相位错位，横屏视频左侧会出现
     * 一条纯绿细线。向上取偶，最多挪 1px；宽高向下取偶。 */
    nx = (x + 1) & ~1;
    ny = (y + 1) & ~1;
    nw = w & ~1;
    nh = h & ~1;
    if (nw < 2 || nh < 2) return;
    if (nx == x && ny == y && nw == w && nh == h) return;
    snprintf(g_w.rect, sizeof(g_w.rect), "<%d,%d,%d,%d>", nx, ny, nw, nh);
    pb2_logf("%s: rect aligned to even x/y/size -> %s", tag, g_w.rect);
}

/*
 * ============ 运行时改矩形：优先走 GstVideoOverlay ============
 *
 * 【为什么需要它】见 PB2_CMD_SET_RECT 里的棘轮注释：`render-rectangle` 是
 * write-only 属性，kmssink 只在自己 reconfigure（分配 plane）那一刻读它。
 * 于是"放大"在运行期根本无效，只能把管线过一次 READY —— 对边下边播的架构
 * 等价于重新拉流（500ms 黑屏）。用户点一下画面想看大一点，结果是黑一下再回来，
 * 连点两下就更容易停在中间态（真机反馈「点两下屏幕后视频跑到下方 / 只露半截」）。
 *
 * 正道是 GstVideoOverlay 接口的 set_render_rectangle()：它直接调用 sink 的实现，
 * kmssink 收到后自己重配 plane，不依赖属性重读、不需要重建管线、放大缩小都有效。
 * 参考系统播放器也是同一套做法（dlopen libgstvideo + dlsym）。
 *
 * 这里同样 dlopen/dlsym：libgstvideo 不是链接期依赖（构建只链 libgstreamer 存根），
 * 运行时它一定在（kmssink 自己就依赖它）。拿不到就返回 0，调用方回落到老的
 * 属性路径 —— 行为与之前逐字节一致，不会因为一个新 API 把能用的设备搞坏。
 */
typedef int (*pb2_overlay_set_rect_fn)(void *overlay, int x, int y, int w, int h);
static pb2_overlay_set_rect_fn g_overlay_set_rect = NULL;
static int g_overlay_probed = 0;

static pb2_overlay_set_rect_fn pb2_overlay_fn(void) {
    if (g_overlay_probed) return g_overlay_set_rect;
    g_overlay_probed = 1;
    void *h = dlopen("libgstvideo-1.0.so.0", RTLD_LAZY | RTLD_GLOBAL);
    if (!h) h = dlopen("libgstvideo-1.0.so", RTLD_LAZY | RTLD_GLOBAL);
    if (h) {
        g_overlay_set_rect =
            (pb2_overlay_set_rect_fn)dlsym(h, "gst_video_overlay_set_render_rectangle");
    }
    pb2_logf("overlay api: %s",
             g_overlay_set_rect ? "gst_video_overlay_set_render_rectangle OK"
                                : "unavailable (fallback: render-rectangle property)");
    return g_overlay_set_rect;
}

/* 把 g_w.rect（"<x,y,w,h>"，已做偶数对齐）交给 overlay 接口。
 * → 1 = sink 受理（矩形已生效），0 = 没有接口 / sink 拒绝，调用方走老路径。 */
static int overlay_apply_rect(void) {
    pb2_overlay_set_rect_fn fn = pb2_overlay_fn();
    int x = 0, y = 0, w = 0, h = 0;
    if (!fn) return 0;
    if (!g_w.vsink_el) return 0;
    if (sscanf(g_w.rect, "<%d,%d,%d,%d>", &x, &y, &w, &h) != 4) return 0;
    if (w < 2 || h < 2) return 0;
    return fn(g_w.vsink_el, x, y, w, h) ? 1 : 0;
}

/*
 * videorate 之后的收尾片段（不含前导空格，以 " ! " 结尾），供两条建链路径共用：
 *   - pad-added 动态分支（demuxer 抛 pad）
 *   - 裸流固定分支（live 的 .flv/.ts：filesrc ! parse ! decode ! ...）
 *
 * sw_scale 非 0 时插 videoscale，让 kmssink 拿到与窗口等大的 buffer，
 * 平面不需要缩放能力 —— X7 Pro 上不必再赌 plane-id。
 * 顺序必须在 videorate 之后：先统一帧率，再缩放，避免逐帧 renegotiate。
 *
 * 【3.5.6：这里曾经是"压扁"的根因】
 *   mppvideodec rotation=270 给的是**旋转意图**（最终由 kmssink 在面板坐标系
 *   里转 90 度上屏），videoscale 看到的仍然是解码出来的**横向帧** 1920x1080。
 *   g_w.rect 却是旋转后的物理竖矩形 253x450 —— 旧代码直接把横帧缩到 253x450，
 *   等于把 16:9 硬拉成 9:16 竖条；源比例越偏离，拉伸越不一样（真机现象：
 *   「视频被压扁了，播放比例不同的视频被压扁的程度不一样」）。
 *
 *   正确目标 = rect 宽高互换后的横向尺寸 450x252：横帧等比缩到它，
 *   kmssink 转 90 度后正好是 252x450，与 render-rectangle 1:1 对齐。
 *
 * sw_scale 取值：
 *   0 = 不插 videoscale（X6 Pro，硬件缩放，行为与 3.5.5 逐字节相同）
 *   1 = 插 videoscale 且按 rotate 自动互换（X7 Pro 默认）
 *   2 = 插 videoscale 但不互换（回退开关：某些固件的 mppvideodec 会**真的**
 *       把 buffer 转成竖帧，那时不该互换。storage 覆盖 {"swScale":2} 即可，
 *       不用重新打包）
 */
static void video_post_rate_desc(char *out, size_t cap) {
    int x = 0, y = 0, w = 0, h = 0, tw, th, bs;
    if (!g_w.sw_scale) {
        /* 与 3.5.5（X6 Pro）完全同构，旋转与缩放全交给 kmssink。
         * 日志里明确写出这一行，真机截图才能确认档位真的生效了。 */
        pb2_logf("video: hw scale, NO videoscale (rect=%s rotate=%d)",
                 g_w.rect, g_w.rotate);
        snprintf(out, cap, "video/x-raw,framerate=%s ! ", PB2_PANEL_RATE);
        return;
    }
    if (sscanf(g_w.rect, "<%d,%d,%d,%d>", &x, &y, &w, &h) != 4 ||
        w < 2 || h < 2) {
        snprintf(out, cap, "video/x-raw,framerate=%s ! ", PB2_PANEL_RATE);
        return;
    }
    tw = w & ~1;
    th = h & ~1;
    if ((g_w.sw_scale == 1 || g_w.sw_scale == 3) && (g_w.rotate == 90 || g_w.rotate == 270)) {
        int t = tw;
        tw = th;
        th = t;
    }
    /*
     * 【3.5.6 第四轮：真正能改变画面大小的旋钮】
     * 真机四档截图里 out=253x450 完全没变、画面始终只占挖洞区一小块 ——
     * X7 Pro 自选的 overlay 平面疑似只做 1:1 贴 buffer：改 render-rectangle
     * 对它的显示尺寸毫无作用。只有把送进 sink 的 buffer 等比放大，画面才会
     * 跟着变大。矩形本身保持与挖洞区等大、不越出面板，kmssink 不会拒绝。
     *
     * 等比放大（宽高同乘）不会引入比例变形；偶数化是 NV12 的硬性要求。
     * 100 = 与矩形等大，等同旧行为（X6 Pro 永远走这条）。
     */
    bs = (g_w.buf_scale >= 50 && g_w.buf_scale <= 400) ? g_w.buf_scale : 100;
    if (bs != 100) {
        tw = (tw * bs + 50) / 100;
        th = (th * bs + 50) / 100;
        tw &= ~1;
        th &= ~1;
        if (tw > 4096) tw = 4096;
        if (th > 4096) th = 4096;
    }
    if (tw < 2 || th < 2) {
        snprintf(out, cap, "video/x-raw,framerate=%s ! ", PB2_PANEL_RATE);
        return;
    }
    /* sw_scale=3 是 3.5.6 第十四轮加的**回退档**：插 videoscale 但用默认
     * bilinear。真机若发现「画面变形」与 lanczos 相关（同一条流、同一个矩形，
     * 只换算法就变），storage 覆盖 bili_device_profile = {"swScale":3} 即可，
     * 不用重装包。 */
    const char *method = (g_w.sw_scale == 3) ? "" : " method=lanczos";
    /* 【第十八轮】不再插 videoflip：真机证明 swScale=2（B 档，竖帧）本来就对，
     * 绿幕只出现在 swScale=1（横帧）档。默认档已改成 2，这条路径保持原样。 */
    pb2_logf("video: videoscale -> %dx%d%s (panel rect %dx%d, rotate=%d, mode=%d, buf=%d%%)",
             tw, th, method, w, h, g_w.rotate, g_w.sw_scale, bs);
    /*
     * 【3.5.6 第十四轮：为什么必须指定 method】
     * 真机反馈「视频播放有一些糊，就算是 360p 的也不应该这么糊」。
     * videoscale 的 method 默认是 **bilinear**，而这里的缩小倍率非常大：
     * 1080p 源 (1920x1080) → 面板上的 444x250，缩小 4.3 倍。双线性在这种
     * 倍率下**不做任何抗锯齿预滤波**，每 4.3 个输入像素只取 2x2，等于抽点：
     * 细节被丢掉，边缘出现锯齿，观感就是"糊"。
     *
     * lanczos 是 6-tap 且**按缩放比例扩展 support**（相当于先低通再重采样），
     * 大倍率缩小时能保住线条和文字边缘。代价是 CPU：每输出像素约 36 次
     * 采样，444x250@30fps 大约 1.2 亿次/秒 —— RK 这颗 U 跑得动，但如果某台
     * 固件上出现持续丢帧，把这里换回 "videoscale"（不加 method）即可，
     * 那是旧行为（bilinear），一像素画质换一点余量。
     * 日志里打出 method 是为了真机一眼能确认这条改动生效了。
     */
    /*
     * 【pixel-aspect-ratio=1/1 是必须写死的】
     * caps 里不写 PAR 时，videoscale 会把源 caps 的 PAR 原样带下来；而
     * kmssink 的 force-aspect-ratio=false 只跳过了"我算信箱"，并不跳过
     * "按 PAR 拉伸" —— 一旦源带了个非 1:1 的 PAR（部分转码稿是真的有），
     * 画面就会横向/纵向被拉，而矩形尺寸完全正确、日志里一点异常都看不出。
     * 这里显式钉成方形像素，把这条路彻底堵掉。
     */
    /*
     * 【3.5.6 第二十五轮：不要 add-borders —— 它补的边是绿色】
     *
     * 第二十四轮曾用 `add-borders=true` 把信箱交给 videoscale（等价参考系统播放器的
     * force-aspect-ratio）。真机上竖屏稿件左右出现大片**绿幕**：videoscale 补边
     * 时按 YUV 全 0 填充，而 NV12 的 (0,0,0) 是绿色，不是黑色。横屏 16:9 只补
     * 11px 看不出来，竖屏要补 150px，一眼就是绿。
     *
     * 现在信箱重新由 JS 算（playVideoRectsFor 的 fit），矩形 = 画面本身，
     * plane 窗口与 buffer 等大，**没有边可补**；画面之外的黑色由 Falcon 的
     * UI 层提供（hole 之外的 fb 黑）。这里回到最朴素的"缩到窗口大小"。
     *
     * sw_scale=0（X6 Pro 的硬件缩放路径）不插 videoscale，不受影响。
     */
    /*
     * 【3.5.6 第二十六轮：全屏 cover】
     * 普通态 crop=0/0，走下面那条逐字节不变的老路径。
     * 全屏横屏时 JS 算出「等比放大后要裁掉多少」，这里让 videoscale 先输出
     * 完整的大画面，再用 videocrop 裁到窗口大小 —— 画面填满整个视口，
     * 比例保持不变，也不依赖 kmssink 的缩放能力（仍 1:1 贴 buffer）。
     */
    if ((g_w.crop_x > 0 || g_w.crop_y > 0) && g_w.sw_scale) {
        int cw = tw + 2 * g_w.crop_x;
        int ch = th + 2 * g_w.crop_y;
        cw &= ~1;
        ch &= ~1;
        if (cw > 2 && ch > 2 && cw <= 4096 && ch <= 4096) {
            pb2_logf("video: cover -> videoscale %dx%d, videocrop x=%d y=%d -> %dx%d",
                     cw, ch, g_w.crop_x, g_w.crop_y, tw, th);
            snprintf(out, cap,
                     "videoscale%s ! "
                     "video/x-raw,width=%d,height=%d,framerate=%s,"
                     "pixel-aspect-ratio=1/1 ! "
                     "videocrop left=%d right=%d top=%d bottom=%d ! ",
                     method, cw, ch, PB2_PANEL_RATE,
                     g_w.crop_x, g_w.crop_x, g_w.crop_y, g_w.crop_y);
            return;
        }
    }
    /*
     * 【3.5.6 第二十九轮 v8：不裁切，画面按原生比例直接缩到窗口】
     *
     * v5~v7 曾在这里插 `videocrop left/right/top/bottom=16`，想切掉解码帧
     * 最外圈那条绿线（H.264 的 1080 按 16 宏块对齐解成 1088，多出的 8 行
     * 没人写 = YUV 全 0，而 NV12 的 0 是绿色不是黑色）。
     *
     * 这条路放弃，理由是它治不了本：
     *   · 裁多少只能猜（8 / 16 / 32 视片源而定），裁多一刀砍掉真画面；
     *   · 要在 do_open 里配"建失败就去掉裁切重建"，每起播多赔一次管线构建
     *     —— 用户已经反馈"打开视频慢了一些"；
     *   · 绿线来自源帧内容，等比缩放只会把它一起缩小，裁切只是把可见区域
     *     往里挪，换一个尺寸又会从别的边露出来。
     *
     * 现在这里回到最朴素的一条：源帧等比缩到窗口，pixel-aspect-ratio 钉死
     * 1:1。矩形由 JS 按视频原生宽高比算（playVideoRectsFor 的 fit），
     * 送进 sink 的 buffer 宽高比 = 片源宽高比 = 窗口宽高比，全程不拉不裁。
     */
    snprintf(out, cap,
             "videoscale%s ! "
             "video/x-raw,width=%d,height=%d,framerate=%s,"
             "pixel-aspect-ratio=1/1 ! ",
             method, tw, th, PB2_PANEL_RATE);
}

static void on_pad_added(GstElement *el, GstPad *pad, void *data) {
    char capsbuf[128];
    const char *cn;
    GstElement *bin = NULL;
    GstPad *sink;
    int rc;
    int mpegversion = 0;
    /* 1024 在加了 videoscale/caps 之后余量变小（desc 是拼接出来的），
     * 这里放大并加截断检查 —— 被截断的 desc 会 parse_bin 失败，
     * 症状就是「整片黑」，日志里却看不出原因。 */
    char desc[1280];
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
        int src_w = 0, src_h = 0;
        pad_caps_size(pad, &src_w, &src_h);
        pb2_logf("pad-added: source=%dx%d (encoded) rotate=%d rect=%s swScale=%d buf=%d%%",
                 src_w, src_h, g_w.rotate, g_w.rect, g_w.sw_scale,
                 (g_w.buf_scale >= 50 && g_w.buf_scale <= 400) ? g_w.buf_scale : 100);
        /*
         * 【P1-3】队列上限按源类型选：
         *   点播 -> 8s/6MB（为了装下 seek 时 demuxer 把落后一路推进到对齐点
         *                  的量，见 PB2_VIDEO_QUEUE_NS 的实测矩阵）
         *   直播 -> 4s/3MB（队列上限就是播放延迟的下限，见
         *                   PB2_LIVE_VIDEO_QUEUE_NS；FLV 单流交织，不需要推进。
         *                   实测 2s 在 B 站直播（GOP 1~2s + CDN 抖动）下余量
         *                   太薄，一次稍长抖动就抽干队列，故放到 4s）
         */
        int qbytes = g_w.is_live ? PB2_LIVE_VIDEO_QUEUE_BYTES : PB2_VIDEO_QUEUE_BYTES;
        long long qns = g_w.is_live ? PB2_LIVE_VIDEO_QUEUE_NS : PB2_VIDEO_QUEUE_NS;
        /*
         * 【首次 seek 必失败的根因修复】点播队列改成**按 buffer 个数**限界。
         *
         * 实测事实（真机，连续 10+ 轮 100% 复现）：
         *   - 每个会话的第一次 seek 必不出首帧，第二/三次必成功；
         *   - 失败窗口里 ffmpeg 卡在 wchan=pipe_write（FIFO 写满 = gst 不再读）；
         *   - 音频一直正常（35 次推送），只有视频 0 次；
         *   - fd 计数全程稳定（dri=5 mpp=1 dmabuf=78）=> 不是引用泄漏；
         *   - 同一进程内用独立程序反复建/拆 kmssink 管线，3/3 都是 106ms 出首帧
         *     => 也不是 plane 被占。
         *
         * 剩下唯一能解释「只有视频被堵、且约 8~10 秒后自愈」的机制就是
         * max-size-time：GStreamer 的 queue 用 **buffer 时间戳跨度** 判断是否超限，
         * 不是墙钟耗时。seek 后视频缓冲的时间戳跨度一旦跳过大数，queue 立刻满
         * -> 反压 demuxer -> demuxer 不再推视频 -> filesrc 不再读 FIFO
         * -> ffmpeg 的 write() 阻塞（观测完全一致）。音频用计数制所以免疫。
         *
         * 改成计数制后：240 个 buffer ≈ 8s @30fps，仍能装下 demuxer 推进落后一路
         * 所需的 T-K（实测最大 3.843s），但对时间戳跨度不再敏感。
         */
        int qbufs = g_w.is_live ? 0 : PB2_VIDEO_QUEUE_BUFS;
        /*
         * 【直播闪屏】直播和点播一律使用严格的 max-lateness=40 / qos=true。
         *
         * 历史：曾经以为「直播每 2~5 秒闪一次屏」是丢帧造成的，于是把直播
         * 改成 max-lateness=-1 + qos=false（不判迟到、一帧不丢）。真机量化
         * （.tmp/spike_test.sh，录 kmssink 平面后逐帧算 Y 平均亮度）证明
         * **这个改动本身就是闪屏的成因**：
         *
         *   late=40  qos=true  -> 45 秒内亮条带 1 次（点播 60 秒 0 次）
         *   late=40  qos=false -> 7 次
         *   late=-1  qos=true  -> 22 次（maxY 211.8，出现真正白屏）
         *   late=-1  qos=false -> 20 次
         *
         * 机制：关掉迟到判定后，起播/抖动期间积压的帧会一次性涌进 kmssink，
         * 每一帧都立刻上屏（skip-vsync=true），于是同一时刻有多个 page flip
         * 排队打到同一个 plane，屏幕上就出现「部分区域闪白一帧」——正是
         * 用户描述的现象。max-lateness=40 的丢弃恰恰是在保护 plane 不被
         * 过载。
         */
        int late_ms = PB2_MAX_LATENESS;
        const char *qos_s = "true";
        /*
         * 【直播「部分区域闪白一帧」的根因与修法】
         *
         * mppvideodec 的 ignore-error 出厂默认为 **true** —— 解码出错时
         * 不报错、照常输出一帧损坏画面。直播是长连接，网络丢包会让某个
         * 参考帧缺失，于是 P 帧解出来的就是「部分区域错位/发白」的坏帧，
         * 用户看到的就是闪白。
         *
         * 真机对照（同一时段交替跑，排除画面内容随时间变化的影响）：
         *   ignore-error=true  -> 40s 内 3 组 / 5 组闪白
         *   ignore-error=false -> 40s 内 0 组 / 0 组闪白
         *
         * 另外三组对照排除了网络与显示链路：
         *   同一份流用 ffmpeg 软解      -> 0 组（软解会丢弃坏帧）
         *   同一份流先录成 FLV 再硬解   -> 0 组（本地文件不丢包）
         *   同一份流直播 + 硬解         -> 4 组
         * 所以不是 CDN 的问题，也不是 kmssink / videorate 的问题。
         *
         * max-errors=16：连续 16 帧都解不出来才报 flow error（默认 -1 是
         * 无限容忍，直播里会一直吐坏帧）。
         */
        const char *mpperr = " ignore-error=false max-errors=16";
        /*
         * videorate 把源帧率归一到面板率（30fps -> 59.88fps，复制帧）。
         * 曾怀疑它复制帧时复用 buffer 造成闪白，实测旁路掉反而更差
         * （40s 内 3 组 vs 1 组），故保留。
         *
         * 【3.6.3】sw_scale=1 时再插一级 videoscale，把 buffer 直接缩到窗口
         * 大小（req->w x req->h）。这样 kmssink 拿到的 buffer 与 render-rectangle
         * 等大，**不需要平面具备缩放能力** —— X7 Pro 上不用再赌 plane-id。
         * 顺序必须在 videorate 之后：先统一帧率再缩放，避免 videoscale 被
         * 逐帧 renegotiate。规格尺寸用旋转后的渲染矩形（mppvideodec 的
         * rotation 已把画面转正，rect 也是按转正后的宽高算出来的）。
         */
        char vrate[320];
        char vpost[256];
        video_post_rate_desc(vpost, sizeof(vpost));
        snprintf(vrate, sizeof(vrate), " videorate ! %s", vpost);
        if (g_w.sw_scale && g_w.req) {
            pb2_logf("video: software scale -> %dx%d (plane needs no scaler)",
                     g_w.req->w, g_w.req->h);
        }
        /*
         * skip-vsync=true：不等 page flip 完成就立刻提交下一帧。
         * 实测把它关掉（skip-vsync=false）闪白次数无差异（1 组 vs 1 组），
         * 所以它不是闪白的原因，保留以维持帧率。
         * 显式设 sync-mode 同样无收益，故不设（用驱动默认 auto flip）。
         */
        const char *skip_s = "skip-vsync=true";
        const char *syncmode = "";
        /*
         * 蓝牙时给视频负 ts-offset 把它整体提前，抵消 A2DP 端到端延迟。
         * 为什么必须放在 kmssink 而不是 alsasink，见
         * PB2_BT_VIDEO_TS_OFFSET_MS 的注释。
         */
        char tsoff[48];
        if (g_w.adev[0] && g_w.bt_ts_offset_ns != 0) {
            /*
             * 取反后直接按有符号打印。**不要**写成 "ts-offset=-%lld" 再传
             * 正值：标定时写负数会得到 `ts-offset=--2000000000`，gst 解析
             * 失败、属性保持默认 0，而日志看起来「设了」——真机踩过一次。
             */
            /*
             * 【绝不能带前导空格】desc 里是 `%%s%%s%%s` 直接拼接，前导空格会让
             * 边界看起来正常、实则后面的 skip-vsync=true 会粘上来，得到
             * `ts-offset=-150000000skip-vsync=true` → parse_bin 失败 →
             * 视频分支建不起来 → 蓝牙下整片黑屏（真机 2026-11-03 踩到）。
             * 分隔符一律由 desc 的格式串负责。
             */
            snprintf(tsoff, sizeof(tsoff), "ts-offset=%lld",
                     -g_w.bt_ts_offset_ns);
        } else {
            tsoff[0] = '\0';
        }
        /*
         * 点播把 max-size-time 归零。queue 的多个上限是「或」关系，
         * 只把 buffers 改成 240 而留着 time=8s 等于没改（time 先满足）。
         * 直播仍保留 2s 时间上限（直播延迟就是队列上限，不能用计数制）。
         */
        if (!g_w.is_live) qns = 0;
        if (g_w.vbranch) {
            pb2_logf("pad-added: video branch exists, ignore extra pad");
            return;
        }
        if (strstr(cn, "h264")) {
            snprintf(desc, sizeof(desc),
                     /* 视频队列必须按「时间/字节」限界，不能按 buffer 个数（见 PB2_VIDEO_QUEUE_*） */
                     "queue max-size-buffers=%d max-size-bytes=%d "
                     "max-size-time=%lld ! "
                     "h264parse ! mppvideodec rotation=%d%s ! "
                     "%s"
                     "%s"
                     "kmssink name=vsink %sdriver-name=rockchip sync=true "
                     /*
                      * 默认严格（PB2_MAX_LATENESS / qos=true）。起播时的
                      * 迟到宽限由 arm_late_grace() 在 set_state(PLAYING) 之后
                      * 用 gst_util_set_object_arg 运行时下发，到点由
                      * update_late_policy() 收紧回来。
                      *
                      * 为什么不在 desc 里写 -1/false 再收紧：seek 重建管线时
                      * 不 arm 宽限（会给 seek 带来 4~5 倍首帧延迟），此时
                      * update_late_policy() 直接 return，sink 就**永远**停在
                      * -1/false —— 实测 seek 落点耗时从 412ms 恶化到 1398~2485ms。
                      */
                     "max-lateness=%d qos=%s force-aspect-ratio=false %s %s %s "
                     "render-rectangle=%s",
                     qbufs, qbytes, qns,
                     decoder_rotation(), mpperr, fpscap, vrate, kms_plane_arg(),
                     late_ms, qos_s, tsoff, skip_s, syncmode, g_w.rect);
        } else if (strstr(cn, "h265") || strstr(cn, "hevc")) {
            snprintf(desc, sizeof(desc),
                     "queue max-size-buffers=%d max-size-bytes=%d "
                     "max-size-time=%lld ! "
                     "h265parse ! mppvideodec rotation=%d%s ! "
                     "%s"
                     "%s"
                     "kmssink name=vsink %sdriver-name=rockchip sync=true "
                     /*
                      * 默认严格（PB2_MAX_LATENESS / qos=true）。起播时的
                      * 迟到宽限由 arm_late_grace() 在 set_state(PLAYING) 之后
                      * 用 gst_util_set_object_arg 运行时下发，到点由
                      * update_late_policy() 收紧回来。
                      *
                      * 为什么不在 desc 里写 -1/false 再收紧：seek 重建管线时
                      * 不 arm 宽限（会给 seek 带来 4~5 倍首帧延迟），此时
                      * update_late_policy() 直接 return，sink 就**永远**停在
                      * -1/false —— 实测 seek 落点耗时从 412ms 恶化到 1398~2485ms。
                      */
                     "max-lateness=%d qos=%s force-aspect-ratio=false %s %s %s "
                     "render-rectangle=%s",
                     qbufs, qbytes, qns,
                     decoder_rotation(), mpperr, fpscap, vrate, kms_plane_arg(),
                     late_ms, qos_s, tsoff, skip_s, syncmode, g_w.rect);
        } else {
            pb2_logf("pad-added: unsupported video caps %s", cn);
            return;
        }
        pb2_logf("pad-added: video desc late=%d qos=%s %s mpp:%s swscale=%d rotate=%d rect=%s",
                 late_ms, qos_s,
                 tsoff[0] ? tsoff : "(no ts-offset)",
                 mpperr[0] ? mpperr : "(vendor defaults)", g_w.sw_scale,
                 g_w.rotate, g_w.rect);
        if (strlen(desc) >= sizeof(desc) - 1) {
            pb2_logf("pad-added: video desc TRUNCATED at %u bytes -> abort branch",
                     (unsigned)strlen(desc));
            pb2_stats_patch_error(&g_w.stats, PB2_E_PIPELINE, "video desc truncated");
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
            char asink_arg[320];
            if (g_w.adev[0]) {
                snprintf(devarg, sizeof(devarg), " device=\"%s\"", g_w.adev);
            } else {
                devarg[0] = '\0';
            }
            /*
             * 蓝牙 vs 扬声器用不同的 ALSA 缓冲，理由见
             * PB2_BT_AUDIO_BUFFER_US 的注释。
             *
             * 【音画偏移不在这里】alsasink 是时钟提供者，给它加 ts-offset
             * 等于平移整条管线（视频跟着时钟走，相对关系不变），真机实测
             * 也证实无效且会让恢复后帧率飙到 99fps。偏移落在 kmssink 上，
             * 见 PB2_BT_VIDEO_TS_OFFSET_MS 的注释。
             */
            if (g_w.adev[0]) {
                snprintf(asink_arg, sizeof(asink_arg),
                         "alsasink name=asink buffer-time=%lld latency-time=%lld%s",
                         (long long)PB2_BT_AUDIO_BUFFER_US,
                         (long long)PB2_BT_AUDIO_LATENCY_US, devarg);
                pb2_logf("audio: BT sink params buffer=%lldus latency=%lldus "
                         "(video ts-offset=-%lldms)",
                         (long long)PB2_BT_AUDIO_BUFFER_US,
                         (long long)PB2_BT_AUDIO_LATENCY_US,
                         g_w.bt_ts_offset_ns / 1000000LL);
            } else {
                snprintf(asink_arg, sizeof(asink_arg),
                         "alsasink name=asink buffer-time=30000 latency-time=10000%s",
                         devarg);
            }
        if ((strncmp(cn, "audio/mpeg", 10) == 0 && mpegversion == 4) ||
            strstr(cn, "aac") || strstr(cn, "AAC")) {
            snprintf(desc, sizeof(desc),
                     "queue max-size-buffers=32 max-size-bytes=0 max-size-time=0 ! "
                     "aacparse ! faad ! audioconvert ! audioresample ! "
                     "audio/x-raw,rate=48000,channels=2 ! volume name=vvol ! "
                     "%s", asink_arg);
        } else if (strstr(cn, "x-ac3") || strstr(cn, "x-eac3") || strstr(cn, "x-a52")) {
            snprintf(desc, sizeof(desc),
                     "queue max-size-buffers=32 ! ac3parse ! a52dec ! audioconvert ! "
                     "audioresample ! audio/x-raw,rate=48000,channels=2 ! volume name=vvol ! "
                     "%s", asink_arg);
        } else {
            /* 未知音频编码：只要有 parser 就走通用链路，失败也只是没声音 */
            pb2_logf("pad-added: audio caps %s, trying generic chain", cn);
            snprintf(desc, sizeof(desc),
                     "queue max-size-buffers=32 ! decodebin ! audioconvert ! "
                     "audioresample ! audio/x-raw,rate=48000,channels=2 ! volume name=vvol ! "
                     "%s", asink_arg);
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
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, on_video_src_probe, NULL, NULL);
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
        g_w.vsink_el_owned = 1;
        /* 【P0-4】成功连上之后才置位 */
        g_w.vbranch = 1;
        g_w.vbranch_ready_ms = pb2_now_ms();
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
            /*
             * 【蓝牙路由：采纳耳机自己的 AVRCP，绝不回写】
             *
             * 这条路径在「插拔耳机触发重建」「seek 重建」时都会走到，此时
             * g_w.vol_pct 还停在上一跳的值（可能是扬声器留下的 0）。如果照
             * 旧代码无条件下发 AVRCP，就会把用户真实的耳机音量抹掉 ——
             * 用户看到的现象是「点开视频音量自动变 0%，重新拉一下才有声」。
             * 读回来当权威，并把 bt_vol_applied 对齐成同值，让
             * apply_bt_avrcp_volume() 自然早退（不下发）。
             */
            if (g_w.bt_present && g_w.adev[0]) {
                int av = g_w.bt_ctl_name[0] ? pb2_mixer_volume_read(g_w.bt_ctl_name) : -1;
                if (av >= 0) {
                    g_w.vol_pct = av;
                    g_w.bt_vol_applied = av;
                    pb2_logf("audio: bt volume adopted from AVRCP %d%% (not overwriting)", av);
                } else {
                    g_w.bt_vol_applied = -1;
                }
                g_w.master_last = pb2_mixer_volume_read(NULL); /* 滑杆基线 */
            }
            apply_volume_now();
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
    if (!g_w.bt_ctl_name[0]) return;
    if (g_w.bt_vol_applied == g_w.vol_pct) return;
    /*
     * 【双向镜像，不再「只升不降」】
     *
     * 旧版只升不降是为了防「按读错的值（conf 里滞后的 0）把耳机压成
     * 静音」。现在源头已经换成 card1 Master（hal-audio -s 立刻写它，
     * 不存在读到旧值的可能），“读错”的前提消失了；而只升不降会让
     * **系统滑杆向下调完全失效**（用户拉小了却不变小）。
     * 所以改回双向：系统滑杆是唯一权威，耳机硬件音量跟随它。
     * 耳机本体按键调音量会在 1s 内被这里拉回滑杆值。
     */
    snprintf(vol, sizeof(vol), "%d%%", g_w.vol_pct);
    argv[0] = (char *)"/usr/bin/amixer";
    argv[1] = (char *)"-D";
    argv[2] = (char *)"bluealsa";
    argv[3] = (char *)"sset";
    argv[4] = (char *)g_w.bt_ctl_name; /* 设备名 - A2DP，探测时填 */
    argv[5] = vol;
    argv[6] = NULL;
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

    /*
     * 【蓝牙路由：软件增益固定 1.0，音量全部交给 AVRCP】
     *
     * AVRCP 是耳机硬件音量，和软件增益是**串联**的：听感 = gain × AVRCP。
     * 两边都设 76% 就是 58%，都设 20% 就是 4%（几乎没声音）。
     * 所以蓝牙下软件增益必须保持 1.0，由 apply_bt_avrcp_volume()
     * 把系统滑杆值镜像到 AVRCP。扬声器路由没有这个问题（只有一级）。
     */
    if (g_w.bt_present && g_w.adev[0]) {
        if (g_w.vol_applied == 100) return;
        g_object_set(g_w.vvol, "volume", 1.0, NULL);
        g_w.vol_applied = 100;
        pb2_logf("audio: volume slider %d%% -> bt: software gain kept 1.000 (AVRCP handles it)",
                 g_w.vol_pct);
        return;
    }

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
 * 取「当前真正生效的音量」，这是播放器唯一应该信的值。
 *
 * 【2026-11-04 真机实测，修「点开视频音量自动变 0%」】
 *   hal-audio -s N ��� card1 Master 立刻变、bluealsa AVRCP 约 4s、
 *   /data/cfg/sys_config.conf 的 spk_volume 约 8~10s（最慢）。
 * 以前这里和 do_open 只读 conf 的 spk_volume，于是「刚调大音量就点视频」
 * 会读到旧值 0 → 软件增益 0 + AVRCP 0% → 耳机静音。
 *
 * 现在先读真实控件（蓝牙读 bluealsa AVRCP、喇叭读 card1 Master），
 * 两者都读不到才退回 conf。会 fork/exec（一次 ~25ms），只在 worker 线程调。
 */
static int read_route_volume(void) {
    int v;

    /*
     * 【顺序必须按路由分流（2026-11-04 二次真机纠错）】
     *
     * card1 Master 是**扬声器功放**的音量，card0 Playback Path 切到蓝牙后
     * 它不再影响耳机输出，但 hal-audio -s N 仍会写它 —— 它就是**系统滑杆的唯一写入点**。
     * 蓝牙路由下它与耳机硬件音量（AVRCP）可以长期不一致（实测抓到
     * Master=0% / AVRCP=44%），因此它不能直接当软件增益，而是当「用户意图」
     * 镜像到 AVRCP（见 apply_bt_avrcp_volume）。
     *
     * hal-audio -s N 的写入时机（实测）：Master 立刻、spk_volume 约 10~12s。
     * **它从头到尾都不写 bluealsa AVRCP**（早前以为它写，其实是播放器自己写的）。
     */
    /*
     * 【三次真机纠错后的结论】**两条路由都读 card1 Master**。
     *
     * 第一版：只读 conf 的 spk_volume。conf 最慢（~10s），“拉大音量→立刻点视频”
     *          读到旧值 0 → 静音。
     * 第二版：蓝牙路由改读 bluealsa AVRCP。AVRCP 是**耳机硬件音量**，
     *          它和软件增益是串联的两级：读它当增益会变成
     *          “0.76 × 0.76 = 58%”、“0.20 × 0.20 = 4%”的双重衰减，不随系统滑杆走。
     * 第三版（现在）：滑杆意图 = Master（hal-audio -s 立刻写它），蓝牙下把
     *          它**镜像到 AVRCP**，软件增益固定 1.0（见 apply_volume_now）。
     *          听感 = AVRCP = Master，线性、无双重衰减。
     */
    v = pb2_mixer_volume_read(NULL); /* NULL = 读 card1 Master */
    if (v >= 0) return v;
    return read_sys_config_int("spk_volume", read_sys_config_int("bt_volume", 100));
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

    /*
     * ① 音量：读**系统滑杆**的当前值（card1 Master，立刻生效），
     *    不读 sys_config.conf —— hal-audio 写 conf 有 10~12s 延迟。
     *    蓝牙路由下这个值会被镜像到 AVRCP（音量在耳机硬件里生效）。
     */
    {
        int master = pb2_mixer_volume_read(NULL); /* 系统滑杆（card1 Master） */

        if (g_w.bt_present && g_w.adev[0]) {
            /*
             * 【蓝牙：滑杆变化才算用户意图】
             *
             * Master 在蓝牙路由下不代表耳机音量（它是扬声器功放），只有
             * 「相对上次采样变了」才能证明用户刚动过系统滑杆。没变时
             * 一律跟随耳机自己的 AVRCP（耳机按键调音量也能生效）。
             */
            int slider_moved = (master >= 0 && master != g_w.master_last);
            int av = -1;
            if (master >= 0) g_w.master_last = master;
            if (g_w.bt_ctl_name[0]) av = pb2_mixer_volume_read(g_w.bt_ctl_name);

            if (slider_moved) {
                g_w.vol_pct = master;
                apply_volume_now();
                apply_bt_avrcp_volume(); /* 唯一会写耳机的路径 */
            } else if (av >= 0 && av != g_w.vol_pct) {
                g_w.vol_pct = av;
                g_w.bt_vol_applied = av;
                apply_volume_now();
                pb2_logf("audio: bt volume adopted from AVRCP (headset key) %d%%", av);
            }
        } else {
            int pct = (master >= 0)
                          ? master
                          : read_sys_config_int("spk_volume",
                                                read_sys_config_int("bt_volume", 100));
            g_w.master_last = master;
            if (pct >= 0 && pct != g_w.vol_pct) {
                g_w.vol_pct = pct;
                apply_volume_now();
            }
        }
    }

    /* ② 路由（只在有会话、已建音频分支时才关心） */
    if (!g_w.req || !g_w.pipeline_alive) return;
    /*
     * 【P1-8】路由变更待办优先：暂停期间攒下的变更，resume 之后第一次
     * tick 就重建，不再等蓝牙探测窗口。
     */
    if (g_w.route_pending && !g_w.paused) {
        g_w.route_pending = 0;
        pb2_logf("audio: route change resumed (was deferred while paused) -> rebuild now");
        goto rebuild;
    }
    /*
     * 蓝牙探测节流：**已连上时放慢到 5s，未连上时保持 1s**。
     *
     * 成本：pb2_bt_pcm_detect 每次要 popen bluetoothctl（devices 25ms +
     * 每台 info 25ms），真机实测单次 50~60ms，而且是 fork/exec ——
     * 在 4 核 A35 上 fork 本身就会抢 CPU。
     *
     * 【P1-8 曾经的错误】为了「插上耳机快点出声」把节流从 5s 降到 1s，
     * 结果**已连蓝牙时也每秒 fork 两次**：日志里 `bt: pcm via bluetoothctl`
     * 每秒一条刷屏，20 秒内 20 条。用户反馈「连蓝牙耳机后播放经常卡顿、
     * 观感只有 1 帧」——每秒 50~60ms 的 fork 风暴叠加 A2DP 编码，
     * 正好卡在解码线程的节拍上。
     *
     * 现在的策略按「有没有需要发现的变化」分档：
     *   - 未连蓝牙（bt_present=0）：1s，用户刚插上耳机能快速切过去；
     *   - 已连蓝牙（bt_present=1）：5s，只需要发现「耳机掉了」，
     *     这个延迟用户完全能接受（声音最多晚 5s 回扬声器）。
     */
    {
        long long period = g_w.bt_present ? 5000 : 1000;
        if (now - g_w.bt_check_ms < period) return;
    }
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
            g_w.bt_ts_offset_ns = read_bt_ts_offset_ns();
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
         * 暂停态不重建（重建会把画面刷出来）。
         *
         * 【P1-8】以前这里只是把 vol_check_ms 清零，指望 resume 后下一次
         * tick「恰好」重新探测到变化再重建 —— 但 resunme 之后蓝牙探测要
         * 重新走 5s 节流窗口，最坏多等 5s，而且如果探测期间状态抖动
         * （pb2_bt_pcm_detect 偶发返回失败）这次变更就被彻底丢掉了。
         * 现在显式记 route_pending，由 refresh_audio_route 开头消费。
         */
        pb2_logf("audio: route change deferred (paused) -> pending");
        g_w.route_pending = 1;
        return;
    }
    if (g_w.seek_pending) return; /* 已经在重建了 */
    if (g_w.is_live) {
        /*
         * 【直播】路由变更不能走 seek 路径（直播 seek 恒被忽略，走了也不会
         * 重建，插拔耳机后音频还固定在旧 device 上）。
         * 交给 live_rebuild_pipeline()：teardown 后 build_pipeline(req, 0)
         * 会重新走 is_live 分支（souphttpsrc ! flvdemux），不需要 ffmpeg。
         * 代价是直播要重新拉一次流（约 0.5~1.5s），但插拔耳机是低频事件，
         * 且这是唯一能让 alsasink 换到新 device= 的办法（PLAYING 下改
         * device 不会重开 PCM）。
         */
        live_rebuild_pipeline("route change");
        return;
    }
    g_w.seek_target = g_w.last_pos_ms;
    g_w.seek_pending = 1;
    g_w.seek_last_ms = pb2_now_ms() - PB2_SEEK_DEBOUNCE_MS - 1;
    pb2_stats_patch_seek(&g_w.stats, g_w.seek_target, 1);
    pb2_logf("audio: rebuild at %lldms for route change", g_w.seek_target);
}

/* ============================ 视频帧计数 probe ============================ */

/*
 * 【诊断】demuxer 视频 src pad 探针。
 *
 * 用途：区分「demuxer 根本没推视频数据」和「link/激活失败」。
 * 主探针挂在分支 bin 的 ghost sink pad（queue 之前），
 * 两者都应该能看到数据；如果这里有而主探针没有，
 * 说明 ghost pad 没激活；两边都没有则是 demuxer 自己不推。
 */
static int on_video_src_probe(GstPad *pad, GstPadProbeInfo *info, void *ud) {
    (void)pad;
    (void)ud;
    if (!g_w.vsrc_logged) {
        void *b = gst_pad_probe_info_get_buffer(info);
        g_w.vsrc_logged = 1;
    }
    return GST_PAD_PROBE_OK;
}

static int on_video_buffer(GstPad *pad, GstPadProbeInfo *info, void *ud) {
    (void)info;
    (void)ud;
    /*
     * 【第二十九轮 v6】首帧打一次 sink 实际协商到的 buffer 尺寸。
     * 绿边排查唯一缺的就是这一行：如果 caps 与 render-rect 相等，绿必来自
     * 源帧边缘（裁源即可）；如果不等，就是平面/管线尺寸对不上，得改别处。
     */
    if (!g_w.vcaps_logged) {
        GstCaps *caps = gst_pad_get_current_caps(pad);
        if (caps) {
            GstStructure *st = gst_caps_get_structure(caps, 0);
            int cw = 0, ch = 0;
            if (st && gst_structure_get_int(st, "width", &cw) &&
                gst_structure_get_int(st, "height", &ch) && cw > 0 && ch > 0) {
                g_w.vcaps_logged = 1;
                pb2_logf("video: sink caps %dx%d, render-rect %s", cw, ch, g_w.rect);
            }
            gst_caps_unref(caps);
        }
    }
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
        /* 首帧时刻单独记一份：check_audio_stall 的「迟迟没有声音」要用它当基准。
         * frame_epoch_ms 一清零就没有起算点了（它的语义是"还在等首帧"）。 */
        g_w.first_frame_ms = pb2_now_ms();
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
        g_w.seek_retry_issued = 0;
        /*
         * 【P1-4】直播恢复出帧 = 重连成功，把连续失败计数清零。
         * 不清的话一次长时间断网攒下的计数会一直占用配额，
         * 之后每次正常的小抖动都会更快触发「放弃」。
         */
        if (g_w.live_reconnect_count > 0) {
            pb2_logf("live: recovered after %d reconnect attempt(s)", g_w.live_reconnect_count);
            g_w.live_reconnect_count = 0;
        }
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
        /*
         * 【首次 seek 必失败的真凶】set_state(NULL) 是**异步**的。
         *
         * 直接 unref 后立刻 build_pipeline()，新管线的 kmssink 会去申请
         * plane-id=76，而旧 kmssink 的 plane 还在释放中 —— 抢不到 plane
         * 就永远没有 video buffer 送进 on_video_buffer，表现为
         * `first frame` 永不出现（真机日志：video branch ready 有、
         * first audio packet 有、但 first frame 没有，15s 后超时），
         * 紧接着 `plane not active yet (active=-1)` 一直报到 streak=4。
         *
         * 真机复现规律非常稳定：**每个会话的第一次 seek 必失败，
         * 第二次/第三次必成功** —— 因为失败那次的管线本来就没拿到 plane，
         * 再拆它时没有 plane 要释放，所以下一次就能立刻拿到。
         *
         * 修法：等状态真正切到 NULL（= sink 已释放 plane）再 unref。
         * 300ms 上界保证 worker 不会被卡住（实测切换 <20ms）。
         */
        /*
         * 【MPP 硬解上下文释放】
         * 先把视频/音频分支单独转 NULL，强制 mppvideodec 走完 dispose。
         * 直接对 pipeline 发 NULL 时，分支 bin 里的硬解器可能还在下游排队，
         * 导致 MPP 上下文在下一条管线建立时还没退干净。
         */
        /*
         * 【绝对不要在这里再 set_state(PAUSED)】
         * set_state(NULL) 已经发出后，任何后续 set_state 都会把 NULL 转换
         * **取消掉**。真机实测过这个错误写法：
         *   [x] teardown: set_state(NULL)
         *   [x] teardown: pipe->PAUSED done in 6ms (cur=2 pend=3)
         *   [x] teardown: state->NULL gr=0 cur=2 pend=3 took 0ms
         * gr=0 是 GST_STATE_CHANGE_FAILURE，cur=2 是 READY —— 管线卡在 READY，
         * kmssink 的 dispose 不执行，DRM plane 76 永远不还给内核。
         * 后果就是「每个会话第一次 seek 必不出首帧」。
         */
        {
            GstState cur = 0, pend = 0;
            GstStateChangeReturn gr;
            long long tw0 = pb2_now_ms();
            gr = gst_element_get_state(g_w.pipe, &cur, &pend,
                                       (GstClockTime)3000 * 1000000ULL);
            pb2_logf("teardown: state->NULL gr=%d cur=%d pend=%d took %lldms",
                     (int)gr, (int)cur, (int)pend, pb2_now_ms() - tw0);
            if (gr == GST_STATE_CHANGE_FAILURE || cur != GST_STATE_NULL) {
                /*
                 * 状态没真正到 NULL 就说明 kmssink 还活着、plane 还没还。
                 * 再补一次 NULL 并等，避免把泄漏带进下一条管线。
                 */
                pb2_logf("teardown: WARN state did not reach NULL (gr=%d cur=%d) -> retry",
                         (int)gr, (int)cur);
                gst_element_set_state(g_w.pipe, GST_STATE_NULL);
                gst_element_get_state(g_w.pipe, &cur, &pend,
                                      (GstClockTime)2000 * 1000000ULL);
                pb2_logf("teardown: retry state->NULL cur=%d pend=%d after %lldms",
                         (int)cur, (int)pend, pb2_now_ms() - tw0);
            }
        }
        gst_object_unref(g_w.pipe);
        g_w.pipe = NULL;
        /*
         * 【历史】这里曾有一个 /tmp/pb2_settle 可调空窗实验（0~30000ms 扫描），
         * 结论是**完全无效** —— 「首次 seek 不出首帧」的真正根因是
         * probe_video_keyframe_ms() 的整数除法把 K 算早了 5 秒（见 src_http.c），
         * 与 plane 交还时序无关。空窗已删除，不要再加回来。
         */
        /*
         * 【首次 seek 必失败的真正根因】DRM plane 的交还是**异步**的。
         *
         * gst_element_get_state(NULL) 只保证 GStreamer 层面状态切到了 NULL，
         * 不保证内核已经把 plane 76 收回。新管线的 kmssink
         * 在旧 plane 还没交还时去申请，申请失败后**不会重试**，
         * 于是 sink pad 不激活 → 分支反压 → demuxer 的 video src pad 被堵住
         * → 一个 video buffer 都不推（真机实测：音频正常、视频探针零 buffer）。
         *
         * 等待判据必须是「summary 里已经没有视频平面」，而不是固定 sleep。
         * 超时 600ms 保底（正常情况下 1~2 个 vblank 就收回了）。
         */
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
    /*
     * 【真正的引用泄漏】vsink / asink 存的是 parse_bin() 返回的**新引用**。
     *
     * 计数推导：parse_bin() 返回时 refcount=1；gst_bin_add() 又 +1（pipeline 自己拿一份）
     * → refcount=2，其中 1 份就是我们存在 g_w.vsink 里的。
     * 以前这里只写 NULL 不 unref → pipeline 析构后 refcount 仍为 1
     * → 分支 bin 和里面的 kmssink **永远不会 finalize**
     * → kmssink 占的 KMS plane 76 永远不释放。
     *
     * 下一条管线的 kmssink 申请不到 plane 就不激活 sink pad
     * → 数据传不下去 → demuxer 也不推 → 探针一个 buffer 都看不到
     * → `first frame` 永不出现 + `plane not active yet (active=-1)`。
     *
     * 为什么只有第一次 seek 失败：第一次 seek 拆的是 open 建的那条管线（它真正拿到了 plane 76），
     * 泄漏就发生在这里；而第一次 seek 失败后建的那条管线**本来就没拿到 plane**，
     * 第二次 seek 拆它时没东西可泄漏，所以又能拿到 plane → 看起来「第二、三次都正常」。
     */
    /*
     * 【重要：这里绝对不能 unref】
     *
     * gst_parse_bin_from_description() 返回的是 **floating 引用**，
     * gst_bin_add() 会把它 sink 掉 —— 也就是 pipeline 成为唯一持有者。
     * 我们存在 g_w.vsink 里的是一个**借用指针**。
     *
     * 曾经在这里加了 gst_object_unref(g_w.vsink)「修泄漏」，结果是把 pipeline
     * 唯一的那份引用减到 0 → 分支 bin 和里面的 kmssink 被**提前析构**，
     * 它们来不及走 PLAYING→NULL 的状态转换，kmssink 的 dispose 不执行
     * → 它占的 DRM plane 76 永远不还给内核（真机实测：此后 debugfs summary
     * 里 `Esmart1-win0` 再也不出现，连续 22 秒采样全为 0）。
     *
     * 分支 bin 的生命周期完全由 pipeline 管：teardown 里 set_state(NULL)
     * + 同步等待 + unref(pipe) 就够了。这里只清指针。
     */
    g_w.vsink = NULL;
    /*
     * 【真正的引用泄漏】vsink_el / vvol 都是 gst_bin_get_by_name() 拿到的
     * **新引用**，以前这里只清指针不 unref —— kmssink 永远不会 finalize，
     * 它占的 KMS plane 76 也就永远不释放。下一条管线的 kmssink 申请
     * 不到 plane 就不会激活 sink pad，数据流不下去，表现就是
     * `plane not active yet (active=-1)` 且 `first frame` 永不出现。
     */
    if (g_w.vsink_el_owned && g_w.vsink_el) {
        gst_object_unref(g_w.vsink_el);
    }
    g_w.vsink_el = NULL;
    g_w.vsink_el_owned = 0;
    g_w.rect_hidden = 0;
    /* 管线没了，迟到宽限自然失效；不清会让下一次 arm 之前误判「已过期」 */
    g_w.late_grace_until_ms = 0;
    g_w.late_grace_armed = 0;
    g_w.asink = NULL;   /* 同 vsink：floating 引用已被 gst_bin_add sink 掉，借用指针不 unref */
    if (g_w.vvol) {
        gst_object_unref(g_w.vvol);
        g_w.vvol = NULL;
    }
    /*
     * 【KMS plane 76 交接窗口 —— 首次 seek 必失败的真正原因】
     *
     * 顺序极其关键：kmssink 占用的 DRM plane 只有在 **kmssink 真正 finalize**
     * 时才会被释放；而 kmssink 的 finalize 又被我们手里的两个额外引用卡住
     * （g_w.vsink 是 parse_bin 的新引用、g_w.vsink_el 是 get_by_name 的新引用）。
     *
     * 所以等待必须放在 **unref 之后**。之前放在 unref 之前（紧跟 set_state(NULL)），
     * 等多久都没用 —— 真机实测 settle=8000ms 依然拿不到 plane，日志可以证明。
     *
     * 释放之后还要给内核一点时间完成 atomic commit（关 plane），
     * 否则新管线的 kmssink 在同一个 vblank 内申请 plane 76 会失败，
     * 而 kmssink 申请失败**不重试** → sink pad 不激活 → 分支反压
     * → demuxer 的 video src pad 被堵 → 一个 video buffer 都不推
     * → `first frame` 永不出现 + `plane not active yet (active=-1)`。
     *
     * 这正好解释「只有第一次 seek 失败」：第一次拆的是 open 建的那条
     * **真正持有 plane 76** 的管线；失败后建的那条本就没拿到 plane，
     * 第二次拆它时无 plane 要释放，所以又能立刻拿到。
     */
    /*
     * unref(pipe) 之后给内核 120ms 完成 atomic commit（关 plane）。
     * 真机实测：kmssink 申请不到 plane 时**不重试**，所以新管线必须在
     * 旧 plane 真正回到 free 列表之后才能建。120ms 覆盖 1~2 个 vblank。
     */
    pb2_sleep_ms(120);
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

/*
 * 直播判据。
 *
 * 【P0-3】现在主判据是 **JS 显式传的 req->isLive**（1=直播 / 0=点播 /
 * -1=老版本 JS 没传）。以前的启发式「durationMs == 0 且是 http」判据过宽：
 * B 站 view 接口对互动视频、部分港澳台/番剧条目会返回 duration: 0，
 * JS 侧 `parsed.durationMs || durationSec*1000` 就落 0 → **点播被接成直播**
 * → 走 souphttpsrc ! flvdemux 而 DASH 分片不是 FLV → flvdemux 建不出 pad
 * → 无画面，且永久失去 seek（do_seek 对直播早退）。用户表现是
 * 「视频放不了，也不能拖进度条」，且完全没有报错。
 *
 * 只有 isLive == -1（老 JS / 直接调 native 的场景）才回落到启发式，并且
 * 要求「durationMs == 0 + http + 路径以 .flv 结尾」三条同时成立，
 * 否则按点播处理 —— 宁可让直播走点播路径（会报错、可见），
 * 也不能让点播静默走直播路径（无画面、无 seek、无错误）。
 *
 * 注意 url_ends_flv 必须**先剥掉 query**，并且**必须用 strncmp 比 4 个字符**：
 * B 站直播 URL 实际长这样
 *   https://.../live-bvc/xxx.flv?expires=...&sign=...
 * 旧代码直接 `strcmp(s + n - 4, ".flv")` 有两个坑叠在一起 ——
 *   ①没剥 query：尾部 4 字符是 `...` 之类，永远不匹配；
 *   ②就算剥了 query，strcmp 比的是**整个剩余字符串**，`.flv?expires=...`
 *     也不等于 `.flv`。所以必须 strncmp(..., ".flv", 4)。
 * 这两个坑叠加的结果就是日志里恒为 `url ends .flv=0`，连佐证都做不到。
 */
static int url_ends_flv(const char *s) {
    size_t n;
    const char *q;
    if (!s) return 0;
    q = strchr(s, '?');
    n = q ? (size_t)(q - s) : strlen(s);
    return n >= 4 && strncmp(s + n - 4, ".flv", 4) == 0;
}

static int is_live_request(const pb2_open_req_t *req) {
    if (!req) return 0;
    if (req->isLive == 1) return 1;
    if (req->isLive == 0) return 0;
    /* isLive == -1：老版本 JS，回落到严格启发式 */
    if (req->durationMs != 0) return 0;
    if (!is_http(req->input)) return 0;
    if (!url_ends_flv(req->input)) {
        pb2_logf("live: WARN isLive 未传且 URL 非 .flv -> treated as VOD "
                 "(durationMs=0 http 不再单独判直播)");
        return 0;
    }
    return 1;
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
    /*
     * 【直播直连】souphttpsrc ! flvdemux，完全绕开 ffmpeg + FIFO。
     * 判据与理由见 pb2_worker_t.is_live 的注释。这里必须**在**
     * pb2_src_open() 之前分流，否则会先起一个注定卡死的 ffmpeg。
     */
    g_w.is_live = is_live_request(req);
    if (g_w.is_live) {
        /*
         * g_w.src 清零：直播没有 pb2_src_t 语义（无 ffmpeg、无 FIFO），
         * 但结构体可能残留上一次点播会话的 is_http/baseMs/pid 等字段。
         * 虽然直播路径上所有消费点都已用 is_live 或 src_ready 挡掉，
         * 清零是防止将来新增代码误读（尤其 pb2_src_alive/reap_if_dead
         * 这类按 pid 判活的函数）。
         */
        memset(&g_w.src, 0, sizeof(g_w.src));
        g_w.src.hold_fd = -1;
        g_w.src.hold_fd2 = -1;
        g_w.base_ms = 0;
        g_w.src_ready = 0;
        /*
         * 【P1-5】直播 URL 实测 600~900 字符，loc1 只有 1200。
         * 一旦 URL 里有需要转义的字符（`"` 或反斜杠）导致膨胀越界，
         * 以前会静默截断，souphttpsrc 拿半截 location 去请求 → CDN 403，
         * 用户看到「直播永远转圈」，日志里只有 soup 的错误，看不出根因。
         * 现在明确判失败：打一条能一眼定位的日志 + patch_error，
         * 直接返回 -1 让 do_open/recover 走统一的失败路径。
         */
        if (desc_escape(req->input, loc1, sizeof(loc1)) != 0) {
            pb2_logf("live: URL TOO LONG for desc buffer (input=%u bytes, cap=%u) "
                     "-> escaped location truncated, refusing to start",
                     (unsigned)strlen(req->input), (unsigned)sizeof(loc1));
            pb2_stats_patch_error(&g_w.stats, PB2_E_OPEN_FAILED,
                                  "live url too long (escaped location truncated)");
            return -1;
        }
        loc2[0] = '\0';
        pb2_logf("live: direct gst path (durationMs=%d, url ends .flv=%d)",
                 (int)req->durationMs, url_ends_flv(req->input));
        /*
         * desc 里只写 location。
         *
         * 为什么 `user-agent` / `extra-headers` 不写进 desc：
         *   - `extra-headers` 的属性类型是 **GstStructure**
         *     （`gst-inspect-1.0 souphttpsrc` 报 `Boxed pointer of type
         *     "GstStructure"`），不是 string。写 `(string)"..."` 会被
         *     gst_util_set_object_arg 拒绝并静默不生效；写裸
         *     `extra-headers,Referer=...` 又会在 desc 词法层和后面的
         *     `! flvdemux` 撞在一起（逗号/空格语义不同），很容易解析成
         *     别的属性。
         *   - 所以这里 desc 只留 location（唯一需要转义的字符串），
         *     UA 与 Referer 在 parse 成功后用 gst_util_set_object_arg
         *     直接设到 souphttpsrc 元素上（见下面 live 分支的收尾）。
         *   - 结构体名必须打头：`extra-headers,Referer=<url>`。只写
         *     `Referer=<url>` 会被当成「无字段名的单值」，静默不生效，
         *     而 B 站直播 CDN 不带 Referer 直接 403（早前实测过）。
         */
        n = snprintf(desc, sizeof(desc),
                     "souphttpsrc name=src location=\"%s\" ! flvdemux name=vd", loc1);
        off = (size_t)(n > 0 ? n : 0);
        goto parse_it;
    }

    if (!src_already_open) {
        if (pb2_src_open(&g_w.src, req->input, req->input2, req->userAgent, req->referer,
                         req->startMs, req->durationMs) != 0) {
            pb2_stats_patch_error(&g_w.stats, PB2_E_OPEN_FAILED, "media source open failed");
            return -1;
        }
        g_w.src_ready = 1;
    }
    g_w.base_ms = g_w.src.is_http ? g_w.src.baseMs : 0;

    /*
     * 【P1-5】点播这里的 location 要么是本地路径、要么是 /tmp/pb2/ 下的 fifo
     * （ffmpeg 那条极长的 CDN URL 不会进 desc），正常不可能触顶。
     * 万一真触顶就告警但不阻断 —— 点播拿半截 FIFO 路径会 open 失败并被
     * check_first_frame 的超时兜住，没必要在这里额外改变错误路径。
     */
    if (desc_escape(g_w.src.location, loc1, sizeof(loc1)) != 0) {
        pb2_logf("WARNING: location truncated to %u bytes (input=%u)",
                 (unsigned)sizeof(loc1), (unsigned)strlen(g_w.src.location));
    }
    if (desc_escape(g_w.src.location2, loc2, sizeof(loc2)) != 0) {
        pb2_logf("WARNING: location2 truncated to %u bytes (input=%u)",
                 (unsigned)sizeof(loc2), (unsigned)strlen(g_w.src.location2));
    }

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
            char vpost[256];
            video_post_rate_desc(vpost, sizeof(vpost));
            n = snprintf(desc, sizeof(desc),
                         "filesrc location=\"%s\" ! %s ! mppvideodec rotation=%d ! "
                         "videorate ! %s"
                         "kmssink name=vsink %sdriver-name=rockchip sync=true "
                         "max-lateness=%d qos=true force-aspect-ratio=false "
                         "skip-vsync=true render-rectangle=%s",
                         loc1, parser, decoder_rotation(), vpost, kms_plane_arg(),
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
     * 【直播直连】souphttpsrc 的 UA / Referer 在这里补设。
     *
     * 为什么不写在 desc 里（重要，改之前先看这段）：
     *   - `extra-headers` 的属性类型是 **GstStructure**
     *     （`gst-inspect-1.0 souphttpsrc` 报 `Boxed pointer of type
     *     "GstStructure"`），不是 string，desc 里无论写
     *     `(string)"..."` 还是裸 `extra-headers,Referer=...` 都会在
     *     gst_parse_launch 的词法层被切碎（逗号是属性分隔符，而结构体
     *     名本身要打头），实测两条写法都设不进去。
     *   - `gst_util_set_object_arg` 用的是 GstStructure 文本语法，
     *     对 boxed 属性走的就是「结构体名打头」的形式，所以
     *     `extra-headers,Referer=<url>` 这一种写法才是对的 —— 只写
     *     `Referer=<url>` 会被当成「无字段名的单值」，静默不生效，
     *     而 B 站直播 CDN 不带 Referer 直接 403（早前实测过）。
     * 注意这是 parse 之后、set_state 之前，souphttpsrc 还没开始请求。
     */
    if (g_w.is_live) {
        GstElement *src_el = gst_bin_get_by_name((GstBin *)g_w.pipe, "src");
        if (src_el) {
            char hv[768];
            const char *ua = (req->userAgent && req->userAgent[0])
                                 ? req->userAgent
                                 : "Mozilla/5.0 (Linux; Android 9) player2";
            const char *rf = (req->referer && req->referer[0]) ? req->referer
                                                              : "https://www.bilibili.com";
            gst_util_set_object_arg(src_el, "user-agent", ua);
            snprintf(hv, sizeof(hv), "extra-headers,Referer=%s", rf);
            gst_util_set_object_arg(src_el, "extra-headers", hv);
            pb2_logf("live: souphttpsrc ua=\"%.40s...\" extra-headers=%.120s", ua, hv);
            /*
             * timeout / retries 显式写死，不依赖默认值。
             * 设备实测的默认值恰好就是 timeout=15（秒，0=不超时）、
             * retries=3，所以这里写的是同一组值 —— 目的是把「直播要的
             * 是长连接而不是点播式重试」这件事钉在代码里：以后若有人调
             * souphttpsrc 的默认值，直播这条路径不会跟着变。
             */
            gst_util_set_object_arg(src_el, "timeout", "15");
            gst_util_set_object_arg(src_el, "retries", "3");
            gst_object_unref(src_el);
        } else {
            pb2_logf("live: WARNING souphttpsrc element 'src' not found in pipeline");
        }
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
            /*
             * 裸流分支没有外层 bin，kmssink 本体就是 vsink 也是 vsink_el。
             * 两个指针指向**同一个新引用**，所以只能由 vsink_el_owned
             * 路径 unref 一次，否则会 double free。
             * 这里 g_w.vsink 存的是借用指针（不另外 unref）。
             */
            g_w.vsink = vs;
            g_w.vsink_el = vs;
            g_w.vsink_el_owned = 1;
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

/*
 * 非阻塞取一条 bus ERROR 并格式化成文本。返回 1 = 取到了。
 *
 * 【为什么必须在 do_open 内部自己取】
 * worker 的 poll_bus() 只在主循环里跑，而 do_open 的起播缓冲门是**阻塞**
 * 轮询（最长 4s）。这 4s 里管线报的错（典型：「Failed to get plane 76」）
 * 没有任何人读 bus，于是：
 *   - 日志里看不到真实原因；
 *   - lastError 一直是 0，最后被 set_state(PLAYING) 的 FAILURE 覆盖成
 *     一句毫无信息量的 "set_state(PLAYING) failed"。
 * 真机 3.6.2 的日志就是这个样子（见 docs/x7pro-adaptation.md §3.6）。
 */
static int peek_bus_error(char *out, size_t cap) {
    GstMessage *msg;
    GError *ge = NULL;
    char *dbg = NULL;

    if (!g_w.bus || !out || cap < 2) return 0;
    msg = gst_bus_timed_pop_filtered(g_w.bus, (GstClockTime)0, GST_MESSAGE_ERROR);
    if (!msg) return 0;
    out[0] = '\0';
    gst_message_parse_error(msg, &ge, &dbg);
    if (ge) {
        snprintf(out, cap, "%s%s%s",
                 ge->message ? ge->message : "bus error",
                 (dbg && dbg[0]) ? " | " : "",
                 (dbg && dbg[0]) ? dbg : "");
        g_error_free(ge);
    } else {
        snprintf(out, cap, "bus error");
    }
    if (dbg) g_free(dbg);
    gst_message_unref(msg);
    return 1;
}

/*
 * OPEN 失败的统一出口：能兜底就兜底，不能就报错。
 *
 * 兜底条件 = 本次用了**显式 plane-id** 且还没兜过。做法是把意图记在
 * g_w.want_plane_retry 上交给 process_cmd —— 不能在 do_open 里递归调用
 * 自己：它开头会 pb2_open_req_free(g_w.req)，而 g_w.req 正是这里要复用的
 * 请求对象，递归第一句就 use-after-free。
 */
static int open_fail_or_retry(const char *why) {
    if (!g_w.plane_fallback_done && g_w.kms_plane_id > 0) {
        g_w.plane_fallback_done = 1;
        g_w.want_plane_retry = 1;
        pb2_logf("OPEN fail (%s) with plane-id=%d -> retry once with auto plane + videoscale",
                 why, g_w.kms_plane_id);
        /* 回 OPENING：JS 侧看到的是「还在起播」，不会弹错 */
        pb2_stats_patch_error(&g_w.stats, PB2_E_NONE, "");
        pb2_stats_patch_state(&g_w.stats, PB2_STATE_OPENING);
        teardown_pipeline();
        g_w.frame_epoch_ms = 0;
        return 1; /* 已安排重试 */
    }
    pb2_stats_patch_error(&g_w.stats, PB2_E_OPEN_FAILED, why);
    pb2_stats_patch_state(&g_w.stats, PB2_STATE_ERROR);
    teardown_pipeline();
    g_w.frame_epoch_ms = 0;
    return 0;
}

/* 这条 bus 错误是不是「音频设备打不开」类？ */
static int is_audio_bus_error(const char *msg) {
    char low[256];
    size_t i;
    if (!msg) return 0;
    for (i = 0; i + 1 < sizeof(low) && msg[i]; i++) {
        char c = msg[i];
        low[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    low[i] = '\0';
    /* 真机文案："Could not open audio device for playback. Device is being
     * used by another application." / 资源路径 /ext/alsa/gst-alsasink */
    if (strstr(low, "audio device") || strstr(low, "alsasink") || strstr(low, "alsa") ||
        strstr(low, "pcm") || strstr(low, "could not open audio")) {
        return 1;
    }
    return 0;
}

/*
 * 音频设备被占时的降级路径。
 *
 * 真机现象（2026-11-08 00:25 日志）：
 *   start gate: bus error in 705ms (v=1/1 frames=4 ...)
 *   bus error -> Could not open audio device for playback.
 *                Device is being used by another application.
 * 视频分支 855ms 就上屏了，**画面完全正常**，但 alsasink 打不开 → 整条管线
 * 被错误终止 → 用户看到「播放错误」。这就是「有概率没声音」的真身：
 * 设备被系统/上一个会话占着的概率性事件。
 *
 * 策略（最多两次重建）：
 *   第一次：等 400ms 再带音频重开（alsasink 关闭/系统提示音结束后通常就能开）
 *   第二次：仍然被占 -> 关掉音频只播视频，并在 status().audioOff 上报，
 *           页面提示「无声：音频设备被占用」。画面绝不因此中断。
 */
static int open_audio_retry(void) {
    if (g_w.audio_retry_stage == 0) {
        g_w.audio_retry_stage = 1;
        g_w.want_audio_retry = 1;
        g_w.reopen_delay_ms = 400; /* 给上一个 alsasink/系统提示音释放的时间 */
        pb2_logf("OPEN audio: device busy -> retry in 400ms (video was fine)");
        pb2_stats_patch_error(&g_w.stats, PB2_E_NONE, "");
        pb2_stats_patch_state(&g_w.stats, PB2_STATE_OPENING);
        teardown_pipeline();
        g_w.frame_epoch_ms = 0;
        return 1;
    }
    if (g_w.audio_retry_stage == 1) {
        g_w.audio_retry_stage = 2;
        g_w.audio_off = 1;
        g_w.want_audio_retry = 1;
        g_w.reopen_delay_ms = 0;
        pb2_logf("OPEN audio: still busy -> continue VIDEO-ONLY (audioOff=1)");
        pb2_stats_patch_error(&g_w.stats, PB2_E_NONE, "");
        pb2_stats_patch_state(&g_w.stats, PB2_STATE_OPENING);
        teardown_pipeline();
        g_w.frame_epoch_ms = 0;
        return 1;
    }
    /* 已经降级为无音频还报音频错？按普通失败处理，避免死循环 */
    pb2_stats_patch_error(&g_w.stats, PB2_E_OPEN_FAILED, "open: audio device busy");
    pb2_stats_patch_state(&g_w.stats, PB2_STATE_ERROR);
    teardown_pipeline();
    g_w.frame_epoch_ms = 0;
    return 0;
}

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
    /*
     * 【X7 Pro 适配】设备显示参数在每次 OPEN 时重新取，确保 seek 重建、
     * 直播重连都用同一套值。老版本 JS 不传（panelOffset=-1/planeId=0）时
     * 与 X6 Pro 行为逐字节一致。
     */
    g_w.panel_x_offset = (req->panelOffset >= 0) ? req->panelOffset : PB2_PANEL_X_OFFSET;
    g_w.kms_plane_id = (req->planeId == 0) ? PB2_KMS_PLANE_ID : req->planeId;
    g_w.sw_scale = (req->swScale >= 1 && req->swScale <= 3) ? req->swScale : 0;
    g_w.buf_scale = (req->bufScale >= 50 && req->bufScale <= 400) ? req->bufScale : 100;
    g_w.crop_x = (req->cropX > 0 && req->cropX <= 2048) ? req->cropX : 0;
    g_w.crop_y = (req->cropY > 0 && req->cropY <= 2048) ? req->cropY : 0;
    /*
     * 【平面兜底重试】上一次 OPEN 已经判定「显式平面不可用」。这次不再用它，
     * 直接切「自动挑平面 + 软件缩放」。注意必须写在上面两行之后 ——
     * 否则会被请求里的 planeId 覆盖回去，重试等于没做。
     */
    if (g_w.want_plane_retry) {
        g_w.want_plane_retry = 0;
        g_w.kms_plane_id = -1;
        g_w.sw_scale = 1;
        pb2_logf("OPEN retry: force planeId=auto + videoscale");
    }
    pb2_logf("OPEN device profile: panelXOffset=%d planeId=%s swScale=%d bufScale=%d%%",
             g_w.panel_x_offset, g_w.kms_plane_id < 0 ? "auto" : "fixed", g_w.sw_scale,
             g_w.buf_scale);
    snprintf(g_w.rect, sizeof(g_w.rect), "<%d,%d,%d,%d>",
             g_w.panel_x_offset + req->x, req->y, req->w, req->h);
    g_w.vcaps_logged = 0;
    /* videoscale 的输出 caps 必须是偶数宽高（NV12），且与 render-rectangle
     * 严格等大。差 1px 都会让 kmssink 再拉一次，画面比例就毁了。 */
    normalize_video_rect("OPEN");
    /* 排障必备：逻辑矩形 + 真正下发的面板矩形。自检页「日志」里能看到，
     * 对不上就是「画面贴边/被裁」的直接证据。 */
    pb2_logf("OPEN rect: logical=%d,%d,%d,%d -> panel %s (offset=%d)",
             req->x, req->y, req->w, req->h, g_w.rect, g_w.panel_x_offset);
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
    /* 【P1-4】新会话：清掉上一次直播重连的计数与排期 */
    g_w.live_reconnect_pending = 0;
    g_w.live_reconnect_count = 0;
    g_w.live_reconnect_at_ms = 0;
    /* 【P1-1】新会话：清掉上一次的源死亡标记与独立节流器 */
    g_w.src_died = 0;
    g_w.src_died_at_ms = 0;
    g_w.last_src_check2_ms = 0;
    g_w.last_src_check_ms = 0;
    g_w.first_frame_logged = 0;
    g_w.first_audio_logged = 0;
    g_w.first_frame_retries = 0;
    g_w.seek_frame_retries = 0;
    g_w.seek_retry_issued = 0; /* 每个新会话重新给满自愈配额 */
    g_w.audio_stall_ab = 0;
    g_w.audio_stall_ms = 0;
    g_w.audio_stall_frames0 = 0;
    g_w.audio_stall_heals = 0; /* 每个新会话重新给满音频自愈配额 */
    /*
     * 【无声回归】OPEN 也必须重置下面三个，不能只依赖 STOP/RELEASE 的
     * reset_session_fields()：
     *   - audio_heals：上一支视频救满 2 次后如果不 STOP 直接开下一支，
     *     配额会被带过去 → 新视频一次都不救，还可能被 audioDead 误报。
     *   - first_frame_ms：不清就会沿用上一支的首帧时刻，新视频的音轨若
     *     慢启动会被「首帧后 4.5s 没声音」当场判死。
     *   - plane_fallback_done：上一次 OPEN 触发过「显式平面不可用 → 自选」
     *     后，下次 OPEN 会重新用平面 76；标志不复位就再也没有兜底，
     *     表现是同一台机器「第一支能放，后面全黑」。
     */
    g_w.audio_heals = 0;
    g_w.first_frame_ms = 0;
    g_w.plane_fallback_done = 0;
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
    g_w.bt_ts_offset_ns = read_bt_ts_offset_ns();
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
     * 音量来源见下面那段（已改成按路由分流，蓝牙以 AVRCP 为权威）。
     *
     * 【2026-11-04 真机实测，三次纠错】
     *   1) 只读 sys_config.conf 的 spk_volume：conf 落盘要 1~12s，读到旧值 0 → 静音。
     *   2) 蓝牙改读 bluealsa AVRCP：AVRCP 与软件增益串联 → 0.76×0.76 = 58% 双重衰减。
     *   3) 两条路由都读 card1 Master 并镜像给耳机：**错**。Master 是扬声器功放，
     *      蓝牙路由下可以和耳机音量长期不一致（实测 Master=0% / AVRCP=50%），
     *      起播把 AVRCP 覆盖成 0% → 用户「点开视频音量自动变 0%，重拉才有声」。
     *   现在：扬声器 = Master；蓝牙 = AVRCP 为权威，只有滑杆真的动了才镜像过去。
     */
    /*
     * 音量基线：
     *   - 扬声器路由：vol_pct = 系统滑杆（card1 Master），软件增益跟随它；
     *   - 蓝牙路由：vol_pct = **耳机自己的 AVRCP**，软件增益固定 1.0。
     * 起播时绝不拿 Master 去覆盖 AVRCP —— 见 master_last 的注释。
     */
    g_w.vol_pct = read_route_volume();
    g_w.vol_applied = -1;
    g_w.master_last = pb2_mixer_volume_read(NULL);
    if (g_w.bt_present && g_w.adev[0]) {
        int av = g_w.bt_ctl_name[0] ? pb2_mixer_volume_read(g_w.bt_ctl_name) : -1;
        if (av >= 0) {
            g_w.vol_pct = av;
            g_w.bt_vol_applied = av; /* 与耳机实际值一致 -> 起播不重复下发 */
            pb2_logf("audio: volume authority = AVRCP %d%% (slider baseline %d%%)",
                     av, g_w.master_last);
        } else {
            g_w.bt_vol_applied = -1;
            pb2_logf("audio: AVRCP unreadable -> fallback to slider %d%%", g_w.vol_pct);
        }
    } else {
        g_w.bt_vol_applied = -1;
    }
    g_w.vol_check_ms = pb2_now_ms();
    g_w.bt_check_ms = pb2_now_ms();
    g_w.route_pending = 0;

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

    int built = build_pipeline(req, 0);
    if (built != 0) {
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

    /*
     * ================== 起播缓冲门（用户方案，2026-11-01） ==================
     *
     * 用户反馈：「为什么不直接加播放缓冲，在点击播放时缓冲 1-2 秒后，
     * 音频和视频同时起播」。
     *
     * 之前的做法（直接 PLAYING + 起播迟到宽限）只能让积压帧**补播**，
     * 用户看到的是「先黑屏 → 画面低帧率追赶 → 2 秒后才正常」。
     *
     * 根因：alsasink 的 preroll 只拿到第一个音频缓冲就完成，管线时钟立刻
     * 起跑；视频分支（matroskademux 解析首簇 + mppvideodec 初始化 + 硬解
     * 首帧）晚 900~2100ms 才出帧，这段时间的帧全部「迟到」。
     *
     * 做法：管线先进 PAUSED —— **时钟不走**，所以没有任何帧会迟到；
     * 分支由 pad-added 动态建，sync_state_with_parent 会把它们同步到
     * PAUSED，各自完成 preroll（视频 = 首帧已解码待显示，音频 = 首个
     * PCM 缓冲已就绪）。等两个分支都就绪再一起切 PLAYING，两路从同一
     * 时刻起跑，天然对齐，没有追赶期。
     *
     * ⚠️ 不能只靠 get_state 判 preroll：desc 里**没有 sink**（分支全靠
     * pad-added），PAUSED 会立即返回 SUCCESS，那时一个分支都还没建，
     * 等于没缓冲。必须自己轮询分支就绪标志。
     *
     * 就绪判据：
     *   - 视频：vbranch 且 frames > 0（首帧真的解出来了）
     *   - 音频：有音轨时必须 abranch；无音轨（req->audio == 0）不等
     * 超时（PB2_START_GATE_TIMEOUT_MS）无条件放行，退化成旧行为，
     * 绝不让用户看到错误或永久等待。
     *
     * seek 不走这个门：seek 有 SEEK first frame 指标，实测直接 PLAYING
     * 只要 413~516ms，加门只会更慢。
     */
    sr = gst_element_set_state(g_w.pipe, GST_STATE_PAUSED);
    pb2_logf("set_state(PAUSED) -> %d (+%lldms)", sr, pb2_now_ms() - t0);

    if (sr == GST_STATE_CHANGE_FAILURE) {
        pb2_logf("set_state(PAUSED) FAILURE -> %s",
                 open_fail_or_retry("set_state(PAUSED) failed") ? "retrying with auto plane"
                                                                : "error");
        return;
    }

    {
        long long t_gate = pb2_now_ms();
        long long gate_ms = g_w.adev[0] ? PB2_START_GATE_TIMEOUT_BT_MS
                              : PB2_START_GATE_TIMEOUT_MS;
        int want_audio = req->audio ? 1 : 0;
        int ready = 0;
        int gate_err = 0;             /* 1 = 门内读到 bus ERROR，提前中止 */
        int audio_err = 0;            /* 1 = 错误来自音频设备（可降级，不判死） */
        char errbuf[192];
        errbuf[0] = '\0';
        /*
         * 诊断覆盖：/tmp/pb2_startgate 写毫秒数可改超时（0 = 关闭缓冲门，
         * 用于 A/B 对比「加门 vs 不加门」的起播表现）。
         */
        {
            FILE *gf = fopen("/tmp/pb2_startgate", "r");
            if (gf) {
                long long v = -1;
                if (fscanf(gf, "%lld", &v) == 1 && v >= 0 && v <= 30000) gate_ms = v;
                fclose(gf);
            }
        }
        while (pb2_now_ms() - t_gate < gate_ms) {
            int v_ok = g_w.vbranch &&
                       atomic_load_explicit(&g_w.probe.frames, memory_order_relaxed) > 0;
            int a_ok = want_audio ? g_w.abranch : 1;
            /*
             * 【3.6.3 关键】门内必须自己读 bus ERROR。
             * 抢不到平面时管线会在几十毫秒内报错（sink 起不来），但 worker
             * 正卡在这个循环里，poll_bus() 根本没机会跑 —— 于是 4s 白等，
             * 最后报成 "set_state(PLAYING) failed"，真实原因丢失。
             * 这里一旦读到错误就立刻中止，并把原文带进兜底/报错。
             */
            if (peek_bus_error(errbuf, sizeof(errbuf))) {
                pb2_logf("start gate: bus error -> %s", errbuf);
                /*
                 * 【音频错误不判死整条管线】alsasink 打不开（设备被占）时
                 * 视频分支往往已经出帧了（真机：855ms 就上屏）。此时把整条
                 * 管线判 ERROR，用户丢掉的是**画面**，代价远大于收益。
                 * 先交给 open_audio_retry() 走「等 400ms 重试 → 不行就无声」。
                 */
                if (is_audio_bus_error(errbuf)) { audio_err = 1; break; }
                /*
                 * 直播不在 OPEN 阶段判死：直播的断流/错误归 live_reconnect
                 * 管（它有重连上限），这里判 ERROR 反而会让用户看到
                 * 「播放错误」而失去自动恢复的机会。继续等到门超时即可。
                 */
                if (!g_w.is_live) { gate_err = 1; break; }
                pb2_sleep_ms(20);
                continue;
            }
            if (v_ok && a_ok) { ready = 1; break; }
            pb2_sleep_ms(20);
        }
        pb2_logf("start gate: %s in %lldms (v=%d/%lld frames=%lld a=%d/%d want_audio=%d)",
                 gate_err ? "bus-error" : (ready ? "both ready" : "timeout"),
                 pb2_now_ms() - t_gate,
                 g_w.vbranch ? 1 : 0, (long long)1,
                 (long long)atomic_load_explicit(&g_w.probe.frames, memory_order_relaxed),
                 g_w.abranch ? 1 : 0, want_audio, want_audio);
        if (audio_err) {
            open_audio_retry();
            return;
        }
        if (gate_err) {
            /* 走统一出口：显式平面时自动切 auto+videoscale 重来一次 */
            open_fail_or_retry(errbuf);
            return;
        }
    }

    sr = gst_element_set_state(g_w.pipe, GST_STATE_PLAYING);
    pb2_logf("set_state(PLAYING) -> %d (+%lldms)", sr, pb2_now_ms() - t0);

    if (sr == GST_STATE_CHANGE_FAILURE) {
        /* 显式 plane-id 抢不到平面时，这里就是第一现场（PAUSED 异步阶段
         * sink 起不来 → 返回 FAILURE）。统一出口会先兜底一次再决定报错。 */
        pb2_logf("set_state(PLAYING) FAILURE -> %s",
                 open_fail_or_retry("set_state(PLAYING) failed") ? "retrying with auto plane"
                                                                 : "error");
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
            /*
             * 【误判「播放错误」的真凶】start gate 已经等到「视频有帧 + 音频
             * 分支就绪」才会走到这里，preroll 事实上早就完成了。但 desc 是
             * 没有 sink 的动态管线（分支全靠 pad-added 建），
             * gst_element_get_state() 对它会**瞬态返回 FAILURE**：
             *
             *   真机日志（pid 26953）：
             *     start gate: both ready in 2888ms (v=1/1 frames=14 a=1/1)
             *     set_state(PLAYING) -> 2
             *     get_state -> 0 cur=3 pend=4     ← 1ms，cur=PAUSED/pend=PLAYING
             *     OPEN failed: preroll failure (sr=0) -> error
             *
             * cur/pend 说明状态机正在往 PLAYING 走。这里如果照旧判失败，
             * teardown 会拆掉刚建好的分支再重建 —— 重建链里有一次丢掉了
             * 音频分支，表现就是「播到一半声音没了」（ab 冻结、画面继续）。
             *
             * 因此：只要「视频分支有帧 + 音频分支就绪（或本来就不要音频）+
             * 状态机不是往 NULL/READY 退」就认可它，并明确打点。
             */
            int want_a = req->audio ? 1 : 0;
            int v_ok = g_w.vbranch &&
                       atomic_load_explicit(&g_w.probe.frames, memory_order_relaxed) > 0;
            int a_ok = g_w.abranch || !want_a;
            int heading_playing = (cur == GST_STATE_PLAYING) ||
                                  (pend == GST_STATE_PLAYING) ||
                                  (cur == GST_STATE_PAUSED && pend == GST_STATE_VOID_PENDING);
            if (v_ok && a_ok && heading_playing) {
                pb2_logf("get_state sr=%d cur=%d pend=%d but frames flowing "
                         "(v=%d a=%d) -> treat as playing",
                         sr, cur, pend, g_w.vbranch ? 1 : 0, g_w.abranch ? 1 : 0);
            } else {
                pb2_logf("OPEN failed: preroll %s (sr=%d cur=%d pend=%d v=%d/%lld a=%d/%d)",
                         sr == GST_STATE_CHANGE_ASYNC ? "timeout" : "failure", sr,
                         cur, pend,
                         g_w.vbranch ? 1 : 0,
                         (long long)atomic_load_explicit(&g_w.probe.frames, memory_order_relaxed),
                         g_w.abranch ? 1 : 0, want_a);
                open_fail_or_retry("open: preroll timeout");
                return;
            }
        }
    }

    /* probe 由 on_pad_added 在分支建起来时挂（那时才能拿到 ghost sink pad）。
     * 这里不调用 attach_probes —— 此刻 vsink/asink 必然还是 NULL。 */

    /*
     * 起点不是 0 时立刻 seek。
     * **只在本地文件路径做**：网络源的起点已经由 ffmpeg -ss 实现，
     * 对 FIFO 再 seek 一次会失败（push 模式无法 seek），反而可能破坏流。
     */
    if (req->startMs > 0 && !g_w.is_live && !g_w.src.is_http) {
        gst_element_seek_simple(g_w.pipe, GST_FORMAT_TIME,
                                GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                                (long long)req->startMs * 1000000LL);
        pb2_logf("initial seek to %lldms (+%lldms)", req->startMs, pb2_now_ms() - t0);
    }

    pb2_stats_patch_state(&g_w.stats, PB2_STATE_PLAYING);
    pb2_mp3_marker_set();
    g_w.last_marker_ms = pb2_now_ms();
    /* 【起播迟到宽限】视频分支晚 900~2100ms 才出帧，这段时间不判迟到 */
    /*
     * 【直播不 arm 迟到宽限】
     * 直播的 desc 里已经把 max-lateness 设成 -1 / qos=false（丢帧会让
     * 整个 GOP 黑掉 = 用户看到的闪屏）。如果这里照常 arm，3 秒后
     * update_late_policy() 会把 sink 收紧回 40/true，闪屏立刻回来。
     */
    arm_late_grace();
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
     * 【直播】seek 无意义，直接忽略。
     *
     * 绝不能落到下面任何一条路径：
     *   - 网络分支会 pb2_src_open(target_ms) 起 ffmpeg 拉 FLV，而直播的
     *     绝对 PTS 让流式 demuxer 永久 PREROLLING（见 is_live 字段注释），
     *     等于把本来正常的直连播放换成一个卡死的 ffmpeg 管线。
     *   - 自愈分支同理会重建 ffmpeg 源。
     *   - 本地分支的 gst_element_seek_simple 对直播 HTTP 源也无效。
     * 唯一正确的语义是「忽略」，同时把 seekPending 清掉，否则 JS 会一直
     * 以为在 seek 中（进度条拖不动是直播的预期行为）。
     */
    if (g_w.is_live) {
        pb2_logf("SEEK ignored: live stream (target=%lld)", target_ms);
        pb2_stats_patch_seek(&g_w.stats, 0, target_ms);
        return;
    }

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
            GstStateChangeReturn gret;
            gst_element_set_state(g_w.pipe, was_paused ? GST_STATE_PAUSED : GST_STATE_PLAYING);
            gret = gst_element_get_state(g_w.pipe, &cur, &pend,
                                         (GstClockTime)PB2_PREROLL_TIMEOUT_MS * 1000000ULL);
        }
        /* probe 已由 pad-added 挂上，这里不重复挂（会双倍计数） */
        g_w.paused = was_paused;
        pb2_stats_patch_state(&g_w.stats,
                              was_paused ? PB2_STATE_PAUSED : PB2_STATE_PLAYING);
        /*
         * 【seek 后**不给**迟到宽限】
         *
         * 真机 A/B（同一台设备、同一个稿件、同一条 seek 序列）：
         *   给宽限   seek 落点耗时 2081/2582ms，首帧 3617/1960ms
         *   不给宽限 seek 落点耗时  412/413ms，首帧  516/621ms
         * 差了 4~5 倍。原因：seek 后 sink 里积压的是「从关键帧 K 到目标 T」
         * 这几秒的帧，宽限让它们一帧不丢地按序翻页，等于先把 K..T 全播一遍
         * 才轮到用户要看的画面；而丢迟到帧时 sink 直接跳到 T，正是 seek 想要的。
         *
         * 用户报的是「**起播**不是从头开始」（OPEN），不是 seek，所以宽限只给 OPEN。
         * 另外 seek 后 A/V 同步由 -copyts 保证（见 src_http.c），不依赖宽限。
         */
        g_w.late_grace_until_ms = 0;
        g_w.late_grace_armed = 0;
        pb2_logf("seek(restart) done total=%lldms base=%lld", pb2_now_ms() - t0, g_w.base_ms);
        g_w.last_pos_ms = clamped;
        pb2_stats_patch_position(&g_w.stats, clamped, dur);
        pb2_stats_patch_seek(&g_w.stats, 0, clamped);
        pb2_probe_reset(&g_w.probe);
        g_w.seek_frames_base = 0;
        g_w.avskew_last_log_ms = 0;
        g_w.seek_frame_reported = 0; /* 首帧到达时由 on_video_buffer 报端到端耗时 */
        /*
         * 【防死循环】只有「真正的用户 seek」才清零重试计数器。
         *
         * 自愈重试本身也是调 do_seek，如果这里无条件清零，
         * 重试一次后计数器又变 0，于是 6s 后又重试……无限循环
         * （真机实测：21678 → 21808 → 21814 → 21820 每 6s 一次）。
         */
        g_w.vbranch_ready_ms = 0;
        /* 同一次会话内的 seek 不清 quota，但必须清掉计时基准：
         * 重建期间 abytes 本来就不动，不清会被看门狗当成又一次停摆。 */
        g_w.audio_stall_ab = 0;
        g_w.audio_stall_ms = 0;
        g_w.audio_stall_frames0 = 0;
        if (!g_w.seek_retry_issued) g_w.seek_frame_retries = 0;
        g_w.seek_retry_issued = 0;
        g_w.vsrc_logged = 0;
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
 * 【P1-4】直播断流 → 自动重连。
 *
 * 触发源有两个：
 *   1. bus 上来 ERROR（souphttpsrc 的 I/O 错误、flvdemux 解析失败）；
 *   2. 连续 PB2_LIVE_STALL_MS 没有视频帧（CDN 不再推数据，soup 也不报错，
 *      表现为「画面定住但管线还活着」—— 这是直播最常见的一种断流）。
 *
 * 与 js2 约定的语义（务必别改）：
 *   - patch_error(PB2_E_LIVE_RECONNECT) 时 **绝不改 state**，保持 PLAYING。
 *     JS 的 readStatus() 看到 state=error 会直接判起播失败、停轮询、
 *     弹「播放失败」，用户被迫退出重进 —— 而直播断流十几秒后通常能自己
 *     连回来，不该让用户重进。
 *   - 重连成功路径（live_rebuild_pipeline）会把 lastError 清成 NONE。
 *   - 连续失败超过 PB2_LIVE_RECONNECT_MAX 次才认输，走正常 error 上报。
 *
 * 只能在 worker 线程调用。
 */
static void live_schedule_reconnect(const char *why) {
    long long now = pb2_now_ms();
    long long backoff;

    if (!g_w.is_live || !g_w.req) return;
    if (g_w.live_reconnect_pending) return; /* 已经排期，别叠加 */

    g_w.live_reconnect_count++;
    if (g_w.live_reconnect_count > PB2_LIVE_RECONNECT_MAX) {
        pb2_logf("live: reconnect giving up after %d attempts (%s) -> error",
                 g_w.live_reconnect_count - 1, why ? why : "?");
        pb2_stats_patch_error(&g_w.stats, PB2_E_PIPELINE, "live stream lost (gave up)");
        pb2_stats_patch_state(&g_w.stats, PB2_STATE_ERROR);
        teardown_pipeline();
        return;
    }

    /*
     * 退避：第一次立刻重连（多数断流是一次瞬时的 CDN 掐流），之后
     * 2s / 4s / 6s / 8s 递增，避免在真·下播时把 CPU 打满。
     */
    backoff = g_w.live_reconnect_count <= 1 ? 0
                                             : (long long)g_w.live_reconnect_count * 2000;
    if (backoff > 8000) backoff = 8000;

    g_w.live_reconnect_pending = 1;
    g_w.live_reconnect_at_ms = now + backoff;
    /*
     * 关键：只报 17，**不动 state**。这里不能调 take_error()。
     */
    pb2_stats_patch_error(&g_w.stats, PB2_E_LIVE_RECONNECT,
                          "live reconnect: stream lost");
    pb2_logf("live: reconnect #%d scheduled in %lldms (reason=%s)",
             g_w.live_reconnect_count, backoff, why ? why : "?");
}

/*
 * 【P1-4】worker 周期任务：直播断流检测 + 到点执行重连。
 *
 * 只在直播路径生效，点播完全不受影响（第一行就 return）。
 */
static void check_live_reconnect(void) {
    long long now;
    long long last;
    long long since_frame_ms;

    if (!g_w.is_live) return;
    if (g_w.eos) return;
    /*
     * 暂停态不判断流：暂停时本来就不出帧，lastFrameMs 会一直停在暂停那一刻，
     * 用它算「多久没帧」必然误判。
     */
    if (g_w.paused) return;

    now = pb2_now_ms();

    /* 到点执行排期的重连 */
    if (g_w.live_reconnect_pending && now >= g_w.live_reconnect_at_ms) {
        g_w.live_reconnect_pending = 0;
        pb2_logf("live: reconnect #%d executing now", g_w.live_reconnect_count);
        /*
         * live_rebuild_pipeline 成功时会把 lastError 清成 NONE（js2 要求
         * 「重连成功必须清零」）；失败则走 live_rebuild_failed()：配额内
         * 继续报 17、state 不动，超配额才报真错误 + ERROR。
         */
        live_rebuild_pipeline("reconnect");
        return;
    }

    if (g_w.live_reconnect_pending) return; /* 等退避窗口 */

    /*
     * 管线已经死了：上一次 live_rebuild_pipeline() 的重建本身失败
     * （build_pipeline / set_state / preroll 任一步），它走
     * live_rebuild_failed() → teardown_pipeline() → pipeline_alive 被清 0。
     *
     * 这里**不能**像以前那样直接 return —— 那会让一次瞬时失败就永久终止
     * 重连循环，用户只能手动退出重进，与「连续失败超过
     * PB2_LIVE_RECONNECT_MAX 次才认输」的意图矛盾。
     *
     * 上限守卫：认输之后 count 会变成 MAX+1，条件不成立，不会每个 tick
     * 都重复排期一遍。
     */
    if (!g_w.pipeline_alive) {
        if (g_w.live_reconnect_count > 0 && g_w.live_reconnect_count <= PB2_LIVE_RECONNECT_MAX) {
            live_schedule_reconnect("rebuild failed");
        }
        return;
    }

    /*
     * 断流检测：距上一帧超过 PB2_LIVE_STALL_MS。
     * 首帧都还没来时交给 check_first_frame() 的 15s 看门狗，这里不插手。
     */
    last = (long long)atomic_load_explicit(&g_w.probe.lastFrameMs, memory_order_relaxed);
    if (last <= 0) return;
    since_frame_ms = now - last;
    if (since_frame_ms < PB2_LIVE_STALL_MS) return;

    pb2_logf("live: no video frame for %lldms (threshold %dms)", since_frame_ms,
             PB2_LIVE_STALL_MS);
    live_schedule_reconnect("no video frame");
}

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

    /*
     * 【P1-4】直播的 bus ERROR 不当致命错误：转成「正在重连」。
     * 必须在 take_error() 之前拦，否则 state 被改成 ERROR，JS 直接放弃。
     */
    if (g_w.is_live && strcmp(msg_name(msg), "error") == 0) {
        GError *ge = NULL;
        char *dbg = NULL;
        gst_message_parse_error(msg, &ge, &dbg);
        if (ge) {
            pb2_logf("live: bus error -> reconnect | %s | %s",
                     ge->message ? ge->message : "bus error", dbg ? dbg : "");
            g_error_free(ge);
        }
        if (dbg) g_free(dbg);
        live_schedule_reconnect("bus error");
        return;
    }

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
     * 【直播】位置不能用 query_position。
     *
     * B 站直播 FLV 的 PTS 是绝对值（实测 ≈2854014 秒 ≈792 小时），
     * query_position 会把这个天数级的值直接报给 JS；而且直播没有
     * 「总时长」这个概念，duration 必须是 0（JS 的 isStreamEnded()
     * 对 durationMs<=0 恒返回 false，正是直播要的语义）。
     * 这里用「本次会话已播时长」当位置：单调递增、从 0 起，
     * 对 JS 的进度条/计时显示都是合理值。
     */
    if (g_w.is_live) {
        g_w.last_pos_ms = live_position_ms();
        pb2_stats_patch_position(&g_w.stats, g_w.last_pos_ms, 0);
        /*
         * 【P1-1】直播分支以前直接 return，把下面的 audioBytes 同步块整个跳过了
         * → 直播的 audioBytes 恒 0，而 index.vue 的 A/V 看门狗要求
         * st.audioBytes > 0，直播永远不满足；「直播到底有没有声音」在
         * status()/诊断里也完全不可观测。这里补上同步（直播没有 avSkew
         * 口径可言，仍然跳过 avskew 那段）。
         */
        {
            unsigned long long ab =
                atomic_load_explicit(&g_w.probe.abytes, memory_order_relaxed);
            if (ab != g_w.last_abytes) {
                g_w.last_abytes = ab;
                pb2_stats_patch_audio_bytes(&g_w.stats, (long long)ab);
            }
        }
        return;
    }
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
/*
 * ============ 起播/seek 后的迟到宽限 ============
 *
 * arm_late_grace() 在 OPEN 与每次 seek 重建管线后调用，把截止时刻设到
 * now + 宽限；update_late_policy() 由 worker tick 每轮检查，到点就把
 * kmssink 的 max-lateness/qos 切回严格值。
 *
 * 为什么不用「重建管线时按剩余时间决定」：宽限是**运行时**属性，
 * gst_util_set_object_arg 对已实例化的 sink 立即生效（spike 已验证
 * render-rectangle 同样的用法），不需要重建。
 */
static void arm_late_grace(void) {
    long long ms = PB2_LATE_GRACE_MS;
    /* 诊断覆盖：/tmp/pb2_lategrace 里写毫秒数 */
    {
        FILE *f = fopen("/tmp/pb2_lategrace", "r");
        if (f) {
            long long v = 0;
            if (fscanf(f, "%lld", &v) == 1 && v >= 0 && v <= 60000) ms = v;
            fclose(f);
        }
    }
    g_w.late_grace_until_ms = (ms > 0) ? (pb2_now_ms() + ms) : 0;
    g_w.late_grace_armed = (ms > 0) ? 1 : 0;
    if (ms <= 0) return;
    /*
     * 运行时把 sink 放宽成「不判迟到」。必须在这里做而不是写在 desc 里：
     * desc 是 seek 重建时也会用的，而 seek 不能有宽限（见调用点注释）。
     * 这里 arm 只在 OPEN 之后调用，sink 此时已实例化，set_object_arg 立即生效。
     */
    if (g_w.pipe && g_w.pipeline_alive && g_w.vsink_el) {
        gst_util_set_object_arg(g_w.vsink_el, "max-lateness", "-1");
        gst_util_set_object_arg(g_w.vsink_el, "qos", "false");
    }
    pb2_logf("late grace: armed %lldms (no late-drop until then)", ms);
}

static void update_late_policy(void) {
    /*
     * 直播的 sink 策略由 desc 决定（-1 / qos=false），worker tick 绝不能
     * 干预，否则闪屏复现。见 do_open 里 arm_late_grace() 处的注释。
     */
    if (g_w.late_grace_until_ms <= 0) return;
    /*
     * 【关键】首帧还没上屏时绝不收紧。
     *
     * 宽限原本从 OPEN/seek 那一刻算 3 秒，但真机实测首帧有时要 4801ms
     * 才到（网络抖动 + mppvideodec 首帧），那时宽限早已过期，积压的帧
     * 照样被判迟到丢光 —— 等于白改。
     *
     * frame_epoch_ms > 0 表示「正在等首帧」。只要还在等，就必须保持
     * -1/false，否则等来的第一帧就是「内容已过 1 秒」的那一帧，
     * 正是用户报的「起播不是从头开始」。
     *
     * 首帧到达时 on_video_buffer() 会把 frame_epoch_ms 清零，这里才开始
     * 按 late_grace_until_ms 计时（arm 时给的是 now+3000，如果首帧来得很晚，
     * 到这一刻可能已经过期，所以首帧到达后还要再续一小段 —— 见下面的
     * 续期逻辑）。
     */
    if (g_w.frame_epoch_ms > 0) {
        /*
         * 还在等首帧。只有 OPEN 置过 late_grace_armed 才顺延；seek 后
         * armed=0，直接取消宽限（见 struct 里 late_grace_armed 的实测数据）。
         */
        if (!g_w.late_grace_armed) { g_w.late_grace_until_ms = 0; return; }
        /* 把截止时刻顺延到「现在 + 宽限」，首帧一到就开始倒数 */
        long long ms = PB2_LATE_GRACE_MS;
        FILE *f = fopen("/tmp/pb2_lategrace", "r");
        if (f) {
            long long v = 0;
            if (fscanf(f, "%lld", &v) == 1 && v >= 0 && v <= 60000) ms = v;
            fclose(f);
        }
        if (ms <= 0) { g_w.late_grace_until_ms = 0; return; }
        g_w.late_grace_until_ms = pb2_now_ms() + ms;
        return;
    }
    if (pb2_now_ms() < g_w.late_grace_until_ms) return;
    /* 到点：切回严格的迟到策略 */
    g_w.late_grace_until_ms = 0;
    g_w.late_grace_armed = 0;
    if (!(g_w.pipe && g_w.pipeline_alive && g_w.vsink_el)) return;
    /* 诊断开关：/tmp/pb2_nolate.on 存在则永不收紧（A/B 对照用） */
    if (access("/tmp/pb2_nolate.on", F_OK) == 0) {
        pb2_logf("late grace: expired but /tmp/pb2_nolate.on present -> keep -1/false");
        return;
    }
    {
        char late[16], qos[12];
        snprintf(late, sizeof(late), "%d", PB2_MAX_LATENESS);
        snprintf(qos, sizeof(qos), "true");
        gst_util_set_object_arg(g_w.vsink_el, "max-lateness", late);
        gst_util_set_object_arg(g_w.vsink_el, "qos", qos);
        pb2_logf("late grace: expired -> max-lateness=%d qos=true (strict)", PB2_MAX_LATENESS);
    }
}

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
        /*
         * 只记错误，绝不重建管线：实测只要进程活着，屏幕唤醒后平面自动恢复。
         *
         * 【P1-4】直播正在重连时不覆盖错误码：js2 靠 lastError==17 判断
         * 「正在重连，别放弃」，被 PLANE_LOST(12) 盖掉就会当成普通播放异常。
         */
        if (!(g_w.is_live && (g_w.live_reconnect_pending || g_w.live_reconnect_count > 0))) {
            pb2_stats_patch_error(&g_w.stats, PB2_E_PLANE_LOST,
                                  "kms plane not active while playing");
        }
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

    /*
     * 【首次 seek 必失败的自愈】
     *
     * 真机稳定复现：每个会话的**第一次** seek 下，matroskademux 的
     * video src pad 一个 buffer 都不推（音频正常、`video branch ready`
     * 正常、但 `first frame` 永不出现），第二、三次 seek 全部正常。
     *
     * 已排除：plane 竞争（teardown 后 plane 已释放，`waited` 一次都未打印）、
     * 引用泄漏（已修）、ffmpeg 本身、debounce（coalesced=0）。
     * 属于 GStreamer 内部的瞬态竞争，再往下定位代价过高。
     *
     * 做法：首帧等到 6s 仍未出现，就**自动重试一次同一落点**。
     * 重试走的就是「第二次 seek」那条已知可靠的路径（真机 100% 成功）。
     * 只重试 1 次防止无限循环；重试仍失败才走原报错路径。
     */
    /*
     * 触发阈值用 2000ms 而不是 6000ms：正常情况下 `video branch ready`
     * 后 6~40ms 内就会有第一个 video buffer（真机实测）。
     * 超过 2s 还一个 buffer 都没有，就是已知的「首次 seek
     * demuxer 不推视频」故障，立即重试比等 6s 快 3 倍。
     */
    /*
     * 【精确触发条件】`!g_w.vsrc_logged` = demuxer 的视频 src pad
     * **一个 buffer 都没推过**。正常情况下 `video branch ready`
     * 后 6~40ms 就有第一个 buffer（真机实测），所以 1s 还没有
     * 就是已知故障，无需等更久。比「固定等 N 秒」更可靠：
     * 不会因为某些片源首帧硬解慢就误重试。
     */
    if (g_w.seek_frame_retries < 1 &&
        g_w.seek_issue_ms > 0 && !g_w.seek_frame_reported &&
        g_w.vbranch_ready_ms > 0 && now - g_w.vbranch_ready_ms > 1500 &&
        !g_w.vsrc_logged &&
        g_w.pipeline_alive && g_w.pipe && g_w.vbranch) {
        g_w.seek_frame_retries++;
        pb2_logf("first frame: none in %lldms after seek -> auto retry (target=%lldms)",
                 now - g_w.frame_epoch_ms, g_w.last_pos_ms);
        g_w.seek_target = g_w.last_pos_ms;
        g_w.seek_pending = 1;
        g_w.seek_last_ms = pb2_now_ms() - PB2_SEEK_DEBOUNCE_MS - 1;
        g_w.frame_epoch_ms = pb2_now_ms();
        g_w.seek_retry_issued = 1;   /* 告知 do_seek：这次不能清零计数器 */
        return;
    }

    /*
     * 【平面兜底】显式 plane-id 在这台机器上可能不可用。
     *
     * DRM plane id 由内核按 DT 分配，换固件就可能变；显式 id 失效的典型表现
     * 不是"报错"，而是「分支建好了、一帧 buffer 都不来」（sink 抢不到平面，
     * pad 不激活 → 反压）。4s（远早于 15s 超时）还没有帧就退回「不写
     * plane-id，让 kmssink 自选」重建一次，每会话只做一次。
     *
     * 与上面 seek 重试的区别：那个针对「demuxer 不推 buffer」（vsrc_logged=0），
     * 这个针对「显式平面不可用」，两者可以叠加，各自只做一次。
     */
    if (!g_w.plane_fallback_done && g_w.kms_plane_id > 0 &&
        g_w.frame_epoch_ms > 0 && now - g_w.frame_epoch_ms >= 4000) {
        g_w.plane_fallback_done = 1;
        pb2_logf("first frame: none in %lldms with plane-id=%d -> fallback to auto plane",
                 now - g_w.frame_epoch_ms, g_w.kms_plane_id);
        g_w.kms_plane_id = -1;
        g_w.seek_target = g_w.last_pos_ms > 0 ? g_w.last_pos_ms : 0;
        g_w.seek_pending = 1;
        g_w.seek_last_ms = pb2_now_ms() - PB2_SEEK_DEBOUNCE_MS - 1;
        g_w.frame_epoch_ms = pb2_now_ms();
        g_w.seek_retry_issued = 1;   /* 告知 do_seek：这次不能清零计数器 */
        return;
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
    /*
     * 【直播】这条探测整条对直播无意义：直播没有 ffmpeg 子进程，
     * src_ready 恒 0（下面 `if (!g_w.src_ready) return;` 其实也会挡住），
     * 但直播的「流断了」是靠现有 bus ERROR/EOS 分支收尾的，不需要
     * 也不应该走 ffmpeg 存活探测。这里显式早退，免得以后有人给直播
     * 也填上 src_ready 就误触发。
     */
    if (g_w.is_live) return;

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
 * 【音频停摆看门狗】视频在跑、音频分支却拿不到数据时自动救回来。
 *
 * 判据刻意保守：必须「视频帧数同期涨了 >= 30 帧」才算数，这样整体卡死
 * （网络断了 / 源死了）不会误触发 —— 那种情况由 check_first_frame 的
 * 源存活探测和 bus 错误路径负责。
 *
 * 救法用「原地 seek」而不是重建管线：seek 走的是已经验证可靠的路径
 * （重开 ffmpeg + 重建管线 + 落点校验），端到端 1~2s，比让用户
 * 一直没声音好得多。
 */
static void check_audio_stall(void) {
    unsigned long long ab;
    unsigned long long fr;
    long long now;
    long long seen;

    if (!g_w.pipeline_alive || !g_w.pipe) return;
    if (!g_w.req || !g_w.req->audio) return;  /* 本来就要求不要音频 */
    if (g_w.paused || g_w.eos) return;
    if (g_w.is_live) return;                  /* 直播自己有 live_reconnect */
    if (g_w.frame_epoch_ms > 0) return;       /* 还在等首帧，交给 check_first_frame */
    if (g_w.seek_pending) return;             /* 已经在重建了 */

    now = pb2_now_ms();
    ab = atomic_load_explicit(&g_w.probe.abytes, memory_order_relaxed);
    fr = atomic_load_explicit(&g_w.probe.frames, memory_order_relaxed);
    seen = g_w.first_frame_ms > 0 ? now - g_w.first_frame_ms : -1;

    /*
     * ① 音频分支根本没建起来（abranch=0）。
     *
     * 判据必须带「有第二路音频输入」或「本来已经有分支」：
     *   - DASH 双输入：req->input2 非空 → 一定应该有音轨，没有就是故障；
     *   - 单文件（durl/本地）：demuxer 没给 audio pad 说明这个文件本来就没音轨，
     *     重建一百次也没用，直接放过。
     * 这一条覆盖用户反馈的「有概率没有声音」最常见的形态：音频 URL 那一路
     * 偶发 403/超时，ffmpeg 的可选映射静默丢掉音轨，画面一切正常。
     */
    if (!g_w.abranch) {
        int want2 = g_w.req->input2 && g_w.req->input2[0];
        if (!want2) return;                   /* 没有音轨的文件，不折腾 */
        if (g_w.audio_heals >= PB2_AUDIO_HEALS) return;
        if (seen < 0 || seen < PB2_AUDIO_START_GRACE_MS) return;
        if (fr < (unsigned long long)PB2_AUDIO_START_FRAMES) return;

        g_w.audio_heals++;
        pb2_logf("audio missing: no audio branch %lldms after first frame "
                 "(video %llu frames, ab=0) -> rebuild pipeline at %lldms "
                 "(heal %d/%d, input2=%s)",
                 seen, fr, g_w.last_pos_ms, g_w.audio_heals, PB2_AUDIO_HEALS,
                 g_w.req->input2);
        g_w.seek_target = g_w.last_pos_ms > 0 ? g_w.last_pos_ms : 0;
        g_w.seek_pending = 1;
        g_w.seek_last_ms = pb2_now_ms() - PB2_SEEK_DEBOUNCE_MS - 1;
        pb2_stats_patch_seek(&g_w.stats, 1, g_w.seek_target);
        return;
    }

    if (g_w.audio_stall_heals >= PB2_AUDIO_STALL_HEALS) return;

    /*
     * ② 分支建了、但一个音频字节都没到。
     *
     * alsasink 打不开（PCM 被占 / bluealsa 没就绪）时分支会自毁，反过来
     * 「分支还在、数据不来」更多是解码链或上游 mapping 的问题。两种都试一次重建：
     * 首帧后 4.5s 还没有任何音频数据，正常播放里不存在这种情况。
     */
    if (ab == 0) {
        /*
         * 【3.6.2 收窄范围：只救 DASH 双输入】
         *
         * 单文件（durl/自检页的 html5 取流）里「0 音频字节」有两种可能：
         *   a) 这个文件本来就没有音轨（B 站某些稿件就是这样）；
         *   b) 真的有音轨但解码链坏了。
         * 两者在 native 侧无法区分，而重建对 (a) 毫无意义，还会在重建瞬间
         * 让状态机短暂进入 OPENING/ERROR —— 真机自检页那次「起播 ok，5s 后
         * 播放错误」正是重建窗口里被读到的暂态。
         * 用户报的「概率性没声音」全部来自 DASH（音频是独立 URL，偶发 403/超时，
         * ffmpeg 的可选映射静默丢轨），所以这里只对 input2 非空的会话自愈。
         */
        if (!(g_w.req->input2 && g_w.req->input2[0])) return;
        if (g_w.audio_heals >= PB2_AUDIO_HEALS) return;
        if (seen < 0 || seen < PB2_AUDIO_START_GRACE_MS) return;
        if (fr < (unsigned long long)PB2_AUDIO_START_FRAMES) return;
        g_w.audio_heals++;
        pb2_logf("audio missing: branch ready but 0 audio bytes after %lldms "
                 "(video %llu frames) -> rebuild pipeline at %lldms (heal %d/%d)",
                 seen, fr, g_w.last_pos_ms, g_w.audio_heals, PB2_AUDIO_HEALS);
        g_w.seek_target = g_w.last_pos_ms > 0 ? g_w.last_pos_ms : 0;
        g_w.seek_pending = 1;
        g_w.seek_last_ms = pb2_now_ms() - PB2_SEEK_DEBOUNCE_MS - 1;
        pb2_stats_patch_seek(&g_w.stats, 1, g_w.seek_target);
        return;
    }
    if (ab != g_w.audio_stall_ab) {
        /* 音频在推进：静默刷新基准。
         * 只在真的冻结过 >= 1s 时才打日志 —— 本函数每几十毫秒跑一次，
         * abytes 每个 tick 都在涨，无条件打日志会把日志刷爆
         * （实测 5000+ 行/分钟，把真正有用的行全淹了）。 */
        if (g_w.audio_stall_ab != 0 && g_w.audio_stall_ms > 0 &&
            now - g_w.audio_stall_ms >= 1000) {
            pb2_logf("audio stall: resumed after %lldms (ab=%llu)",
                     now - g_w.audio_stall_ms, ab);
        }
        g_w.audio_stall_ab = ab;
        g_w.audio_stall_ms = now;
        g_w.audio_stall_frames0 = fr;
        return;
    }

    if (g_w.audio_stall_ms <= 0) {
        g_w.audio_stall_ms = now;
        g_w.audio_stall_frames0 = fr;
        return;
    }
    if (now - g_w.audio_stall_ms < PB2_AUDIO_STALL_MS) return;

    /* 视频同期必须还在动，否则是整体卡死，不归这里管 */
    if (fr - g_w.audio_stall_frames0 < (unsigned long long)PB2_AUDIO_STALL_FRAMES) return;

    g_w.audio_stall_heals++;
    pb2_logf("audio stall: abytes frozen %llu for %lldms while video advanced "
             "%llu frames -> in-place seek to re-sync audio (heal %d/%d, target=%lldms)",
             ab, now - g_w.audio_stall_ms,
             fr - g_w.audio_stall_frames0,
             g_w.audio_stall_heals, PB2_AUDIO_STALL_HEALS, g_w.last_pos_ms);

    g_w.seek_target = g_w.last_pos_ms > 0 ? g_w.last_pos_ms : 0;
    g_w.seek_pending = 1;
    g_w.seek_last_ms = pb2_now_ms() - PB2_SEEK_DEBOUNCE_MS - 1;
    g_w.audio_stall_ms = now;
    g_w.audio_stall_frames0 = fr;
    pb2_stats_patch_seek(&g_w.stats, 1, g_w.seek_target);
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

/*
 * 【P1-6】会话真正结束（STOP / RELEASE）时清掉**会话级**字段。
 *
 * 为什么不放进 teardown_pipeline_ex()：那个函数也被 live_rebuild_pipeline()
 * 的重建路径调用，而重建依赖 t_start 保持原值来算直播位置
 * （live_position_ms() = now - t_start）。在 teardown 里清 t_start，
 * 重建后位置会瞬间跳成「从开机起算」的天数级数字。
 *
 * 所以在会话结束点单独清，重建路径完全不受影响。
 * 清理后 status() 不会再用旧 t_start 算出假位置 —— 虽然 refresh_position()
 * 有 `!g_w.pipe || !g_w.pipeline_alive` 早退挡住了，但那是巧合而非设计。
 * 顺带把待办的重连也清掉，避免上一次直播的计数泄漏到下一次点播。
 */
static void reset_session_fields(void) {
    g_w.is_live = 0;
    /* 音频兜底状态属于「一次 OPEN 周期」，会话结束必须清，否则下一支视频
     * 会直接以无声启动（audioOff 残留会让页面一直显示「设备被占用」）。 */
    g_w.audio_off = 0;
    g_w.audio_retry_stage = 0;
    g_w.want_audio_retry = 0;
    g_w.reopen_delay_ms = 0;
    g_w.t_start = 0;
    g_w.pause_started_ms = 0;
    g_w.live_reconnect_pending = 0;
    g_w.live_reconnect_count = 0;
    g_w.live_reconnect_at_ms = 0;
    g_w.route_pending = 0;
    /* 无声自愈的配额按「会话」重置：一次 open→stop 里最多重建 2 次。
     * 绝不能放进 teardown_pipeline_ex() —— seek/重建都会走那里，配额会被
     * 无限续上，变成「没声音就一直重建」的死循环。 */
    g_w.audio_heals = 0;
    g_w.first_frame_ms = 0;
    g_w.plane_fallback_done = 0;
}

static void process_cmd(pb2_cmd_t *c) {
    switch (c->type) {
        case PB2_CMD_OPEN: {
            pb2_open_req_t *r = (pb2_open_req_t *)c->ptr;
            int guard;

            /* 新的播放请求：本会话一次性的兜底状态全部从零开始 */
            g_w.want_plane_retry = 0;
            g_w.want_audio_retry = 0;
            g_w.reopen_delay_ms = 0;
            g_w.audio_retry_stage = 0;
            g_w.audio_off = 0;

            do_open(r);
            /*
             * 兜底重建循环。do_open 判定需要重来时会置标志并**原地返回**
             * （它不能递归自己：开头就 free(g_w.req)，而 g_w.req 正是要复用
             * 的对象，见 open_req_dup 的注释）。这里最多两轮：
             *   ① 平面：显式 plane-id 抢不到 → auto + videoscale
             *   ② 音频：alsasink 设备被占 → 先原样重试，再降级为无声
             * 每轮都重新 clone 当前请求（上一轮的 r 已经被 do_open 回收）。
             */
            for (guard = 0; guard < 3; guard++) {
                pb2_open_req_t *dup;
                int delay;
                int audio_off;

                if (!g_w.want_plane_retry && !g_w.want_audio_retry) break;

                delay = g_w.reopen_delay_ms;
                audio_off = g_w.audio_off;
                g_w.want_plane_retry = 0;
                g_w.want_audio_retry = 0;
                g_w.reopen_delay_ms = 0;

                dup = open_req_dup(g_w.req);
                if (!dup) {
                    pb2_logf("OPEN retry: clone failed -> error");
                    pb2_stats_patch_error(&g_w.stats, PB2_E_OPEN_FAILED,
                                          "open failed (retry alloc)");
                    pb2_stats_patch_state(&g_w.stats, PB2_STATE_ERROR);
                    break;
                }
                /* 音频兜底第二轮：直接不带音频重建，保证画面先出来 */
                if (audio_off) dup->audio = 0;

                if (delay > 0) {
                    pb2_logf("OPEN retry: waiting %dms before rebuild (audio device release)",
                             delay);
                    pb2_sleep_ms(delay);
                }
                pb2_logf("OPEN retry: rebuild (audio=%d)", dup->audio);
                do_open(dup); /* 内部会 free 掉上一轮的 g_w.req */
            }
            break;
        }
        case PB2_CMD_PAUSE:
            if (g_w.pipe && g_w.pipeline_alive) {
                long long t0 = pb2_now_ms();
                gst_element_set_state(g_w.pipe, GST_STATE_PAUSED);
                g_w.paused = 1;
                /*
                 * 【P0-2】记下暂停时刻，供 live_position_ms() 冻结直播位置；
                 * RESUME 用它把 t_start 往后推，等价于暂停期间时钟不走。
                 */
                g_w.pause_started_ms = t0;
                pb2_stats_patch_state(&g_w.stats, PB2_STATE_PAUSED);
                pb2_logf("PAUSE took %lldms", pb2_now_ms() - t0);
            }
            break;
        case PB2_CMD_RESUME:
            if (g_w.pipe && g_w.pipeline_alive) {
                long long t0 = pb2_now_ms();
                gst_element_set_state(g_w.pipe, GST_STATE_PLAYING);
                /*
                 * 【P0-2】把暂停时长补进 t_start：直播位置 = now - t_start，
                 * t_start 后移 Δ 就等于「暂停这段不计入已播时长」。
                 */
                if (g_w.pause_started_ms > 0 && t0 > g_w.pause_started_ms) {
                    long long delta = t0 - g_w.pause_started_ms;
                    g_w.t_start += delta;
                    pb2_logf("RESUME paused %lldms -> t_start += %lldms",
                             delta, delta);
                }
                g_w.pause_started_ms = 0;
                g_w.paused = 0;
                /*
                 * 【暂停恢复绝不能给迟到宽限】
                 * 恢复瞬间 PTS 全部过期的积压必须被 max-lateness=40 + qos=true
                 * 丢掉，否则一秒内涌出 400+ 帧（v3.5.2 修过的「恢复后卡一下」
                 * 回归）。这里顺手把可能还在的宽限清掉。
                 */
                g_w.late_grace_until_ms = 0;
                g_w.late_grace_armed = 0;
                g_w.last_fps_ms = t0;
                g_w.fps_frames_base =
                    atomic_load_explicit(&g_w.probe.frames, memory_order_relaxed);
                /*
                 * 【P1-7】把音频路由的两条节流清零，让「暂停期间攒下的
                 * 路由变更」(g_w.route_pending) 在下一次 worker tick 立刻
                 * 生效，而不是再等 1s 的 vol_check 窗口。插着耳机暂停、
                 * 拔掉耳机再 resume 时，声音要马上回到扬声器。
                 */
                g_w.vol_check_ms = 0;
                g_w.bt_check_ms = 0;
                pb2_stats_patch_state(&g_w.stats, PB2_STATE_PLAYING);
                pb2_logf("RESUME took %lldms", pb2_now_ms() - t0);
            }
            break;
        case PB2_CMD_SET_SOUND:
            /*
             * 【3.5.6 第五轮】一键开声。真机「画面正常但没有声音」时，
             * 多半是全局静音开关把 Playback Path 设成了 OFF（音频数据
             * 仍在走，自检页 `音=xxxxx/有` 也照常增长）。
             * 文件操作与 amixer 全在 worker 线程，JSAPI 零阻塞。
             */
            pb2_logf("SET_SOUND: before muted=%d src=%s",
                     pb2_mute_enabled(), pb2_mute_reason());
            if (c->i64 != 0) pb2_force_sound();
            pb2_logf("SET_SOUND: after muted=%d src=%s",
                     pb2_mute_enabled(), pb2_mute_reason());
            break;
        case PB2_CMD_RECOVER:
            /*
             * 【P0-1】真正的自愈入口。
             *
             * 直播：重拉 souphttpsrc（唯一能救活断流的动作）。
             * 点播：以当前位置投递一次原地 seek，与旧 JS 侧
             *       seekBy(currentPos) 等价，但省掉一次 JS→native 往返。
             *
             * 这里不检查 paused：暂停态下看门狗本来就不该触发，真触发了
             * 说明位置也不可信，重建是更安全的动作。
             */
            if (!g_w.req) {
                pb2_logf("RECOVER ignored: no session");
            } else if (g_w.is_live) {
                pb2_logf("RECOVER live: rebuild souphttpsrc (pos=%lldms)",
                         live_position_ms());
                live_rebuild_pipeline("recover");
            } else if (!g_w.pipe || !g_w.pipeline_alive) {
                pb2_logf("RECOVER video: no pipeline -> treated as open retry");
                pb2_stats_patch_error(&g_w.stats, PB2_E_OPEN_FAILED,
                                      "recover: pipeline gone");
                pb2_stats_patch_state(&g_w.stats, PB2_STATE_ERROR);
            } else {
                long long tgt = g_w.last_pos_ms;
                if (tgt < 0) tgt = 0;
                g_w.seek_target = tgt;
                g_w.seek_pending = 1;
                g_w.seek_last_ms = pb2_now_ms() - PB2_SEEK_DEBOUNCE_MS - 1;
                pb2_stats_patch_seek(&g_w.stats, 1, tgt);
                pb2_logf("RECOVER video: in-place seek@%lldms", tgt);
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
        {
            /*
             * 键盘避让：运行时改 render-rectangle，不重建管线。
             * i64 = x | y<<16 | w<<32 | h<<48（见 core/queue.h）。
             *
             * 单一出口：所有分支（含直播重建）都必须走到本块末尾的
             * 脉冲收尾，否则「暂停态 + 直播 + 键盘恢复」会让管线
             * 永远停在 PLAYING。
             */
            int rect_pulsed = 0;   /* 已经为本次 SET_RECT 临时切到 PLAYING */
            int rect_rebuilt = 0;  /* 走了重建路径，管线状态由重建方负责 */
            int rx, ry, rw, rh, want_hide;
            char rect[48];
            long long t0_rect = pb2_now_ms();

            if (!(g_w.pipe && g_w.pipeline_alive && g_w.vsink_el)) {
                pb2_logf("SET_RECT ignored: no live pipeline");
                break;
            }

            rx = (int)(short)((c->i64 >> 0) & 0xFFFF);
            ry = (int)(short)((c->i64 >> 16) & 0xFFFF);
            rw = (int)(unsigned short)((c->i64 >> 32) & 0xFFFF);
            rh = (int)(unsigned short)((c->i64 >> 48) & 0xFFFF);

            /*
             * 【隐藏平面必须用 1x1，绝不能用 0x0】真机实测：
             *   <107,0,240,427> -> dst pos[107,0] rect[240 x 427]  正常
             *   <0,0,0,0>       -> dst pos[0,0]   rect[480 x 960]  **全屏！**
             *   <0,0,1,1>       -> dst pos[0,0]   rect[1 x 1]      真正不可见
             * kmssink 对零宽高既不报错也不隐藏，而是回退成「未设置矩形
             * => 全屏」，键盘避让期间会把 falcon 键盘整个盖住。
             * 所以 0（及任何 <1 的值）统一提升成 1x1。
             */
            if (rw < 1) rw = 1;
            if (rh < 1) rh = 1;
            /* 隐藏时不加面板偏移，免得 1x1 被推到非零坐标 */
            if (rw > 1 || rh > 1) rx += (g_w.panel_x_offset > 0 ? g_w.panel_x_offset : PB2_PANEL_X_OFFSET);
            want_hide = (rw <= 1 && rh <= 1) ? 1 : 0;
            snprintf(rect, sizeof(rect), "<%d,%d,%d,%d>", rx, ry, rw, rh);

            /*
             * ================== kmssink render-rectangle 是单向棘轮 ==================
             *
             * 真机实测（.tmp/mkv/rect35.c，真实管线结构）：
             *   240x427 --直接 set--> 1x1        生效
             *   1x1 --直接 set--> 240x427        **无效**（6 轮全部停在 1x1）
             *   1x1 --pipe PAUSED 循环--> 240x427 **无效**（rect38，5 轮全失败）
             *   1x1 --sink READY/PLAYING--> 240x427 **无效**（rect33，平面 RELEASED 不回来）
             *   1x1 --sink NULL--> 240x427       **无效**（rect36(b)，RELEASED）
             *   1x1 --pipe READY -> set rect -> pipe PLAYING--> 240x427  **有效**（rect36(a)）
             *
             * 即：**只能缩小，不能放大**；唯一可靠的「放大」途径是让整条
             * 管线过一次 READY 重置。而 pipe READY 会关掉 filesrc 的 FIFO
             * 读端、流位置全部丢失 —— 对「ffmpeg -> FIFO -> filesrc」的边下
             * 边播架构等价于「按当前播放位置重新拉一路源」，也就是走已经
             * 充分验证过的 do_seek 重启路径（三跳 drift=0ms、first frame
             * 413~516ms）。所以分支策略：
             *   1) 请求隐藏 -> 直接 set 1x1（~0ms，实测有效），记 rect_hidden
             *   2) 请求恢复且当前是隐藏态 -> 更新 g_w.rect 并投递一次原地 seek，
             *      build_pipeline 用新 g_w.rect 建管线，平面按正确矩形重建
             *   3) 请求改矩形且从未隐藏过 -> 直接 set（正常尺寸之间不触发棘轮）
             *
             * 为什么 1x1 之后必须走 seek 而不能「重设 render-rectangle 再让
             * kmssink 自己重配」：render-rectangle 是 **write-only** 属性
             * （g_object_get 报 "property 'render-rectangle' of object class
             * 'GstKMSSink' is not readable"），kmssink 只在重新 configure
             * （分配 plane）时才读它，而 1x1 之后 sink 不再 configure。
             */

            /*
             * ============ 【暂停态必须脉冲一次 PLAYING】============
             *
             * 真机实测（2026-11-01 18:36，debugfs 读 Esmart1-win0 的 dst）：
             *   播放态  set <0,0,1,1>  ->  dst pos[0,0] rect[1 x 1]       ✅ 生效
             *   暂停态  set <0,0,1,1>  ->  dst pos[107,299] rect[240x426] ❌ 完全无效
             *
             * 根因同上：write-only 属性 + PAUSED 不 reconfigure。暂停态要改
             * 矩形，先临时 set_state(PLAYING) 触发一次 reconfigure，设完立刻
             * set_state(PAUSED) 冻回去。PLAYING 期间最多多渲染 1 帧（几十 ms），
             * 用户感知不到；不这么做的话「暂停后点评论框」时视频平面会一直
             * 盖在系统键盘上（用户反馈）。
             *
             * 只对暂停态脉冲：播放态本来就会 reconfigure，加脉冲纯属多余
             * 且可能引起掉帧。
             */
            /*
             * 【顺序绝对不能反】属性必须先设、再脉冲 PLAYING。
             *
             * render-rectangle 是 write-only，kmssink 只在 reconfigure
             * （分配 plane）那一刻读它。所以：
             *   先脉冲再设属性 -> reconfigure 读到的是**旧值**，设完不会
             *                     再 configure 一次 -> 暂停态完全无效
             *   先设属性再脉冲 -> reconfigure 读到新值 -> 生效
             * 上一版就是反的，探针里「看起来生效」是因为 2.5s 后恢复动作
             * 又触发了一次 configure，把结果掩盖了。
             *
             * 脉冲时长给 200ms：实测 mppvideodec 从 PAUSED 恢复出帧
             * 需要 100~180ms，60ms 经常还没走到 configure 就切回去了。
             */
            if (want_hide) {
                gst_util_set_object_arg(g_w.vsink_el, "render-rectangle", rect);
                g_w.rect_hidden = 1;
                if (g_w.paused && g_w.pipe && g_w.pipeline_alive) {
                    gst_element_set_state(g_w.pipe, GST_STATE_PLAYING);
                    pb2_sleep_ms(200);
                    rect_pulsed = 1;
                }
                pb2_logf("SET_RECT %s (hide) took %lldms%s", rect,
                         pb2_now_ms() - t0_rect,
                         rect_pulsed ? " [property first, then pulse PLAYING]" : "");
            } else if (g_w.rect_hidden) {
                /*
                 * 恢复：把目标矩形写进 g_w.rect（build_pipeline 用它），
                 * 然后重建管线让平面按新矩形重配。
                 *
                 * 【直播】不能走 seek 通道 —— 直播 seek 恒被忽略，那样键盘
                 * 收起后画面会永远停在 1x1。改用 live_rebuild_pipeline()，
                 * 它 teardown 后重新建管线，build_pipeline 会读新的 g_w.rect。
                 *
                 * 非直播仍走原来那条验证过的「原地 seek」通道
                 * （seek_last_ms 回拨到过去，下一次 worker tick 立刻执行）。
                 */
                snprintf(g_w.rect, sizeof(g_w.rect), "%s", rect);
                normalize_video_rect("SET_RECT restore");
                g_w.rect_hidden = 0;
                if (g_w.is_live) {
                    pb2_logf("SET_RECT %s (restore: live rebuild, no seek path)", rect);
                    live_rebuild_pipeline("rect restore");
                    rect_rebuilt = 1;
                } else {
                    g_w.seek_target = g_w.last_pos_ms > 0 ? g_w.last_pos_ms : 0;
                    g_w.seek_pending = 1;
                    g_w.seek_last_ms = pb2_now_ms() - PB2_SEEK_DEBOUNCE_MS - 1;
                    pb2_stats_patch_seek(&g_w.stats, 1, g_w.seek_target);
                    pb2_logf("SET_RECT %s (restore: 1x1 无法放大，投递原地 seek@%lldms 重建管线)",
                             rect, g_w.seek_target);
                    rect_rebuilt = 1;
                }
            } else {
                /* 正常尺寸之间改矩形。
                 *
                 * 【3.5.6 第九轮】优先走 GstVideoOverlay.set_render_rectangle()：
                 * 它是运行期改平面的正道 —— sink 自己重配 plane，放大缩小都生效，
                 * 也不用把属性写进去等下一次 configure。老路径（写 write-only 属性
                 * + 暂停态脉冲）保留为回落：拿不到 libgstvideo 的设备行为不变。
                 *
                 * 为什么必须换：用户点画面收起/展开控件时矩形要来回变，老路径在
                 * "放大"方向不可靠，连点两下就会停在中间态 —— 真机表现是
                 * 「点两下屏幕以后视频跑到下方 / 只露半截」。 */
                /*
                 * 【3.5.6 第十四轮：只移动、不缩放时不要重建管线】
                 *
                 * videoscale 的输出 caps 只由 width/height 决定，与 x/y 无关。
                 * 第十四轮把全屏改成"只改位置"（视频段高度普通态与全屏态完全相同），
                 * 于是进出全屏时 w/h 一模一样，只有 x 变 —— 这种情况重建管线是
                 * 纯浪费，还会黑一下。这里记住旧尺寸，normalize 之后比较一次。
                 */
                int old_w = 0, old_h = 0, new_w = 0, new_h = 0, size_same = 0;
                sscanf(g_w.rect, "<%*d,%*d,%d,%d>", &old_w, &old_h);
                snprintf(g_w.rect, sizeof(g_w.rect), "%s", rect);
                normalize_video_rect("SET_RECT");
                sscanf(g_w.rect, "<%*d,%*d,%d,%d>", &new_w, &new_h);
                size_same = (old_w > 0 && old_h > 0 && old_w == new_w && old_h == new_h);
                /*
                 * ============ 【3.5.6 第十轮：sw_scale 档绝不即时改平面】 ============
                 *
                 * X7 Pro 的管线里插了
                 *   videoscale ! video/x-raw,width=<g_w.rect.w>,height=<g_w.rect.h>
                 * （video_post_rate_desc），caps 是 **OPEN 那一刻**按 g_w.rect 算好的。
                 * 播放中只改 render-rectangle，kmssink 拿到的是旧尺寸 buffer，
                 * 却要往新矩形里贴 —— 真机照片里「点一下隐藏进度条，画面变宽
                 * 却被推到下半屏、底部被裁」就是这么来的。文件里那句
                 * "X7 自选平面疑似只做 1:1 贴 buffer，改 render-rectangle 对
                 * 显示尺寸毫无作用" 是同一件事的另一半。
                 *
                 * 唯一可靠的「换尺寸」途径是让 build_pipeline 用新 g_w.rect
                 * 重算 caps 并重建平面。这里只把新矩形记下来，然后投递一次
                 * 原地 seek（与 1x1 恢复分支同一条已经充分验证过的路径）；
                 * 直播的 seek 恒被忽略，改走 live_rebuild_pipeline。
                 *
                 * 页面侧已经保证**只有进/出全屏**才会发 SET_RECT（第十轮），
                 * 控件显隐/字幕开关全部改成 DOM 纯黑占位，所以这里一次重建
                 * 只对应一次用户主动的全屏切换，不会在日常操作里频繁黑屏。
                 */
                /*
                 * 【3.5.6 第十五轮：overlay 拿不到时，连"只移动"也必须重建】
                 *
                 * 第十四轮给 size_same 开了一条捷径：x/y 变了、w/h 没变时不重建，
                 * 直接走 overlay 把平面挪过去。但 overlay 需要 dlopen 到
                 * libgstvideo；拿不到时旧代码会掉进最后的"写 render-rectangle
                 * 属性"分支 —— 而 X7 的 kmssink 只在 configure 那一刻读这个属性，
                 * 播放中写进去**没有任何效果**（文件里反复记过这件事）。
                 *
                 * 真机表现就是：点「全屏」，左右两栏确实让位了，但画面停在原地，
                 * 用户只能说"全屏还是不行"。日志里却是一行"SET_RECT ... took 3ms"，
                 * 看起来完全正常 —— 这种"成功但无效"比报错更难查。
                 *
                 * 现在把判据收紧：sw_scale 档只要没有 overlay 接口，就一律走重建
                 * （原地 seek，caps 跟着重算），不再赌属性生效。
                 */
                int need_rebuild = 0;
                if (g_w.sw_scale) {
                    need_rebuild = !size_same || (pb2_overlay_fn() == NULL);
                }
                if (need_rebuild) {
                    if (g_w.is_live) {
                        live_rebuild_pipeline("SET_RECT deferred");
                        pb2_logf("SET_RECT %s deferred (sw_scale: live rebuild)", g_w.rect);
                    } else {
                        g_w.seek_target = g_w.last_pos_ms > 0 ? g_w.last_pos_ms : 0;
                        g_w.seek_pending = 1;
                        g_w.seek_last_ms = pb2_now_ms() - PB2_SEEK_DEBOUNCE_MS - 1;
                        pb2_stats_patch_seek(&g_w.stats, 1, g_w.seek_target);
                        pb2_logf("SET_RECT %s deferred (sw_scale: caps fixed at OPEN; rebuild via in-place seek@%lld%s)",
                                 g_w.rect, g_w.seek_target,
                                 size_same ? ", move-only but overlay unavailable" : "");
                    }
                    rect_rebuilt = 1;
                } else if (overlay_apply_rect()) {
                    /* X6（无 videoscale），或 X7 上「只移动不改尺寸」：overlay 是
                     * 运行期改平面的正道 —— sink 自己重配 plane，buffer 尺寸没变，
                     * caps 不用重协商，画面不会变形也不会黑。 */
                    pb2_logf("SET_RECT %s via overlay%s (took %lldms)", g_w.rect,
                             size_same ? " [move only, no rebuild]" : "",
                             pb2_now_ms() - t0_rect);
                } else {
                    gst_util_set_object_arg(g_w.vsink_el, "render-rectangle", g_w.rect);
                    if (g_w.paused && g_w.pipe && g_w.pipeline_alive) {
                        gst_element_set_state(g_w.pipe, GST_STATE_PLAYING);
                        pb2_sleep_ms(200);
                        rect_pulsed = 1;
                    }
                    pb2_logf("SET_RECT %s took %lldms%s", rect,
                             pb2_now_ms() - t0_rect,
                             rect_pulsed ? " [property first, then pulse PLAYING]" : "");
                }
            }

            /*
             * 单一出口的脉冲收尾：只有本次真的脉冲过、管线还活着、
             * 会话仍是暂停态、且没走重建路径（重建方自己决定状态）时，
             * 才把状态冻回 PAUSED。
             */
            if (rect_pulsed && !rect_rebuilt &&
                g_w.paused && g_w.pipe && g_w.pipeline_alive) {
                gst_element_set_state(g_w.pipe, GST_STATE_PAUSED);
                pb2_logf("SET_RECT: pulse done -> back to PAUSED");
            }
            break;
        }
        case PB2_CMD_STOP:
            pb2_logf("STOP");
            teardown_pipeline();
            reap_dying(); /* 【P1-3】旧源不会自己消失，停止时一并回收 */
            reset_session_fields();
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
            reset_session_fields();
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
        /*
         * 【诊断】支持通过 /tmp/pb2_gstdebug 临时打开 GStreamer 调试输出
         * （写入 /tmp/pb2_gst.log），用于定位 mppvideodec 启动耗时。
         * 不存在该文件时行为不变。
         */
        {
            FILE *gd = fopen("/tmp/pb2_gstdebug", "r");
            if (gd) {
                char spec[256];
                if (fgets(spec, sizeof(spec), gd)) {
                    size_t n = strlen(spec);
                    while (n > 0 && (spec[n-1] == 10 || spec[n-1] == 13)) spec[--n] = 0;
                    if (n > 0) setenv("GST_DEBUG", spec, 1);
                }
                fclose(gd);
                setenv("GST_DEBUG_FILE", "/tmp/pb2_gst.log", 1);
                setenv("GST_DEBUG_NO_COLOR", "1", 1);
            }
        }
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

            /*
             * 【直播】deferred seek 一律丢弃。
             *
             * 直播下 do_seek() 会打一行 `SEEK ignored: live stream` 就返回，
             * 在这里拦一道只是省掉多余的日志噪声 —— 更重要的是挡住
             * refresh_audio_route() 投递的那次「原地 seek」（插拔耳机触发），
             * 那条走 do_seek 会变成空转，直播不会被打断。
             */
            if (g_w.seek_pending && now - g_w.seek_last_ms >= PB2_SEEK_DEBOUNCE_MS) {
                long long target = g_w.seek_target;
                g_w.seek_pending = 0;
                if (g_w.is_live) {
                    pb2_logf("SEEK dropped: live stream (queued target=%lld)", target);
                    pb2_stats_patch_seek(&g_w.stats, 0, target);
                } else {
                    do_seek(target);
                    continue;
                }
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
            /* 【起播迟到宽限】到点把 kmssink 切回严格迟到策略 */
            update_late_policy();
            check_plane();
            /* 【P0-1】首帧看门狗：15s 无帧或源先死 → error（含拆管线） */
            check_first_frame();
            /* 【P1-1】首帧之后源中途死掉 → 放掉 hold_fd 让 demuxer 发 EOS */
            check_src_after_first_frame();
            /* 【音频停摆看门狗】视频在跑但音频字节冻结 → 原地 seek 救回来 */
            check_audio_stall();
            /* 【P1-4】直播断流检测 + 到点自动重连（点播路径第一行就 return） */
            check_live_reconnect();
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

/*
 * 【X7 Pro 排障】本次会话最后一次下发给 kmssink 的 render-rectangle。
 *
 * 黑屏/画面偏移时最需要知道的就是这串数字：pmid 的挖洞是按视口 CSS 算的，
 * 平面是按面板坐标算的，两边一旦对不上，用户看到的就是「画面贴在底部/被压扁」。
 * 把真正生效的 <x,y,w,h> 暴露到 info().rect，自检页一行就能比对。
 */
const char *pb2_session_rect(void) { return g_w.rect; }

/* 【X7 Pro 适配】info()/diag 读当前会话实际生效的设备参数（不是宏默认值） */
int pb2_session_panel_offset(void) {
    return g_w.panel_x_offset > 0 ? g_w.panel_x_offset : PB2_PANEL_X_OFFSET;
}

int pb2_session_plane_id(void) { return g_w.kms_plane_id; }
int pb2_session_sw_scale(void) { return g_w.sw_scale; }
int pb2_session_buf_scale(void) {
    return (g_w.buf_scale >= 50 && g_w.buf_scale <= 400) ? g_w.buf_scale : 100;
}
/* 1 = 本会话因音频设备被占而主动降级为无声（片源本身有音轨） */
int pb2_session_audio_off(void) { return g_w.audio_off; }

unsigned long long pb2_session_frames(void) {
    return atomic_load_explicit(&g_w.probe.frames, memory_order_relaxed);
}

/*
 * 音频健康（status().audioDead 的真值来源）。
 *
 * 语义：**期望有声音、但确实一个音频字节都没有**。
 * 只在「已经自愈过一次仍无效」时才报 1 —— 起播前 4.5s 的窗口里不报警，
 * 免得正常慢启动被 UI 打成「无声」。
 *
 * 以前 jsapi 里把 audioDead 写死成 0，页面那条「无声：…」的提示永远不会出现，
 * 用户只能自己发现没声音。
 */
int pb2_session_audio_dead(void) {
    if (!g_w.req || !g_w.req->audio) return 0;
    if (!g_w.pipeline_alive) return 0;
    if (g_w.paused || g_w.eos || g_w.is_live) return 0;
    if (g_w.audio_heals <= 0) return 0;
    if (g_w.first_frame_ms <= 0) return 0;
    if (pb2_now_ms() - g_w.first_frame_ms < PB2_AUDIO_START_GRACE_MS) return 0;
    if (atomic_load_explicit(&g_w.probe.abytes, memory_order_relaxed) > 0) return 0;
    if (!g_w.abranch && !(g_w.req->input2 && g_w.req->input2[0])) return 0;
    return 1;
}

int pb2_session_audio_heals(void) {
    return g_w.audio_heals;
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
