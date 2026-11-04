#include "cmd.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "core/clock.h"
#include "core/log.h"

extern char **environ;

#define PB2_MP3_MARKER "/tmp/mp3playing"

/*
 * ============================ 全局静音开关 ============================
 *
 * 用户明确要求：调试/测试期间词典笔**一律静音**。以前 pb2_mixer_setup()
 * 无条件把 card0 "Playback Path" 设成 SPK、card1 Master 设成 100%，于是
 * 每次 open/seek 重建管线都会把声音打开一次 —— 表现就是「一会儿有声音，
 * 一会儿没有」，因为别处（release / 手工 / 系统）又会把它按回去。
 *
 * 所以声音必须由一个**单一权威开关**决定，且默认静音：
 *   1) 文件 /tmp/pb2_mute 存在  => 强制静音（方便随时手工切换，无需重编译）
 *   2) 文件 /tmp/pb2_sound 存在 => 强制有声（覆盖上一条，便于将来调试）
 *   3) 环境变量 PB2_MUTE=1      => 静音；PB2_MUTE=0 => 有声
 *   4) 都没有                     => 默认静音（宁可没声音，不能吓到用户）
 *
 * 状态每调用一次就重算一次（见 pb2_mute_enabled），所以 touch/rm 标志文件
 * 立刻生效，不需要重启进程。
 */
#define PB2_MUTE_FILE "/tmp/pb2_mute"
#define PB2_SOUND_FILE "/tmp/pb2_sound"

static int g_mute_state = -1; /* -1 未计算, 0 有声, 1 静音 */
static const char *g_mute_source = "default";

/*
 * 关于 SIGCHLD：本文件**不再改写它**（见 pb2_run 里【P1-13】的注释）。
 * 宿主 miniapp 依赖 SIGCHLD=SIG_IGN 免去 waitpid 回收子进程，我们作为
 * dlopen 进来的库去改这个进程级状态会影响宿主，所以彻底不碰：
 * 拿不到退出码就如实返回"已执行"，由调用方回读校验兜底（mixer_one）。
 */

/* 文件是否存在（不需要读内容，access 一次 <10us） */
static int file_exists(const char *p) { return access(p, F_OK) == 0; }

/*
 * 重算静音状态。每次都会重新 stat 文件，所以 /tmp/pb2_mute 的增删
 * 在下次 open 时就生效，不用重启进程。
 */
int pb2_mute_reload(void) {
    const char *env = getenv("PB2_MUTE");

    if (file_exists(PB2_MUTE_FILE)) {
        g_mute_state = 1;
        g_mute_source = "file:" PB2_MUTE_FILE;
    } else if (file_exists(PB2_SOUND_FILE)) {
        g_mute_state = 0;
        g_mute_source = "file:" PB2_SOUND_FILE;
    } else if (env && env[0]) {
        if (env[0] == '1') {
            g_mute_state = 1;
            g_mute_source = "env:PB2_MUTE=1";
        } else if (env[0] == '0') {
            g_mute_state = 0;
            g_mute_source = "env:PB2_MUTE=0";
        } else {
            g_mute_state = 1;
            g_mute_source = "env:PB2_MUTE(unknown->mute)";
        }
    } else {
        /* 默认有声（发布版语义）。
         * 测试期曾默认静音（用户要求「测试必须静音」）；现在用户反馈
         * 「播放时没声音，需要手动修改音量」，发布版必须默认有声。
         * 测试需要静音时：touch /tmp/pb2_mute（文件优先级最高）或设 PB2_MUTE=1。
         */
        g_mute_state = 0;
        g_mute_source = "default(unmuted)";
    }
    return g_mute_state;
}

/*
 * 【每次都重算】不做缓存：两次 access() 加起来 <20us，相比 amixer/ffmpeg 的
 * 几十毫秒可以忽略，换来的是 touch/rm /tmp/pb2_mute 立即生效（调试必需）。
 * 之前用 "g_mute_state<0 才算" 的缓存，导致中途增删标志文件不生效，
 * 与注释里宣称的行为不一致。
 */
int pb2_mute_enabled(void) {
    pb2_mute_reload();
    return g_mute_state == 1;
}

const char *pb2_mute_reason(void) {
    pb2_mute_reload();
    return g_mute_source;
}

/*
 * 最小环境变量表。
 *
 * 为什么需要它：miniapp 宿主是「被 dlopen 进来的 .so」所在的宿主进程，
 * `extern char **environ` 在这个进程里是否存在、是否被厂商代码改写过都不确定。
 * posix_spawn(..., envp) 传 NULL 或非法指针会直接 EFAULT/EINVAL。
 * 所以先探测真实 environ，可不可靠都兜底用它。
 */
static char *g_pb2_min_env[] = {(char *)"PATH=/usr/bin:/bin:/sbin:/usr/sbin",
                                (char *)"LD_LIBRARY_PATH=/usr/lib:/lib",
                                (char *)"HOME=/tmp",
                                NULL};

/* /proc/<pid> 是否还存在（僵尸也算存在）。只看 stat 能否读到。 */
static int proc_alive(pid_t pid) {
    char path[64];
    char buf[64];
    if (pid <= 0) return 0;
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    return pb2_read_small_file(path, buf, sizeof(buf)) > 0;
}

static void sleep_ms(long ms) {
    struct timespec ts;
    if (ms <= 0) return;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int pb2_run(char *const argv[], char *out, int out_cap, int timeout_ms) {
    pid_t pid = 0;
    int pipefd[2] = {-1, -1};
    posix_spawn_file_actions_t fa;
    int fa_rc = 0;
    int rc;
    int status = 0;
    int w = 0;
    int werr = 0;
    long long t0;
    long long deadline;
    long long elapsed;
    char **envp;
    const char *env_how;
    int timed_out = 0;

    if (!argv || !argv[0]) return -1;

    t0 = pb2_now_ms();
    if (timeout_ms <= 0) timeout_ms = 2000;

    /*
     * environ 探测：宿主可能根本没有合法的 environ（dlopen 进来的代码），
     * 也可能只有一两个条目。不可靠就换成自建最小 env。
     */
    envp = environ;
    if (!envp || envp[0] == NULL) {
        envp = g_pb2_min_env;
        env_how = "min(environ null)";
    } else if (strchr(envp[0], '=') == NULL) {
        envp = g_pb2_min_env;
        env_how = "min(environ invalid)";
    } else {
        env_how = "host";
    }
    pb2_logf("pb2_run: exec %s env=%s environ=%p environ0=%s", argv[0], env_how, (void *)environ,
             (environ && environ[0]) ? environ[0] : "(null)");

    if (out && out_cap > 0) {
        out[0] = '\0';
        if (pipe(pipefd) != 0) {
            pb2_logf("pb2_run: pipe() failed errno=%d (%s)", errno, strerror(errno));
            out = NULL;
            out_cap = 0;
        }
    }

    fa_rc = posix_spawn_file_actions_init(&fa);
    if (fa_rc != 0) {
        if (out && out_cap > 0) {
            close(pipefd[0]);
            close(pipefd[1]);
        }
        pb2_logf("pb2_run: file_actions_init failed rc=%d (%s)", fa_rc, strerror(fa_rc));
        return -1;
    }
    if (out && out_cap > 0) {
        int r1 = posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
        int r2 = posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDERR_FILENO);
        int r3 = posix_spawn_file_actions_addclose(&fa, pipefd[0]);
        int r4 = posix_spawn_file_actions_addclose(&fa, pipefd[1]);
        if (r1 || r2 || r3 || r4) {
            pb2_logf("pb2_run: file_actions(dup2/close) failed dup2o=%d dup2e=%d clo=%d clw=%d",
                     r1, r2, r3, r4);
        }
    } else {
        int r1 = posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        int r2 = posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        int r3 = posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        if (r1 || r2 || r3) {
            pb2_logf("pb2_run: file_actions(/dev/null) failed out=%d err=%d in=%d", r1, r2, r3);
        }
    }

    /*
     * 【P1-13】**不再改写 SIGCHLD**，只靠 ECHILD -> /proc 兜底。
     *
     * 历史：宿主 miniapp 把 SIGCHLD 设成 SIG_IGN（实测 /proc/<pid>/status 的
     * SigIgn = 0000000000011006，bit16 即 SIGCHLD），子进程退出后内核自动回收，
     * waitpid 永远 ECHILD。以前的做法是 spawn 前把 SIGCHLD 临时改成 SIG_DFL、
     * wait 完再恢复 —— 能拿到退出码，但代价是**改写了宿主的进程级状态**：
     * SIGCHLD 是所有线程共享的，改的那几百微秒里宿主自己的子进程会被变成僵尸
     * 而没人回收（宿主正是靠 SIG_IGN 才不用 waitpid 的）。
     *
     * 现在改为完全不碰 SIGCHLD，接受 ECHILD：
     *   waitpid 返回 ECHILD 不代表失败，它只说明「本进程已没有可等待的子进程」
     *   （内核替我们收走了）。此时子进程必定已经退出，直接当作"已执行"返回，
     *   不再轮询 /proc，也不再越过 deadline 去 SIGKILL —— 那个 pid 可能已被复用。
     *
     * 兜底路径是完整的：读上层（out 缓冲）走的是 pipe，子进程退出时内核关闭
     * 写端，read 必然返回 0，所以输出照样能收全，不依赖退出码。
     *
     * 代价：拿不到退出码（sset 失败也会被当成"已执行"）。这正是
     * mixer_one() 必须用 `amixer sget` 回读结果做权威判定的原因（P1-12）——
     * 回读看到的就是控制器的真实状态，比退出码更硬。
     */
    rc = posix_spawn(&pid, argv[0], &fa, NULL, argv, envp);
    if (rc != 0) {
        int saved_errno = errno;
        posix_spawn_file_actions_destroy(&fa);
        if (out && out_cap > 0) {
            close(pipefd[0]);
            close(pipefd[1]);
        }
        pb2_logf("pb2_run: posix_spawn(%s) failed rc=%d (%s) errno=%d (%s)", argv[0], rc,
                 strerror(rc), saved_errno, strerror(saved_errno));
        return -1;
    }
    posix_spawn_file_actions_destroy(&fa);
    pb2_logf("pb2_run: %s spawned pid=%d (+%lldms)", argv[0], (int)pid, pb2_now_ms() - t0);

    if (out && out_cap > 0) {
        close(pipefd[1]);
    }

    /* 收集输出（非阻塞读，配合超时） */
    if (out && out_cap > 0) {
        int total = 0;
        int fl = fcntl(pipefd[0], F_GETFL, 0);
        fcntl(pipefd[0], F_SETFL, fl | O_NONBLOCK);
        deadline = t0 + timeout_ms;
        for (;;) {
            char tmp[256];
            ssize_t n = read(pipefd[0], tmp, sizeof(tmp));
            if (n > 0) {
                if (total < out_cap - 1) {
                    int cp = (int)n;
                    if (total + cp > out_cap - 1) cp = out_cap - 1 - total;
                    memcpy(out + total, tmp, (size_t)cp);
                    total += cp;
                }
                continue;
            }
            if (n == 0) break;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (pb2_now_ms() > deadline) break;
                sleep_ms(2);
                continue;
            }
            break;
        }
        out[total] = '\0';
        close(pipefd[0]);
    }

    /* 等子进程退出，带超时 */
    deadline = t0 + timeout_ms;
    for (;;) {
        errno = 0;
        w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (w < 0) {
            werr = errno;
            if (werr == EINTR) continue;
            /*
             * ECHILD：本进程已没有可等待的子进程 —— 宿主 SIGCHLD=SIG_IGN，
             * 内核不等我们 waitpid 就把退出的子进程收走了。
             * 【P1-13】不再通过改写 SIGCHLD 来避免这条路径，见 spawn 前的注释。
             */
            if (werr == ECHILD) {
                /*
                 * ECHILD 的准确含义是「本进程没有可等待的子进程了」。
                 * 子进程是我们 spawn 出来的，如果它还活着，waitpid(WNOHANG)
                 * 会返回 0 而不是 ECHILD ⇒ 走到这里说明它**已经退出且已被内核
                 * 回收**（宿主 SIGCHLD=SIG_IGN，内核不等 waitpid 就收走了）。
                 *
                 * 所以这里不需要再轮询 /proc（那只会在 pid 被复用后误判"还活着"
                 * 并让我们对着无关进程发 SIGKILL）。直接当作"已执行"返回。
                 * 输出也不受影响：子进程退出时内核关闭 pipe 写端，上面读输出的
                 * 循环已经拿到 EOF，内容是全的。
                 *
                 * 唯一损失是拿不到退出码 —— 由调用方回读校验兜底（mixer_one）。
                 */
                pb2_logf("sigchld: ECHILD (host kept SIGCHLD=SIG_IGN, we never touch it) "
                         "-> already reaped, rc unknown, /proc/%d %s",
                         (int)pid, proc_alive(pid) ? "still listed (pid likely recycled)" : "gone");
                pb2_logf("pb2_run: %s done (rc unknown, treated as executed) +%lldms", argv[0],
                         pb2_now_ms() - t0);
                return 0;
            }
            pb2_logf("pb2_run: %s waitpid failed w=%d errno=%d (%s)", argv[0], w, werr,
                     strerror(werr));
            return -1;
        }
        if (pb2_now_ms() > deadline) {
            timed_out = 1;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            pb2_logf("pb2_run: %s timeout %dms, killed (WIFSIGNALED=%d sig=%d)", argv[0],
                     timeout_ms, WIFSIGNALED(status) ? 1 : 0,
                     WIFSIGNALED(status) ? WTERMSIG(status) : -1);
            return -1;
        }
        sleep_ms(2);
    }

    elapsed = pb2_now_ms() - t0;
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        pb2_logf("pb2_run: %s exited code=%d +%lldms (WIFEXITED=1)", argv[0], code, elapsed);
        return code;
    }
    if (WIFSIGNALED(status)) {
        pb2_logf("pb2_run: %s killed by signal=%d +%lldms (WIFSIGNALED=1 timeout=%d)", argv[0],
                 WTERMSIG(status), elapsed, timed_out);
        return -1;
    }
    pb2_logf("pb2_run: %s unknown status=0x%x +%lldms", argv[0], status, elapsed);
    return -1;
}

/*
 * amixer 设置 + 回读校验。
 *
 * 【P1-12】成功判定的**唯一权威来源是 sget 回读**，不是 sset 的退出码。
 * 两个原因：
 *   1) 宿主 SIGCHLD=SIG_IGN 时 pb2_run 拿不到退出码，只能返回 0
 *      （"已执行但不知道结果"）—— 拿它当成功等于永远报成功；
 *   2) 就算拿到退出码，"退出 0" 也只表示 amixer 不报错，不代表控制器真的变了
 *      （比如 volume 被 clamp、enum item 名不完全匹配时 amixer 仍可能退出 0）。
 *
 * 所以返回值 = 回读内容里找得到 expect 才为 1。
 * 参数 card 只支持 0/1，两套 argv 写成字面量以保证指针数组类型合法。
 */
#define PB2_RC_UNSET (-2) /* 未尝试（区别于真正的失败 -1） */
static int mixer_one(int card, const char *ctl, const char *val, const char *expect) {
    char out[1024];
    int rc0 = PB2_RC_UNSET;
    int rc1 = PB2_RC_UNSET;
    int get_rc = PB2_RC_UNSET;
    int ok;

    if (card == 0) {
        char *a0[] = {(char *)"/usr/bin/amixer", (char *)"-c", (char *)"0",
                      (char *)"sset", (char *)ctl, (char *)val, NULL};
        char *a1[] = {(char *)"/bin/amixer", (char *)"-c", (char *)"0",
                      (char *)"sset", (char *)ctl, (char *)val, NULL};
        char *g0[] = {(char *)"/usr/bin/amixer", (char *)"-c", (char *)"0",
                      (char *)"sget", (char *)ctl, NULL};
        rc0 = pb2_run(a0, NULL, 0, 1500);
        if (rc0 != 0) rc1 = pb2_run(a1, NULL, 0, 1500);
        out[0] = '\0';
        get_rc = pb2_run(g0, out, sizeof(out), 1500);
    } else {
        char *a0[] = {(char *)"/usr/bin/amixer", (char *)"-c", (char *)"1",
                      (char *)"sset", (char *)ctl, (char *)val, NULL};
        char *a1[] = {(char *)"/bin/amixer", (char *)"-c", (char *)"1",
                      (char *)"sset", (char *)ctl, (char *)val, NULL};
        char *g0[] = {(char *)"/usr/bin/amixer", (char *)"-c", (char *)"1",
                      (char *)"sget", (char *)ctl, NULL};
        rc0 = pb2_run(a0, NULL, 0, 1500);
        if (rc0 != 0) rc1 = pb2_run(a1, NULL, 0, 1500);
        out[0] = '\0';
        get_rc = pb2_run(g0, out, sizeof(out), 1500);
    }

    /* 【P1-12】判定只看回读：sget 输出里必须出现 expect */
    ok = (expect && expect[0] && strstr(out, expect) != NULL) ? 1 : 0;

    /* 把回读第一行打出来，方便肉眼确认（去掉换行避免多行日志） */
    {
        char first[160];
        size_t i = 0;
        int started = 0;
        const char *p = out;
        /* 找形如 "Item0: 'SPK'" 或 "Front Left: ..." 的有效行 */
        while (*p && i < sizeof(first) - 1) {
            if (*p == '\n') {
                if (started) break;
            } else {
                started = 1;
                if (*p != '\r') first[i++] = *p;
            }
            p++;
        }
        first[i] = '\0';
        if (!started) strcpy(first, "(empty)");
        /*
         * 日志语义（P1-12）：
         *   verify=OK   回读确认生效（唯一成功判据）
         *   verify=MISS 回读不含 expect —— 这才是真失败
         * rc 一栏：-2 = 未尝试（前一个成功了就不再试备用路径），
         *          0 = 执行过且 pb2_run 认为正常，-1 = spawn 失败/超时。
         * get 一栏是 sget 自己的返回，仅供诊断。
         */
        pb2_logf("mixer: card%d %s=%s set rc=%d/%d get=%d verify=%s sample=\"%s\"", card, ctl,
                 val, rc0, rc1, get_rc, ok ? "OK" : "MISS", first);
    }
    return ok;
}

/*
 * 读取某个 enum 控件的当前选中项（返回不带引号的值，如 "OFF" / "SPK"）。
 *
 * 为什么需要：修「音量特别大 / 关了声音还有声」时不能无脑把 Playback Path
 * 设成 SPK —— 用户可能已经选了 HP / BT / SPK_HP，或者明确设成 OFF（静音）。
 * 必须先读再决定，才能做到「只补必要的、绝不覆盖用户的选择」。
 *
 * 解析目标：amixer sget 输出里的 `Item0: 'SPK'` 行，取出单引号内内容。
 * 返回 0 = 读到，-1 = 读不到（调用方应保守不动）。
 */
static int mixer_get(int card, const char *ctl, char *out, size_t outsz) {
    char buf[2048];
    char *argv0[8];
    const char *item;
    size_t n = 0;

    if (!out || outsz == 0) return -1;
    out[0] = '\0';
    buf[0] = '\0';

    argv0[0] = (char *)"/usr/bin/amixer";
    argv0[1] = (char *)"-c";
    argv0[2] = (char *)(card == 0 ? "0" : "1");
    argv0[3] = (char *)"sget";
    argv0[4] = (char *)ctl;
    argv0[5] = NULL;
    if (pb2_run(argv0, buf, sizeof(buf), 1500) != 0) {
        argv0[0] = (char *)"/bin/amixer";
        buf[0] = '\0';
        if (pb2_run(argv0, buf, sizeof(buf), 1500) != 0) return -1;
    }

    item = strstr(buf, "Item0:");
    if (!item) return -1;
    item = strchr(item, '\'');
    if (!item) return -1;
    item++;
    while (*item && *item != '\'' && n + 1 < outsz) out[n++] = *item++;
    out[n] = '\0';
    return n > 0 ? 0 : -1;
}

/*
 * 读取 pvolume 类控件（如 card1 Master）的当前百分比，返回 0-100，读不到返回 -1。
 *
 * 为什么不能用 mixer_get：mixer_get 解析的是 enum 控件的 `Item0: 'SPK'` 行，
 * 而 pvolume 控件（Master）输出的是
 *     Front Left: Playback 142 [56%] [-20.52dB]
 * 没有 Item0 行。所以必须单独解析 `[NN%]`。
 *
 * 用途：判断用户是否在系统设置里「关闭声音」。真机实测系统关闭声音 =
 * `hal-audio -s 0` → card1 Master=0，而 Playback Path 仍是 SPK。
 * 所以「是否静音」只能看 Master 百分比，不能看 Path。
 */
/*
 * 从 amixer sget 的输出里解析第一个 "[NN%]"，返回 0-100，找不到返回 -1。
 * 抽出来是因为 card1 Master 和 bluealsa AVRCP 两种读法的解析完全一样。
 */
static int parse_pct(const char *buf) {
    const char *p = buf;

    if (!buf) return -1;
    while ((p = strchr(p, '[')) != NULL) {
        const char *q = p + 1;
        int v = 0;
        int got = 0;
        while (*q >= '0' && *q <= '9') {
            v = v * 10 + (*q - '0');
            q++;
            got = 1;
            if (v > 1000) break;
        }
        if (got && *q == '%') return v;
        p++;
    }
    return -1;
}

static int mixer_get_pct(int card, const char *ctl) {
    char buf[2048];
    char *argv0[8];

    buf[0] = '\0';
    argv0[0] = (char *)"/usr/bin/amixer";
    argv0[1] = (char *)"-c";
    argv0[2] = (char *)(card == 0 ? "0" : "1");
    argv0[3] = (char *)"sget";
    argv0[4] = (char *)ctl;
    argv0[5] = NULL;
    if (pb2_run(argv0, buf, sizeof(buf), 1500) != 0) {
        argv0[0] = (char *)"/bin/amixer";
        buf[0] = '\0';
        if (pb2_run(argv0, buf, sizeof(buf), 1500) != 0) return -1;
    }

    return parse_pct(buf);
}

/*
 * 读 bluealsa 的 AVRCP 绝对音量（0-100），读不到返回 -1。
 *
 * bluealsa 把每台设备的 AVRCP 音量暴露成独立 mixer 控件（"<别名> - A2DP"），
 * 必须用 `amixer -D bluealsa` 而不是 `-c <n>`。
 */
static int mixer_get_pct_bluealsa(const char *ctl) {
    char buf[2048];
    char *argv0[8];

    buf[0] = '\0';
    argv0[0] = (char *)"/usr/bin/amixer";
    argv0[1] = (char *)"-D";
    argv0[2] = (char *)"bluealsa";
    argv0[3] = (char *)"sget";
    argv0[4] = (char *)ctl;
    argv0[5] = NULL;
    if (pb2_run(argv0, buf, sizeof(buf), 1500) != 0) {
        argv0[0] = (char *)"/bin/amixer";
        buf[0] = '\0';
        if (pb2_run(argv0, buf, sizeof(buf), 1500) != 0) return -1;
    }
    return parse_pct(buf);
}

/*
 * 读「当前真正生效」的音量百分比（0-100），读不到返回 -1。
 *
 * 【为什么需要这个（2026-11-04 真机实测）】
 * `hal-audio -s N`（控制中心/设置页背后的那个二进制）会**同时**把
 *   - card1 Master（numid=54）立刻写成 N 对应的 0..255 值；
 *   - bluealsa 的 AVRCP 绝对音量写成 N%（约 4s 后）；
 *   - /data/cfg/sys_config.conf 的 spk_volume 写成 N（**约 8~10s 后**）。
 * 也就是说 conf 是**最慢**的那个信号。起播时读 conf 会读到上一次留下的旧值
 * （用户刚调大音量时尤其明显：conf 里还是 0），于是把软件增益和耳机 AVRCP
 * 一起压成 0% —— 这正是「点开视频音量自动变 0%、调大也没声」的根因。
 *
 * 所以：**能读真实控件就不要读 conf**。conf 只作最后兜底。
 *
 * bt_ctl_name 非空 = 当前走蓝牙，读 AVRCP；否则读 card1 Master。
 * 会 fork/exec（一次 ~25ms），只在 worker 线程调用。
 */
int pb2_mixer_volume_read(const char *bt_ctl_name) {
    if (bt_ctl_name && bt_ctl_name[0]) {
        /*
         * 蓝牙路由：只读 bluealsa AVRCP，**绝不回退到 Master**。
         * Master 是 card1 扬声器功放的音量，Playback Path 切到蓝牙后它
         * 不再影响耳机输出，两边可以长期不一致（实测 Master=0% /
         * AVRCP=44%）。旧版在这里 fallback 到 Master，导致蓝牙下读到 0
         * 并把软件增益归零 —— 用户听到的就是「连上耳机没声音」。
         * 读不到就返回 -1，由调用方自己决定怎么兜底。
         */
        return mixer_get_pct_bluealsa(bt_ctl_name);
    }
    return mixer_get_pct(1, "Master");
}

/*
 * 打开声音通路 —— 但**绝不碰音量**。
 *
 * 【用户反馈（2026-11-01）】「开始播放时音量默认是特别大的，不管我在系统设置里
 * 是多少，就算是关闭声音，在播放时也会出现声音」。
 *
 * 根因就是这里原来无条件执行：
 *     mixer_one(1, "Master", "100%", "[100%]")   ← 把用户音量覆盖成 100%
 *     mixer_one(0, "Playback Path", "SPK", ...)  ← 把用户"关闭声音"(OFF)覆盖成扬声器
 * 两条都会覆盖用户在系统设置里的选择，而且每次 open/seek 重建管线都重跑一遍。
 *
 * 现在的语义：**播放器只负责"通路选通"，音量完全由用户/系统决定**。
 *   - card1 Master（功放增益）**一个字都不改**，用户设多少就是多少；
 *     用户设 0（静音）时我们保持 0，绝不拉回 100%。
 *   - card0 Playback Path 只在**当前是 OFF 且用户没有主动静音**时才切到 SPK
 *     （因为 OFF 意味着"没有任何输出通路"，不切就完全没声；这不是音量问题
 *     而是路由问题）。用户若把 Path 设成 HP/BT 等其它通路，我们尊重不动。
 *
 * 所以本函数现在只做一件事：**确保输出通路不是 OFF**，其余一概不碰。
 * 静音开关（/tmp/pb2_mute 等）仍然优先于一切：静音时连通路都不碰。
 */
int pb2_mixer_setup(void) {
    int path_ok = 1;

    if (pb2_mute_enabled()) {
        pb2_logf("mixer: muted=1 (source=%s) -> skipped (no amixer ran)", pb2_mute_reason());
        return 0;
    }

    /*
     * 【关键：用户静音时什么都不做】
     *
     * 系统「关闭声音」的实现是 `hal-audio -s 0` → card1 Master=0，而
     * **Playback Path 仍然是 SPK**（真机实测：hal-audio -s 0 后
     * path=Item0: 'SPK'，card1 values=0,0）。
     *
     * 所以「Playback Path == OFF」并不代表用户想静音 —— 它只代表"当前没有
     * 任何输出通路"（通常是上一个 app 收尾时设的）。而用户真正表达"关闭声音"
     * 的方式是 Master=0。
     *
     * 因此顺序必须是：**先看 Master，Master=0 就一个字节都不改**。
     * 否则我们会把 Path 从 OFF 切成 SPK，虽然 Master=0 时理论上仍无声，
     * 但这会让"用户已静音"的状态变得不可解释（路径被打开、音量却为 0），
     * 也让 /tmp/pb2_mute 之外多出一条"隐式静音"路径。
     */
    {
        int pct = mixer_get_pct(1, "Master");
        if (pct < 0) {
            /*
             * 读不到 Master 就保守不动。宁可没声（用户可手动调），
             * 也不要覆盖用户设置。
             */
            pb2_logf("mixer: master read failed -> leave everything untouched");
            return 0;
        }
        if (pct == 0) {
            pb2_logf("mixer: master=0%% (user muted via system) -> leave path untouched");
            return 0;
        }
        pb2_logf("mixer: master=%d%% (user volume, untouched)", pct);
    }

    /*
     * 只在通路确实是 OFF 时才选通 SPK。
     * 用 sget 先读，避免无谓地覆盖用户设置的 HP / BT / SPK_HP 等通路。
     */
    {
        char cur[64];
        cur[0] = '\0';
        if (mixer_get(0, "Playback Path", cur, sizeof(cur)) == 0) {
            if (strcmp(cur, "OFF") == 0) {
                path_ok = mixer_one(0, "Playback Path", "SPK", "'SPK'");
                pb2_logf("mixer: path was OFF -> set SPK rc=%d", path_ok);
            } else {
                pb2_logf("mixer: path=%s kept (user/system choice, not touching)", cur);
            }
        } else {
            /* 读不到就保守不动 —— 宁可能不出声也不要覆盖用户设置 */
            pb2_logf("mixer: path read failed -> leave untouched");
        }
    }

    /*
     * 【绝不设置 Master】用户的音量就是最终音量。
     * 历史：这里曾是 `mixer_one(1, "Master", "100%", "[100%]")`，是"音量特别大"
     * 的直接原因（系统配置 /data/cfg/sys_config.conf 里 spk_volume=35，对应
     * card1 Master=142；我们曾把它拉到 255 = 100%）。
     * 删除后播放器不再有任何改动音量的路径（除静音开关）。
     */
    pb2_logf("mixer setup done (path_ok=%d, master=untouched)", path_ok);
    return path_ok ? 0 : -1;
}

/*
 * 强制静音：只切**路由**（card0 Playback Path=OFF），不碰 card1 Master。
 *
 * 为什么不再设 Master=0：Master 是用户/系统的音量设置，把它压到 0 就等于
 * 销毁用户的设置 —— 静音结束后无法还原，用户下次播放会发现"音量变了"。
 * 而 Playback Path=OFF 是纯路由开关（没有任何输出通路），已经足够静音，
 * 且完全不改变音量数值。这样 mute 与音量彻底解耦。
 */
int pb2_mixer_mute(void) {
    int ok;
    int r0 = PB2_RC_UNSET;
    int r1 = PB2_RC_UNSET;
    {
        char *argv0[] = {(char *)"/usr/bin/amixer", (char *)"-c", (char *)"0",
                         (char *)"sset", (char *)"Playback Path", (char *)"OFF", NULL};
        char *argv1[] = {(char *)"/bin/amixer", (char *)"-c", (char *)"0",
                         (char *)"sset", (char *)"Playback Path", (char *)"OFF", NULL};
        r0 = pb2_run(argv0, NULL, 0, 1500);
        if (r0 != 0) r0 = pb2_run(argv1, NULL, 0, 1500);
    }
    (void)r1;
    ok = (r0 == 0) ? 1 : 0;
    pb2_logf("mixer mute done ok=%d (path=%d, master untouched)", ok, r0);
    return ok ? 0 : -1;
}

/*
 * 【OPEN 前的同步兜底】
 *
 * 历史：这里原本同步设 card1 Master=0（约 10ms），目的是在完整 mixer 跑到
 * 之前把增益压到 0，防止"起播那一刻漏声"。
 *
 * 【现在不需要了】因为 pb2_mixer_setup() 已经**不再碰 Master**，播放器全程
 * 不改变音量，也就不存在"增益被拉到 100% 需要紧急压回"的窗口。真正需要
 * 兜底的是路由（Playback Path 是 OFF 时没声），而那由 setup 在 worker tick
 * 里处理，且用户可感知的延迟只有几十毫秒，不影响正确性。
 *
 * 保留函数（供静音开关场景使用）：静音模式下确实要立刻断掉输出通路。
 * 注意这里也只切路由，不动 Master —— 与 pb2_mixer_mute() 保持一致。
 */
int pb2_mixer_pre_mute(void) {
    int rc;
    {
        char *a0[] = {(char *)"/usr/bin/amixer", (char *)"-c", (char *)"0",
                      (char *)"sset", (char *)"Playback Path", (char *)"OFF", NULL};
        char *a1[] = {(char *)"/bin/amixer", (char *)"-c", (char *)"0",
                      (char *)"sset", (char *)"Playback Path", (char *)"OFF", NULL};
        rc = pb2_run(a0, NULL, 0, 1500);
        if (rc != 0) rc = pb2_run(a1, NULL, 0, 1500);
    }
    pb2_logf("mixer: pre-mute at open (rc=%d, Playback Path=OFF, master untouched)", rc);
    return rc == 0 ? 0 : -1;
}

/* ========================= 蓝牙 A2DP 输出设备探测 =========================
 *
 * 见 cmd.h 里 pb2_bt_pcm_detect() 的注释。这里只说实现上的三个坑：
 *
 * 坑 1：`bluealsa-aplay -L` 在本机会 stack smashing 崩溃（v3.0.0, Mar 18 2024），
 *       abort 前 stdout 未 flush，popen 永远读到空 ⇒ 只看它会导致蓝牙永远
 *       选不中。所以它是「首选但不唯一」，必须有 BlueZ 回退。
 *
 * 坑 2：匹配串必须是 "bluealsa:" 而不是 "bluealsa:DEV="。真实输出形如
 *       `bluealsa:SRV=org.bluealsa,DEV=<MAC>,PROFILE=a2dp`，DEV 前面还有
 *       SRV=...（v1 注释里明确记着这个坑「已踩过」）。
 *
 * 坑 3：`bluetoothctl devices` 只列**已配对**设备，不区分是否连接。必须逐台
 *       `info` 看 `Connected: yes`，且必须有 `Audio Sink`（A2DP Sink =
 *       0000110b）。只有 AVRCP/HFP 的设备能连上但放不出声，误选就是静默无声。
 */

#define PB2_BTCTL "/usr/bin/bluetoothctl"
#define PB2_BT_LIST_CMD "/usr/bin/bluealsa-aplay -L 2>/dev/null"
#define PB2_BT_MAX_DEVS 8

/* 单台设备是否「已连接 + 有 A2DP Sink」→ 1/0 */
static int bt_dev_is_connected_sink(const char *mac) {
    char cmd[192];
    char line[256];
    FILE *fp;
    int connected = 0;
    int sink = 0;

    if (!mac || !mac[0]) return 0;
    snprintf(cmd, sizeof(cmd), "%s info '%s' 2>/dev/null", PB2_BTCTL, mac);
    fp = popen(cmd, "r");
    if (!fp) return 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Connected: yes")) {
            connected = 1;
        } else if (strstr(line, "Audio Sink")) {
            sink = 1;
        }
    }
    pclose(fp);
    return (connected && sink) ? 1 : 0;
}

/* BlueZ 回退：扫已配对设备，挑第一台「已连接 + A2DP Sink」 */
static int bt_detect_via_bluez(char *out, int out_cap) {
    char macs[PB2_BT_MAX_DEVS][24];
    int n_mac = 0;
    int i;
    char cmd[192];
    char line[256];
    FILE *fp;

    snprintf(cmd, sizeof(cmd), "%s devices 2>/dev/null", PB2_BTCTL);
    fp = popen(cmd, "r");
    if (!fp) return -1;
    while (fgets(line, sizeof(line), fp) && n_mac < PB2_BT_MAX_DEVS) {
        /* 形如 "Device B0:A3:F2:AD:7E:FA Redmi Buds 6" */
        char mac[24];
        if (strncmp(line, "Device ", 7) != 0) continue;
        if (sscanf(line + 7, "%23s", mac) != 1) continue;
        /* MAC 形如 XX:XX:XX:XX:XX:XX，长度 17 */
        if (strlen(mac) != 17 || mac[2] != ':' || mac[5] != ':') continue;
        snprintf(macs[n_mac], sizeof(macs[n_mac]), "%s", mac);
        n_mac++;
    }
    pclose(fp); /* 先关外层 pipe 再逐台 info，避免嵌套 popen 管道缠绕 */

    for (i = 0; i < n_mac; i++) {
        if (!bt_dev_is_connected_sink(macs[i])) continue;
        snprintf(out, (size_t)out_cap, "bluealsa:DEV=%s,PROFILE=a2dp", macs[i]);
        return 0;
    }
    return -1;
}

int pb2_bt_pcm_detect(char *out, int out_cap) {
    FILE *fp;
    char line[256];

    if (!out || out_cap <= 0) return -1;
    out[0] = '\0';

    /* ① 首选 bluealsa-aplay -L（拿到的是完整 PCM 名，最省事） */
    fp = popen(PB2_BT_LIST_CMD, "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            char *p = strstr(line, "bluealsa:");
            if (p) {
                char *end = p;
                while (*end && *end != ' ' && *end != '\n' && *end != '\r' &&
                       *end != '\t') {
                    end++;
                }
                if (end > p && (int)(end - p) < out_cap) {
                    memcpy(out, p, (size_t)(end - p));
                    out[end - p] = '\0';
                    pclose(fp);
                    pb2_logf("bt: pcm via bluealsa-aplay -L -> %s", out);
                    return 0;
                }
            }
        }
        pclose(fp);
    }

    /* ② 回退 BlueZ（本机 bluealsa-aplay -L 会崩，这是实际生效的路径） */
    if (bt_detect_via_bluez(out, out_cap) == 0) {
        pb2_logf("bt: pcm via bluetoothctl -> %s", out);
        return 0;
    }

    pb2_logf("bt: no connected A2DP sink, use default pcm (speaker)");
    return -1;
}

int pb2_bt_name_detect(const char *mac, char *out, int out_cap) {
    char cmd[192];
    char line[256];
    FILE *fp;

    if (!mac || !mac[0] || !out || out_cap <= 0) return -1;
    out[0] = '\0';
    snprintf(cmd, sizeof(cmd), "%s devices 2>/dev/null", PB2_BTCTL);
    fp = popen(cmd, "r");
    if (!fp) return -1;
    while (fgets(line, sizeof(line), fp)) {
        /* 形如 "Device B0:A3:F2:AD:7E:FA Redmi Buds 6" */
        char m[24];
        char *name;
        char *nl;
        if (strncmp(line, "Device ", 7) != 0) continue;
        if (sscanf(line + 7, "%23s", m) != 1) continue;
        if (strcmp(m, mac) != 0) continue;
        /* 名字在 MAC 之后，前导一个空格 */
        name = line + 7 + strlen(m);
        while (*name == ' ' || *name == '\t') name++;
        nl = strchr(name, '\n');
        if (nl) *nl = '\0';
        nl = strchr(name, '\r');
        if (nl) *nl = '\0';
        if (!name[0]) continue;
        snprintf(out, (size_t)out_cap, "%s", name);
        pclose(fp);
        return 0;
    }
    pclose(fp);
    return -1;
}

int pb2_mp3_marker_set(void) {
    int fd = open(PB2_MP3_MARKER, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    close(fd);
    return 0;
}

int pb2_mp3_marker_clear(void) {
    return unlink(PB2_MP3_MARKER);
}

long pb2_rss_kb(void) {
    char buf[256];
    unsigned long size = 0;
    unsigned long resident = 0;
    if (pb2_read_small_file("/proc/self/statm", buf, sizeof(buf)) <= 0) return -1;
    if (sscanf(buf, "%lu %lu", &size, &resident) != 2) return -1;
    return (long)(resident * (unsigned long)(sysconf(_SC_PAGESIZE) / 1024));
}
