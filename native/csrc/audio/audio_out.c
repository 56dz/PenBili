/*
 * audio_out.c - 音频输出准备
 *
 * 见 audio_out.h 顶部注释。核心事实（真机实测，ADR §6.4）：
 *   - alsasink 默认打开 card1（aw883xx 功放），不是 card0
 *   - card1 的 Master Playback Volume 出厂是 0,0（-46dB 静音）
 *   - card0 的 "Playback Path" 出厂不是 SPK
 * 两条都必须显式设置，否则 hw_ptr 正常前进但完全没声音。
 */
#include "audio_out.h"

#include <stdio.h>
#include <string.h>

#include "core/clock.h"
#include "core/log.h"
#include "session/cmd.h"

/*
 * 遍历 /proc/asound/card{0..3}/pcm{0..3}/sub{0..3}/status，
 * 找 state: RUNNING 且不是内置 codec（rk817 / aw883xx）的设备。
 *
 * 用途：蓝牙音箱/耳机接上时，希望 alsasink 走外设而不是内置喇叭。
 * 目前只做探测，具体 device= 由 session 决定。
 */
int pb2_audio_external_present(void) {
    static const char *builtin[] = {"rockchiprk817", "rockchipaw883xx"};
    char path[96];
    char buf[256];
    int card;
    int pcm;
    int sub;
    int found = 0;
    int probed = 0;

    for (card = 0; card < 4 && !found; card++) {
        for (pcm = 0; pcm < 4 && !found; pcm++) {
            for (sub = 0; sub < 4; sub++) {
                snprintf(path, sizeof(path),
                         "/proc/asound/card%d/pcm%d%s/sub%d/status", card, pcm,
                         pcm == 0 ? "p" : "c", sub);
                if (pb2_read_small_file(path, buf, sizeof(buf)) <= 0) continue;
                probed++;
                if (!strstr(buf, "state: RUNNING")) continue;
                {
                    /* 同一张卡的内置标记：读 /proc/asound/cardN/id */
                    char idpath[64];
                    char id[64];
                    int is_builtin = 0;
                    size_t i;
                    snprintf(idpath, sizeof(idpath), "/proc/asound/card%d/id", card);
                    if (pb2_read_small_file(idpath, id, sizeof(id)) > 0) {
                        for (i = 0; i < sizeof(builtin) / sizeof(builtin[0]); i++) {
                            if (strstr(id, builtin[i])) {
                                is_builtin = 1;
                                break;
                            }
                        }
                    }
                    if (!is_builtin) {
                        pb2_logf("audio: external device on card%d pcm%d%s", card, pcm,
                                 pcm == 0 ? "p" : "c");
                        found = 1;
                        break;
                    }
                }
            }
        }
    }

    if (probed == 0) return -1;
    return found;
}

/*
 * 音频路由准备。
 *
 * 【全局静音】want_audio=1 但现在处于静音模式时，走 mute 而不是 setup：
 * 绝不能因为「这个视频有音轨」就把扬声器打开。用户要求全程静音。
 */
int pb2_audio_prepare(int want_audio) {
    if (want_audio && !pb2_mute_enabled()) {
        /* 两条都必须做：card0 选通扬声器通路，card1 打开功放增益 */
        int r = pb2_mixer_setup();
        pb2_logf("audio: prepare(spk) -> %d", r);
        return r;
    }
    {
        int r = pb2_mixer_mute();
        pb2_logf("audio: prepare(mute, want=%d muted=%d source=%s) -> %d", want_audio,
                 pb2_mute_enabled(), pb2_mute_reason(), r);
        return r;
    }
}

/*
 * release / 退出后的音频状态恢复。
 *
 * 原实现把 Master 拉回 100%（"还给下一个 app 一个能出声的音量"）。
 * 但在静音模式下这就是「突然有声音」的元凶之一：stop/release 之后立刻
 * 把功放打开，用户听到的就是播一会儿、停一会儿、又有声音。
 *
 * 现在：静音模式 => 强制重新静音（两条都设，幂等）。
 *       非静音模式 => 保持原来的恢复行为。
 */
void pb2_audio_restore(void) {
    if (pb2_mute_enabled()) {
        pb2_mixer_mute();
        pb2_logf("audio: restore -> forced MUTE (source=%s)", pb2_mute_reason());
        return;
    }
    {
        char *a0[] = {(char *)"/usr/bin/amixer", (char *)"-c", (char *)"1",
                      (char *)"sset", (char *)"Master", (char *)"100%", NULL};
        char *b0[] = {(char *)"/bin/amixer", (char *)"-c", (char *)"1",
                      (char *)"sset", (char *)"Master", (char *)"100%", NULL};
        if (pb2_run(a0, NULL, 0, 1500) != 0) pb2_run(b0, NULL, 0, 1500);
        pb2_logf("audio: restored -> Master 100%% (muted=0)");
    }
}
