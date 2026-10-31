/*
 * cmd.h - 只在 worker 线程里跑的"危险"操作
 *
 * 这些操作会 fork/exec、会阻塞几十毫秒到几百毫秒，**绝对不能在 JSAPI 线程调用**。
 * 统一放在这里，谁调用谁负责保证自己不在 JS 线程上。
 */
#ifndef PENBILI_PB2_CMD_H
#define PENBILI_PB2_CMD_H

/*
 * 跑一条 shell 命令并等待结束，捕获 stdout 到 out（可为 NULL）。
 * 内部用 posix_spawn + waitpid(WNOHANG 轮询)，带 timeout_ms 超时（超时则 SIGKILL）。
 *
 * 返回值：
 *   >= 0  命令已执行（0 通常表示成功，非 0 是子进程退出码）
 *   <  0  执行失败 / 超时 / waitpid 出错
 *
 * 【P1-13】宿主 miniapp 把 SIGCHLD 设成 SIG_IGN，子进程退出后由内核直接回收，
 * waitpid 会返回 ECHILD。这种情况下本函数**不改写 SIGCHLD**（那会污染宿主进程的
 * 进程级状态），而是把"子进程已被回收"当作已执行，返回 **0 且拿不到真实退出码**。
 * 所以调用方不能把 0 当成功证明：需要确认效果时请自行回读校验
 * （例如 mixer_one 用 `amixer sget` 回读，而不是信 sset 的返回码）。
 *
 * 只用 worker 线程调用。
 */
int pb2_run(char *const argv[], char *out, int out_cap, int timeout_ms);

/*
 * 全局静音开关（用户硬性要求：测试/调试期间一律静音）。
 *
 * 优先级（pb2_mute_reload 每次重新 stat，无需重启进程）：
 *   1) /tmp/pb2_mute 存在        -> 静音
 *   2) /tmp/pb2_sound 存在       -> 有声
 *   3) 环境变量 PB2_MUTE=1/0     -> 静音/有声
 *   4) 都没有                     -> **默认静音**
 *
 * pb2_mute_enabled() 返回 1 表示静音：此时 pb2_mixer_setup() 一条 amixer
 * 都不会执行。
 */
int pb2_mute_reload(void);
int pb2_mute_enabled(void);
const char *pb2_mute_reason(void);

/*
 * 配置混音器：让声音从扬声器出来并且不是静音。
 *   amixer -c 0 sset "Playback Path" "SPK"
 *   amixer -c 1 sset "Master" 100%
 * alsasink 默认打开 card1（aw883xx 功放），它的 Master 出厂是 0,0（-46dB 静音），
 * 所以两条都必须做。
 *
 * **静音开关为真时直接返回 0 且不执行任何命令。**
 * 只在 worker 线程调用。
 *
 * 【P1-12】非静音时返回 0 当且仅当**两条都经 sget 回读确认生效**：
 *   card0 回读含 `Item0: 'SPK'` 且 card1 回读含 `[100%]`。
 * 原来写成 `ok0 || ok1` 的语义已废弃 —— 只成功一半会被记成完全成功。
 * 判定不看 sset 的退出码（宿主 SIGCHLD=SIG_IGN 时拿不到，见 pb2_run）。
 */
int pb2_mixer_setup(void);

/*
 * 强制静音：把 card0 Playback Path 设为 OFF、card1 Master 设为 0。
 *
 * **不受静音开关影响，静音模式下照样执行** —— 它本身就是"保持静音"这条
 * 硬性约束的执行者（release / 退出时也要走它，保证会话结束后设备仍是静音）。
 * 需要"静音时零 amixer"的只有 pb2_mixer_setup() 那条开扬声器的路径。
 *
 * 【P1-12】返回 0 当且仅当两路 pb2_run 都返回 0（原来用 || 语义）。
 */
int pb2_mixer_mute(void);

/*
 * OPEN 之前的同步兜底静音（只设 card1 Master=0，约 10~17ms，不回读）。
 *
 * 完整版 pb2_mixer_setup() 被挪到 worker 空闲 tick，起播那一刻有个几十到
 * 几百毫秒的窗口 mixer 还没跑；如果设备当时不是静音态就会漏声。
 * do_open() 在 build_pipeline() 之前同步调这个把增益先压到 0。
 */
int pb2_mixer_pre_mute(void);

/*
 * /tmp/mp3playing 标记文件管理。
 * /usr/bin/system_sleep_wakeup 里的 check_mp3_status() 检查这个文件，
 * 存在则跳过整机休眠 —— 这是厂商自己的约定，也是挡住 600s 息屏休眠的唯一手段。
 *
 * 用 open(O_CREAT)+close()，不 fork、不 system()，几微秒完成。
 */
int pb2_mp3_marker_set(void);
int pb2_mp3_marker_clear(void);

/*
 * 读 /proc/self/statm 算常驻内存（KB），用于长跑泄漏验证。
 * 只在 worker 线程或调试接口调用。
 */
long pb2_rss_kb(void);

#endif /* PENBILI_PB2_CMD_H */
