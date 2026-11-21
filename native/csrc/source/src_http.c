/*
 * src_http.c - 媒体源实现（本地直读 / ffmpeg -> FIFO -> Matroska）
 *
 * 设计依据见 src_http.h 顶部注释。这里记录实现层面的几个硬约束。
 *
 * ============================ 命令行构造 ============================
 * 下面的示意图是「最小可用形态」，**完整 argv 以 spawn_ffmpeg() 为准**，
 * 真实顺序为：
 *   PB2_FFMPEG -hide_banner -loglevel error -nostdin
 *              [-user_agent UA] [-headers "Referer: <REF>"]
 *              -ss BASE -i INPUT
 *              -map 0:v? （MAP_VIDEO 只放这一个；MAP_AUDIO 换成 0:a?；
 *                          MAP_BOTH 两个都放）
 *              -c copy -copyts -fflags +genpts
 *              （-copyts 见 spawn_ffmpeg() 内注释：保留原始 PTS 是双路
 *                ffmpeg seek 后 A/V 内容对齐的关键，goal-b29d60aa 问题 1）
 *              [-t TLIMIT] -f matroska -
 * `-t` 只在 durationMs > 0 且 baseMs < durationMs 时出现。⚠️ -copyts 下
 * `-t` 按原始时间轴判断（pts < t），值必须 = durationMs + 5s 余量
 * （不能减 baseMs！旧公式在 base 大时会把所有包滤掉，ffmpeg rc=0 但
 * 0 包输出 → 秒退无 stderr，见 spawn_ffmpeg() 内长注释）。
 *
 * 单源单 FIFO（input 有，input2 空）：
 *   ffmpeg -hide_banner -loglevel error -user_agent UA -headers "Referer: R\r\n"
 *          -ss BASE -i INPUT -map 0:v? -map 0:a? -c copy -copyts -f matroska -
 * 双源单 FIFO（input + input2，B 站 DASH，A/V 同步方案 A）：
 *   ffmpeg ... -ss BASE -i INPUT -ss BASE -i INPUT2 -map 0:v? -map 1:a?
 *          -c copy -copyts -fflags +genpts -f matroska -        -> 同一 条 FIFO
 *
 * 要点：
 *   1) **-ss 必须在 -i 之前**（input seek 走索引，快且精确）
 *   2) **-ss 只作用于紧跟的那个输入**，双输入时两路都要写一遍（踩过的坑）
 *   3) **-f matroska 必须显式给**（输出到 "-" 时 ffmpeg 无法从扩展名推断）
 *   4) **-map 用 `?` 后缀**，这样没有音轨时不会因 "does not contain any stream" 失败
 *      （旧实现 player.c 就在这里踩过坑）
 *   5) `-loglevel error` 抑制 "Broken pipe" 噪音（gst 关读端时 ffmpeg 必然报这个）
 *   6) 双输入合计**只有一个** demuxer/segment，音视频 PTS 同轴，seek 后天然对齐；
 *      详见 MAP_DUAL_IN 处注释（旧的「一源一 ffmpeg、一源一 FIFO」会让视频从
 *      关键帧 K 起、音频从 T 起，两条独立 segment 各自重基 => 音频领先 T-K）。
 *
 * ============================ FIFO 生命周期 ============================
 *   1) mkfifo(/tmp/pb2/pb2-<pid>-<seq>.fifo, 0600)
 *      现在所有 HTTP 路径都只有**一条** FIFO（双输入也合并成一条）；
 *      pb2_src_t 仍保留 fifo2/hold_fd2 字段（-1 / 空串）供本地双文件路径使用。
 *   2) hold_fd = open(fifo, O_RDWR|O_CLOEXEC|O_NONBLOCK)  <- 关键，见 src_http.h
 *   3) posix_spawn ffmpeg，用 posix_spawn_file_actions_addopen 把它的 stdout
 *      直接指到 FIFO 的 O_WRONLY 打开（不是 dup2）
 *   4) 返回 fifo 路径给 session，session 交给 filesrc
 *   5) close 时 reap_pair 并行回收两个 ffmpeg -> 关 hold_fd/hold_fd2 -> unlink
 */
#include "src_http.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "core/clock.h"
#include "core/log.h"

extern char **environ;

/* environ 不可靠时的最小环境（宿主 miniapp 里 environ 可能为空/非法） */
static char *g_min_env[] = {(char *)"PATH=/usr/bin:/bin:/sbin:/usr/sbin",
                            (char *)"LD_LIBRARY_PATH=/usr/lib:/lib",
                            (char *)"HOME=/tmp",
                            NULL};

#define PB2_FFMPEG "/usr/bin/ffmpeg"
#define PB2_FFMPEG_ALT "/bin/ffmpeg"
#define PB2_FFPROBE "/usr/bin/ffprobe"

/* 关键帧探测的单条缓存（见 probe_video_keyframe_ms 顶部注释） */
static char g_kf_input[768];
static long long g_kf_target = -1;
static long long g_kf_k = -1;

/* ffmpeg 输出模式 */
#define MAP_BOTH 0  /* -map 0:v? -map 0:a?（单源，两路都在一个容器里） */
#define MAP_VIDEO 1 /* -map 0:v?（旧双 FIFO 的视频那条，已不再使用） */
#define MAP_AUDIO 2 /* -map 0:a?（旧双 FIFO 的音频那条，已不再使用） */
/*
 * 【A/V 同步修复（方案 A）】双输入单输出：
 *   ffmpeg -ss T -i <videoUrl> -ss T -i <audioUrl>
 *          -map 0:v? -map 1:a? -c copy -copyts -fflags +genpts [-t ..]
 *          -f matroska -            -> 一条 FIFO
 *
 * 为什么改成这样（旧双 FIFO 方案的根本缺陷）：
 *   旧方案视频、音频各起一个 ffmpeg、各写一条 FIFO、各接一个 matroskademux。
 *   两个 demuxer 各自把 segment.start 定成「自己第一条流的 PTS」：
 *     视频首包 PTS = K（-ss T 会退到 T 之前最近的关键帧，B 站 1~2s 一个）
 *     音频首包 PTS = T（音频没有关键帧概念，-ss 精确）
 *   => 视频 running_time = PTS-K、音频 running_time = PTS-T，
 *   两条分支都在 base_time 起播 => 内容 K 和内容 T 同时播 => 音频领先 T-K。
 *
 *   单 demuxer 只有一个 segment（start = 全局最小 PTS = K），
 *   内容 c 在视频/音频两路的 PTS 都是 c => running_time 都是 c-K => 天然对齐。
 *
 * 代价（已接受）：K..T 这段没有音频数据，音频要等 T-K 才出声（本来就没有那
 * 一段音频可播）；而旧的 -avoid_negative_ts make_zero 单 FIFO 双输入会卡
 * PREROLLING 的老坑，在 -copyts 下两路时间戳同轴（只差 T-K，正常 interleave），
 * 不再复现。
 */
#define MAP_DUAL_IN 3 /* 双输入、单输出（B 站 DASH：视频 + 音频两个 URL） */

/* 全局序号，用于生成唯一 FIFO 名（worker 单线程，不需要原子） */
static int g_seq = 0;

/* 【诊断】http 源的 ffmpeg stderr 写到 /tmp/pb2/err-<fifo>（默认 1）。
 * 置 0 恢复 /dev/null 行为。排查秒退/断流时看这个文件。 */
static int s_http_diag_stderr = 1;

static int is_http(const char *s) {
    return s && (strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0);
}

static void ensure_dir(void) {
    struct stat st;
    if (stat(PB2_SRC_DIR, &st) == 0 && S_ISDIR(st.st_mode)) return;
    if (mkdir(PB2_SRC_DIR, 0755) != 0 && errno != EEXIST) {
        pb2_logf("src: mkdir %s failed: %s", PB2_SRC_DIR, strerror(errno));
    }
}

/* 建一条 FIFO 并返回其 O_RDWR hold fd（永不阻塞）。失败返回 -1。 */
static int make_fifo(char *path, size_t cap) {
    int fd;
    unlink(path);
    if (mkfifo(path, 0600) != 0) {
        pb2_logf("src: mkfifo %s failed: %s", path, strerror(errno));
        path[0] = '\0';
        return -1;
    }
    /*
     * O_RDWR 打开 FIFO：Linux 特有行为，不会阻塞，且让写端立刻可打开。
     * 我们不从这个 fd 读任何数据，所以数据全部流向 filesrc 的读端。
     * 这个 fd 必须保持到整个播放结束，否则 ffmpeg 会收到 EPIPE 提前退出。
     */
    fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        pb2_logf("src: open fifo(O_RDWR) %s failed: %s", path, strerror(errno));
        unlink(path);
        path[0] = '\0';
        return -1;
    }
    (void)cap;
    return fd;
}

/*
 * 起 ffmpeg 写 FIFO。
 *
 * 注意：ffmpeg 的 stdout 必须 dup2 到 FIFO 写端。用 posix_spawn 的
 * addopen 直接打开 FIFO 路径即可（内核会等读端，而我们已经用 hold_fd
 * 以 O_RDWR 打开过了，所以不会阻塞）。
 */
/*
 * 【P1-4】把完整 ffmpeg argv 打进日志。
 *
 * 日志单行上限 1024 字节（core/log.c: PB2_LOG_MAX_LINE），而 B 站 CDN URL
 * 动辄 1KB+，所以 **一个 argv 元素打一行**，长值截中间保留头尾：
 *   - 以 '-' 开头的（参数名）一律原样完整打印，绝不被截
 *   - 长值（URL / UA / Referer）保留头 96 + 尾 64，中间用 ... 表示
 * 这样 `-t`/`-ss`/`-map`/`-f` 等关键参数一眼可查，又能确认 URL 确实带上了。
 */
static void log_argv(const char *tag, char *const argv[]) {
    int n = 0;
    int j;
    while (argv[n]) n++;
    pb2_logf("src: %s argv count=%d", tag, n);
    for (j = 0; j < n; j++) {
        const char *a = argv[j];
        size_t len = a ? strlen(a) : 0;
        if (!a) continue;
        if (a[0] == '-' || len <= 200) {
            pb2_logf("src: %s argv[%d]=%s", tag, j, a);
        } else {
            char mid[256];
            snprintf(mid, sizeof(mid), "%.96s...(%u bytes)...%s", a, (unsigned)len,
                     a + len - 64);
            pb2_logf("src: %s argv[%d]=%s", tag, j, mid);
        }
    }
}

/*
 * 【诊断】把**完整未截断**的 argv 落盘到 /tmp/pb2/argv-<tag>.txt。
 *
 * 为什么需要：日志单行 1024 字节上限逼得 log_argv() 必须截断 URL（B 站
 * CDN URL 约 700 字节，截断后就无法复现）。排查 403 / 时间戳问题时需要
 * 拿真 URL 在设备上手工跑同一条 ffmpeg 命令并用 ffprobe 看首包 PTS。
 * 一个元素一行，每次 spawn 覆盖写。失败时静默跳过（纯诊断，不影响播放）。
 */
static void dump_argv_full(const char *tag, char *const argv[]) {
    char path[160];
    FILE *fp;
    int j;
    snprintf(path, sizeof(path), "%s/argv-%s.txt", PB2_SRC_DIR, tag);
    fp = fopen(path, "w");
    if (!fp) return;
    for (j = 0; argv[j]; j++) fprintf(fp, "%s\n", argv[j]);
    fclose(fp);
}

/*
 * ============ 探测视频关键帧 K（修「seek 后开头静音 T-K 秒」） ============
 *
 * 【问题】用户反馈：拖动进度条后，视频先无声播放 1~2 秒，音频才加入
 * （加入之后音画是同步的）。这是我上一轮「A/V 同步修复」引入的副作用。
 *
 * 【机制】双输入单 demux + `-copyts` 下：
 *   · 视频是 `-c copy`，seek 只能落在**关键帧**上 → 实际输出从 K（≤T）开始；
 *   · 音频不是视频、没有关键帧概念，`-ss T` 是**精确**的 → 从 T 开始；
 *   · `-copyts` 保留原始 PTS，于是视频首包 PTS≈K、音频首包 PTS≈T；
 *   · matroskademux 取全局最小 PTS（=K）当 segment 起点，
 *     所以内容 K 在时钟 0 渲染，而音频要等时钟 T-K 才出声。
 *   ⇒ 视频先播 K→T 这段画面，全程静音，T-K 之后音频才加入。
 *
 * 【实测（BV1SDJC6dE4t，T=60）】
 *   现状：视频首包 pts=55.055、音频首包 pts=59.989 → 静音 4.93s
 *   关键帧间隔实测：T=61..65 全部落到 K=60.060，T=66 跳到 65.065
 *   ⇒ B 站这个片源 GOP≈5s，最坏情况静音接近 5 秒。
 *
 * 【修法】把音频也 seek 到**同一个关键帧 K**，两路首包 PTS 就对齐了：
 *   视频 -ss T（仍让 ffmpeg 自己找 K，等价于 -ss K）
 *   音频 -ss K（显式给，精确落到 K 附近）
 * 实测（T=62/100/200）：视频首包 60.060/95.095/195.195，
 *                      音频首包 60.053/95.083/95.179 → 只差 7~16ms，听不出来。
 *
 * 【代价】多一次 ffprobe（约 550~570ms，`-read_intervals T%+2` 只读 2 秒窗口，
 * 不是全片扫描）。只在 seek（baseMs > 0）时做，起播（baseMs == 0）不做 ——
 * 起播本来就从头开始，K=0 无差值。探测失败（网络抖动/ffprobe 不存在）时
 * 返回 -1，调用方退回「音频也用 T」，即旧行为（有静音但不错位）。
 */
/*
 * 给一路 HTTP 输入追加自动重连参数（都是 per-input 选项，调用方须在
 * 每个 `-i` 之前调用一次）。理由见调用点长注释。
 *
 * 注意 **不要** 加 `-reconnect_at_eof 1`：点播 m4s 正常读完就是 EOF，
 * 重连会把它从头再拉一遍，表现为「播完自动重播」。
 */
static void add_reconnect_args(char **argv, int *ip) {
    int i = *ip;
    argv[i++] = (char *)"-reconnect";
    argv[i++] = (char *)"1";
    argv[i++] = (char *)"-reconnect_streamed";
    argv[i++] = (char *)"1";
    argv[i++] = (char *)"-reconnect_delay_max";
    argv[i++] = (char *)"5";
    argv[i++] = (char *)"-reconnect_on_http_error";
    argv[i++] = (char *)"4xx,5xx";
    *ip = i;
}

static long long probe_video_keyframe_ms(const char *input, const char *ua, const char *ref,
                                        long long targetMs) {
    char cmd[1024];
    char uaarg[384];
    char refarg[704];
    char buf[512];
    FILE *fp;
    long long k = -1;

    /*
     * 【1 条缓存】自愈重试（check_first_frame 的 retry 分支）会用**同一个
     * baseMs** 再调一次 pb2_src_open。ffprobe 一次 550~750ms，重试时再探一次
     * 纯属浪费，还会拖长「源秒退后自愈」的恢复时间。同一个 (input, targetMs)
     * 只探一次。URL 每次刷新都会变，所以用 input 全串做 key 不会串味。
     */
    if (g_kf_k >= 0 && g_kf_target == targetMs && input && input[0] &&
        strncmp(g_kf_input, input, sizeof(g_kf_input) - 1) == 0) {
        pb2_logf("kf: cache hit K=%lldms (T=%lldms)", g_kf_k, targetMs);
        return g_kf_k;
    }

    if (!input || !input[0] || targetMs <= 0) return -1;
    if (strncmp(input, "http://", 7) != 0 && strncmp(input, "https://", 8) != 0) return -1;

    /* 参数都要单引号包住：UA 含空格和括号，Referer 含冒号和斜杠。
     * 单引号内的单引号无法转义，直接放弃带该参数的探测（回退旧行为）。 */
    uaarg[0] = '\0';
    if (ua && ua[0]) {
        if (strchr(ua, '\'')) return -1;
        snprintf(uaarg, sizeof(uaarg), " -user_agent '%s'", ua);
    }
    refarg[0] = '\0';
    if (ref && ref[0]) {
        if (strchr(ref, '\'')) return -1;
        snprintf(refarg, sizeof(refarg), " -headers 'Referer: %s'", ref);
    }

    /*
     * 【窗口只用 +1，且第一行就是关键帧 —— 这是实测出来的，不是猜的】
     *
     * ffprobe 的 `-read_intervals T%+1` 会**先 seek 到 T 之前最近的关键帧**
     * 再开始输出包，所以输出的第一行必然是 K（而不是从 T 精确开始）。设备实测：
     *   T=28  -> 25.000000,K_     T=60  -> 55.000000,K_
     *   T=200 -> 195.000000,K_    T=698 -> 695.000000,K_
     * 耗时 616~760ms，与窗口大小基本无关（主要是 HTTP 建连）。
     *
     * 以前写的是「先用 %+2，失败再退 %+6」，但 %+2 经常拿不到 K（GOP 实测 5s，
     * 从 T 起 2 秒内可能一个 K 都没有），于是**每次都要跑两遍 ffprobe**，
     * 实测探测 1575ms（737+838），把 seek 端到端推到 3234ms。
     * 改单次 %+1 后探测稳定在 ~700ms，端到端回落到 ~2000ms 以内。
     */
    /*
     * 【致命 bug 修复】以前这里传的是 `targetMs / 1000`（整数除法，截断到秒）：
     *   T=316629ms -> 传 316 -> ffprobe 从 316.000s 找关键帧 -> 拿到 310.910s
     *   真实关键帧是 315.915s（实测 ffprobe -read_intervals 316.629%+1 第一行）
     * 于是音频被 seek 到比视频早 5 秒的位置，A/V 偏 5s，matroskademux 被迫
     * 丢视频追音频（Synchronizing stream 0 ... by advancing time），而视频
     * 丢掉后必须等到下一个关键帧才能解码（GOP≈5s）→ 首次 seek 卡 11~12 秒
     * 完全不出帧。改成毫秒精度的秒串（与 -ss 参数同格式）后偏差消失。
     */
    {
        char tsec[32];
        snprintf(tsec, sizeof(tsec), "%lld.%03lld", targetMs / 1000, targetMs % 1000);
        snprintf(cmd, sizeof(cmd),
                 "%s -v error%s%s -select_streams v -show_entries packet=pts_time,flags "
                 "-read_intervals %s%%+1 -of csv=p=0 '%s' 2>/dev/null",
                 PB2_FFPROBE, uaarg, refarg, tsec, input);
    }

    fp = popen(cmd, "r");
    if (!fp) {
        pb2_logf("kf: popen failed (%s)", strerror(errno));
        return -1;
    }
    while (fgets(buf, sizeof(buf), fp)) {
        /* 行格式 "55.054813,K_" —— 逗号后是 flags，K 表示关键帧 */
        char *comma = strchr(buf, ',');
        if (!comma) continue;
        if (strncmp(comma + 1, "K", 1) != 0) continue;
        *comma = '\0';
        k = (long long)(atof(buf) * 1000.0 + 0.5);
        break;
    }
    pclose(fp);

    /*
     * 一次没探到 K 就放弃，调用方回退「音频也用 T」= 旧行为（有静音但不错位）。
     * 不再做第二次 ffprobe —— 双倍延迟比偶尔的静音更糟。
     */
    if (k >= 0) {
        snprintf(g_kf_input, sizeof(g_kf_input), "%s", input);
        g_kf_target = targetMs;
        g_kf_k = k;
    } else {
        pb2_logf("kf: no keyframe within +1s of %lldms, fallback to exact -ss",
                 targetMs);
    }
    return k;
}

static pid_t spawn_ffmpeg(const char *fifo, const char *input, const char *input2,
                          const char *ua, const char *ref, long long baseMs,
                          long long durationMs, int map_mode) {
    posix_spawn_file_actions_t fa;
    pid_t pid = 0;
    char ss[32];
    char ss2[32];
    long long audio_ss_ms = -1;
    char tlimit[32];
    char uabuf[320];
    char hdrbuf[640];
    int use_headers = 0;
    int use_ua = 0;
    int have_t = 0;
    /* 【诊断】ffmpeg stderr 落盘（默认 /dev/null 看不到死因）：
     * 秒退/断流时 cat 这个文件就知道是 403/超时/Range 不支持。 */
    char errfile[PB2_SRC_FIFO_MAX + 32];

    /* argv 上限：双输入各有 -ss/-input 两组，再加固定项，64 足够 */
    char *argv[64];
    int i = 0;

    snprintf(ss, sizeof(ss), "%lld.%03lld", baseMs / 1000, baseMs % 1000);
    if (baseMs < 0) snprintf(ss, sizeof(ss), "0");

    /*
     * 【关键 3】seek 时探测视频关键帧 K，让音频也 seek 到 K（见函数顶部长注释）。
     * 只在双输入 + seek（baseMs>0）时做；探测失败保持 -1 → 走旧行为（音频也用 T）。
     * 起播（baseMs==0）不做：K 本来就是 0，没有差值，省掉 550ms。
     */
    if (map_mode == MAP_DUAL_IN && input2 && input2[0] && baseMs > 0) {
        long long k = probe_video_keyframe_ms(input, ua, ref, baseMs);
        if (k >= 0 && k <= baseMs) {
            audio_ss_ms = k;
        } else {
            pb2_logf("src: keyframe probe failed (k=%lld, T=%lldms) -> audio seeks to T "
                     "(may be silent for up to one GOP)",
                     k, baseMs);
        }
    }

    /* 【关键】-copyts 语义坑（真机实测，2026-10-31）：
     * -copyts 下输出包保留原始 PTS（≈ -ss 的秒数），而 ffmpeg 的 `-t` 是
     * 「输出包 pts < t 则保留」——参照系是原始时间轴，不是播放时间轴！
     * 旧公式 remain = durationMs - baseMs 在 base 大时（如 base=949887ms、
     * dur=1266517ms → t=321.630）远小于原始 pts（≈946s）→ 所有包被 -t
     * 丢弃 → ffmpeg rc=0 退出但 0 packets muxed、stderr 全空 → 玩家端
     * 表现为「seek 后源秒退、无首帧」（goal-b29d60aa 第三跳 TIMEOUT）。
     * 实测临界点：T=945 → 831B（0 包）；T=946 → 117KB（正常），即原始
     * pts 946.044s。修法：copyts 下 -t 按原始时间轴 = durationMs + 5s。
     * （注意：音频路 0 基正常？不——音频路同样带 -copyts，原始 pts 同量级，
     * 同样受此影响。统一按 durationMs+5s 修。）
     */
    if (durationMs > 0 && baseMs < durationMs) {
        long long remain = durationMs + 5000; /* 原始时间轴截断点 + 5s 余量 */
        snprintf(tlimit, sizeof(tlimit), "%lld.%03lld", remain / 1000, remain % 1000);
        have_t = 1;
    }

    if (ua && ua[0]) {
        snprintf(uabuf, sizeof(uabuf), "%s", ua);
        use_ua = 1;
    }
    if (ref && ref[0]) {
        /* ffmpeg 的 -headers 需要 CRLF 结尾 */
        snprintf(hdrbuf, sizeof(hdrbuf), "Referer: %s\r\n", ref);
        use_headers = 1;
    }

    argv[i++] = (char *)PB2_FFMPEG;
    argv[i++] = (char *)"-hide_banner";
    argv[i++] = (char *)"-loglevel";
    argv[i++] = (char *)"error";
    argv[i++] = (char *)"-nostdin";
    if (use_ua) {
        argv[i++] = (char *)"-user_agent";
        argv[i++] = uabuf;
    }
    if (use_headers) {
        argv[i++] = (char *)"-headers";
        argv[i++] = hdrbuf;
    }
    argv[i++] = (char *)"-ss";
    argv[i++] = ss;
    /*
     * 【音频/视频中途断流】B 站的点播音视频来自**两台不同的 CDN 主机**
     * （实测 2026-11-04：视频 upos-sz-mirrorbd.bilivideo.com、音频
     * upos-sz-estgcos.bilivideo.com），任意一路的 TCP 连接在中途没了后续
     * 数据，ffmpeg 默认**不报错也不重连** —— 现场日志里 err-*.fifo 是 0 字节，
     * 而 gst 侧的 abytes 冻结 45 秒、画面照常，用户听到「播到一半声音没了」。
     *
     * 这些都是 per-input 的 HTTP 选项，两路都必须写一遍（同 -ss/-headers）。
     *   -reconnect          断开后自动重连
     *   -reconnect_streamed 作用于不可 seek 的流（m4s 就是流式）
     *   -reconnect_delay_max 退避上限；给 5s，太大会让用户干等
     *   -reconnect_at_eof 0 点播源正常结束时**不能**重连，否则会从头重播
     */
    add_reconnect_args(argv, &i);
    argv[i++] = (char *)"-i";
    argv[i++] = (char *)input;

    if (map_mode == MAP_DUAL_IN) {
        /*
         * 【关键 1】-ss 只作用于**紧跟其后的那一个**输入（input option，不是
         * global option），所以第二路必须再写一次 -ss，否则音频会从 0 开始
         * 下载/输出 —— 这是踩过的坑。
         *
         * 【关键 2】-user_agent / -headers 同样是 per-input 选项！真机实测
         * （2026-11-01 07:36）：只在第一个 -i 前给 headers 时，第二路音频
         * 请求不带 Referer/UA → B 站 CDN 直接 403 Forbidden →
         * `src: a ...: Server returned 403 Forbidden (access denied)` →
         * ffmpeg 秒退 → `first frame: src died before any frame`。
         * 所以第二路**必须再写一遍** -user_agent / -headers。
         */
        if (use_ua) {
            argv[i++] = (char *)"-user_agent";
            argv[i++] = uabuf;
        }
        if (use_headers) {
            argv[i++] = (char *)"-headers";
            argv[i++] = hdrbuf;
        }
        /*
         * 【关键 3：修「seek 后开头静音 T-K 秒」】音频要 seek 到视频关键帧 K，
         * 不能也 seek 到 T。视频 `-c copy` 只能从关键帧起，音频 `-ss T` 是
         * 精确的 —— 两路首包 PTS 差 T-K，`-copyts` 下 matroskademux 以
         * min(K,T)=K 为 segment 起点，于是视频先播 K→T 这段静音画面。
         * 详见 probe_video_keyframe_ms() 顶部注释与实测数据。
         */
        if (audio_ss_ms >= 0 && audio_ss_ms != baseMs) {
            snprintf(ss2, sizeof(ss2), "%lld.%03lld", audio_ss_ms / 1000, audio_ss_ms % 1000);
            argv[i++] = (char *)"-ss";
            argv[i++] = ss2;
            pb2_logf("src: audio -ss aligned to video keyframe K=%lldms (target T=%lldms, "
                     "would-be silent %lldms)",
                     audio_ss_ms, baseMs, baseMs - audio_ss_ms);
        } else {
            argv[i++] = (char *)"-ss";
            argv[i++] = ss;
        }
        /* 第二路同样要重连参数，理由见第一路前的长注释（per-input 选项） */
        add_reconnect_args(argv, &i);
        argv[i++] = (char *)"-i";
        argv[i++] = (char *)input2;
    }

    if (map_mode == MAP_DUAL_IN) {
        argv[i++] = (char *)"-map";
        argv[i++] = (char *)"0:v?";
        argv[i++] = (char *)"-map";
        argv[i++] = (char *)"1:a?";
    } else if (map_mode == MAP_VIDEO) {
        argv[i++] = (char *)"-map";
        argv[i++] = (char *)"0:v?";
    } else if (map_mode == MAP_AUDIO) {
        argv[i++] = (char *)"-map";
        argv[i++] = (char *)"0:a?";
    } else {
        argv[i++] = (char *)"-map";
        argv[i++] = (char *)"0:v?";
        argv[i++] = (char *)"-map";
        argv[i++] = (char *)"0:a?";
    }

    argv[i++] = (char *)"-c";
    argv[i++] = (char *)"copy";
    /*
     * 【A/V 同步修复（问题 1，goal-b29d60aa）】必须 -copyts：
     *
     * 旧行为（无 -copyts）：双路 ffmpeg 各自把输出 PTS 重基到 0 ——
     *   视频 -ss T -c copy 实际从关键帧 K（≤ T）开始输出，内容 K 的 PTS=0；
     *   音频 -ss T 精确从 T 开始输出，内容 T 的 PTS 也=0。
     * 两路都在 GStreamer 时钟 0 起播 → 视频显示内容 K、音频播内容 T，
     * 内容差 T-K（B 站 m4s 关键帧间隔常见 1~2s）→ 用户听到「音频快 1~2 秒」。
     *
     * -copyts 保留原始（相对分片起点的）PTS：视频首帧 PTS≈K、音频首帧 PTS≈T。
     * matroskademux 下基准时钟取全管线最小 PTS=K → 视频时钟 0 渲染内容 K；
     * 音频缓冲 PTS=T 要等到时钟 T-K 才播 —— 时钟 T-K 时视频恰好显示内容 K+(T-K)=T。
     * 内容逐帧对齐，起播时音频侧会静默等 T-K（kmssink 先 preroll，符合预期）。
     *
     * 注意：-copyts 时 -avoid_negative_ts make_zero 会与之冲突（copyts 语义
     * 就是保留原始 ts 不做移位），所以删掉 make_zero。实测分片 PTS 从 0 附近
     * 开始，不会出现大数值 PTS 溢出问题。
     */
    argv[i++] = (char *)"-copyts";
    argv[i++] = (char *)"-fflags";
    argv[i++] = (char *)"+genpts";
    if (have_t) {
        argv[i++] = (char *)"-t";
        argv[i++] = tlimit;
    }
    argv[i++] = (char *)"-f";
    argv[i++] = (char *)"matroska";
    argv[i++] = (char *)"-";
    argv[i] = NULL;

    /* 【P1-4】每次 spawn 都把完整 argv 打一遍（元素一行，长值截中间） */
    log_argv(map_mode == MAP_DUAL_IN
                 ? "dual"
                 : (map_mode == MAP_VIDEO ? "v" : (map_mode == MAP_AUDIO ? "a" : "both")),
             argv);
    /* 完整未截断 argv 落盘，供人工复现（URL 有 ~700B，日志装不下） */
    dump_argv_full(map_mode == MAP_DUAL_IN
                       ? "dual"
                       : (map_mode == MAP_VIDEO ? "v" : (map_mode == MAP_AUDIO ? "a" : "both")),
                   argv);
    /* -t 是否真的带上，单独一行，方便 grep */
    pb2_logf("src: ffmpeg -t check: have_t=%d tlimit=%s durMs=%lld baseMs=%lld", have_t,
             have_t ? tlimit : "(absent)", durationMs, baseMs);

    {
        int r1 = posix_spawn_file_actions_init(&fa);
        int r2 = posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, fifo, O_WRONLY, 0);
        int r3;
        /* 【诊断】stderr 落盘到 /tmp/pb2/err-<fifo basename>（每次 spawn 覆盖写）。
         * 仅 http 源启用；本地文件几乎不会死，保留 /dev/null。 */
        if (s_http_diag_stderr && input && (strncmp(input, "http://", 7) == 0 ||
                                            strncmp(input, "https://", 8) == 0)) {
            const char *base = strrchr(fifo, '/');
            snprintf(errfile, sizeof(errfile), "%s/err-%s", PB2_SRC_DIR,
                     base ? base + 1 : fifo);
            r3 = posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, errfile,
                                                  O_WRONLY | O_CREAT | O_TRUNC, 0600);
        } else {
            r3 = posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        }
        int r4 = posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        if (r1 || r2 || r3 || r4) {
            pb2_logf("src: file_actions failed init=%d out=%d err=%d in=%d", r1, r2, r3, r4);
        }
    }

    {
        /*
         * environ 兜底：宿主进程里 environ 不一定可靠（同 session/cmd.c 的注释）。
         * ffmpeg 只需要 PATH / LD_LIBRARY_PATH。
         */
        char **envp = environ;
        const char *env_how = "host";
        if (!envp || !envp[0] || !strchr(envp[0], '=')) {
            envp = g_min_env;
            env_how = "min";
        }
        {
            int rc = posix_spawn(&pid, PB2_FFMPEG, &fa, NULL, argv, envp);
            if (rc != 0) {
                /* 试备用路径 */
                argv[0] = (char *)PB2_FFMPEG_ALT;
                rc = posix_spawn(&pid, PB2_FFMPEG_ALT, &fa, NULL, argv, envp);
            }
            posix_spawn_file_actions_destroy(&fa);
            if (rc != 0) {
                pb2_logf("src: posix_spawn ffmpeg failed rc=%d (%s) errno=%d (%s) env=%s", rc,
                         strerror(rc), errno, strerror(errno), env_how);
                /*
                 * 【P1-3】失败必须返回 -1，不能返回 0。
                 * pid_t 的 0 是合法值域，且 `pid <= 0` 是调用点的成功判据边界；
                 * 返回 0 会让「spawn 失败」和「尚未启动」两种状态无法区分，
                 * 任何新代码只要写成 `if (pid == 0)` 就会把失败当成没启动。
                 * 统一用 -1 表示失败。
                 */
                return -1;
            }
        }
    }

    pb2_logf("src: ffmpeg pid=%d ss=%s t=%s map=%d in=%s%s%s", (int)pid, ss,
             have_t ? tlimit : "(none)", map_mode, input,
             (map_mode == MAP_DUAL_IN && input2) ? " in2=" : "",
             (map_mode == MAP_DUAL_IN && input2) ? input2 : "");
    return pid;
}

int pb2_src_open(pb2_src_t *s, const char *input, const char *input2, const char *ua,
                 const char *ref, long long baseMs, long long durationMs) {
    if (!s || !input || !input[0]) return -1;
    memset(s, 0, sizeof(*s));
    s->hold_fd = -1;
    s->hold_fd2 = -1;
    s->baseMs = baseMs < 0 ? 0 : baseMs;

    if (!is_http(input)) {
        /*
         * 本地文件：完全不需要 ffmpeg。
         * 但要注意：本地文件是**完整文件**，位置直接来自 query_position()，
         * 所以 baseMs 必须是 0（除非 session 自己在 seek）。
         *
         * 双本地文件（测试用）走两个 filesrc，location2 非空。
         */
        snprintf(s->location, sizeof(s->location), "%s", input);
        if (input2 && input2[0] && !is_http(input2)) {
            snprintf(s->location2, sizeof(s->location2), "%s", input2);
        }
        s->is_http = 0;
        s->active = 1;
        pb2_logf("src: local file %s%s%s", s->location, s->location2[0] ? " + " : "",
                 s->location2);
        return 0;
    }

    ensure_dir();

    if (input2 && input2[0]) {
        /* ---------- 单 FIFO + 单 ffmpeg 双输入（B 站 DASH，方案 A） ----------
         *
         * 视频、音频两个 URL 交给**同一个** ffmpeg、同一条 FIFO、GStreamer 侧
         * 只保留一个 filesrc + 一个 matroskademux。这样只有一个 segment，
         * 内容 c 在两路的 PTS 都是 c => running_time 天然对齐（详见文件头
         * MAP_DUAL_IN 的注释）。旧的「双 FIFO + 双 ffmpeg + 双 demuxer」会让
         * 视频从关键帧 K 起、音频从 T 起，两个独立 segment 各自重基到 0，
         * 于是音频领先 T-K（seek 后「画面落后音频」的真因）。
         */
        snprintf(s->fifo, sizeof(s->fifo), "%s/pb2-%d-%d.fifo", PB2_SRC_DIR, (int)getpid(),
                 ++g_seq);
        s->hold_fd = make_fifo(s->fifo, sizeof(s->fifo));
        if (s->hold_fd < 0) return -1;

        s->pid = spawn_ffmpeg(s->fifo, input, input2, ua, ref, s->baseMs, durationMs, MAP_DUAL_IN);
        if (s->pid <= 0) {
            pb2_logf("src: dual-input spawn failed (pid=%d)", (int)s->pid);
            pb2_src_close(s);
            return -1;
        }

        snprintf(s->location, sizeof(s->location), "%s", s->fifo);
        /* location2 留空：session 侧不会再建第二个 filesrc/demuxer */
        s->is_http = 1;
        s->dual = 1; /* 语义改为「双输入」，FIFO/demuxer 仍只有一套 */
        s->active = 1;
        pb2_logf("src: dual-input single fifo %s base=%lldms (v+a in one ffmpeg)",
                 s->fifo, s->baseMs);
        return 0;
    }

    /* ---------- 单 FIFO（源自带音轨，或只有视频轨） ---------- */
    snprintf(s->fifo, sizeof(s->fifo), "%s/pb2-%d-%d.fifo", PB2_SRC_DIR, (int)getpid(),
             ++g_seq);
    s->hold_fd = make_fifo(s->fifo, sizeof(s->fifo));
    if (s->hold_fd < 0) return -1;

    s->pid = spawn_ffmpeg(s->fifo, input, NULL, ua, ref, s->baseMs, durationMs, MAP_BOTH);
    if (s->pid <= 0) {
        close(s->hold_fd);
        s->hold_fd = -1;
        unlink(s->fifo);
        s->fifo[0] = '\0';
        return -1;
    }

    snprintf(s->location, sizeof(s->location), "%s", s->fifo);
    s->is_http = 1;
    s->active = 1;
    pb2_logf("src: single fifo %s base=%lldms", s->fifo, s->baseMs);
    return 0;
}

/* 并行回收两个 ffmpeg 进程：SIGTERM 后最多等 PB2_REAP_GRACE_MS，再 SIGKILL
 *
 * 【P1-3 修复要点】
 *   1) 旧实现每个 pid 独占 200ms 静默窗口，两条串行 => 关键路径白吃 ~500ms
 *      （真机日志：`pid=1597 closed` +296ms、`pid2=1598 closed` +502ms）。
 *   2) 宿主 miniapp 把 SIGCHLD 设成 SIG_IGN，子进程退出会被内核自动回收，
 *      这时 waitpid 返回 -1/ECHILD —— 那也等于"已退出"，不能当成失败一直等。
 *   3) 所以这里只是"发信号 + 短等"，两条并行之后总耗时不再是各自之和。
 */
#define PB2_REAP_GRACE_MS 60

/* 进程是否还在（僵尸也算在）。SIGCHLD=SIG_IGN 下 waitpid 拿不到，只能查 /proc。 */
static int pid_alive(pid_t pid) {
    char path[64];
    char buf[64];
    if (pid <= 0) return 0;
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    return pb2_read_small_file(path, buf, sizeof(buf)) > 0;
}

/*
 * 【P1-3 核心】并行 kill + 统一回收。
 *
 * 旧实现 pb2_src_close 里是 reap(pid) → reap(pid2)，每个独占最多 200ms，
 * 串行下关键路径白吃 ~500ms（真机日志：`pid=1597 closed` +296ms、
 * `pid2=1598 closed` +502ms，新 ffmpeg 到 +503ms 才启动）。
 *
 * 这里改成：两条 SIGTERM 一次发完 → 同一个循环里同时观察两个 pid →
 * 任一超过宽限才单独 SIGKILL。最坏情况耗时等于"一个"进程的回收时间。
 */
static void reap_pair(pid_t a, pid_t b) {
    int st = 0;
    int a_done = (a <= 0);
    int b_done = (b <= 0);
    long long t0 = pb2_now_ms();
    long long kill_at = t0 + PB2_REAP_GRACE_MS;
    int a_killed = 0;
    int b_killed = 0;

    if (a_done && b_done) return;

    if (!a_done) kill(a, SIGTERM);
    if (!b_done) kill(b, SIGTERM);

    for (;;) {
        long long now = pb2_now_ms();
        if (!a_done) {
            pid_t w = waitpid(a, &st, WNOHANG);
            /* w<0 且 ECHILD：宿主 SIGCHLD=SIG_IGN，子进程已被自动回收 */
            if (w == a || (w < 0 && errno != EINTR)) a_done = 1;
        }
        if (!b_done) {
            pid_t w = waitpid(b, &st, WNOHANG);
            if (w == b || (w < 0 && errno != EINTR)) b_done = 1;
        }
        if (a_done && b_done) break;

        if (now > kill_at) {
            if (!a_done && !a_killed) {
                kill(a, SIGKILL);
                a_killed = 1;
            }
            if (!b_done && !b_killed) {
                kill(b, SIGKILL);
                b_killed = 1;
            }
            /* SIGKILL 之后只再等一个短窗口，不再无限拖 */
            if (now > kill_at + 200) {
                if (!a_done && !pid_alive(a)) a_done = 1;
                if (!b_done && !pid_alive(b)) b_done = 1;
                if (!a_done || !b_done) {
                    pb2_logf("src: reap timeout (a=%d done=%d b=%d done=%d)", (int)a, a_done,
                             (int)b, b_done);
                }
                break;
            }
        }
        {
            struct timespec ts = {0, 2000000L}; /* 2ms */
            nanosleep(&ts, NULL);
        }
    }
    pb2_logf("src: reap pair %d/%d took %lldms (killed=%d/%d)", (int)a, (int)b,
             pb2_now_ms() - t0, a_killed, b_killed);
}

void pb2_src_close(pb2_src_t *s) {
    if (!s) return;
    /* 【P1-3】两个 ffmpeg 并行 kill，然后统一回收：不再一个 200ms、一个 200ms */
    reap_pair(s->pid, s->pid2);
    if (s->pid > 0) pb2_logf("src: ffmpeg pid=%d closed", (int)s->pid);
    s->pid = 0;
    if (s->pid2 > 0) pb2_logf("src: ffmpeg pid2=%d closed", (int)s->pid2);
    s->pid2 = 0;

    if (s->hold_fd >= 0) {
        close(s->hold_fd);
        s->hold_fd = -1;
    }
    if (s->hold_fd2 >= 0) {
        close(s->hold_fd2);
        s->hold_fd2 = -1;
    }
    if (s->fifo[0]) {
        unlink(s->fifo);
        s->fifo[0] = '\0';
    }
    if (s->fifo2[0]) {
        unlink(s->fifo2);
        s->fifo2[0] = '\0';
    }
    s->active = 0;
    s->dual = 0;
}

/*
 * 【P0-1】ffmpeg 是否还在。
 *
 * 注意宿主 miniapp 把 SIGCHLD 设成了 SIG_IGN（`/proc/<pid>/status` 的 SigIgn
 * bit16），子进程退出后内核**自动回收**，waitpid 会直接返回 -1/ECHILD，
 * 拿不到退出码也拿不到 pid。这时必须退化成查 /proc：进程不存在 = 已退出。
 * 以前这个函数全工程零调用，等于「ffmpeg 秒退」完全无人发现，首帧永远不来
 * 而 native 一直报 playing。
 */
int pb2_src_alive(pb2_src_t *s) {
    int st = 0;
    pid_t w;
    if (!s) return 0;
    if (!s->active) return 0;
    if (s->pid <= 0 && s->pid2 <= 0) return 1; /* 本地文件源 */
    if (s->pid > 0) {
        errno = 0;
        w = waitpid(s->pid, &st, WNOHANG);
        if (w == s->pid) {
            pb2_logf("src: ffmpeg pid=%d exited status=%d", (int)s->pid, st);
            s->pid = 0;
            return 0;
        }
        if (w < 0 && errno == ECHILD && !pid_alive(s->pid)) {
            pb2_logf("src: ffmpeg pid=%d gone (ECHILD + /proc 不存在)", (int)s->pid);
            s->pid = 0;
            return 0;
        }
    }
    if (s->pid2 > 0) {
        errno = 0;
        w = waitpid(s->pid2, &st, WNOHANG);
        if (w == s->pid2) {
            pb2_logf("src: ffmpeg pid2=%d exited status=%d", (int)s->pid2, st);
            s->pid2 = 0;
            /* 音频先退出不算致命：视频还在就能继续播（画面优先） */
            return 1;
        }
        if (w < 0 && errno == ECHILD && !pid_alive(s->pid2)) {
            pb2_logf("src: ffmpeg pid2=%d gone (ECHILD + /proc 不存在)", (int)s->pid2);
            s->pid2 = 0;
            return 1;
        }
    }
    return 1;
}

/*
 * 【P1-1 / P1-2】首帧之后也要能发现「源死了」。
 *
 * 两个问题的合流点：
 *
 *   P1-1 `pipelineAlive` 名不副实 —— 它只表示 gst_parse_launch 成功，
 *   ffmpeg 因网络错误退出后没人知道，status() 还报 alive=1。
 *
 *   P1-2 FIFO 是用 O_RDWR 打开的（make_fifo），本进程自己就是读端之一，
 *   所以写端（ffmpeg）消失后**读端永远等不到 EOF**：filesrc 不结束、
 *   matroskademux 不结束、管线不发 GST_MESSAGE_EOS。「正常播完」和
 *   「ffmpeg 崩了」在 GStreamer 侧长得一模一样，都无法收尾。
 *
 * 修法：把「检测源死」和「放掉 hold fd 让读端能 EOF」合成一个动作 ——
 * 只有确认写端进程真的没了，才 close(hold_fd)，这样：
 *   - 活着的时候行为完全不变（不会误伤正在写的 ffmpeg 收到 EPIPE）
 *   - 死了之后 filesrc 立刻能读到 EOF → EOS → 走正常的收尾路径
 *   - hold_fd 用 -1 做哨兵，pb2_src_close() 里的 close 判断天然幂等
 *
 * 返回 1 = 主源（视频 ffmpeg）已死且 hold_fd 已放掉；0 = 还活着/无需处理。
 * 音频 pid2 先死不算致命（画面优先），只放掉对应的 hold_fd2。
 * 幂等：重复调用只在第一次有副作用。
 */
int pb2_src_reap_if_dead(pb2_src_t *s) {
    int main_dead = 0;
    if (!s || !s->active) return 0;

    if (s->pid > 0 && !pid_alive(s->pid)) {
        pb2_logf("src: main ffmpeg pid=%d is gone -> release hold_fd (reader can EOF)",
                 (int)s->pid);
        s->pid = 0;
        main_dead = 1;
    }
    if (s->pid2 > 0 && !pid_alive(s->pid2)) {
        pb2_logf("src: audio ffmpeg pid2=%d is gone -> release hold_fd2", (int)s->pid2);
        s->pid2 = 0;
        if (s->hold_fd2 >= 0) {
            close(s->hold_fd2);
            s->hold_fd2 = -1;
        }
    }
    if (main_dead && s->hold_fd >= 0) {
        close(s->hold_fd);
        s->hold_fd = -1;
    }
    return main_dead;
}

void pb2_src_cleanup_stale(void) {
    /*
     * 删 /tmp/pb2 下所有 pb2-<pid>-<seq>[-v|-a].fifo 中属于**已不存在进程**的残留。
     *
     * 为什么按 pid 判断：进程被 SIGKILL 时来不及 unlink，FIFO 会留下来。
     * 但如果有别的会话正在用（同进程不同 seq，或另一个进程），不能删。
     * kill(pid, 0) 返回 ESRCH 说明该进程已不存在，可以安全删除。
     */
    DIR *d = opendir(PB2_SRC_DIR);
    struct dirent *de;
    if (!d) return;

    while ((de = readdir(d)) != NULL) {
        int pid = 0;
        char seq[32];
        char path[PB2_SRC_FIFO_MAX];
        /* 只认 pb2-<pid>-<seq>.fifo / pb2-<pid>-<seq>-v.fifo / -a.fifo */
        if (sscanf(de->d_name, "pb2-%d-%31[0-9].fifo", &pid, seq) != 2 &&
            sscanf(de->d_name, "pb2-%d-%31[0-9]-v.fifo", &pid, seq) != 2 &&
            sscanf(de->d_name, "pb2-%d-%31[0-9]-a.fifo", &pid, seq) != 2) {
            continue;
        }
        if (pid == (int)getpid()) continue; /* 自己的由 pb2_src_close 负责 */
        if (pid <= 0) continue;
        /*
         * 【P2-8】不能写成 `kill(pid,0) == 0 || errno == EPERM`：
         * kill 成功返回 0 时 errno 根本没被设置，右半边读到的是**上一次调用
         * 残留的 errno**，可能刚好是 EPERM → 误判「还活着」→ 残留 FIFO 永不清理。
         * 必须先判返回值，再单独判 errno；且失败前显式 errno=0。
         */
        errno = 0;
        if (kill((pid_t)pid, 0) == 0) continue; /* 还活着 */
        if (errno == EPERM) continue;           /* 活着但不属于我们 */
        snprintf(path, sizeof(path), "%s/%s", PB2_SRC_DIR, de->d_name);
        if (unlink(path) == 0) pb2_logf("src: removed stale fifo %s", de->d_name);
    }
    closedir(d);
}
