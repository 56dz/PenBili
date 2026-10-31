/*
 * audio_out.h - 音频输出准备（混音器 + 声道可用性）
 *
 * ============================ 为什么需要这一层 ============================
 * 音频解码与播放本身完全由管线里的 alsasink 完成（aacparse ! faad !
 * audioconvert ! audioresample ! audio/x-raw,rate=48000,channels=2 ! alsasink）。
 * 本模块**不碰 PCM 数据**，只负责让 alsasink 真的能出声：
 *
 *   1) alsasink 默认打开的是 **card1（aw883xx 功放）**，不是 card0。
 *      而 card1 的 Master Playback Volume 出厂是 0,0（-46dB，等于静音），
 *      card0 的 "Playback Path" 出厂也不是 SPK。两条都必须显式设置，
 *      否则 hw_ptr 会正常前进（看起来在播）但完全没有声音。
 *      实测（ADR §6.4）：声学从 -62.2dB 提升到 -35.5dB（+26.7dB）。
 *
 *   2) 蓝牙/耳机等外部声卡存在时，希望优先走外设。这一层提供探测，
 *      由 session 决定给 alsasink 传 device= 还是用默认。
 *
 * ============================ 调用约束 ============================
 * pb2_audio_prepare() 会 fork/exec amixer，耗时 10~200ms，
 * **只能在 worker 线程调用**，绝不能在 JSAPI 线程。
 */
#ifndef PENBILI_PB2_AUDIO_OUT_H
#define PENBILI_PB2_AUDIO_OUT_H

/*
 * 音频路由准备。
 *
 * 决策只看两件事：本次要不要音轨（want_audio）+ 全局静音开关。
 *   want_audio=1 且未静音  -> pb2_mixer_setup()：card0 "Playback Path"=SPK，
 *                             card1 Master=100%（两条都由 sget 回读确认）
 *   其它一切情况（含 want_audio=1 但处于静音）-> pb2_mixer_mute()：
 *                             card0 "Playback Path"=OFF，card1 Master=0
 *
 * 【全局静音】静音模式下**绝不会**因为「这个视频有音轨」就打开扬声器；
 * setup 那条路径一条 amixer 都不执行。mute 路径照常执行 —— 它本身就是
 * 把设备压到静音态的执行者。
 *
 * 返回：setup 时 0 当且仅当两条都回读确认（P1-12 的 AND 语义）；
 *       mute 时 0 当且仅当两路 pb2_run 都返回 0。-1 表示失败（不致命，只是没声音）。
 *
 * 只在 worker 线程调用。
 */
int pb2_audio_prepare(int want_audio);

/*
 * 探测当前是否应该用外置声卡（蓝牙/耳机）。
 * 扫 /proc/asound 下各 card 的 pcm 子目录，找非内置的 RUNNING 设备。
 * 返回 1 = 找到外置设备，0 = 没有（走内置），-1 = 无法判断。
 *
 * 【现状】v2 里**没有任何调用点**：session 的 desc 不传 device=，pb2_open_req_t
 * 的 audioDevice 也只是被拷贝和 free、从未参与决策。也就是说蓝牙/耳机并不会
 * 被自动选中。保留本函数是为了将来接外置声卡路由，别以为它已经在生效。
 *
 * 只在 worker 线程调用（会读多个 procfs 文件）。
 */
int pb2_audio_external_present(void);

/*
 * release / 退出时的音频收尾。
 *
 * 静音模式（当前默认）=> 强制重新静音（card0 OFF + card1 0，幂等），
 *                        **不恢复音量** —— 否则 stop/release 后立刻把功放打开，
 *                        用户听到的就是「播一会儿、停一会儿、又有声音」。
 * 非静音模式          => 恢复 Master=100%（把音量还给下一个 app）。
 *
 * 只在 worker 线程调用。
 */
void pb2_audio_restore(void);

#endif /* PENBILI_PB2_AUDIO_OUT_H */
