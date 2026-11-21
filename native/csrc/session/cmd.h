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
 * 确保输出通路可用 —— **绝不改变音量**。
 *
 * 【2026-11-01 用户反馈】「开始播放时音量默认是特别大的，不管我在系统设置里
 * 是多少，就算是关闭声音，在播放时也会出现声音」。
 * 根因：这里原来无条件执行 `amixer -c 0 sset "Playback Path" "SPK"` 和
 * `amixer -c 1 sset "Master" 100%`，把用户在系统设置里的选择全盖掉了。
 *
 * 现在只做一件事：**读 Playback Path，若为 OFF 才切 SPK**（OFF = 无输出通路，
 * 不切就完全没声，这是路由问题不是音量问题）。若已是 SPK/HP/BT 等，一律不动。
 * card1 Master 完全不碰 —— 用户音量就是最终音量。
 *
 * 这与旧 v1 实现（`native/csrc/player.c` 的 `audio_route_ensure()`）语义一致：
 * 那里也是「只在 OFF 时切 SPK，从不碰 Master」。
 *
 * **静音开关为真时直接返回 0 且不执行任何命令。**
 * 只在 worker 线程调用。
 */
int pb2_mixer_setup(void);

/*
 * 读「当前真正生效」的音量百分比（0-100），读不到返回 -1。
 *
 * bt_ctl_name 非空 = 读 bluealsa 的 AVRCP 绝对音量（形如 "L3 anc - A2DP"）；
 * 空 = 读 card1 Master。两者都读不到才返回 -1，调用方再退回 sys_config.conf。
 *
 * 【为什么不能用 sys_config.conf 当权威（2026-11-04 真机实测）】
 * `hal-audio -s N` 写 Master 是**立刻**生效、写 AVRCP 约 4s、写 conf 约 8~10s。
 * 起播读 conf 会拿到上一次的旧值，用户刚调大音量时 conf 里可能还是 0，
 * 结果播放器把软件增益和耳机 AVRCP 一起压成 0% —— 听感上就是「点开视频
 * 音量自动变 0%，播放中调大也没声」。
 *
 * 会 fork/exec（一次 ~25ms），只在 worker 线程调用。
 */
int pb2_mixer_volume_read(const char *bt_ctl_name);

/*
 * 强制静音：只把 card0 Playback Path 设为 OFF，**不碰 card1 Master**。
 *
 * 为什么不再设 Master=0：Master 是用户/系统的音量设置，压到 0 就等于销毁
 * 用户设置且无法还原。Playback Path=OFF 是纯路由开关，已足够静音。
 *
 * **不受静音开关影响，静音模式下照样执行** —— 它本身就是"保持静音"这条
 * 硬性约束的执行者（release / 退出时也要走它，保证会话结束后设备仍是静音）。
 * 需要"静音时零 amixer"的只有 pb2_mixer_setup() 那条开通路的路径。
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
 * 蓝牙 A2DP 音频输出设备探测（返回 ALSA PCM 名，如
 * `bluealsa:DEV=B0:A3:F2:AD:7E:FA,PROFILE=a2dp`）。
 *
 * 【2026-11-01 用户反馈】「我连上蓝牙耳机了，播放视频的声音还是外放」。
 *
 * 根因：本机音频不走一个统一的「系统混音器」，而是三条互不相干的通路：
 *   1) 内置喇叭：`/etc/asound.conf` 的 `pcm.!default -> speaker -> spk_softvol
 *      -> resample_spk -> hw:1,0`（aw883xx 功放）
 *   2) 蓝牙 A2DP：bluealsa 的 D-Bus 通道，PCM 名形如
 *      `bluealsa:DEV=<MAC>,PROFILE=a2dp`，**完全不经过 asound.conf**
 *   3) 有线耳机：asound.conf 里的 `ana_phone` / `tc_analog` 等
 * 所以「连上蓝牙」不会自动改变 `pcm.!default`，播放器必须自己挑 device。
 *
 * 旧 v1（`native/csrc/player.c` 的 `detect_bt_pcm()`）有此逻辑，player2 重写
 * 时丢了。这里按 v1 的两级策略恢复：
 *   ① 首选 `bluealsa-aplay -L`（输出里直接有完整 PCM 名）
 *   ② 但本机该命令**会 stack smashing 崩溃**（abort 前 stdout 未 flush ⇒
 *      popen 永远读到空），所以必须有 ③
 *   ③ 回退 BlueZ：`bluetoothctl devices` 列已配对设备，逐台 `info` 判
 *      `Connected: yes` + `Audio Sink`（A2DP Sink = 0000110b；只有 AVRCP
 *      的设备能连但放不出声，必须排除），命中即拼出 PCM 名。
 *
 * 返回 0 = 找到蓝牙设备并写进 out；-1 = 没有（调用方用默认 PCM = 喇叭）。
 * 会 fork/exec（两次 popen，~40~120ms），**只在 worker 线程调用**。
 */
int pb2_bt_pcm_detect(char *out, int out_cap);

/*
 * 取某台蓝牙设备的别名（"Redmi Buds 6"），用于拼 bluealsa 的 mixer 控件名
 * （形如 "Redmi Buds 6 - A2DP"）。mac 传 "B0:A3:F2:AD:7E:FA" 这种格式。
 * 成功返回 0 并写 out，失败返回 -1。
 *
 * 为什么要这个：bluealsa 把每台设备的 AVRCP 绝对音量暴露成一个独立 mixer
 * 控件，名字是 "<别名> - A2DP"，而 amixer 只能按名字操作（没有按 MAC 的
 * 寻址方式）。不查别名就没法同步耳机音量。
 */
int pb2_bt_name_detect(const char *mac, char *out, int out_cap);

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
