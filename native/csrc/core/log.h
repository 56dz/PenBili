/*
 * log.h - player2 极简日志
 *
 * 设计要点：
 *   - 默认关闭。开启方式（任一）：
 *       环境变量 PENBILI_PB2_LOG=1
 *       存在文件 /tmp/pb2.enable
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

#endif /* PENBILI_PB2_LOG_H */
