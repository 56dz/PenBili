#include "log.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "clock.h"

#define PB2_LOG_PATH "/tmp/pb2.log"
#define PB2_LOG_SWITCH "/tmp/pb2.enable"
#define PB2_LOG_MAX_LINE 1024

static int g_fd = -1;
static int g_on = 0;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

void pb2_log_init(void) {
    const char *env;
    if (g_fd >= 0) return;

    env = getenv("PENBILI_PB2_LOG");
    if (env && env[0] && strcmp(env, "0") != 0) {
        g_on = 1;
    } else if (access(PB2_LOG_SWITCH, F_OK) == 0) {
        g_on = 1;
    }
    if (!g_on) return;

    g_fd = open(PB2_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
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
