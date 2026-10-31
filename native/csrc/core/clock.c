#include "clock.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * 真机实测（X6 Pro）：
 *   关屏  bl_power=4 brightness=5
 *   亮屏  bl_power=0 brightness=5（hal-screen on 后）
 *   `hal-screen set 80` 只改 brightness（113），**不改 bl_power**
 * 所以 bl_power 是唯一可靠的判据，brightness 只在 bl_power 读不到时兜底。
 *
 * 注意：`/tmp/.screen_state` 这个厂商文件实测方向是反的（熄屏时写
 * screen_on=1、亮屏后写 screen_on=0），**绝不能再用它当判据**，
 * 之前 "plane missing (active=0 screenOff=0)" 误报就是它造成的。
 */
#define PB2_BL_POWER_PATH "/sys/class/backlight/backlight/bl_power"
#define PB2_BRIGHTNESS_PATH "/sys/class/backlight/backlight/brightness"
#define PB2_BRIGHT_OFF_MAX 20

long long pb2_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

long long pb2_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + (long long)(ts.tv_nsec / 1000L);
}

int pb2_read_small_file(const char *path, char *buf, size_t cap) {
    int fd;
    ssize_t n;
    if (!path || !buf || cap == 0) return -1;
    buf[0] = '\0';
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) {
        buf[0] = '\0';
        return -1;
    }
    buf[n] = '\0';
    return (int)n;
}

int pb2_screen_off(void) {
    char buf[64];

    /*
     * 1) 背光 bl_power —— 0 表示背光供电（亮），非 0（通常是 4）表示断电。
     *    这是本机唯一可靠的判据（见文件头注释）。
     */
    if (pb2_read_small_file(PB2_BL_POWER_PATH, buf, sizeof(buf)) > 0) {
        if (buf[0] == '0') return 0;
        if (buf[0] >= '1' && buf[0] <= '9') return 1;
    }

    /* 2) 兜底：brightness <= 20 视为关屏 */
    if (pb2_read_small_file(PB2_BRIGHTNESS_PATH, buf, sizeof(buf)) > 0) {
        int br = atoi(buf);
        if (br > 0) return br <= PB2_BRIGHT_OFF_MAX ? 1 : 0;
    }

    return -1;
}
