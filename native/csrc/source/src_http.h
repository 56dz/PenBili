/*
 * src_http.h - 媒体源：本地文件直读，或 ffmpeg 拉流转 FIFO（视频/音频各自一条）
 *
 * ============================ 为什么需要 ffmpeg 中转 ============================
 * 真机实测（spike-gst）：**GStreamer 直连 HTTP 永远无法 seek**。
 *   - souphttpsrc 继承 GstBaseSrc 但只实现 push 模式 create()，不实现 pull_range
 *   - qtdemux 在 push 模式下直接丢弃 seek 事件：
 *       "qtdemux: ignoring seek in push mode in current state"
 *   - qtdemux 报 "seekable: 1 (0 - 2305151)" 是骗人的（只看 Accept-Ranges/Content-Length）
 *   - 实测 netseek 到 30000ms：RETURNED=0，pos 仍按 1x 前进，根本没跳
 *
 * 所以网络源必须由 ffmpeg 负责定位（它走 HTTP Range），GStreamer 只读本地。
 *
 * ============================ 为什么是 FIFO + Matroska ============================
 * 中间不落盘，ffmpeg 直接写命名管道（FIFO），GStreamer 用 filesrc 读同一个 FIFO。
 * 容器必须选 **Matroska**：
 *   - Matroska 是流式友好容器，头部写完即可解码，不需要回填索引
 *   - MP4 需要 moov（在尾部，除非 faststart 前置），且 filesrc 会把文件长度快照，
 *     边写边读会永久卡死（实测 growtest 永久卡死）
 *   - mpegts 在 tsdemux 上双分支 preroll 永久卡死 + ffmpeg 输出时间戳错乱，已排除
 *   - fragmented mp4 视频帧间隔严重超标（videorate 疯狂复制帧），已排除
 *
 * 实测（lead，/tmp/probe2）：边下边播 1269ms 等到头、2282ms 首帧、
 * 之后持续出帧无停顿；暂停恢复首帧 34ms、POST_RESUME_FPS=29.97。
 *
 * ============================ 为什么是「双 FIFO」而不是一条 FIFO ============================
 * 【lead 实测，必须遵守】单 FIFO 双输入会永久卡在 PREROLLING：
 *   `ffmpeg -ss 5 -i video -ss 5 -i audio -map 0:v -map 1:a -c copy -f matroska -`
 *   时，ffmpeg 只把**第一路**（视频）重基到 0，第二路（音频）保留原始时间戳：
 *       video: 0.000, 0.267, 0.133 ...
 *       audio: 4.992, 5.016, 5.039 ...   <- 差 5 秒
 *   matroskademux 双分支时，音频要等 5 秒才有数据，但 preroll 必须两路都出首帧
 *   => 永久等待。`-avoid_negative_ts make_zero`、`-fflags +genpts`、
 *   `-reset_timestamps 1`、`-copyts -start_at_zero`、`-max_interleave_delta 0`
 *   **全部无效**（lead 逐一试过）。
 *
 * 解法（lead 已实测 PLAYING=1 成功）：**视频、音频各起一个 ffmpeg，各写一条 FIFO，
 * GStreamer 侧两个独立的 filesrc + 两个独立的 demuxer，但必须挂在同一个 pipeline 上**
 * （共享同一个 GstClock，音视频才对齐）。两路各自从 0 起，天然对齐。
 *
 * ============================ 三条路径 ============================
 *   1) input2 非空（B 站 DASH：视频一路、音频一路）-> 双 FIFO + 双 demux
 *   2) input2 为空且源自带音轨            -> 单 FIFO + 单 demux（lead 验证 vid_fps=29.67）
 *   3) input2 为空且源只有视频轨          -> 单 FIFO + 单 demux 只接 video 分支
 *   （2/3 由 session 的 pad-added 动态分支自动区分，src 层不用管）
 *
 * ============================ FIFO 打开顺序（重要） ============================
 * 直接 mkfifo 后让 ffmpeg 写、filesrc 读会互相阻塞（写端等读者、读端等写者）。
 * 解法：本模块先以 **O_RDWR** 打开 FIFO（Linux 允许，永不阻塞），
 * 这样写端和读端都能立刻打开成功；本模块从不读这个 fd，
 * 所以数据全部流向 filesrc。
 *
 * 【P1-2 代价】O_RDWR 意味着**我们自己也是读端**，写端全没了以后 filesrc
 * 也等不到 EOF —— 「正常播完」和「ffmpeg 崩了」在 GStreamer 侧一模一样，
 * 都收不了尾。所以不能只是「ffmpeg 退出后关掉它即可」，必须由
 * pb2_src_reap_if_dead() 确认写端进程真的死了之后主动 close(hold_fd)；
 * 正常收尾走 pb2_src_close()。
 *
 * ============================ 调用约束 ============================
 * 全部只能在 worker 线程调用（会 fork/exec、会 open FIFO）。
 */
#ifndef PENBILI_PB2_SRC_HTTP_H
#define PENBILI_PB2_SRC_HTTP_H

#include <sys/types.h>

#define PB2_SRC_DIR "/tmp/pb2"
#define PB2_SRC_FIFO_MAX 128

typedef struct {
    char location[PB2_SRC_FIFO_MAX]; /* 交给 filesrc 的路径（主源：视频） */
    /*
     * 第二路路径（交给第二个 filesrc）。
     *   - 本地双文件：第二个本地文件
     *   - 网络双 FIFO：音频 FIFO
     *   - 其它：空串
     */
    char location2[PB2_SRC_FIFO_MAX];

    char fifo[PB2_SRC_FIFO_MAX];  /* 主 FIFO（非空表示要清理） */
    char fifo2[PB2_SRC_FIFO_MAX]; /* 副 FIFO（双 FIFO 模式） */
    int hold_fd;                  /* 主 FIFO 的 O_RDWR fd，-1 表示无 */
    int hold_fd2;                 /* 副 FIFO 的 O_RDWR fd */
    pid_t pid;                    /* 主 ffmpeg pid，0 表示无 */
    pid_t pid2;                   /* 副 ffmpeg pid */
    long long baseMs;             /* ffmpeg -ss 的基准（对 JS 报告位置要加它） */
    int is_http;
    int dual;                     /* 1 = 双 FIFO 双 demux 模式 */
    int active;
} pb2_src_t;

/*
 * 启动媒体源。
 *   input    主源：http(s) URL 或本地路径（视频）
 *   input2   可选第二源（B 站 DASH 音频），NULL/空表示没有
 *   ua/ref   可选 HTTP 头（B 站 CDN 不带 Referer 会 403）
 *   baseMs   起始位置（ms），ffmpeg 用 -ss 前置实现
 *   durationMs 总时长（ms），>0 时用于给 ffmpeg -t 设上限，避免多下无用数据
 * 返回 0 成功，-1 失败。
 */
int pb2_src_open(pb2_src_t *s, const char *input, const char *input2, const char *ua,
                 const char *ref, long long baseMs, long long durationMs);

/*
 * 关闭媒体源：并行 SIGTERM 两个 ffmpeg（reap_pair，宽限 PB2_REAP_GRACE_MS 后
 * SIGKILL）、关 hold_fd/hold_fd2、删 FIFO。不是串行 kill+wait —— 串行回收
 * 曾吃掉约 500ms 的 seek 关键路径（P1-3）。
 * 幂等，可重复调用。
 */
void pb2_src_close(pb2_src_t *s);

/*
 * 非阻塞检查媒体源是否还活着。
 *
 * 主路径是 waitpid(pid, &st, WNOHANG)，但宿主进程把 SIGCHLD 设成 SIG_IGN，
 * 子进程退出后被内核自动回收，waitpid 会返 -1 / ECHILD 而拿不到真实状态。
 * 所以 errno==ECHILD 时还要退化成读 /proc/<pid>/stat 判活（见 pid_alive()）——
 * 只看 waitpid 返回值会把「已退出且已被回收」误判成「还活着」。
 *
 * 返回 1 = 活着（或本地文件源），0 = 已退出。
 * 注意：它只看进程，不动 fd；要让读端收到 EOF 请用 pb2_src_reap_if_dead()。
 */
int pb2_src_alive(pb2_src_t *s);

/*
 * 【P1-1 / P1-2】在「首帧之后」也要能发现源已死，并让读端能收到 EOF。
 *
 * 为什么需要这个而不是只用 pb2_src_alive()：
 *   - pb2_src_alive() 只回答「进程还在吗」，不动 fd。而 FIFO 是用 O_RDWR
 *     打开的（本进程自己也是读端，见本文件顶部第 47-51 行），所以写端消失后
 *     filesrc 永远等不到 EOF，matroskademux 不发 EOS —— 「正常播完」和
 *     「ffmpeg 崩了」在 GStreamer 侧长得一模一样，都无法收尾。
 *   - 本函数在确认写端进程真的没了之后，才 close(hold_fd)（用 -1 做哨兵，
 *     与 pb2_src_close() 幂等），这样 filesrc 立刻读到 EOF → EOS → 正常收尾。
 *     ffmpeg 活着的时候绝不 close，否则它会收到 EPIPE 提前退出。
 *
 * 返回 1 = 主源（视频 ffmpeg）已死且 hold_fd 已释放（调用方应当把它当 EOS
 *          处理：收尾 / 置 error / 拆管线）；0 = 还活着或无需处理。
 * 音频 pid2 先死不算致命（画面优先），只放掉 hold_fd2，返回值仍是 0。
 *
 * 建议调用点：session.c 的 worker 队列空分支周期任务里，按 PB2_SRC_CHECK_MS
 * 的节奏调用一次。**用自己的节流字段**（session.c 的 last_src_check2_ms），
 * 不能和首帧前的 check_first_frame() 共用 last_src_check_ms —— 两者轮流把
 * 同一个时间戳推到现在，谁都等不到 PB2_SRC_CHECK_MS，探测会被饿死。
 * 幂等，可重复调用。
 */
int pb2_src_reap_if_dead(pb2_src_t *s);

/*
 * 清理 /tmp/pb2 下残留 FIFO（属主进程已死）。
 * 进程启动时调一次，防止上个进程被 SIGKILL 后留下垃圾。
 * 只在 worker 线程调用。
 */
void pb2_src_cleanup_stale(void);

#endif /* PENBILI_PB2_SRC_HTTP_H */
