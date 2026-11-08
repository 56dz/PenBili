/*
 * queue.h - JSAPI 线程 -> worker 线程的命令队列 + 只读统计快照
 *
 * ============================ 设计约束 ============================
 * JSAPI 线程（QuickJS 单线程）**绝对不允许阻塞**：p99 < 5ms、最坏 < 20ms。
 * 所以这里的所有入口都是：
 *   - 无锁 SPSC 环形队列（生产者=JS 线程，消费者=worker 线程）；
 *   - 生产者只做一次原子 load + 几次普通写 + 一次原子 store；
 *   - 唤醒用 pthread_cond_signal（glibc 上是 futex wake，~1us，不阻塞）。
 *
 * 单生产者假设：QuickJS 是单线程的，所有 JSAPI 回调都在同一个线程里跑，
 * 因此 SPSC 成立。若将来有第二个线程调用 JSAPI，必须改成 MPSC。
 *
 * ============================ 统计快照 ============================
 * status() 要读十几个字段，逐个 _Atomic 读会有撕裂（读到半新半旧）。
 *
 * 分成两块：
 *   1) pb2_stats_t —— 由 **worker 线程单独拥有写权** 的字段，用 seqlock：
 *      worker 写前 seq++（变奇数），写完 seq++（变偶数）；
 *      读者读 seq、读数据、再读 seq，不一致或为奇数就重试。
 *      读者**永远不写、不加锁、不阻塞**，最多重试 8 次。
 *   2) pb2_probe_t —— 由 **GStreamer 流线程（pad probe）** 写的字段
 *      （frames / lastFrameMs / abytes），用 relaxed 原子量。它们不参与
 *      seqlock，因为流线程不是 worker，不能进 seqlock 的写者角色。
 *      status() 直接原子读这几个值，最多差一帧，对 UI 无影响。
 */
#ifndef PENBILI_PB2_QUEUE_H
#define PENBILI_PB2_QUEUE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

/* ---------------- 命令 ---------------- */

typedef enum {
    PB2_CMD_NONE = 0,
    PB2_CMD_OPEN = 1,     /* ptr -> pb2_open_req_t（worker 负责 free） */
    PB2_CMD_PAUSE = 3,
    PB2_CMD_RESUME = 4,
    PB2_CMD_SEEK = 5,     /* i64 = 目标毫秒 */
    PB2_CMD_STOP = 6,
    PB2_CMD_RELEASE = 7,
    /*
     * 【PB2_CMD_SET_RECT】运行时改 kmssink 的 render-rectangle。
     * i64 编码 4 个 int16：x | y<<16 | w<<32 | h<<48（负坐标用二进制补码，
     * 480x960 面板内坐标范围足够）。worker 收到后对 g_w.vsink 调
     * gst_util_set_object_arg(sink,"render-rectangle","<x,y,w,h>") ——
     * 实测（spike-report §12）立即生效、无需重建管线。
     * 用途：评论输入唤起系统键盘时把视频平面缩小避让（键盘是 UI 层，
     * 被 KMS overlay 平面盖住，只能动平面矩形）。
     * 【原 PB2_CMD_REDRAW 是个无实现占位，值 8 保留给兼容，新命令从 11 起。】
     */
    PB2_CMD_SET_RECT = 11, /*
                            * i64 = x | (y<<16) | (w<<32) | (h<<48)
                            * w==0 或 h==0 → 隐藏平面（矩形 0,0,0,0）。
                            */
    /*
     * 【PB2_CMD_RECOVER】原地自愈，无参数。
     *
     * 以前 JS 侧 recoverSession() 只能探测到 native 没有 recover 导出，
     * 于是退化成 seekBy(currentPos)。点播下这条路能用（seek 会重建管线），
     * 但直播下 do_seek() 是早退的 —— 看门狗 3 次配额全打空转，
     * 用户看到「画面冻住、点哪里都没反应，只能返回列表重进」。
     *
     * 现在 native 提供真正的自愈入口：
     *   - 直播 → live_rebuild_pipeline("recover")（重拉 souphttpsrc）
     *   - 点播 → 以当前位置投递一次原地 seek（与旧 JS 行为一致，但少一次往返）
     */
    PB2_CMD_RECOVER = 12,
    /*
     * 【PB2_CMD_SET_SOUND】运行时一键解除静音（3.5.6 第五轮）。
     * i64 = 1 强制有声 / 0 只恢复自动判定。worker 收到后执行
     * pb2_force_sound()：删 /tmp/pb2_mute、建 /tmp/pb2_sound、
     * 打开 Playback Path，并在系统音量为 0 时给一个可听的默认值。
     * 必须走 worker：这些操作会 fork/exec amixer（几十毫秒），
     * JSAPI 线程绝不允许阻塞。
     */
    PB2_CMD_SET_SOUND = 13,
    PB2_CMD_QUIT = 10
} pb2_cmd_type_t;

typedef struct {
    int type;
    long long i64;   /* seek 目标 */
    void *ptr;       /* OPEN 请求体 */
} pb2_cmd_t;

#define PB2_CMD_CAP 64 /* 必须是 2 的幂 */

typedef struct {
    pb2_cmd_t slots[PB2_CMD_CAP];
    _Atomic unsigned int head; /* 生产者写 */
    _Atomic unsigned int tail; /* 消费者写 */
    pthread_mutex_t mu;
    pthread_cond_t cv;
    _Atomic int quit;
} pb2_cmdq_t;

void pb2_cmdq_init(pb2_cmdq_t *q);
void pb2_cmdq_destroy(pb2_cmdq_t *q);

/*
 * 投递一条命令。**从不阻塞**。
 * 返回 0 成功，-1 队列满（调用者负责 free(cmd->ptr)）。
 */
int pb2_cmdq_push(pb2_cmdq_t *q, const pb2_cmd_t *cmd);

/*
 * 消费者取一条。**不阻塞**。
 * 返回 0 取到，-1 空。
 */
int pb2_cmdq_pop(pb2_cmdq_t *q, pb2_cmd_t *out);

/*
 * worker 专用：最多等 wait_ms 毫秒直到有新命令或 quit。
 * 返回 0 = 有命令或 quit 置位。
 * 【已知偏差 P2-7】实现（queue.c 的 pb2_cmdq_wait）目前**总是返回 0**，
 * 超时也不返回 -1（唯一调用方 worker_main 不读返回值，因此无实际影响）。
 * 这里的 -1 = 超时是设计契约，尚未落实；改实现前不要依赖它。
 * 生产者 signal 时不需要持有 mu（数据可见性由 ring 的 acquire/release 保证）。
 */
int pb2_cmdq_wait(pb2_cmdq_t *q, int wait_ms);

void pb2_cmdq_wake(pb2_cmdq_t *q);
void pb2_cmdq_quit(pb2_cmdq_t *q);
int pb2_cmdq_quit_flag(pb2_cmdq_t *q);

/* ---------------- 播放状态 ---------------- */

enum {
    PB2_STATE_IDLE = 0,
    PB2_STATE_OPENING = 1,
    PB2_STATE_PLAYING = 2,
    PB2_STATE_PAUSED = 3,
    PB2_STATE_ERROR = 4,
    PB2_STATE_STOPPED = 5
};

/* 错误码（与 docs/design/player2-api.md 一致） */
#define PB2_E_NONE 0
#define PB2_E_BAD_ARG 1
#define PB2_E_BAD_INPUT 2
#define PB2_E_BAD_HEADERS 3
#define PB2_E_BAD_RECT 4
#define PB2_E_GST_INIT 5
#define PB2_E_PIPELINE 6
#define PB2_E_OPEN_FAILED 7
#define PB2_E_NO_STREAM 8
#define PB2_E_BUSY 9
#define PB2_E_SEEK 10
#define PB2_E_EOS 11
#define PB2_E_PLANE_LOST 12
#define PB2_E_TIMEOUT 13
#define PB2_E_INTERNAL 14
/*
 * 【P0-1】15s 内没有任何一帧上屏（URL 过期 / 断网 / ffmpeg 秒退）。
 * JS 侧 watchdog 只看 lastError != 0 + state == 'error'，所以这里只要给一个
 * 非 0 且稳定的码即可；单独开码是为了日志/诊断能区分「没数据」和「解码失败」。
 */
#define PB2_E_NO_FIRST_FRAME 15
/* 【P0-4】动态分支建不起来（ghost sink pad 缺失 / gst_pad_link 失败） */
#define PB2_E_BRANCH_LINK 16

#define PB2_ERRMSG_CAP 192

typedef struct {
    /* seqlock 版本号（奇数=正在写） */
    _Atomic unsigned int seq;

    int state;
    long long positionMs;
    long long durationMs;
    int fps;                        /* 最近 1s 实测 fps */
    int hasAudio;
    long long audioBytes;           /* 已消耗的音频字节（近似，用于诊断） */
    int seekPending;
    long long seekTargetMs;
    int lastError;                  /* 0 = 无；其它为 PB2_E_* */
    char lastErrorMsg[PB2_ERRMSG_CAP];
    int planeActive;                /* KMS 平面 Esmart1-win0 是否 ACTIVE */
    int planeLostStreak;            /* 连续检测到平面丢失的次数 */
    int pipelineAlive;              /* 管线对象是否还在 */

    /*
     * 【P1-8】resyncing / resyncingMs / resyncCount：native 侧自动恢复的
     * 「让位信号」。JS 两个看门狗都用
     *     !st.gateActive && (!st.resyncing || st.resyncingMs > 15000)
     * 做让位条件；player2 以前一个字段都不给，导致让位条件恒真（看门狗裸奔）。
     *
     * v2 没有音频重同步机制（单一权威时钟），resyncing 只在 seek 重建管线
     * 到首帧上屏这段时间置 1 —— 那正是「画面暂时没有新帧」但 native 正在
     * 自己处理的时刻，JS 必须让位。
     */
    int resyncing;                  /* 1 = seek 重建中（等首帧） */
    long long resyncStartMs;        /* resyncing 置 1 的单调毫秒时刻 */
    int resyncCount;                /* 本会话累计 resync 次数 */
} pb2_stats_t;

void pb2_stats_init(pb2_stats_t *s);

/* 只有 worker 线程调用 */
void pb2_stats_write(pb2_stats_t *s, const pb2_stats_t *src);

/* 任何线程可调用，无锁，最多重试 8 次后放弃（返回 -1） */
int pb2_stats_read(pb2_stats_t *s, pb2_stats_t *out);

/* 单字段快速更新（worker 用，避免为了改一个字段复制整个结构体） */
void pb2_stats_patch_state(pb2_stats_t *s, int state);
void pb2_stats_patch_error(pb2_stats_t *s, int code, const char *msg);
void pb2_stats_patch_position(pb2_stats_t *s, long long pos, long long dur);
void pb2_stats_patch_seek(pb2_stats_t *s, int pending, long long target);
void pb2_stats_patch_plane(pb2_stats_t *s, int active, int streak);
void pb2_stats_patch_audio(pb2_stats_t *s, int hasAudio, long long bytes);
/*
 * 【P1-7】只更新 audioBytes，绝不动 hasAudio。
 * hasAudio 的权威来源是「音频分支建出来了没有」（pad-added 置 1 / teardown 清 0），
 * 不能被 refresh_position() 的字节数变化顺手写掉 —— seek 重建窗口里
 * abranch 还是 0，会把 hasAudio 闪成 0。周期刷新字节数一律用这个函数。
 */
void pb2_stats_patch_audio_bytes(pb2_stats_t *s, long long bytes);
void pb2_stats_patch_fps(pb2_stats_t *s, int fps);
void pb2_stats_patch_alive(pb2_stats_t *s, int alive);
/* 【P1-8】resyncing 让位信号：start_ms <= 0 表示结束 resync（同时清零计时） */
void pb2_stats_patch_resync(pb2_stats_t *s, int resyncing, long long start_ms);

/* ---------------- pad probe 写、status 读的小计数器 ---------------- */

typedef struct {
    _Atomic unsigned long long frames;   /* 视频 sink pad 收到的 buffer 数 */
    _Atomic long long lastFrameMs;       /* 最近一帧到达的单调毫秒 */
    _Atomic unsigned long long abytes;   /* 音频 sink pad 收到的字节数 */
} pb2_probe_t;

void pb2_probe_init(pb2_probe_t *p);
unsigned long long pb2_probe_frames(pb2_probe_t *p);
long long pb2_probe_last_frame_ms(pb2_probe_t *p);
unsigned long long pb2_probe_abytes(pb2_probe_t *p);
void pb2_probe_reset(pb2_probe_t *p);

#endif /* PENBILI_PB2_QUEUE_H */
