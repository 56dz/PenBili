/*
 * jsapi_player2.c —— player2 模块：X6 Pro (RK3566 + GStreamer/MPP/KMS) 视频播放
 *
 * ============================ 这个文件只做三件事 ============================
 *
 *   1) 参数解析与校验（把 JS 的 15 个位置参数变成 pb2_open_req_t）
 *   2) 把命令投递给 worker 线程（**全部非阻塞**）
 *   3) 从无锁快照读状态，拼成 JS 对象
 *
 * 所有 GStreamer 调用、fork/exec、文件 IO、sleep 都在 worker 线程
 * （session/session.c）里做。本文件里**没有任何一次阻塞调用**，
 * 这是硬规则 1（JSAPI 线程 p99 < 5ms、最坏 < 20ms）。
 *
 * 唯一需要解释的例外是 release()：它要等 worker 退出。
 * 做法见 js_release —— 先用原子标志让 worker 自己收尾，再**有界**地
 * 等一个 cond（不是 join），超时就直接返回，绝不让 JS 线程无限挂住。
 *
 * ============================ 为什么不能用 player.c ============================
 * player.c 是 X5（Cvitek CV1826、无 GStreamer、fb0 贴图、aplay 出声）的实现，
 * 保留为 v1 回退路径。X6 Pro 的显示/解码/音频路径完全不同，且 player.c 的
 * 三个 P0 缺陷（seek 同步杀进程重建、pause 用 SIGSTOP、无条件落盘 dump）
 * 正是本次重写要消灭的对象。所以这里是**全新实现**，不 include 也不改 player.c。
 */
#include <jquick_config.h>
#include <jsmodules/JSCModuleExtension.h>
#include <quickjs/quickjs.h>

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* access() —— installAmr 先查文件是否真存在 */

#include "core/clock.h"
#include "core/log.h"
#include "core/queue.h"
#include "session/cmd.h"
#include "session/session.h"

#define PB2_VERSION "3.0.0"
#define PB2_MODULE_NAME "player2"

/* 与 src/services/player.js 的 JS 侧规则保持一致 */
#define PB2_MAX_INPUT_LEN 1024
#define PB2_MAX_USER_AGENT_LEN 255
#define PB2_MAX_REFERER_LEN 512

/* ============================ 小工具 ============================ */

static JSValue mk_bool(JSContext *ctx, const char *k, int v, JSValue obj) {
    JS_SetPropertyStr(ctx, obj, k, JS_NewBool(ctx, v ? 1 : 0));
    return obj;
}

static JSValue mk_int(JSContext *ctx, const char *k, long long v, JSValue obj) {
    JS_SetPropertyStr(ctx, obj, k, JS_NewInt64(ctx, (int64_t)v));
    return obj;
}

static JSValue mk_str(JSContext *ctx, const char *k, const char *v, JSValue obj) {
    JS_SetPropertyStr(ctx, obj, k, v ? JS_NewString(ctx, v) : JS_NULL);
    return obj;
}

/* 错误返回：{ ok:false, code, error }。code 是字符串，与 JS 层约定一致。 */
static JSValue mk_fail(JSContext *ctx, const char *code, const char *msg) {
    JSValue res = JS_NewObject(ctx);
    mk_bool(ctx, "ok", 0, res);
    mk_str(ctx, "code", code, res);
    mk_str(ctx, "error", msg, res);
    return res;
}

static JSValue mk_ok(JSContext *ctx) {
    JSValue res = JS_NewObject(ctx);
    mk_bool(ctx, "ok", 1, res);
    return res;
}

/* 把 argv[i] 读成 C 字符串（调用者负责 JS_FreeCString）。缺省返回 NULL。 */
static const char *arg_str(JSContext *ctx, int argc, JSValueConst *argv, int i) {
    if (i >= argc) return NULL;
    if (JS_IsUndefined(argv[i]) || JS_IsNull(argv[i])) return NULL;
    return JS_ToCString(ctx, argv[i]);
}

static long long arg_int(JSContext *ctx, int argc, JSValueConst *argv, int i, long long dflt) {
    int64_t v = (int64_t)dflt;
    if (i >= argc) return dflt;
    if (JS_IsUndefined(argv[i]) || JS_IsNull(argv[i])) return dflt;
    if (JS_ToInt64(ctx, &v, argv[i]) != 0) return dflt;
    return (long long)v;
}

/*
 * input 校验（与 JS 侧 isSafeInput 同规则）：
 *   - 非空、≤1024
 *   - 无空白/控制字符（会破坏 argv 与 URL 语义）
 *   - 无 ".."（防目录穿越，本地路径场景）
 *   - 必须是 http(s):// 开头，或以 / 开头的绝对路径
 */
static int valid_input(const char *s) {
    size_t n;
    size_t i;
    if (!s || !s[0]) return 0;
    n = strlen(s);
    if (n > PB2_MAX_INPUT_LEN) return 0;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch <= 0x20 || ch == 0x7f) return 0;
    }
    if (strstr(s, "..")) return 0;
    if (strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0) return 1;
    return s[0] == '/';
}

/* UA：非空时必须是 1..255 的可打印 ASCII */
static int valid_user_agent(const char *s) {
    size_t n;
    size_t i;
    if (!s || !s[0]) return 1;
    n = strlen(s);
    if (n > PB2_MAX_USER_AGENT_LEN) return 0;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch > 0x7e) return 0;
    }
    return 1;
}

/* Referer：非空时必须是 http(s) URL，无空白/控制字符，≤512 */
static int valid_referer(const char *s) {
    size_t n;
    size_t i;
    if (!s || !s[0]) return 1;
    n = strlen(s);
    if (n > PB2_MAX_REFERER_LEN) return 0;
    if (strncmp(s, "http://", 7) != 0 && strncmp(s, "https://", 8) != 0) return 0;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch <= 0x20 || ch == 0x7f) return 0;
    }
    return 1;
}

static char *dup_or_null(const char *s) {
    if (!s) return NULL;
    return strdup(s);
}

/* ============================ open ============================ */

/*
 * open(input, startMs, durationMs, fps, audio, transpose,
 *      x, y, w, h, audioDevice, userAgent, referer, input2, fpsCap)
 *
 * 15 个位置参数，顺序与 src/services/player.js 的 buildOpenArgs 严格一致。
 * 立即返回：只做参数校验 + strdup + 入队，不做任何耗时操作。
 */
static JSValue js_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *input = NULL;
    const char *audio_dev = NULL;
    const char *ua = NULL;
    const char *ref = NULL;
    const char *input2 = NULL;
    long long start_ms;
    long long duration_ms;
    long long fps;
    long long audio;
    long long transpose;
    long long x, y, w, h;
    long long fps_cap;
    pb2_open_req_t *req;
    JSValue res;
    (void)this_val;

    if (argc < 10) {
        return mk_fail(ctx, "PLAYER_BAD_ARG", "open() 需要至少 10 个参数");
    }

    input = arg_str(ctx, argc, argv, 0);
    if (!valid_input(input)) {
        if (input) JS_FreeCString(ctx, input);
        return mk_fail(ctx, "PLAYER_BAD_INPUT", "input 非法（需 http(s):// 或 / 开头，无空白）");
    }

    start_ms = arg_int(ctx, argc, argv, 1, 0);
    duration_ms = arg_int(ctx, argc, argv, 2, 0);
    fps = arg_int(ctx, argc, argv, 3, 24);
    audio = arg_int(ctx, argc, argv, 4, 1);
    transpose = arg_int(ctx, argc, argv, 5, 1);
    x = arg_int(ctx, argc, argv, 6, 0);
    y = arg_int(ctx, argc, argv, 7, 0);
    w = arg_int(ctx, argc, argv, 8, 0);
    h = arg_int(ctx, argc, argv, 9, 0);
    audio_dev = arg_str(ctx, argc, argv, 10);
    ua = arg_str(ctx, argc, argv, 11);
    ref = arg_str(ctx, argc, argv, 12);
    input2 = arg_str(ctx, argc, argv, 13);
    fps_cap = arg_int(ctx, argc, argv, 14, 0);

    if (!valid_user_agent(ua)) {
        JS_FreeCString(ctx, input);
        if (audio_dev) JS_FreeCString(ctx, audio_dev);
        if (ua) JS_FreeCString(ctx, ua);
        if (ref) JS_FreeCString(ctx, ref);
        if (input2) JS_FreeCString(ctx, input2);
        return mk_fail(ctx, "PLAYER_BAD_HEADERS", "userAgent 非法");
    }
    if (!valid_referer(ref)) {
        JS_FreeCString(ctx, input);
        if (audio_dev) JS_FreeCString(ctx, audio_dev);
        if (ua) JS_FreeCString(ctx, ua);
        if (ref) JS_FreeCString(ctx, ref);
        if (input2) JS_FreeCString(ctx, input2);
        return mk_fail(ctx, "PLAYER_BAD_HEADERS", "referer 非法");
    }
    /* input2 可选，但给了就必须合法（它是 B 站 DASH 的音频轨 URL） */
    if (input2 && input2[0] && !valid_input(input2)) {
        JS_FreeCString(ctx, input);
        if (audio_dev) JS_FreeCString(ctx, audio_dev);
        if (ua) JS_FreeCString(ctx, ua);
        if (ref) JS_FreeCString(ctx, ref);
        JS_FreeCString(ctx, input2);
        return mk_fail(ctx, "PLAYER_BAD_INPUT", "input2 非法");
    }
    /* 矩形必须落在面板内：x 是 0..266 的逻辑列坐标（native 加 107 偏移），
     * y/w/h 是物理像素。给 0 或负数一律拒绝——写死一个默认值会让 JS 层
     * 以为布局生效了，实际画错位置。 */
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || x < 0 || x > 4096 || y < 0 || y > 4096) {
        JS_FreeCString(ctx, input);
        if (audio_dev) JS_FreeCString(ctx, audio_dev);
        if (ua) JS_FreeCString(ctx, ua);
        if (ref) JS_FreeCString(ctx, ref);
        if (input2) JS_FreeCString(ctx, input2);
        return mk_fail(ctx, "PLAYER_BAD_RECT", "矩形非法（w/h 需 1..4096）");
    }

    if (start_ms < 0) start_ms = 0;
    if (duration_ms < 0) duration_ms = 0;
    if (fps < 1) fps = 24;
    if (fps > 240) fps = 240;
    if (transpose < 1 || transpose > 3) transpose = 1;
    if (fps_cap < 1 || fps_cap > 120) fps_cap = 0;

    req = (pb2_open_req_t *)calloc(1, sizeof(*req));
    if (!req) {
        JS_FreeCString(ctx, input);
        if (audio_dev) JS_FreeCString(ctx, audio_dev);
        if (ua) JS_FreeCString(ctx, ua);
        if (ref) JS_FreeCString(ctx, ref);
        if (input2) JS_FreeCString(ctx, input2);
        return mk_fail(ctx, "PLAYER_NATIVE_MISSING", "out of memory");
    }

    req->input = dup_or_null(input);
    req->input2 = dup_or_null(input2 && input2[0] ? input2 : NULL);
    req->userAgent = dup_or_null(ua);
    req->referer = dup_or_null(ref);
    req->audioDevice = dup_or_null(audio_dev);
    req->startMs = start_ms;
    req->durationMs = duration_ms;
    req->fps = (int)fps;
    req->fpsCap = (int)fps_cap;
    req->audio = audio ? 1 : 0;
    req->transpose = (int)transpose;
    req->x = (int)x;
    req->y = (int)y;
    req->w = (int)w;
    req->h = (int)h;
    /*
     * 【P0-3】第 16 个参数 = 显式直播意图（1=直播 / 0=点播 / 缺省 -1）。
     *
     * native 不再靠「durationMs == 0 且是 http」猜直播 —— B 站 view 接口
     * 对互动视频/部分港澳台条目会返回 duration: 0，那种点播被猜成直播后
     * 会走 flvdemux（DASH 分片不是 FLV）→ 无画面 + 永久失去 seek，且不报错。
     * JS 侧 play_session.js 在 resolveLiveUrl / resolvePlayUrl 里明确知道
     * 自己在干什么，所以由它传意图，native 只信。
     *
     * 缺省 -1 保留给老版本 JS 与直接调 native 的测试脚本（回落到严格启发式）。
     */
    req->isLive = (argc >= 16) ? (int)arg_int(ctx, argc, argv, 15, -1) : -1;
    if (req->isLive != 0 && req->isLive != 1) req->isLive = -1;

    JS_FreeCString(ctx, input);
    if (audio_dev) JS_FreeCString(ctx, audio_dev);
    if (ua) JS_FreeCString(ctx, ua);
    if (ref) JS_FreeCString(ctx, ref);
    if (input2) JS_FreeCString(ctx, input2);

    /* worker 线程在第一次用到时启动；重复调用幂等 */
    if (pb2_session_start() != 0) {
        pb2_open_req_free(req);
        return mk_fail(ctx, "PLAYER_NATIVE_MISSING", "worker thread start failed");
    }

    if (pb2_session_cmd(PB2_CMD_OPEN, 0, req) != 0) {
        /* 队列满：req 归我们 free（契约见 queue.h） */
        pb2_open_req_free(req);
        return mk_fail(ctx, "PLAYER_BUSY", "command queue full");
    }

    res = mk_ok(ctx);
    /*
     * 【兼容字段】open 的返回值在 v1（player.c:3781-3785）里带
     * outWidth/outHeight/fps/audioRate/audioBt，JS 侧
     * src/services/play_session.js:356 与 src/services/bili/probe.js:229
     * 都读它们打日志/存进 session。v2 若缺这些键，日志就是
     * `open ok out=undefinedxundefined fps=undefined`（已真机复现），
     * 且 play.session.outWidth/outHeight 变成 undefined 传导到 UI。
     *
     * 这里给的 outWidth/outHeight 是**本模块输出到平面的像素矩形**
     * （也就是 KMS 上屏尺寸 = 请求里的 w/h），不是源分辨率 ——
     * v2 的 src_http 用 `-c copy` 不做缩放，解码后由 kmssink 的
     * render-rectangle 直接摆位，所以输出尺寸就等于矩形。
     */
    mk_int(ctx, "outWidth", w, res);
    mk_int(ctx, "outHeight", h, res);
    mk_int(ctx, "fps", fps, res);
    mk_int(ctx, "audioRate", audio ? 48000 : 0, res);
    mk_bool(ctx, "audioBt", 0, res);
    return res;
}

/* ============================ 简单命令 ============================ */

static JSValue js_pause(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    /*
     * 硬规则 2：暂停**绝不冻结进程**（不用 SIGSTOP）。
     * 真正的 set_state(PAUSED) 在 worker 上做（实测 0.2ms）。
     * 这里只入队，立即返回。
     */
    if (pb2_session_cmd(PB2_CMD_PAUSE, 0, NULL) != 0) {
        return mk_fail(ctx, "PLAYER_BUSY", "queue full");
    }
    return mk_ok(ctx);
}

static JSValue js_resume(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (pb2_session_cmd(PB2_CMD_RESUME, 0, NULL) != 0) {
        return mk_fail(ctx, "PLAYER_BUSY", "queue full");
    }
    return mk_ok(ctx);
}

static JSValue js_seek(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int64_t ms = 0;
    JSValue res;
    (void)this_val;
    if (argc < 1) return mk_fail(ctx, "PLAYER_BAD_ARG", "seek(positionMs)");
    JS_ToInt64(ctx, &ms, argv[0]);
    if (ms < 0) ms = 0;
    /*
     * 硬规则 3 + debounce：这里只覆盖 worker 的 seekTarget 并置 pending，
     * 真正的 seek 在 PB2_SEEK_DEBOUNCE_MS 后由 worker 执行最后一个目标。
     * 背靠背 seek 若每个都执行，实测 firstsinkbuf 高达 2367ms。
     * 立即返回，JS 线程零阻塞。
     */
    if (pb2_session_cmd(PB2_CMD_SEEK, ms, NULL) != 0) {
        return mk_fail(ctx, "PLAYER_BUSY", "queue full");
    }
    /*
     * 【缺陷：seek 后进度条归零】返回值必须带回落点。
     *
     * JS 侧 src/services/play_session.js:415 读的是
     *     const settled = Number(r.seekTargetMs);
     *     positionMs: settled > 0 ? settled : Number(r.positionMs) || 0
     * 以前这里只 return mk_ok（只有 {ok:1}）—— seekTargetMs 和 positionMs
     * 都是 undefined，`settled > 0` 假、`Number(undefined) || 0` = 0，
     * 于是 seekTo() 把 positionMs 写成 0，页面 2041 行 `this.play.positionMs = r.positionMs`
     * 把 thumb 归零。用户看到的就是「一拖进度条就跳回开头」。
     *
     * 这里不查 stats 快照：命令刚入队，worker 还没跑（stats 里 seekTargetMs
     * 仍是上一次的值），查了反而更错。直接用本次请求的 ms —— 它就是本会话
     * 最新的覆盖式目标（worker 最终会执行最后一个目标，与这里一致）。
     */
    res = mk_ok(ctx);
    mk_int(ctx, "seekTargetMs", (long long)ms, res);
    mk_bool(ctx, "seekPending", 1, res);
    mk_int(ctx, "positionMs", (long long)ms, res);
    return res;
}

static JSValue js_stop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (pb2_session_cmd(PB2_CMD_STOP, 0, NULL) != 0) {
        return mk_fail(ctx, "PLAYER_BUSY", "queue full");
    }
    return mk_ok(ctx);
}

/*
 * setRect(x, y, w, h)：运行时改视频平面的 render-rectangle（键盘避让）。
 * 非阻塞：只是往 worker 命令队列塞一个 i64（x | y<<16 | w<<32 | h<<48），
 * worker 空闲时对 kmssink 调 gst_util_set_object_arg，立即生效、不重建管线
 * （实测见 spike-report §12）。坐标是屏幕绝对坐标（左上原点），与 open 的
 * x/y 同坐标系；w==0 或 h==0 会把平面缩成 0（等效隐藏）。
 */
static JSValue js_set_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int32_t x = 0, y = 0, w = 0, h = 0;
    int64_t packed;
    /* JS_ToInt32 对 undefined/缺失参数返回 0（且不抛异常），正好是「不动」语义；
       对真正的类型错误会置异常，这里统一按 0 处理即可（避让矩形错一帧无碍）。 */
    if (argc >= 1) JS_ToInt32(ctx, &x, argv[0]);
    if (argc >= 2) JS_ToInt32(ctx, &y, argv[1]);
    if (argc >= 3) JS_ToInt32(ctx, &w, argv[2]);
    if (argc >= 4) JS_ToInt32(ctx, &h, argv[3]);
    /* 面板 480x960，坐标 clamp 到 int16 范围足够；负值允许（补码编码） */
    if (x < -32768) x = -32768; if (x > 32767) x = 32767;
    if (y < -32768) y = -32768; if (y > 32767) y = 32767;
    if (w < 0) w = 0; if (w > 65535) w = 65535;
    if (h < 0) h = 0; if (h > 65535) h = 65535;
    packed = (int64_t)((uint32_t)(uint16_t)(int16_t)x)
           | ((int64_t)((uint32_t)(uint16_t)(int16_t)y) << 16)
           | ((int64_t)((uint32_t)(uint16_t)w) << 32)
           | ((int64_t)((uint32_t)(uint16_t)h) << 48);
    if (pb2_session_cmd(PB2_CMD_SET_RECT, packed, NULL) != 0) {
        return mk_fail(ctx, "PLAYER_BUSY", "queue full");
    }
    return mk_ok(ctx);
}

/*
 * recover()：原地自愈，无参数、非阻塞。
 *
 * 【为什么必须有这个导出】以前 JS 侧 play_session.js 的 recoverSession()
 * 探测不到 native 的 recover，只能退化成 seekBy(currentPos)：
 *   - 点播能用（seek 会重建管线），只是多绕一圈；
 *   - **直播完全无效** —— do_seek() 对直播是早退的，于是 index.vue 的两个
 *     stall 看门狗（8000ms / 25000ms）触发 3 次配额全打空转，
 *     用户看到「画面冻住、进度条还在走、点哪里都没反应，只能返回列表重进」。
 * 现在 native 直接提供入口：直播重拉 souphttpsrc，点播投递原地 seek。
 */
static JSValue js_recover(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (pb2_session_cmd(PB2_CMD_RECOVER, 0, NULL) != 0) {
        return mk_fail(ctx, "PLAYER_BUSY", "queue full");
    }
    return mk_ok(ctx);
}

/*
 * installAmr(path)：用系统 miniapp_cli 安装本地 .amr。
 *
 * 为什么绕到 native：设备上的 $falcon.jsapi 里**没有 pm 模块**
 * （`strings /usr/lib/libfalcon.so` 的模块名池只有 http/storage/modal/nvue/updater，
 * 虽然有 register_pm_jsapi 符号，但运行时取不到），所以 JS 侧调
 * pm.installPackage 一律拿到 undefined。
 *
 * 已验证可行的替代路径（真机实测 ret=0）：
 *   /bin/miniapp_cli install <amr>
 * 它会把包装到 a/b 槽位并返回 `{"appid": "...", "ret": 0}` —— 解析 ret 即可判定成败。
 *
 * 参数：filePath 必须是**已存在的本地绝对路径**（先做 access 检查，
 * 免得把「下载没成功」伪装成「安装失败」，两者的用户提示完全不同）。
 * 超时 60s：解包 + md5 校验 + 落盘，大包可能十几秒。
 */
static JSValue js_install_amr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *path = arg_str(ctx, argc, argv, 0);
    if (!path || !path[0]) {
        if (path) JS_FreeCString(ctx, path);
        return mk_fail(ctx, "BAD_ARGS", "filePath required");
    }
    if (access(path, R_OK) != 0) {
        pb2_logf("install: file not readable: %s", path);
        JS_FreeCString(ctx, path);
        return mk_fail(ctx, "FILE_NOT_FOUND", "amr not found");
    }

    char *argv2[4];
    char out[512];
    argv2[0] = (char *)"/bin/miniapp_cli";
    argv2[1] = (char *)"install";
    argv2[2] = (char *)path;
    argv2[3] = NULL;
    out[0] = '\0';
    int rc = pb2_run(argv2, out, (int)sizeof(out), 60000);
    pb2_logf("install: miniapp_cli install %s rc=%d out=%s", path, rc, out);

    /* 判据用输出里的 "ret": 0 —— miniapp_cli 即使失败也常常返回 rc=0，
     * 只有 ret 字段才代表装包结果。 */
    int ok = (strstr(out, "\"ret\": 0") != NULL) || (strstr(out, "\"ret\":0") != NULL);
    JS_FreeCString(ctx, path);

    JSValue res = JS_NewObject(ctx);
    mk_bool(ctx, "ok", ok ? 1 : 0, res);
    mk_int(ctx, "rc", rc, res);
    mk_str(ctx, "out", out, res);
    if (!ok) {
        mk_str(ctx, "code", "INSTALL_FAILED", res);
        mk_str(ctx, "error", "miniapp_cli install failed", res);
    }
    return res;
}

/*
 * release()：让 worker 收尾，但**有上界**地等。
 *
 * pb2_session_shutdown 内部不是无上界的 pthread_join —— 它先置 quit 标志，
 * 然后在 worker_done 条件变量上做 PB2_SHUTDOWN_WAIT_MS（300ms）的 timedwait：
 *   - worker 在 300ms 内收尾完毕 -> join 立即返回（回收 pthread 资源）；
 *   - 超时 -> pthread_detach 后直接返回，并记账 worker_detached，
 *     下一次 pb2_session_start 会先等旧线程真正退出再 memset(&g_w)。
 *
 * 为什么要这个上界：worker 可能正卡在 12s 的 gst_element_get_state、
 * 3s 的本地 seek 轮询、reap_pair 的 waitpid 或 6 次 amixer spawn 上，
 * 无上界 join 会把 QuickJS 主线程（= UI 线程）冻住十几秒 —— 这正是
 * v3.5.2 里 release 阻塞 4.9~5s 的病根。
 */
static JSValue js_release(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    /* 让 worker 拆管线 + 关 ffmpeg + 删 FIFO + 删 /tmp/mp3playing，然后退出 */
    pb2_session_shutdown();
    return mk_ok(ctx);
}

/* ============================ status / info ============================ */

/*
 * status() 只读无锁快照（seqlock），不加锁、不碰 Gst 对象。
 * 每个字段的语义见 docs/design/player2-api.md。
 */
static JSValue js_status(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    pb2_stats_t st;
    pb2_probe_t *probe = NULL;
    JSValue res;
    unsigned long long frames = 0;
    long long stall = -1;
    long long abytes = 0;
    static const char *names[] = { "idle", "opening", "playing", "paused", "error", "stopped" };
    const char *state_name;
    (void)this_val; (void)argc; (void)argv;

    pb2_session_stats(&st);
    pb2_session_probe(&probe);
    if (probe) {
        frames = pb2_probe_frames(probe);
        abytes = (long long)pb2_probe_abytes(probe);
    }
    stall = pb2_session_stall_ms();

    if (st.state >= 0 && st.state <= 5) {
        state_name = names[st.state];
    } else {
        state_name = "idle";
    }

    res = JS_NewObject(ctx);
    mk_bool(ctx, "ok", 1, res);
    mk_str(ctx, "state", state_name, res);
    mk_int(ctx, "positionMs", st.positionMs, res);
    mk_int(ctx, "durationMs", st.durationMs, res);
    mk_int(ctx, "frames", (long long)frames, res);
    mk_int(ctx, "fps", st.fps, res);
    mk_bool(ctx, "hasAudio", st.hasAudio, res);
    mk_int(ctx, "audioBytes", abytes > 0 ? abytes : st.audioBytes, res);
    /* videoStallMs：距上一帧的毫秒数。没有过帧时 -1（JS 层据此区分"还没起播"和"卡住"） */
    mk_int(ctx, "videoStallMs", stall, res);
    mk_bool(ctx, "seekPending", st.seekPending, res);
    mk_int(ctx, "seekTargetMs", st.seekTargetMs, res);
    mk_int(ctx, "lastError", st.lastError, res);
    mk_str(ctx, "lastErrorMsg", st.lastErrorMsg, res);
    mk_bool(ctx, "planeActive", st.planeActive, res);
    mk_bool(ctx, "pipelineAlive", st.pipelineAlive, res);
    /*
     * ==================== 【P1-8】兼容字段 ====================
     *
     * JS 层（src/pages/index/index.vue 的两个看门狗、src/services/play_session.js
     * 的 readStatus）沿用了 v1 的 status 字段名。v2 架构里其中一部分概念**不存在**
     * （v1 那套 avsync 音画重同步、环形音频缓冲、aplay 进程、pipe 背压）——
     * 但我们**必须**给出这些键，而且给「安全的假值」，原因有两个：
     *
     *   1) 字段缺失时 JS 拿到 undefined。`!undefined` 是 true，所以看门狗的
     *      让位条件 `!st.gateActive && (!st.resyncing || ...)` 恒成立 —— 看门狗
     *      变得过于激进，会在正常播放时误判卡死并杀掉健康会话。
     *   2) 有些字段被当作「异常计数」比较（如 videoSkips 是否在增长），
     *      undefined 参与比较得到 NaN，比较恒假，告警路径永远走不到。
     *
     * 取值原则：
     *   - 能反映真实状态的，取真值（resyncing / resyncingMs / resyncCount）。
     *   - 架构上不存在的，给「不会触发任何告警」的中性值 0 / false，逐条注明。
     */

    /*
     * resyncing：v2 里唯一会让画面「暂时没有新帧、但 native 正在自己处理」
     * 的窗口，就是 seek 重建管线到首帧上屏这一段（以及 OPEN 起播）。
     * 这段时间 JS 看门狗**必须让位**，否则它会在 seek 的 600ms 里误判卡死。
     */
    mk_bool(ctx, "resyncing", st.resyncing, res);
    mk_int(ctx, "resyncingMs",
           st.resyncing && st.resyncStartMs > 0 ? pb2_now_ms() - st.resyncStartMs : 0, res);
    mk_int(ctx, "resyncCount", st.resyncCount, res);

    /*
     * gateActive / gateWaitMs：v1 的「起播定格门」——等音频 ring 攒够数据
     * 才允许视频上屏。v2 用单一权威时钟（kmssink 与 alsasink 共享 pipeline
     * 时钟），**根本不存在这个门**，所以恒 false / 0。给 false 是安全的：
     * 看门狗只在它为 true 时让位，恒 false 表示「不因为门而让位」，
     * 其余判据仍然生效。
     */
    mk_bool(ctx, "gateActive", 0, res);
    mk_int(ctx, "gateWaitMs", 0, res);

    /*
     * avDriftMs / avDriftNowMs：v1 用 audio_anchor_ms 推算音视频漂移。
     * v2 没有独立音频锚（同一 pipeline 时钟），无此概念，恒 0。
     * JS 的 numSigned() 保留负号，0 不会被判为异常。
     */
    mk_int(ctx, "avDriftMs", 0, res);
    mk_int(ctx, "avDriftNowMs", 0, res);

    /*
     * videoSkips：v1 追赶时主动丢帧的计数。v2 丢帧由 kmssink 的
     * max-lateness=40 + qos 决定，不在 C 层计数，恒 0。
     * JS 判断「丢帧是否持续增长」，恒 0 即「从不增长」，安全。
     */
    mk_int(ctx, "videoSkips", 0, res);

    /*
     * videoRestarts：v1 因 A/V 失步重建视频子进程的次数。v2 的对应概念是
     * seek（含看门狗触发的 recover），已计入 resyncCount，这里保持 0
     * 以免 JS 把它当成额外异常信号重复告警。
     */
    mk_int(ctx, "videoRestarts", 0, res);

    /*
     * audioBufMs：v1 环形缓冲水位。v2 音频走 alsasink（buffer-time=30ms），
     * 没有 C 层可读的水位，恒 0。
     */
    mk_int(ctx, "audioBufMs", 0, res);

    /* resyncLeadMs：v1 的「提前多久开始重同步」。v2 无此调度，恒 0。 */
    mk_int(ctx, "resyncLeadMs", 0, res);

    /*
     * audioLateMs / audioLateVisibleMs：v1 的音频迟到度量，-1 表示「无数据」。
     * v2 无独立音频时间轴，给 -1（JS 的 numLatency() 专门保留 -1 语义，
     * 表示「不可用」，不会触发告警）。
     */
    mk_int(ctx, "audioLateMs", -1, res);
    mk_int(ctx, "audioLateVisibleMs", -1, res);

    /*
     * 以下四个是 v1 的音频进程健康度计数器（丢包/欠载/写错误/进程死亡）。
     * v2 没有 aplay 子进程，也没有 C 层环形缓冲，这些异常**架构上不可能
     * 发生**，所以恒 0 / false。给 0 而不是省略，是为了让 JS 的
     * `st.audioDropped > 0` 一类判断得到明确的 false。
     */
    mk_int(ctx, "audioDropped", 0, res);
    mk_int(ctx, "audioUnderruns", 0, res);
    mk_int(ctx, "audioWrErrors", 0, res);
    mk_int(ctx, "audioRingDrops", 0, res);
    mk_bool(ctx, "audioDead", 0, res);
    /*
     * screenOff：息屏/背光断电时为 true。
     *
     * JS 看门狗**必须**用它做门禁：息屏期间内核 disable CRTC，平面停止提交，
     * videoStallMs 会持续增长，但音频还在播、管线完全健康。若看门狗不看这个
     * 标志就会误判"卡死"→ 杀管线重建，把一个本来能自动恢复的会话毁掉
     * （唤醒后平面会自动回到 ACTIVE，实测 vid_changes 恢复）。
     *
     * 读的是 /tmp/.screen_state（<32B）和 bl_power，耗时 <50us，不需要缓存。
     */
    mk_bool(ctx, "screenOff", pb2_screen_off() == 1, res);
    /*
     * muted：全局静音开关状态（1=静音，0=有声）。
     *
     * JS 侧可以用它显示「已静音」角标，也可以在 toggle 后据此判断。
     * 开关来源见 cmd.h 顶部注释：/tmp/pb2_mute > /tmp/pb2_sound >
     * PB2_MUTE 环境变量 > 默认静音。
     */
    mk_bool(ctx, "muted", pb2_mute_enabled(), res);
    mk_str(ctx, "mutedBy", pb2_mute_reason(), res);
    return res;
}

static JSValue js_info(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue res = JS_NewObject(ctx);
    (void)this_val; (void)argc; (void)argv;
    mk_bool(ctx, "ok", 1, res);
    mk_str(ctx, "module", PB2_MODULE_NAME, res);
    mk_str(ctx, "version", PB2_VERSION, res);
    mk_str(ctx, "gstVersion", pb2_session_gst_version(), res);
    mk_bool(ctx, "registryReady", pb2_session_gst_ready(), res);
    mk_int(ctx, "planeId", 76, res);
    mk_int(ctx, "panelXOffset", 107, res);
    mk_bool(ctx, "logOn", pb2_log_on(), res);
    mk_int(ctx, "rssKb", (long long)pb2_rss_kb(), res);
    return res;
}

/* ============================ 模块注册 ============================ */

/*
 * 导出名清单。JS 侧（task-7）用 import('player2') 取 default 或具名导出。
 *
 * 与 player 模块的差异（故意的，不要"对齐"）：
 *   - 没有 pauseRender / resumeRender：v1 用它们贴 fb 缓冲，v2 的 kmssink
 *     自己持有平面，暂停就是 set_state(PAUSED)，平面定格不黑，不需要重贴。
 *   - 没有 redraw：同上，且 render-rectangle 只在 open 时确定一次；
 *     布局变化请重新 open（或将来加 setRect）。
 *   - 新增 info 里的 gstVersion / registryReady / logOn / rssKb 便于诊断。
 */
#define PB2_EXPORTS(X) \
    X("open", js_open, 15) \
    X("pause", js_pause, 0) \
    X("resume", js_resume, 0) \
    X("seek", js_seek, 1) \
    X("status", js_status, 0) \
    X("stop", js_stop, 0) \
    X("setRect", js_set_rect, 4) \
    X("recover", js_recover, 0) \
    X("installAmr", js_install_amr, 1) \
    X("release", js_release, 0) \
    X("info", js_info, 0)

static int player2_module_init(JSContext *ctx, JSModuleDef *m) {
    JSValue def = JS_NewObject(ctx);

#define PB2_SET_PROP(name, fn, nargs) \
    JS_SetPropertyStr(ctx, def, name, JS_NewCFunction(ctx, fn, name, nargs));
    PB2_EXPORTS(PB2_SET_PROP)
#undef PB2_SET_PROP

    JS_SetModuleExport(ctx, m, "default", def);

#define PB2_SET_EXPORT(name, fn, nargs) \
    JS_SetModuleExport(ctx, m, name, JS_NewCFunction(ctx, fn, name, nargs));
    PB2_EXPORTS(PB2_SET_EXPORT)
#undef PB2_SET_EXPORT
    return 0;
}

static JSModuleDef *player2_module_load(JSContext *ctx, const char *moduleName) {
    JSModuleDef *m;
    if (strcmp(moduleName, PB2_MODULE_NAME) != 0) return NULL;
    m = JS_NewCModule(ctx, moduleName, player2_module_init);
    if (!m) return NULL;
    JS_AddModuleExport(ctx, m, "default");
#define PB2_ADD_EXPORT(name, fn, nargs) JS_AddModuleExport(ctx, m, name);
    PB2_EXPORTS(PB2_ADD_EXPORT)
#undef PB2_ADD_EXPORT
    return m;
}

extern void registerCModuleLoader(const char *moduleName, LoadCModuleFunction loader);

JQUICK_EXPORT void custom_init_jsapis(void) {
    /*
     * ffmpeg 子进程在管道对端关闭时会收到 SIGPIPE。忽略它，
     * 让 write 返回 EPIPE 由代码处理（旧实现也这么做，是必需的）。
     */
    signal(SIGPIPE, SIG_IGN);
    pb2_log_init();
    pb2_logf("player2 module init, version=%s", PB2_VERSION);
    registerCModuleLoader(PB2_MODULE_NAME, &player2_module_load);
}
