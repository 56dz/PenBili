/*
 * clock.h - 单调时间与屏幕状态探测
 *
 * 所有计时都用 CLOCK_MONOTONIC，绝不用 gettimeofday（会被 NTP 跳变影响，
 * 旧代码用 wall clock 算 fps 导致长跑后 fps 抖动）。
 */
#ifndef PENBILI_PB2_CLOCK_H
#define PENBILI_PB2_CLOCK_H

#include <stddef.h>

/* 单调毫秒，进程内唯一时间基准 */
long long pb2_now_ms(void);

/* 单调微秒 */
long long pb2_now_us(void);

/*
 * 阻塞睡眠（毫秒）。用 nanosleep 循环实现，被信号打断会自动补足剩余时间
 * （EINTR 时重算），不依赖 usleep（POSIX 已废弃且遇 EINTR 直接返回）。
 * 只在 worker 线程里用，绝不可以在 JS 线程调用 —— JSAPI 零阻塞是硬规则。
 */
void pb2_sleep_ms(long long ms);

/*
 * 读屏幕是否处于熄屏/断电状态。
 * 返回 1 = 熄屏（不应该指望平面还在），0 = 亮屏，-1 = 无法判断。
 *
 * 依据（优先顺序）：
 *   1) /sys/class/backlight/backlight/bl_power（0=亮，1~9=断电，本机实测灭屏是 4）
 *   2) /sys/class/backlight/backlight/brightness（<=20 视为灭屏；bl_power 读不到才用）
 *
 * **绝不能用 `/tmp/.screen_state`**：该厂商文件实测方向是反的（熄屏时写
 * screen_on=1、亮屏后写 screen_on=0），曾经是 "plane missing
 * (active=0 screenOff=0)" 每秒误报的元凶。详见 clock.c 文件头注释。
 *
 * 两个 sysfs 文件都 <32 字节，单次 open+read+close 在 sysfs 上 <50us，
 * 可以在 status() 里每次调用。
 */
int pb2_screen_off(void);

/* 读一个不超过 cap-1 字节的小文本文件到 buf（自动 NUL 结尾）。
 * 返回读到的字节数，失败返回 -1。不做循环 read（这类文件都是一次读完）。 */
int pb2_read_small_file(const char *path, char *buf, size_t cap);

#endif /* PENBILI_PB2_CLOCK_H */
