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
 * 打开声音。
 *
 * 【全局静音】静音开关为真时**一条 amixer 都不执行**，直接返回 0：
 * 不去设 Playback Path=SPK，也不去设 Master=100%。这是修「一会儿有声音
 * 一会儿没声音」的关键 —— 每次 open/seek 都不再擅自打开扬声器。
 */
int pb2_mixer_setup(void) {
    int ok0;
    int ok1;
    int ok;

    if (pb2_mute_enabled()) {
        pb2_logf("mixer: muted=1 (source=%s) -> skipped (no amixer ran)", pb2_mute_reason());
        return 0;
    }

    ok0 = mixer_one(0, "Playback Path", "SPK", "'SPK'");
    ok1 = mixer_one(1, "Master", "100%", "[100%]");
    /*
     * 【P1-12】必须是 AND。原来写成 `(ok0 && ok1) ? 1 : ((ok0 || ok1) ? 1 : 0)`
     * 化简后等于 `ok0 || ok1` —— 只成功一半（比如 card0 设上了 SPK 但 card1
     * Master 仍是 0）也会记成完全成功，日志骗人，上层也无从判断。
     * 现在日志的 path/master 两列与 ok 严丝合缝：ok=1 当且仅当两列都是 1。
     */
    ok = (ok0 && ok1) ? 1 : 0;
    pb2_logf("mixer setup done ok=%d (path=%d master=%d) muted=0", ok, ok0, ok1);
    return ok ? 0 : -1;
}

/*
 * 强制静音：两条都设上，忽略静音开关（幂等，重复调用无害）。
 * release / 退出时也要走这里，保证会话结束后设备仍是静音。
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
    {
        char *argv0[] = {(char *)"/usr/bin/amixer", (char *)"-c", (char *)"1",
                         (char *)"sset", (char *)"Master", (char *)"0", NULL};
        char *argv1[] = {(char *)"/bin/amixer", (char *)"-c", (char *)"1",
                         (char *)"sset", (char *)"Master", (char *)"0", NULL};
        r1 = pb2_run(argv0, NULL, 0, 1500);
        if (r1 != 0) r1 = pb2_run(argv1, NULL, 0, 1500);
    }
    /* 【P1-12】同 setup：两路都成功才算静音成功（原来是 || 语义） */
    ok = (r0 == 0 && r1 == 0) ? 1 : 0;
    pb2_logf("mixer mute done ok=%d (path=%d master=%d)", ok, r0, r1);
    return ok ? 0 : -1;
}

/*
 * 【OPEN 前的同步兜底静音】
 *
 * mixer 的完整版（带回读校验，4 次 amixer ≈150ms）已经移到 worker 空闲 tick，
 * 好处是 OPEN 从 186ms 回到 13ms，坏处是「起播那一刻」有个几十到几百毫秒的
 * 窗口 mixer 还没跑。如果设备当时不是静音态（用户手工调过 / 上个 app 留下
 * Master=100%），这段窗口就会漏声。
 *
 * 所以 OPEN 之前先同步跑一条最便宜的静音（只设 card1 Master=0，约 10ms，
 * 不回读校验 —— 兜底不需要证明，只需要尽快把增益降到 0）。
 * card0 Playback Path 由 tick 里的完整版负责，避免这里再多花 17ms。
 */
int pb2_mixer_pre_mute(void) {
    int rc;
    {
        char *a0[] = {(char *)"/usr/bin/amixer", (char *)"-c", (char *)"1",
                      (char *)"sset", (char *)"Master", (char *)"0", NULL};
        char *a1[] = {(char *)"/bin/amixer", (char *)"-c", (char *)"1",
                      (char *)"sset", (char *)"Master", (char *)"0", NULL};
        rc = pb2_run(a0, NULL, 0, 1500);
        if (rc != 0) rc = pb2_run(a1, NULL, 0, 1500);
    }
    pb2_logf("mixer: pre-mute at open (rc=%d, card1 Master=0)", rc);
    return rc == 0 ? 0 : -1;
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
