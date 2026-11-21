#include "queue.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "log.h"

/* ============================ 命令队列 ============================ */

void pb2_cmdq_init(pb2_cmdq_t *q) {
    memset(q->slots, 0, sizeof(q->slots));
    atomic_store_explicit(&q->head, 0, memory_order_relaxed);
    atomic_store_explicit(&q->tail, 0, memory_order_relaxed);
    atomic_store_explicit(&q->quit, 0, memory_order_relaxed);
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
}

void pb2_cmdq_destroy(pb2_cmdq_t *q) {
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->cv);
}

int pb2_cmdq_push(pb2_cmdq_t *q, const pb2_cmd_t *cmd) {
    unsigned int head = atomic_load_explicit(&q->head, memory_order_relaxed);
    unsigned int tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    unsigned int next = (head + 1) & (PB2_CMD_CAP - 1);

    if (next == tail) return -1; /* 满 */

    q->slots[head] = *cmd;
    /* release：保证 slot 内容对消费者可见后才更新 head */
    atomic_store_explicit(&q->head, next, memory_order_release);

    /*
     * 唤醒 worker。持锁 signal 是 glibc 的保守做法（避免 lost wakeup 与
     * destroy 竞态），代价是一次 uncontended futex 操作，约 1us，可接受。
     * 如果这里出现争用，说明 worker 正在处理命令，本来也不需要额外唤醒。
     */
    if (pthread_mutex_trylock(&q->mu) == 0) {
        pthread_cond_signal(&q->cv);
        pthread_mutex_unlock(&q->mu);
    }
    return 0;
}

int pb2_cmdq_pop(pb2_cmdq_t *q, pb2_cmd_t *out) {
    unsigned int tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    unsigned int head = atomic_load_explicit(&q->head, memory_order_acquire);

    if (tail == head) return -1;

    *out = q->slots[tail];
    atomic_store_explicit(&q->tail, (tail + 1) & (PB2_CMD_CAP - 1), memory_order_release);
    return 0;
}

int pb2_cmdq_wait(pb2_cmdq_t *q, int wait_ms) {
    struct timespec ts;
    unsigned int head;
    unsigned int tail;

    head = atomic_load_explicit(&q->head, memory_order_acquire);
    tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    if (head != tail) return 0;
    if (atomic_load_explicit(&q->quit, memory_order_acquire)) return 0;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += wait_ms / 1000;
    ts.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&q->mu);
    /* 持锁后重新检查，避免 push 在 trylock 失败时丢掉唤醒 */
    head = atomic_load_explicit(&q->head, memory_order_acquire);
    tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    if (head == tail && !atomic_load_explicit(&q->quit, memory_order_acquire)) {
        (void)pthread_cond_timedwait(&q->cv, &q->mu, &ts);
    }
    pthread_mutex_unlock(&q->mu);
    return 0;
}

void pb2_cmdq_wake(pb2_cmdq_t *q) {
    pthread_mutex_lock(&q->mu);
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

void pb2_cmdq_quit(pb2_cmdq_t *q) {
    atomic_store_explicit(&q->quit, 1, memory_order_release);
    pb2_cmdq_wake(q);
}

int pb2_cmdq_quit_flag(pb2_cmdq_t *q) {
    return atomic_load_explicit(&q->quit, memory_order_acquire);
}

/* ============================ 统计快照 ============================ */

void pb2_stats_init(pb2_stats_t *s) {
    memset(s, 0, sizeof(*s));
    atomic_store_explicit(&s->seq, 0, memory_order_relaxed);
    s->state = PB2_STATE_IDLE;
    s->lastError = PB2_E_NONE;
}

void pb2_stats_write(pb2_stats_t *s, const pb2_stats_t *src) {
    unsigned int v = atomic_load_explicit(&s->seq, memory_order_relaxed);

    /* 进入写临界区：seq 变奇数 */
    atomic_store_explicit(&s->seq, v + 1, memory_order_relaxed);
    /*
     * 【P1-5】进入屏障：必须是 store-store 屏障（release 或 seq_cst），
     * **不能是 acquire**。原因：
     *
     *   写者要保证的顺序是「seq=odd 这个 store 先可见，之后才可见字段写」，
     *   即需要拦住后面的**普通字段写**被重排到 seq=odd 之前。
     *   - acquire 屏障只约束「后面的 load 不前移」，对 store→store 排序无效
     *     （aarch64 上编译成 dmb ishld，根本不遮挡普通 store 的重排），
     *     用了它照样能被观察到「数据已新、seq 仍是偶数」的撕裂快照。
     *   - release 屏障约束「前面的写不后移、后面的 store 不前移」，
     *     正是这里需要的 store-store 语义（等价于内核的 smp_wmb）。
     *   - seq_cst 在 release 之上再加全局总序，语义更强，同样正确。
     *
     * 这里选 seq_cst：进入临界区时把 seq 的可见性抬到最强，代价只是
     * 一条 dmb ish，pb2_stats_write 本来就不是热路径（worker 周期任务里几十毫秒一次）。
     */
    atomic_thread_fence(memory_order_seq_cst);

    s->state = src->state;
    s->positionMs = src->positionMs;
    s->durationMs = src->durationMs;
    s->fps = src->fps;
    s->hasAudio = src->hasAudio;
    s->audioBytes = src->audioBytes;
    s->seekPending = src->seekPending;
    s->seekTargetMs = src->seekTargetMs;
    s->lastError = src->lastError;
    memcpy(s->lastErrorMsg, src->lastErrorMsg, sizeof(s->lastErrorMsg));
    s->planeActive = src->planeActive;
    s->planeLostStreak = src->planeLostStreak;
    s->pipelineAlive = src->pipelineAlive;
    s->resyncing = src->resyncing;
    s->resyncStartMs = src->resyncStartMs;
    s->resyncCount = src->resyncCount;

    /* 离开写临界区：seq 变偶数 */
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&s->seq, v + 2, memory_order_relaxed);
}

int pb2_stats_read(pb2_stats_t *s, pb2_stats_t *out) {
    int i;
    for (i = 0; i < 8; i++) {
        unsigned int v1 = atomic_load_explicit(&s->seq, memory_order_acquire);
        if (v1 & 1u) continue; /* 正在写 */
        atomic_thread_fence(memory_order_acquire);

        out->state = s->state;
        out->positionMs = s->positionMs;
        out->durationMs = s->durationMs;
        out->fps = s->fps;
        out->hasAudio = s->hasAudio;
        out->audioBytes = s->audioBytes;
        out->seekPending = s->seekPending;
        out->seekTargetMs = s->seekTargetMs;
        out->lastError = s->lastError;
        memcpy(out->lastErrorMsg, s->lastErrorMsg, sizeof(out->lastErrorMsg));
        out->planeActive = s->planeActive;
        out->planeLostStreak = s->planeLostStreak;
        out->pipelineAlive = s->pipelineAlive;
        out->resyncing = s->resyncing;
        out->resyncStartMs = s->resyncStartMs;
        out->resyncCount = s->resyncCount;

        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&s->seq, memory_order_relaxed) == v1) return 0;
    }
    /* 8 次都被撕裂（几乎不可能）：返回最后一次读到的内容，宁可略旧不要卡住 */
    return -1;
}

/* ---- 单字段更新：读-改-写整个结构体，仍然是 worker 独占写 ---- */

static void stats_begin(pb2_stats_t *s, pb2_stats_t *tmp) {
    /*
     * 【P1-6】读撕裂（8 次全是奇数 seq）时 pb2_stats_read 返回 -1，tmp 内容是
     * 未定义的半截值。以前这里直接 (void) 忽略返回值，stats_end 会把这份垃圾
     * 整体写回去 —— 一次 patch 就把所有字段覆盖成旧值/半截值。
     * 现在退化成「全零 + 保留 state」，至少不会把 position/duration 打成随机数。
     */
    if (pb2_stats_read(s, tmp) != 0) {
        int st = s->state;
        memset(tmp, 0, sizeof(*tmp));
        tmp->state = st;
    }
}

static void stats_end(pb2_stats_t *s, pb2_stats_t *tmp) {
    pb2_stats_write(s, tmp);
}

void pb2_stats_patch_state(pb2_stats_t *s, int state) {
    pb2_stats_t t;
    stats_begin(s, &t);
    t.state = state;
    stats_end(s, &t);
}

void pb2_stats_patch_error(pb2_stats_t *s, int code, const char *msg) {
    pb2_stats_t t;
    stats_begin(s, &t);
    t.lastError = code;
    if (msg) {
        snprintf(t.lastErrorMsg, sizeof(t.lastErrorMsg), "%s", msg);
    } else {
        t.lastErrorMsg[0] = '\0';
    }
    stats_end(s, &t);
}

void pb2_stats_patch_position(pb2_stats_t *s, long long pos, long long dur) {
    pb2_stats_t t;
    stats_begin(s, &t);
    t.positionMs = pos;
    t.durationMs = dur;
    stats_end(s, &t);
}

void pb2_stats_patch_seek(pb2_stats_t *s, int pending, long long target) {
    pb2_stats_t t;
    stats_begin(s, &t);
    t.seekPending = pending;
    t.seekTargetMs = target;
    stats_end(s, &t);
}

void pb2_stats_patch_plane(pb2_stats_t *s, int active, int streak) {
    pb2_stats_t t;
    stats_begin(s, &t);
    t.planeActive = active;
    t.planeLostStreak = streak;
    stats_end(s, &t);
}

void pb2_stats_patch_audio(pb2_stats_t *s, int hasAudio, long long bytes) {
    pb2_stats_t t;
    stats_begin(s, &t);
    t.hasAudio = hasAudio;
    t.audioBytes = bytes;
    stats_end(s, &t);
}

/*
 * 【P1-7】只更新「已收到的音频字节数」，**绝不动 hasAudio**。
 *
 * 为什么需要单独一个函数：refresh_position() 每 100ms 观察到 abytes 变化就
 * 想更新 audioBytes，但它顺手把 hasAudio 也一起写了。而 hasAudio 的唯一
 * 权威来源是「音频分支建出来了没有」（on_pad_added 置 1 / teardown 清 0）。
 * seek 重建管线的那几百毫秒里 abranch 还是 0，只要此时 abytes 一变，
 * 就会把 hasAudio: 0 写进 stats 闪一下 —— JS 侧 `!!st.hasAudio` 会当真。
 *
 * 所以把「字节数」和「有没有音轨」这两个语义彻底分开：这个函数只碰字节数。
 */
void pb2_stats_patch_audio_bytes(pb2_stats_t *s, long long bytes) {
    pb2_stats_t t;
    stats_begin(s, &t);
    t.audioBytes = bytes;
    stats_end(s, &t);
}

void pb2_stats_patch_fps(pb2_stats_t *s, int fps) {
    pb2_stats_t t;
    stats_begin(s, &t);
    t.fps = fps;
    stats_end(s, &t);
}

void pb2_stats_patch_alive(pb2_stats_t *s, int alive) {
    pb2_stats_t t;
    stats_begin(s, &t);
    t.pipelineAlive = alive;
    stats_end(s, &t);
}

void pb2_stats_patch_resync(pb2_stats_t *s, int resyncing, long long start_ms) {
    pb2_stats_t t;
    stats_begin(s, &t);
    t.resyncing = resyncing ? 1 : 0;
    t.resyncStartMs = resyncing ? start_ms : 0;
    stats_end(s, &t);
}

/* ============================ pad probe 计数器 ============================ */

void pb2_probe_init(pb2_probe_t *p) {
    atomic_store_explicit(&p->frames, 0, memory_order_relaxed);
    atomic_store_explicit(&p->lastFrameMs, 0, memory_order_relaxed);
    atomic_store_explicit(&p->abytes, 0, memory_order_relaxed);
}

unsigned long long pb2_probe_frames(pb2_probe_t *p) {
    return atomic_load_explicit(&p->frames, memory_order_relaxed);
}

long long pb2_probe_last_frame_ms(pb2_probe_t *p) {
    return atomic_load_explicit(&p->lastFrameMs, memory_order_relaxed);
}

unsigned long long pb2_probe_abytes(pb2_probe_t *p) {
    return atomic_load_explicit(&p->abytes, memory_order_relaxed);
}

void pb2_probe_reset(pb2_probe_t *p) {
    atomic_store_explicit(&p->frames, 0, memory_order_relaxed);
    atomic_store_explicit(&p->lastFrameMs, 0, memory_order_relaxed);
    atomic_store_explicit(&p->abytes, 0, memory_order_relaxed);
}
