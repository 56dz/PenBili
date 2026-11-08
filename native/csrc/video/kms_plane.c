#include "kms_plane.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/log.h"

#define PB2_DRM_SUMMARY "/sys/kernel/debug/dri/0/summary"
#define PB2_SUMMARY_CAP 65536

static char g_buf[PB2_SUMMARY_CAP];
static _Atomic int g_cached = -1;

/*
 * 循环 read 直到返回 <= 0。
 * 单次 read 只能拿到前 576 字节（debugfs 的 seq_file 行为），必须循环。
 */
static int read_summary(void) {
    int fd;
    size_t total = 0;

    fd = open(PB2_DRM_SUMMARY, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;

    for (;;) {
        ssize_t n;
        if (total >= sizeof(g_buf) - 1) break;
        n = read(fd, g_buf + total, sizeof(g_buf) - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(fd);
    g_buf[total] = '\0';
    return (int)total;
}

/*
 * 从窗口名所在位置往后找这一段的结束（下一个窗口头）。
 *
 * summary 的实际结构（真机实测，974 字节）：
 *     Video Port0: ACTIVE
 *         Connector: DSI-1
 *         Esmart0-win0: ACTIVE
 *             win_id: 0
 *             buf[0]: addr: 0x0000000000000000 pitch: 1920 offset: 0
 *         Esmart1-win0: ACTIVE
 *             win_id: 1
 *             ...
 *             buf[0]: addr: 0x0000000002534000 pitch: 720 offset: 0
 *
 * 窗口头的样子是「行首若干空格 + 名字 + -win + 数字 + ": " + 状态」。
 * 旧代码用 strstr(p, "win_id:") 当段尾 —— 而 win_id 行紧跟在窗口头后面，
 * 于是 buf[0] 那行永远落在段外，`a < seg_end` 恒假、永远返回 0。
 */
static const char *find_block_end(const char *p) {
    const char *q = p;
    /* 先跳过窗口头这一行 */
    while (*q && *q != '\n') q++;
    while (*q) {
        const char *line = q + 1; /* 下一行行首 */
        const char *s = line;
        const char *win;
        /* 行首空白 */
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0') break;
        /* 缩进比窗口头浅 => 已经不是这个窗口的字段了 */
        if ((s - line) == 0) return line;
        /* 该行是否又是一个 "<name>-win<N>:" 窗口头 */
        win = strstr(s, "-win");
        if (win) {
            const char *r = win + 4;
            int digits = 0;
            while (*r >= '0' && *r <= '9') {
                r++;
                digits++;
            }
            if (digits > 0 && *r == ':') return line;
        }
        q = line;
        while (*q && *q != '\n') q++;
    }
    return q;
}

/*
 * 【X7 Pro 适配】找不到 X6 Pro 的 Esmart1-win0 时，扫描 summary 里其它
 * ACTIVE 的窗口。UI 合成层（Esmart0-win* / Cluster0-win*）必须排除，
 * 否则会把常亮的 UI 平面误当成"视频平面 ACTIVE"，巡检永远绿。
 *
 * 只用于诊断/巡检，不参与管线构建；kmssink 真正用哪个平面由
 * open 的 planeId 参数决定（-1 = 自动选择）。
 */
static const char *find_video_window(const char *buf) {
    const char *line = buf;
    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        const char *s = line;
        const char *win;
        const char *r;
        const char *q;
        int digits = 0;
        int is_ui;
        int active = 0;

        while (s < line + len && (*s == ' ' || *s == '\t')) s++;
        win = s;
        while (win + 4 <= line + len && strncmp(win, "-win", 4) != 0) win++;
        if (win + 4 > line + len) goto next_line;
        r = win + 4;
        while (r < line + len && *r >= '0' && *r <= '9') { r++; digits++; }
        if (digits == 0 || r >= line + len || *r != ':') goto next_line;

        is_ui = (strncmp(s, "Esmart0-win", 11) == 0) ||
                (strncmp(s, "Cluster0-win", 12) == 0);
        for (q = r; q + 6 <= line + len; q++) {
            if (strncmp(q, "ACTIVE", 6) == 0) { active = 1; break; }
        }
        if (!is_ui && active) return s;

    next_line:
        if (!eol) break;
        line = eol + 1;
    }
    return NULL;
}

/*
 * 只读导出整份 summary 给诊断页。使用调用方缓冲，不碰 worker 的 g_buf。
 */
int pb2_kms_summary(char *out, size_t cap) {
    int fd;
    size_t total = 0;
    if (!out || cap < 2) return -1;
    fd = open(PB2_DRM_SUMMARY, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    for (;;) {
        ssize_t n = read(fd, out + total, cap - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= cap - 1) break;
    }
    close(fd);
    out[total] = '\0';
    return (int)total;
}

int pb2_plane_query(void) {
    const char *p;
    const char *seg_end;
    const char *a;
    int active = 0;

    if (read_summary() <= 0) {
        atomic_store_explicit(&g_cached, -1, memory_order_relaxed);
        return -1;
    }

    p = strstr(g_buf, PB2_PLANE_WIN);
    if (!p) p = find_video_window(g_buf); /* X7 Pro：窗口名不同也能找到视频平面 */
    if (!p) {
        atomic_store_explicit(&g_cached, -1, memory_order_relaxed);
        return -1;
    }

    seg_end = find_block_end(p);
    if (!seg_end || seg_end < p) seg_end = g_buf + strlen(g_buf);

    /* 窗口头这一行必须带 ACTIVE */
    {
        const char *nl = p;
        while (nl < seg_end && *nl && *nl != '\n') nl++;
        {
            const char *q = p;
            while (q + 6 <= nl) {
                if (strncmp(q, "ACTIVE", 6) == 0) {
                    active = 1;
                    break;
                }
                q++;
            }
        }
    }
    if (!active) {
        atomic_store_explicit(&g_cached, 0, memory_order_relaxed);
        return 0;
    }

    /* 有 ACTIVE 还要看地址非 0（0 表示没绑 framebuffer，等于没画面） */
    a = strstr(p, "buf[0]: addr:");
    if (!a || a >= seg_end) {
        atomic_store_explicit(&g_cached, 0, memory_order_relaxed);
        return 0;
    }
    {
        const char *v = a + strlen("buf[0]: addr:");
        unsigned long long addr;
        while (*v == ' ') v++;
        addr = strtoull(v, NULL, 0);
        if (addr == 0ULL) {
            atomic_store_explicit(&g_cached, 0, memory_order_relaxed);
            return 0;
        }
    }

    atomic_store_explicit(&g_cached, 1, memory_order_relaxed);
    return 1;
}

int pb2_plane_cached(void) {
    return atomic_load_explicit(&g_cached, memory_order_relaxed);
}

void pb2_plane_set_cached(int active) {
    atomic_store_explicit(&g_cached, active, memory_order_relaxed);
}
