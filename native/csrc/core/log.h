/*
 * log.h - player2 极简日志
 *
 * 设计要点：
 *   - **3.6.2 起默认开启**（真机排障需要：之前的「播放错误：」空消息没有任何
 *     可回溯证据）。关闭方式：环境变量 PENBILI_PB2_LOG=0。
 *     文件超过 PB2_LOG_MAX_BYTES 时在 init 阶段截断，避免长期驻留进程写爆 tmpfs。
 *   - 写 /tmp/pb2.log（O_APPEND），每行带单调毫秒时间戳。
 *   - 不在热路径上做格式化以外的工作；写 tmpfs 是内存操作，不会阻塞在磁盘上。
 *   - 进程退出时不主动 close（miniapp 进程长期驻留，fd 由进程回收）。
 */
#ifndef PENBILI_PB2_LOG_H
#define PENBILI_PB2_LOG_H

void pb2_log_init(void);
int pb2_log_on(void);
void pb2_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* 无论开关如何都写一行（仅用于致命错误） */
void pb2_logf_always(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/*
 * 把日志尾部读进 out（NUL 结尾），返回写入字节数（0 = 空/不可读）。
 * 只读最后 cap-1 字节，并把起点对齐到行首（避免半行 JSON/时间戳）。
 * 供 jsapi 的 logTail() 给自检页显示 —— 真机没有 adb 时，一张截图就能定位。
 */
int pb2_log_tail(char *out, unsigned int cap);

#endif /* PENBILI_PB2_LOG_H */
