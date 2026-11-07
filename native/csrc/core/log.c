#include "log.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "clock.h"

#define PB2_LOG_PATH "/tmp/pb2.log"
#define PB2_LOG_SWITCH "/tmp/pb2.enable"
#define PB2_LOG_MAX_LINE 1024
/* 日志文件上限：超过就从头截断。tmpfs 只有几十 MB，长期驻留进程不能无限涨。
 * 256KB ≈ 2500 行，足够覆盖一次起播 + seek 全流程。 */
#define PB2_LOG_MAX_BYTES (256 * 1024)

static int g_fd = -1;
static int g_on = 0;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

void pb2_log_init(void) {
    const char *env;
    int truncate = 0;
    if (g_fd >= 0) return;

    /*
     * 【3.6.2】默认开启。
     *
     * 真机自检页只显示出「播放错误：」而错误文本是空的 —— 因为 native 的
     * lastErrorMsg 只在部分路径被填充，而 /tmp/pb2.log 默认不写，等于没有
     * 任何现场证据。诊断类小程序的日志成本可以忽略（每秒最多几十行，
     * tmpfs 内存操作），所以改成默认开，除非显式 PENBILI_PB2_LOG=0。
     */
    env = getenv("PENBILI_PB2_LOG");
    if (env && env[0]) {
        g_on = strcmp(env, "0") != 0;
    } else {
        g_on = 1;
    }
    if (!g_on) return;

    /* 超过上限就整文件截断（不用轮转：排障只关心最近这一次）。 */
    {
        struct stat st;
        if (stat(PB2_LOG_PATH, &st) == 0 && st.st_size > PB2_LOG_MAX_BYTES) truncate = 1;
    }
    g_fd = open(PB2_LOG_PATH, O_WRONLY | O_CREAT | O_CLOEXEC | (truncate ? O_TRUNC : O_APPEND), 0644);
    if (g_fd < 0) {
        g_on = 0;
        return;
    }
    pb2_logf("=== player2 log start (pid=%d) ===", (int)getpid());
}

int pb2_log_on(void) { return g_on; }

static void log_write(const char *fmt, va_list ap) {
    char line[PB2_LOG_MAX_LINE];
    char full[PB2_LOG_MAX_LINE + 32];
    int n;
    int m;
    long long ms;

    if (g_fd < 0) return;

    n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
    line[n] = '\n';
    line[n + 1] = '\0';

    ms = pb2_now_ms();
    m = snprintf(full, sizeof(full), "[%lld.%03lld] ", ms / 1000, ms % 1000);
    if (m < 0) return;

    pthread_mutex_lock(&g_mu);
    if (g_fd >= 0) {
        ssize_t w1 = write(g_fd, full, (size_t)m);
        ssize_t w2 = write(g_fd, line, (size_t)n + 1);
        (void)w1;
        (void)w2;
    }
    pthread_mutex_unlock(&g_mu);
}

void pb2_logf(const char *fmt, ...) {
    va_list ap;
    if (!g_on || g_fd < 0) return;
    va_start(ap, fmt);
    log_write(fmt, ap);
    va_end(ap);
}

void pb2_logf_always(const char *fmt, ...) {
    va_list ap;
    if (g_fd < 0) return;
    va_start(ap, fmt);
    log_write(fmt, ap);
    va_end(ap);
}

/*
 * 读日志尾部（自检页「日志」用）。
 *
 * 只读最后 cap-1 字节：一次起播 + 一次 seek 的关键行都在最后几百行内。
 * 起点必须对齐到行首，否则首行是半截（时间戳断在中间，人看着像乱码）。
 */
int pb2_log_tail(char *out, unsigned int cap) {
    int fd;
    long long size;
    long long start;
    ssize_t n;
    unsigned int got = 0;
    char *nl;

    if (!out || cap < 2) return 0;
    out[0] = '\0';

    fd = open(PB2_LOG_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    size = (long long)lseek(fd, 0, SEEK_END);
    if (size <= 0) {
        close(fd);
        return 0;
    }
    start = size - (long long)(cap - 1);
    if (start < 0) start = 0;
    if (lseek(fd, start, SEEK_SET) < 0) {
        close(fd);
        return 0;
    }
    while (got + 1 < cap) {
        n = read(fd, out + got, (size_t)(cap - 1 - got));
        if (n <= 0) break;
        got += (unsigned int)n;
    }
    close(fd);
    out[got] = '\0';
    /* 截断读时首行多半是半行：从第一个换行之后开始 */
    if (start > 0) {
        nl = strchr(out, '\n');
        if (nl) {
            unsigned int skip = (unsigned int)(nl + 1 - out);
            if (skip < got) {
                memmove(out, out + skip, got - skip);
                got -= skip;
                out[got] = '\0';
            }
        }
    }
    return (int)got;
}
