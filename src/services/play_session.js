// 播放会话编排（可注入、可取消；页面只消费稳定接口）
//
// 视频链：**无条件 view**（权威 cid/时长/标题/显示宽高）→ WBI 密钥 → playurl html5 qn 阶梯
//         → player.open(按真实宽高信箱适配的居中矩形 + UA/Referer)。
// 直播链：搜索 kind=live → getRoomPlayInfo 取 FLV 直链（见 services/bili/live.js）
//         → 同一个 player.open（音频进程拉同一条 URL，durationMs=0）。
// 控制：toggle / seekBy(±20s) / readStatus / closeSession，全部幂等、结构化错误。
//
// 硬解（v3.0.0 / X6 Pro）：native 侧已从「ffmpeg 软解成 RGB 帧」改为「ffmpeg 只搬压缩包 +
// gst mppvideodec 硬解上屏」。软解时代的像素预算已由 pixelBudgetFor（用户设置）取代。
import { QN_LADDER, HTML5_FPS, buildPlayurlQuery, parseHtml5Response, parseDashResponse, validateStreamUrl } from './bili/playurl.js';
import { fetchLivePlayUrl, LIVE_FPS } from './bili/live.js';
import { deriveMixinKey } from './bili/wbi.js';
// 【X7 Pro 适配】视频矩形改由设备 profile 计算：X6 Pro 结果与旧的
// playVideoRects 完全一致；X7 Pro 会按 936x280 视口缩放并把面板列偏移、
// plane-id 一起下发给 native（旧代码把 107 / 76 写死在 C 里）。
import { ensureProfile, playVideoRectsFor } from './device_profile.js';
import { dashLadderFor, videoFpsCapFor, liveFpsCapFor, liveQnFor, pixelBudgetFor } from './video_settings.js';

export const SEEK_STEP_MS = 20000;

/* ================= native 数值字段归一化（P0-1 防御性修复） =================
 *
 * 为什么必须有：native 的 status() 用**哨兵值**表达"还没测到"，最典型的是
 * videoStallMs 在"从未出过一帧"时返回 **-1**。旧代码写的是
 *     videoStallMs: Number(st.videoStallMs) || 0
 * 而 Number(-1) 是 -1（真值，|| 不会兜底）→ -1 原样透传到页面看门狗的
 * `videoStallMs > 8000` 判断，恒为 false → 看门狗永不触发 → 界面永久"加载中…"，
 * 用户只能杀进程。B 站 m4s 直链带 deadline（约 2 小时过期）是最常见的踩中路径。
 *
 * 三个工具的分工（不要混用）：
 *   num(v, dflt)        —— 计数/位置/字节/时长：非 number、非有限数、负数 → dflt。
 *                          用于 frames/positionMs/audioBytes/videoStallMs 等"负数无意义"的量。
 *   numSigned(v, dflt)  —— 可为负的诊断量：只挡非 number 与非有限数，保留负数。
 *                          avDriftMs（音频位置−画面位置，负=声音滞后）、
 *                          audioBufMs（可闻锚修正量）、lastError（native 错误码本身可为负，
 *                          真机见过 -5/EIO）都属于这一类。
 *   numLatency(v)       —— 音画起播差：**-1 是"未测到"的有效语义**，必须原样保留，
 *                          页面用 `>= 0` 判断"是否测到了"。缺失时也返回 -1。
 * ========================================================================= */
export function num(v, dflt) {
  const d = typeof dflt === 'number' ? dflt : 0;
  const n = typeof v === 'number' ? v : typeof v === 'string' && v !== '' ? Number(v) : NaN;
  return isFinite(n) && n >= 0 ? n : d;
}

export function numSigned(v, dflt) {
  const d = typeof dflt === 'number' ? dflt : 0;
  if (typeof v === 'number') return isFinite(v) ? v : d;
  if (v === null || v === undefined || v === '') return d;
  const n = Number(v);
  return isFinite(n) ? n : d;
}

export function numLatency(v) {
  if (typeof v === 'number') return isFinite(v) ? v : -1;
  if (v === null || v === undefined || v === '') return -1;
  const n = Number(v);
  return isFinite(n) ? n : -1;
}

/* native status().state 白名单。
 *
 * native 自己的枚举是 idle/opening/playing/paused/error/stopped（见
 * native/csrc/jsapi_player2.c 的 names[]），页面另有两个 UI 态 loading/seeking/ended。
 * 这里只放行这些已知值，其余（含拼错、native 将来新增未知态、返回对象/数组等）
 * 一律落到 'error' —— 让界面显示"可返回重试"，而不是停在一个谁都分支不到的状态上
 * 无限"加载中…"。
 *
 * 'stopped' 特殊处理：它是 native 对 stop() 之后的正常态（不是故障），归一到 'idle'，
 * 否则正常停止会被界面当成错误。 */
const KNOWN_STATES = {
  idle: 'idle',
  opening: 'opening',
  playing: 'playing',
  paused: 'paused',
  seeking: 'seeking',
  ended: 'ended',
  error: 'error',
  stopped: 'idle'
};

export function normalizeState(raw, ctx) {
  if (typeof raw === 'string' && KNOWN_STATES[raw] !== undefined) return KNOWN_STATES[raw];
  const shown = typeof raw === 'string' ? raw : raw === undefined ? 'undefined' : JSON.stringify(raw);
  if (ctx && ctx.log) {
    ctx.log('[bili] 未识别的播放器状态 state=' + shown + ' → 按 error 处理（避免界面卡在未知态）');
  }
  return 'error';
}

/* ================= 流末尾判据（n9，播放到结尾的自愈死循环） =================
 *
 * 背景（真机 2026-11-01 08:22 实测的死循环）：
 *   `dur=114900ms` 的视频播到 114879ms 后，源 ffmpeg 正常读完退出（"source died
 *   after first frame"），但 **demuxer 一直没发 EOS** → native 的
 *   check_src_after_first_frame() 3 秒后只 `patch_alive(0)`，**不改 state** →
 *   `state` 永远停在 `playing`。JS 看门狗看到「videoStall 持续增长 + 音频冻结」，
 *   判定假死，执行原地 seek(pos)（此时 pos 已到 118776 > dur，被 clamp 到 113900）
 *   → 重开 ffmpeg 读 1 秒又死 → **无限循环，界面永远不显示「已播完」**。
 *
 * 为什么不能只靠 state：native 只在真正收到 EOS 时才 patch_state(STOPPED)
 * （native/csrc/session/session.c:1608），而这条路径上 EOS 根本没来；即使来了，
 * `stopped` 也会被 normalizeState 归一成 'idle'（那是 stop() 的正常态），
 * 页面等的 `'ended'` 在 native 白名单里不存在 —— 三者叠加，末尾永远无信号。
 *
 * 所以必须在 JS 侧用**可得的量**推断「这是正常播完，不是卡死」：
 *   1) 位置已经走到时长尾部（`dur - END_TAIL_MS` 以内）→ 正常播完；
 *   2) 管线已死（`pipelineAlive === false`，即 native 的 alive=0）且位置已到
 *      `dur - END_DEAD_TAIL_MS` 以内 → 源读完退出的典型形态，也按播完处理；
 *      更靠前的位置判死不成立（那才是真故障，交给看门狗）。
 * 直播（durationMs <= 0）**永远不判末尾**：时长未知，位置没有参照。
 */
export const END_TAIL_MS = 1000; // 位置进到最后 1s 即算播完
export const END_DEAD_TAIL_MS = 3000; // 管线已死时放宽到 3s（源退出到 EOS 的窗口）

export function isStreamEnded(st, durationMs) {
  if (!st || typeof st !== 'object') return false;
  const dur = num(durationMs !== undefined ? durationMs : st.durationMs, 0);
  if (dur <= 0) return false; // 直播/时长未知：不判末尾
  const pos = num(st.positionMs, 0);
  if (pos <= 0) return false; // 还没起来，交给「无首帧」看门狗
  if (pos >= dur - END_TAIL_MS) return true;
  /* pipelineAlive 只有在明确为 false 时才算已知死亡；老 native 没有该字段时
   * readStatus 已经补成 true，这里再挡一层 undefined/null，避免误判。 */
  if (st.pipelineAlive === false && pos >= dur - END_DEAD_TAIL_MS) return true;
  return false;
}

/* ================= 直播终止判据（P1-4） =================
 *
 * 点播末尾靠 isStreamEnded()（比位置与时长），直播没有时长可用，必须换判据。
 *
 * 直播 EOS（主播下播）在 native 侧走的是：
 *   session.c 的 `BUS EOS` → `pb2_stats_patch_state(&g_w.stats, PB2_STATE_STOPPED)`
 * 而 jsapi_player2.c 的 state 白名单是 {idle, opening, playing, paused, error, stopped}
 * —— **没有 ended**；normalizeState 又把 `stopped` 归一成 `'idle'`（表示 stop() 的正常态）。
 * 三条叠加的结果：主播下播后 tick 拿到的 state 是 `'idle'`，页面原有的
 * `if (st.state === 'ended')` 分支永远进不去 → 界面停在「播放中」，进度条（直播位置
 * 就是墙钟秒数）还在继续走，用户只能自己返回。
 *
 * 所以这里用「live + state 落回 idle/ended」判终止，再用两个证据把
 * 「刚进页面还没起播」（state 也在 idle/opening，位置 0、没出过帧）排除掉：
 *   · everPlayed：本会话曾经出现过 playing（页面侧维护的历史事实）；
 *   · frames > 0：至少真的出过帧。
 * 只要有一条成立，就说明这场直播确实播起来过、现在停了 = 已结束。
 *
 * ★ **不能用 positionMs > 0 当证据**：直播位置 = 墙钟（native live_position_ms()），
 * 从 open 那一刻就开始走，一帧都没出也照样每秒 +1000。用它会在一进页面、native
 * 还没 patch 到 opening 的那一个 tick 上把「刚打开」误判成「已结束」。
 *
 * 注意：也**不要**用「位置不再增长」判 —— 同样是墙钟，EOS 之后管线还在、位置照样涨。 */
export function isLiveEnded(st, live, everPlayed) {
  if (live !== true) return false; // 只对直播生效，点播走 isStreamEnded
  if (!st || typeof st !== 'object') return false;
  /* state 只认 native 明确收回的两个值：
   *   idle  = STOPPED（EOS）经 normalizeState 归一后的结果；
   *   ended = 将来 native 真加了 PB2_STATE_ENDED 时的直通值。 */
  if (st.state !== 'idle' && st.state !== 'ended') return false;
  if (everPlayed === true) return true;
  /* 没观察到过 playing（轮询 1s 一次，主播刚下播就点进去时会漏）：
   * 用「出过帧」兜底，避免把「刚进页面还没起播」误判成已结束。 */
  return num(st.frames, 0) > 0;
}

/* ================= 直播重连提示（P1-4 的 JS 侧配套） =================
 *
 * native 直播走 souphttpsrc（timeout=15s、retries=3），最坏 45 秒里画面冻住而
 * state 仍是 playing —— 用户看不到任何反馈。native2 会在重连开始时
 * `pb2_stats_patch_error(&g_w.stats, PB2_E_LIVE_RECONNECT, "...")`，
 * 重连成功清零（**故意不改 state**，否则 readStatus 会走 ok:false 让页面判失败）。
 *
 * 这里只做「码 → 是否正在重连」的翻译。码值 17 与 native2 对齐
 * （core/queue.h 已占用 0..16，17 是下一个空位）。
 * 另留一条 lastErrorMsg 含 `reconnect` 的兜底：万一 native 最终换了码，提示仍能出来。
 */
export const LIVE_RECONNECT_ERR = 17;

export function isLiveReconnecting(st, live) {
  if (live !== true) return false;
  if (!st || typeof st !== 'object') return false;
  /* 真错误态（state === 'error'）交给页面原有的失败路径，不抢它的提示 */
  if (st.state === 'error') return false;
  if (numSigned(st.lastError, 0) === LIVE_RECONNECT_ERR) return true;
  const msg = typeof st.lastErrorMsg === 'string' ? st.lastErrorMsg : '';
  return /reconnect/i.test(msg);
}

/* 解码像素预算的历史默认值（= 1080p 全尺寸），保留导出供测试/诊断引用。
 * 真正生效的预算现在来自「我的 → 视频调节 → 分辨率」（pixelBudgetFor）。
 *
 * 历史：v2.6.0 设 310000（≈ 852x480 以下）是因为 4×A53 软解 720p/1080p 只有 93% 实时吞吐。
 * v3.0.0 换成 ffmpeg 只 copy + gst mppvideodec 硬解后，这个限制就没有依据了。真机实测
 * （2026-10-30，同一段 8s 素材、fakesink、rotation=270）：
 *   480p@60  解码 3168ms → 渲染 58.12fps dropped 0
 *   720p@60  解码 5271ms → 渲染 57.71fps dropped 0
 *   1080p@60 解码 10347ms → 渲染 55.54fps dropped 0
 * 即便 1080p60 带旋转也远超实时（且播放时 ffmpeg 不解码，只搬压缩包）。 */
export const MAX_DECODE_PIXELS = 1920 * 1080;

/* 直播：→ {ok, live:true, url, audioUrl:'', qn, cid, roomid, aid:0, durationMs:0,
 *          title, width, height, host, fps} | {ok:false, stage, code?, message}
 *
 * 每次播放都重新取直链（URL 带 expires，过期即 403）——转码时代的「服务器地址 +
 * 解析缓存」已随 server/live_proxy.py 一起删除。
 * 【audioUrl 固定为空 —— 直播走单输入单管线，不再经 ffmpeg】
 * v1（player.c）确实用「同一 URL 的独立进程 spawn_ffaudio」解过「单进程串行写两个输出时
 * 视频 pipe 背压连带音频停摆」的问题。但 v2（player2）的直播路径**不再经过 ffmpeg**：
 * 实测（2026-11-01 X6 Pro）B 站直播 FLV 的 PTS 是绝对值（≈2854014 秒 ≈ 792 小时），
 * 流式 matroskademux 在这种绝对 PTS + 非 seekable FIFO 下**永久 PREROLLING**
 * （`ffmpeg -c copy -f matroska -` → FIFO → filesrc，带/不带 -copyts 都 rc=124、
 * buffers=0；落成普通文件再读才正常）。所以 v2 直播改为 GStreamer 直连
 * `souphttpsrc ! flvdemux`，实测 45s 出 3018 帧、零 error、真实 kmssink 上屏
 * vid_changes=146/6s、big_gaps(>80ms)=0。
 *
 * FLV 单流本身已含音轨，flvdemux 会同时给出 video/x-h264 和 audio/mpeg 两个 pad，
 * 音视频天然同容器同 interleave、不需要对齐。**若这里仍回填 audioUrl=url，
 * native 会按 MAP_DUAL_IN 用同一个 URL 开第二条连接**（日志里 in= 和 in2= 完全相同），
 * 对直播纯属浪费，还会让 native 的「直播直连」判据失效。
 * 所以直播固定 audioUrl=''，让 native 走单输入直连路径。 */
export async function resolveLiveUrl(ctx, item) {
  const roomid = Number(item && item.roomid) || 0;
  if (!(roomid > 0)) return { ok: false, stage: 'param', message: '直播间号无效' };
  const r = await fetchLivePlayUrl(ctx.client, roomid, {
    log: ctx.log,
    qn: liveQnFor(ctx.videoSettings)
  });
  if (cancelled(ctx)) return { ok: false, stage: 'cancel', message: '已取消' };
  if (!r.ok) return r;
  return {
    ok: true,
    live: true,
    url: r.url,
    /* 固定空：直播走 native 的「单输入直连」路径（souphttpsrc ! flvdemux），
     * 见本函数头注释。绝不能回填 r.url，否则 native 会开第二条连接。 */
    audioUrl: '',
    qn: r.qn,
    cid: roomid,
    roomid: roomid,
    aid: 0, /* 直播不接评论（reply 无 oid） */
    durationMs: 0, /* 时长未知：不判播完、seek 不设上界 */
    title: (item && item.title) || '',
    /* 用服务端上报的真实分辨率（实测 1280×720）而不是写死的输出尺寸：
     * 硬解路径下 kmssink 的 render-rectangle 做硬件缩放，信箱比例必须按源算。 */
    width: r.width || 1280,
    height: r.height || 720,
    host: r.host || '',
    fps: LIVE_FPS
  };
}

// WBI 密钥会话级缓存（约每日轮换，单次 app 会话内复用安全）。
// → {ok:true, key} | {ok:false, stage, code, message}
//   失败时**必须带出 stage/code/message**：nav 在笔上是间歇性失败（真机日志
//   `load search 首次失败(wbi)` 反复出现），只回 null 会让 UI 只能显示 code=?，无法诊断。
export async function ensureMixinResult(ctx) {
  if (ctx.mixinMemo && ctx.mixinMemo.key) return { ok: true, key: ctx.mixinMemo.key };
  const r = await ctx.client.fetchWbiKeys();
  if (!r.ok) {
    return {
      ok: false,
      stage: r.stage || 'wbi',
      code: r.code,
      message: r.message || 'WBI 密钥获取失败',
      // nav 内部已多趟退避重试（见 client.fetchWbiKeys）→ 页面层不必再整体重跑一遍
      wbiRetried: true
    };
  }
  const key = deriveMixinKey(r.imgKey, r.subKey);
  if (!/^[0-9a-f]{32}$/.test(key)) {
    return { ok: false, stage: 'wbi', message: 'mixin_key 派生失败' };
  }
  if (ctx.mixinMemo) {
    ctx.mixinMemo.key = key;
    ctx.mixinMemo.at = Date.now();
  }
  return { ok: true, key: key };
}

// 兼容入口：只要 key（无 key 即 null）。需要错误细节的调用方用 ensureMixinResult。
export async function ensureMixin(ctx) {
  const r = await ensureMixinResult(ctx);
  return r.ok ? r.key : null;
}

function cancelled(ctx) {
  return !!(ctx.cancelled && ctx.cancelled());
}

// → {ok, url, audioUrl, qn, cid, durationMs, title, width, height, host} | {ok:false, stage, message}
export async function resolveVideoUrl(ctx, item) {
  // 直播（search kind=live）：不走 bvid/view/playurl 链，直接构造服务端点播地址
  if (item && item.kind === 'live') return resolveLiveUrl(ctx, item);
  if (!item || !/^BV[0-9A-Za-z]{10}$/.test(item.bvid || '')) {
    return { ok: false, stage: 'param', message: '稿件标识不合法' };
  }
  // 无条件 view：权威 cid/时长/标题 + **显示宽高**（feed 层没有 width/height；
  // 竖屏稿件靠 view.dimension 算真实信箱比例，否则会被拉伸成横屏）
  const v = await ctx.client.fetchView(item.bvid);
  if (cancelled(ctx)) return { ok: false, stage: 'cancel', message: '已取消' };
  if (!v.ok) return { ok: false, stage: 'view', subStage: v.stage, message: v.message };
  const cid = v.cid;
  const durationSec = v.duration;
  const width = v.width || 640;
  const height = v.height || 360;
  const title = item.title || v.title || '';
  const aid = v.aid || 0; /* 评论上下文（x/v2/reply oid） */
  const mixin = await ensureMixin(ctx);
  if (cancelled(ctx)) return { ok: false, stage: 'cancel', message: '已取消' };
  if (!mixin) return { ok: false, stage: 'wbi', message: 'WBI 密钥获取失败' };
  const nowSec = () => (ctx.nowSec ? ctx.nowSec() : Math.floor(Date.now() / 1000));
  const fatal = (res) => res && res.stage === 'api' && (res.code === -404 || res.code === 62002 || res.code === 62004);

  // —— 主路径：DASH 双流（fnval=16）
  //   音频轨独立 ~66kbps（设备实测带宽 646kbps 的 1/10）→ 声音供给不再被稿件码率/带宽
  //   贴顶拖垮（音频卡顿根因，白噪对照实验已证链路清白）；视频选 avc1 避开 HEVC 软解。
  let last = null;
  let stop = false;
  /* v3.2.0：qn 阶梯改由「我的 → 视频调节 → 分辨率/帧率」决定（见 services/video_settings.js）。
   * 默认（auto + 自动帧率）与旧行为完全一致 [116,80,74,32,16]；用户选了 30fps 时
   * 30fps 档会被提到前面（116/74 是 60fps 专档，先试它们等于白拉双倍码率）。 */
  const ladder = dashLadderFor(ctx.videoSettings);
  const lastIdx = ladder.length - 1;
  for (let i = 0; i < ladder.length; i++) {
    const qn = ladder[i];
    const built = buildPlayurlQuery({ bvid: item.bvid, cid: cid, qn: qn, fnval: 16 }, mixin, nowSec());
    if (!built.ok) return { ok: false, stage: 'param', message: built.message };
    const res = await ctx.client.fetchPlayurl(built.query);
    if (cancelled(ctx)) return { ok: false, stage: 'cancel', message: '已取消' };
    const parsed = parseDashResponse(res, qn, i < lastIdx);
    if (!parsed.ok) {
      last = { stage: parsed.stage === 'api' ? 'playurl' : parsed.stage, message: parsed.message };
      if (ctx.log) {
        ctx.log('[bili] dash 尝试失败 qn' + qn + ': ' + parsed.message + (parsed.avail ? ' | 可用档位: ' + parsed.avail : ''));
      }
      if (fatal(res)) {
        stop = true; // 版权/不存在：阶梯与 durl 回退都无意义
        break;
      }
      continue;
    }
    const vv = validateStreamUrl(parsed.videoUrl);
    if (!vv.ok) {
      last = { stage: 'param', message: 'URL ' + vv.reason };
      if (ctx.log) ctx.log('[bili] dash 尝试失败 qn' + qn + ': URL ' + vv.reason);
      continue;
    }
    // 解码预算硬校验：硬解时代上限已是 1080p 全尺寸，但用户可在「我的 → 视频调节」
    // 里选更低的分辨率（省流/弱网），此时按所选档的上限拒载更高档，让阶梯继续降级。
    // 注意 parseDashResponse 在池内按像素降序取最大，而 qn 池为空时会退回全量池 ——
    // 所以这里必须再按实际宽高卡一次，否则「选 480p」仍可能拿到 1080p。
    const px = (parsed.width || 0) * (parsed.height || 0);
    const budget = pixelBudgetFor(ctx.videoSettings);
    if (px > budget) {
      last = { stage: 'playurl', message: 'DASH 分辨率超解码预算' };
      if (ctx.log) {
        ctx.log('[bili] dash qn' + parsed.qn + ' ' + parsed.width + 'x' + parsed.height +
          ' (' + px + 'px > ' + budget + ') 超解码预算 → 回退 durl 360p');
      }
      continue;
    }
    let audioUrl = parsed.audioUrl || '';
    if (audioUrl) {
      const av = validateStreamUrl(audioUrl);
      if (!av.ok) {
        // 音频 URL 坏 → 降级无音轨（宁可静音不中断播放）
        if (ctx.log) ctx.log('[bili] dash audio URL 弃用: ' + av.reason);
        audioUrl = '';
      }
    }
    if (ctx.log) {
      ctx.log(
        '[bili] stream DASH qn=' + parsed.qn + ' ' + (parsed.width || '?') + 'x' + (parsed.height || '?') +
          ' vb=' + parsed.bandwidth + ' ' + parsed.codecs +
          ' ab=' + parsed.audioBandwidth + ' host=' + vv.host +
          ' | 可用档位: ' + parsed.avail
      );
    }
    return {
      ok: true,
      url: parsed.videoUrl,
      audioUrl: audioUrl,
      qn: parsed.qn || qn,
      cid: cid,
      aid: aid,
      durationMs: parsed.durationMs || Math.max(0, Math.floor(durationSec * 1000)),
      title: title,
      width: width,
      height: height,
      host: vv.host
    };
  }

  // —— 回退：durl 单文件（fnval=1，html5 老路径，保底可播）
  if (!stop) {
    for (let i = 0; i < QN_LADDER.length; i++) {
      const qn = QN_LADDER[i];
      const built = buildPlayurlQuery({ bvid: item.bvid, cid: cid, qn: qn }, mixin, nowSec());
      if (!built.ok) return { ok: false, stage: 'param', message: built.message };
      const res = await ctx.client.fetchPlayurl(built.query);
      if (cancelled(ctx)) return { ok: false, stage: 'cancel', message: '已取消' };
      const parsed = parseHtml5Response(res);
      if (!parsed.ok) {
        last = { stage: parsed.stage === 'api' ? 'playurl' : parsed.stage, message: parsed.message };
        if (ctx.log) ctx.log('[bili] playurl 尝试失败 qn' + qn + ': ' + parsed.message);
        if (fatal(res)) break;
        continue;
      }
      const v2 = validateStreamUrl(parsed.url);
      if (!v2.ok) {
        last = { stage: 'param', message: 'URL ' + v2.reason };
        if (ctx.log) ctx.log('[bili] playurl 尝试失败 qn' + qn + ': URL ' + v2.reason);
        continue;
      }
      if (ctx.log) ctx.log('[bili] stream durl(回退) qn=' + (parsed.quality || qn) + ' ' + v2.length + 'c host=' + v2.host);
      return {
        ok: true,
        url: parsed.url,
        audioUrl: '',
        qn: parsed.quality || qn,
        cid: cid,
        aid: aid,
        durationMs: Math.max(0, Math.floor(durationSec * 1000)),
        title: title,
        width: width,
        height: height,
        host: v2.host
      };
    }
  }
  return { ok: false, stage: (last && last.stage) || 'playurl', message: (last && last.message) || '未取到播放地址' };
}

// → {ok, session} | {ok:false, stage, message, code?} | {ok:false, stage:'cancel'}
export async function openVideo(ctx, item) {
  const rs = await resolveVideoUrl(ctx, item);
  if (!rs.ok) return rs;
  if (cancelled(ctx)) return { ok: false, stage: 'cancel', message: '已取消' };
  // 矩形按**稿件真实显示宽高**做信箱适配（竖屏 → 物理竖柱，不再被拉伸成横屏）。
  // 直播没有进度条，视频区用 LIVE_VIDEO_DESIGN（266 高）；点播是 240 高 + 26px 进度条。
  // profile 会把设计坐标乘 setViewPort 的缩放系数，并按本机视口宽度做旋转变换：
  // X7 Pro 的面板行数是 936 而不是 960，列偏移是 100 而不是 107。
  // 这两个矩形必须与 falcon 侧 hole 的几何严格一致（见 base.less），
  // 否则会出现「画面与点击热区错位」。
  const profile = await ensureProfile();
  const rects = playVideoRectsFor(rs.width, rs.height, profile, rs.live === true);
  /* 帧率上限（0 = 不限）：native 侧在 mppvideodec 之后插 videorate max-rate=N。
   * 为什么必须有这道闸：部分稿件服务端**只给 60fps 档**（例如 1080P60），
   * 光靠「请求 30fps 档」会直接落空；videorate 只做上限不插帧，低帧率内容直通。 */
  const fpsCap = rs.live ? liveFpsCapFor(ctx.videoSettings) : videoFpsCapFor(ctx.videoSettings);
  const openRes = await ctx.player.openSession({
    input: rs.url,
    input2: rs.audioUrl || '', // DASH 音频轨第二输入（空 = durl 回退 / 直播单流路径）
    startMs: 0,
    durationMs: rs.durationMs, // 直播 = 0（时长未知）
    fps: fpsCap > 0 ? Math.min(rs.fps || HTML5_FPS, fpsCap) : (rs.fps || HTML5_FPS),
    audio: true,
    transpose: profile.transpose,
    rect: rects.physical,
    /* 【X7 Pro 适配】native 不再写死 107 / 76：
     * panelOffset = 逻辑带在 480 宽面板里的起始列；
     * planeId = -1 时 kmssink 自动挑空闲 overlay 平面（X7 Pro 默认）；
     * swScale = 1 时 native 在解码后插 videoscale，把 buffer 缩到窗口大小 ——
     * 自选平面可能没有缩放器，靠它保证画面比例正确（X7 Pro 默认开）。 */
    panelOffset: profile.panelXOffset,
    planeId: profile.planeId,
    swScale: profile.swScale,
    /* '' = 自动：已连蓝牙耳机走 bluealsa，否则内置扬声器（player.c 的 detect_bt_pcm） */
    audioDevice: '',
    userAgent: ctx.mediaUa,
    referer: ctx.mediaReferer,
    fpsCap: fpsCap,
    /*
     * 显式直播意图（native open 的第 16 个位置参数）。
     *
     * 不能靠 native 的「durationMs == 0 且是 http」猜：B 站 view 接口对
     * 互动视频/部分港澳台条目会返回 duration: 0，那种点播被猜成直播后
     * 会走 souphttpsrc ! flvdemux（DASH 分片不是 FLV）→ 无画面，
     * 且永久失去 seek（do_seek 对直播早退），用户只看到「放不了、拖不动」。
     * 这里我们是明确知道意图的，直接告诉 native。
     */
    isLive: rs.live === true
  });
  if (!openRes || openRes.ok !== true) {
    return {
      ok: false,
      stage: 'open',
      code: openRes ? openRes.code : undefined,
      message: (openRes && (openRes.code + ': ' + openRes.error)) || '播放器启动失败'
    };
  }
  if (ctx.log) {
    ctx.log(
      '[bili] open ok out=' + openRes.outWidth + 'x' + openRes.outHeight +
        ' fps=' + openRes.fps +
        ' audio=' + (openRes.audioRate || '?') + (openRes.audioBt ? '(BT)' : '') +
        (rs.live ? ' LIVE' : rs.audioUrl ? ' DASH' : ' durl')
    );
  }
  return {
    ok: true,
    session: {
      kind: rs.live ? 'live' : 'video',
      live: !!rs.live,
      roomid: rs.roomid || 0,
      bvid: rs.live ? 'live:' + rs.roomid : item.bvid, /* 直播无 bvid（不入历史，日志用它占位） */
      cid: rs.cid,
      title: rs.title || item.title || '',
      up: item.up || '',
      desc: item.desc || '', /* 简介（用户手绘稿 m11950：左列上方要显示标题和简介） */
      cover: item.cover || '',
      qn: rs.qn,
      aid: rs.aid || 0, /* 评论 oid（直播 = 0 → 评论不可用） */
      durationMs: rs.durationMs,
      outWidth: openRes.outWidth,
      outHeight: openRes.outHeight,
      /* 键盘避让恢复用：open 时下发给 native 的物理矩形（= open 参数里的 x/y/width/height）。
       * 注意坐标系：x 是「视频逻辑区左缘」，**不含**面板偏移 —— native 在
       * build_pipeline 与 PB2_CMD_SET_RECT 里统一加当前会话的 panelOffset
       * （X6 Pro=107、X7 Pro=100，由 device_profile.js 下发）。
       * 所以这里原样回传给 setVideoRect 即可，不要在 JS 侧再加偏移。
       * X6 Pro 真机实测：orig.x=0 → render-rectangle 是 <107,299,240,427>。 */
      rectPhysical: rects.physical,
      deviceProfile: profile.name
    }
  };
}

// playing=true → 暂停；false → 继续 → {ok, state} | {ok:false, message}
export async function togglePlay(ctx, playing) {
  const r = playing ? await ctx.player.pause() : await ctx.player.resume();
  if (!r || r.ok === false) {
    const detail = r ? [r.code, r.error].filter(Boolean).join(': ') : '';
    return { ok: false, message: detail || '控制失败' };
  }
  return { ok: true, state: r.state };
}

// 绝对定位（P1-7 修复）：直接给目标毫秒，**不用** positionMs 算 delta。
//
// 旧实现是 seekBy(positionMs, dur, goal - positionMs)，而 positionMs 来自每秒一次的
// 轮询快照，最多旧 1s → 落点系统性偏 ±1000ms（defect-inventory P1-7）。拖进度条本质
// 是"我要跳到 X"，没有任何理由绕一圈减法。
//
// 钳制 [0, dur-1s]：越界 seek 会让 native 侧 gst 立刻 EOS（ADR §6.3 边界），
// 所以上界必须留 1s 余量；时长未知（直播）时不设上界。
// → {ok, target} | {ok:false, message}
export async function seekTo(ctx, targetMs, durationMs) {
  const dur = Number(durationMs) || 0;
  let target = Math.floor(Number(targetMs) || 0);
  if (target < 0) target = 0;
  if (dur > 0 && target > dur - 1000) target = Math.max(0, dur - 1000);
  const r = await ctx.player.seek(target);
  if (!r || r.ok === false) {
    const detail = r ? [r.code, r.error].filter(Boolean).join(': ') : '';
    return { ok: false, message: detail || '定位失败' };
  }
  /* v2：seek 是覆盖式的（native 侧 debounce），positionMs 可能还是旧落点 —— 有
   * seekTargetMs 就以它为准，否则回落到 native 给的 positionMs。
   * 注意 r.ok===true 只代表"请求已受理"，不代表首帧已上屏；页面不该等它。 */
  const settled = Number(r.seekTargetMs);
  return {
    ok: true,
    positionMs: settled > 0 ? settled : Number(r.positionMs) || 0,
    target: target,
    pending: !!r.seekPending
  };
}

// ±相对定位（按钮 ±20s 用），钳制规则同 seekTo → {ok, positionMs, target} | {ok:false, message}
export async function seekBy(ctx, positionMs, durationMs, deltaMs) {
  let target = (Number(positionMs) || 0) + (Number(deltaMs) || 0);
  if (target < 0) target = 0;
  const dur = Number(durationMs) || 0;
  if (dur > 0 && target > dur - 1000) target = Math.max(0, dur - 1000);
  const r = await ctx.player.seek(target);
  if (!r || r.ok === false) {
    const detail = r ? [r.code, r.error].filter(Boolean).join(': ') : '';
    return { ok: false, message: detail || '定位失败' };
  }
  const settled = Number(r.seekTargetMs);
  return { ok: true, positionMs: settled > 0 ? settled : Number(r.positionMs) || 0, target: target };
}

// 原地自愈（任务 D）：优先 native recover()，不支持时返回 {ok:false, code:'PLAYER_UNSUPPORTED'}。
// 页面据此决定是"重试一次 seek(pos)"还是"只提示不动手"——绝不再用 onSeek(0.001) 全量重启。
export async function recoverSession(ctx) {
  if (!ctx || !ctx.player || typeof ctx.player.recover !== 'function') {
    return { ok: false, code: 'PLAYER_UNSUPPORTED', message: '当前播放器实现不支持原地恢复' };
  }
  try {
    const r = await ctx.player.recover();
    if (!r || r.ok === false) {
      const detail = r ? [r.code, r.error].filter(Boolean).join(': ') : '';
      return { ok: false, code: (r && r.code) || 'PLAYER_RECOVER_FAILED', message: detail || '原地恢复失败' };
    }
    return { ok: true, state: r.state };
  } catch (e) {
    return { ok: false, code: 'PLAYER_RECOVER_FAILED', message: (e && e.message) || '原地恢复失败' };
  }
}

// → {ok, state, frames, positionMs, audioBytes, audioDropped, audioUnderruns, audioWrErrors, audioRingDrops}
// | {ok:false, state:'error', message}
// 音频卡顿定位证据链（v1.3.0 环形推送）：
//   ab 增速断+u=0+画面冻=ffmpeg供给断；rd>0=环满(BT持续慢)；u 持续涨=欠载等待；w=aplay写失败；ad=写错误丢字节
export async function readStatus(ctx) {
  const st = await ctx.player.status();
  const zero = {
    audioBytes: 0,
    audioDropped: 0,
    audioUnderruns: 0,
    audioWrErrors: 0,
    audioRingDrops: 0
  };
  if (!st || st.ok === false) {
    return Object.assign(
      { ok: false, state: 'error', message: (st && (st.error || st.code)) || '状态读取失败', frames: 0, positionMs: 0 },
      zero
    );
  }
  if (st.state === 'error') {
    return Object.assign(
      {
        ok: false,
        state: 'error',
        message: st.error || '播放错误',
        frames: st.frames || 0,
        positionMs: st.positionMs || 0
      },
      {
        audioBytes: Number(st.audioBytes) || 0,
        audioDropped: Number(st.audioDropped) || 0,
        audioUnderruns: Number(st.audioUnderruns) || 0,
        audioWrErrors: Number(st.audioWrErrors) || 0,
        audioRingDrops: Number(st.audioRingDrops) || 0,
        audioDead: !!st.audioDead
      }
    );
  }
  return {
    ok: true,
    state: normalizeState(st.state, ctx),
    frames: num(st.frames, 0),
    positionMs: num(st.positionMs, 0),
    audioBytes: num(st.audioBytes, 0),
    audioDropped: num(st.audioDropped, 0),
    audioUnderruns: num(st.audioUnderruns, 0),
    audioWrErrors: num(st.audioWrErrors, 0),
    audioRingDrops: num(st.audioRingDrops, 0),
    audioDead: !!st.audioDead,
    /* A/V 巡检（v1.7.1）：playing 态距最近帧的毫秒（>8000=视频停帧）；gateWaitMs=起播门时长；
     * gateActive=门进行中（此时巡检让位，防弱网首帧期被误判打断）。
     *
     * videoStallMs 必须过 num()：native 在"从未出过一帧"时返回哨兵 **-1**
     * （见 native/csrc/jsapi_player2.c 的 `long long stall = -1;` 与
     * `mk_int(ctx, "videoStallMs", stall, res);`），而 -1 是真值，
     * 旧写法 `Number(-1) || 0` 兜不住它 → -1 直通页面看门狗的
     * `videoStallMs > 8000`（恒 false）→ 看门狗彻底失效 → 界面永久"加载中…"。
     * 归一化后 -1 → 0，真正的停帧（>8000 的正数）照样透传。 */
    videoStallMs: num(st.videoStallMs, 0),
    gateWaitMs: num(st.gateWaitMs, 0),
    gateActive: !!st.gateActive,
    /* v3.5.1 起播时序：声音比画面首帧晚多少毫秒才开写（-1=未测到）。
     * 这是「音画载入不同时」的直接量化指标，用于验证定格门是否真的把两者对齐。
     * **-1 是有意义的**（页面用 `>= 0` 判断"是否已经测到"）→ 走 numLatency 保留负值。 */
    audioLateMs: numLatency(st.audioLateMs),
    /* v3.5.1：声音比「画面可见」（KMS 平面 ACTIVE）晚多少毫秒。这才是用户感知的
     * 音画不同步量；audioLateMs 含 gst 启动耗时，会虚高 ~1.2s。同样保留 -1 语义。 */
    audioLateVisibleMs: numLatency(st.audioLateVisibleMs),

    /* v1.8.0 音画对齐诊断：videoSkips=丢帧快进计数（追音频钟）；avDriftMs=可闻音频位置−画面位置
     * （正=声音超前，**负=声音滞后** → 必须保留负号，否则诊断瞎掉一半）
     * v1.9.0：audioBufMs=可闻锚修正量（aplay 管道+ALSA 缓冲，恒定即正常，也可能为负）
     * v1.9.3：videoRestarts=视频断流重启次数（>0=发生过 CDN 断流，表现为短暂追赶后恢复） */
    videoSkips: num(st.videoSkips, 0),
    avDriftMs: numSigned(st.avDriftMs, 0),
    audioBufMs: numSigned(st.audioBufMs, 0),
    videoRestarts: num(st.videoRestarts, 0),
    /* v2.1.0 音画同步分层：resyncing=视频流正在强制重同步（首帧未产出）→ 页面显示"加载中…"；
     * resyncCount=漂移触发的强制重同步次数；avDriftNowMs=native 侧最近漂移（诊断，可为负）；
     * resyncLeadMs=重同步前移量（诊断）。
     *
     * resyncing 的"字段缺失"必须读作 false（= 不存在重同步这回事）：老 native 没有这个
     * 字段时，页面看门狗的让位条件 `!st.resyncing` 会成立 → 看门狗照常工作。
     * 反过来若把缺失当 true，看门狗就永久裸奔了（P0-1 第 4 条）。 */
    resyncing: st.resyncing === true,
    resyncCount: num(st.resyncCount, 0),
    avDriftNowMs: numSigned(st.avDriftNowMs, 0),
    resyncLeadMs: numSigned(st.resyncLeadMs, 0),
    /* v2.2.4：本次重同步已持续毫秒（未重同步为 0）——页面据此只在"恢复确实慢"时才弹「加载中」 */
    resyncingMs: num(st.resyncingMs, 0),

    /* ---- player2 新字段透传（缺失一律给安全默认值，v1 下恒为默认值）----
     * seekPending/seekTargetMs：覆盖式 seek 的进行态。拖动进度条时页面据此知道
     *   "我下发的落点还没生效"，避免用旧 positionMs 回写 UI 造成进度条回跳。
     * planeActive：KMS 显示平面是否 ACTIVE（false = 画面没在屏上）。
     * pipelineAlive：native 管线进程是否存活（false = 需要 recover/open）。
     * durationMs/fps/hasAudio：native 侧实测值（页面可用它校正 session 里的估算）。 */
    seekPending: st.seekPending === true,
    seekTargetMs: num(st.seekTargetMs, 0),
    planeActive: st.planeActive === undefined ? true : !!st.planeActive,
    pipelineAlive: st.pipelineAlive === undefined ? true : !!st.pipelineAlive,
    nativeDurationMs: num(st.durationMs, 0),
    /* durationMs：native 侧时长的**同名别名**（n10）。
     * 起因：页面 tryRecover() 写的是 `Number(st && st.durationMs) || 0`，而这里只导出
     * nativeDurationMs → 取到 undefined → 恒 0 → seekBy 不设上界，播完还往前 seek。
     * 保留 nativeDurationMs 是为了不破坏既有单测与调用点，两个字段同值。 */
    durationMs: num(st.durationMs, 0),
    nativeFps: num(st.fps, 0),
    hasAudio: st.hasAudio === undefined ? true : !!st.hasAudio,
    /* lastError 是 native 的 errno，真机见过 -5（EIO）→ 走 numSigned 保留负号 */
    lastError: numSigned(st.lastError, 0),
    lastErrorMsg: typeof st.lastErrorMsg === 'string' ? st.lastErrorMsg : '',
    /* muted/mutedBy：全局静音开关（player2 新增）。老 native 没有该字段 → 按"未静音"
     * 处理，不能因为字段缺失就在界面上给人打一个「已静音」角标。 */
    muted: st.muted === true,
    mutedBy: typeof st.mutedBy === 'string' ? st.mutedBy : '',
    /* screenOff：屏幕是否真的黑了（player2 读 /sys/class/backlight/*\/bl_power 判定，
     * 比 brightness JSAPI 的 getScreenState() 可靠 —— 后者把"调暗 DIM=3"也算非亮屏）。
     * 三态：true=熄屏/背光断电、false=亮、null=旧 native 没有该字段（页面回退到 JSAPI）。 */
    screenOff: st.screenOff === undefined ? null : !!st.screenOff
  };
}

/* ================= seek 落点等待（探针 + 页面共用的判据） =================
 *
 * 为什么不能"发完就查一次"：player2 的 seek 是**覆盖式 debounce**
 * （native/csrc/session/session.c 的 PB2_SEEK_DEBOUNCE_MS=250）——
 *   · js_seek() 只把目标塞进队列并立刻返回（JS 零阻塞）；
 *   · worker 要等最后一次 seek 之后满 250ms 才真正执行 do_seek()；
 *   · do_seek() 内部还要「起新 ffmpeg(-ss) → 拆旧管线 → 重建 → preroll」，
 *     真机实测 seek(restart) done total≈505ms，到首帧约 550~620ms。
 * 所以「落点到了没有」必须轮询等，而且必须同时满足两个条件：
 *
 *   1) settled —— 观察到 seekPending 从 true 变回 false。do_seek() 收尾时
 *      才 pb2_stats_patch_seek(&stats, 0, clamped)，这一步是"命令确实被 native
 *      执行过"的唯一证据。只看位置会被"目标离当前位置很近"骗过去（假阳性）。
 *      老 native（v1）没有 seekPending 字段 → 读作 false，等价于不设这道门。
 *   2) 落点 |positionMs - target| <= near。do_seek 收尾时会把 positionMs
 *      直接 patch 成 clamped（ffmpeg -ss 的请求值），且 base_ms 也设为 clamped，
 *      之后 refresh_position 报 base_ms + 播放前进量 → 关键帧对齐不会让落点
 *      回退到 target 之前，drift 恒 >= 0。
 *
 * 之前探针用 `frames > f0` 判完成是彻底假阳性：正常播放每帧都让 frames+1，
 * 判据永远立刻满足，于是 N 个 seek 全挤进同一个 debounce 窗口，前 N-1 个
 * 根本没执行（真机 pb2.log 里连一条 `SEEK target=` 都没有）。
 *
 * 依赖全部可注入（read/sleep/now），便于单测用假序列跑完四种分支，
 * 不必真等 8 秒、也不依赖系统时钟。
 * ========================================================================= */
export const SEEK_LAND_NEAR_MS = 5000; // 落点容差：关键帧对齐 + base_ms 对齐的余量
export const SEEK_LAND_DEADLINE_MS = 8000; // 落点等待上限（实测 550~620ms，约 13 倍余量）
export const SEEK_LAND_POLL_MS = 100; // 采样间隔
export const SEEK_FRAME_DEADLINE_MS = 3000; // 首帧软上限（native 预算 PB2_SEEK_TARGET_MS=800ms）
/* 源秒退自愈（check_first_frame -> retry）需要 teardown + 重新拉起 ffmpeg（网络重连 1~2s）。
 * probe 若仍用 3s 软上限，会在重试完成前判 TIMEOUT 并 onBack() 拆会话，把飞行中的重试杀掉
 * （真机 20:43 复现：retry 1 @31.7s，shutdown done @34.1s）。探针场景放宽到 9s，
 * 正常播放路径（waitFirstFrame 的默认 3s）不受影响。 */
/* 【首次 seek 必失败自愈】native 侧已加自动重试：首帧 6s 不出就重走一次 seek，
 * 重试本身再需 ~1.5s（含关键帧探测 0.6s + 首帧 0.9s），所以探针必须等到 12s
 * 才能把自愈后的真实首帧计入；不放宽的话探针会在自愈完成前就报 TIMEOUT。 */
export const SEEK_FRAME_RETRY_DEADLINE_MS = 12000;

/* =========================================================================
 * 「定位中…」何时可以收起（页面 tick 的 seek 段判据）
 *
 * 历史 bug（真机 23:12:51 复现，自检 seek+20s 报 5023→6039）：
 *   旧判据只看 `st.frames > frames0`。seek 下发后约 200ms 内 **旧管线仍在出帧**
 *   （native 的 seek debounce 还没到点、do_seek 还没开始），帧数一涨就被误判成
 *   "新管线首帧已到" → `_seekNoteAt` 清零 → 下一轮 tick 用 native 的**旧**位置
 *   覆盖页面刚写下的落点。用户看到的是"拖完往左弹一下"，看门狗也会提前解除让位。
 *
 * 正确判据（与 waitLanded 同一套语义）：
 *   1) `st.seekPending` 必须已落回 false —— 只有它变 false 才证明 do_seek 真的跑完；
 *   2) 且（帧数已越过下发前的基线 **或** 位置已靠近落点）。
 *      为什么要有"位置靠近"这一条：do_seek 收尾会 pb2_probe_reset() 把 frames 归零，
 *      重建管线后累计帧数可能长期小于 seek 前的值，光看帧数会等到超时。
 *   老 native（v1）没有 seekPending 字段 → 读作 false，等价于不设这道门。
 *
 * 纯函数，不依赖时钟，便于单测。
 * ========================================================================= */
export function seekNoteDone(st, frames0, targetMs, near, sawPending) {
  if (!st || typeof st !== 'object') return false;
  /* native 明确还在 seek 中 → 一律不算完成。 */
  if (st.seekPending === true) return false;

  const f0 = Number(frames0) || 0;
  const frames = Number(st.frames) || 0;
  const grew = frames > f0;

  const tol = Number(near) > 0 ? Number(near) : SEEK_LAND_NEAR_MS;
  const pos = Number(st.positionMs);
  const target = Number(targetMs);
  /* 落点是否已靠近。target<=0 表示「这条 seek 没有明确落点」（例如按 ±20s 时
   * native 回的是相对目标解析后的值，万一拿不到就退回只看帧数）。 */
  const nearTarget =
    isFinite(pos) && isFinite(target) && target > 0 && Math.abs(pos - target) <= tol;

  /* A. 没见到 pending 也能判完成的情况：帧数涨了 **且** 位置已在落点附近。
   *    为什么这两个必须同时成立：seek 下发后约 200ms 内旧管线仍在出帧（帧数会涨），
   *    但那时 positionMs 还是 seek 前的旧位置 —— 单独看帧数就是旧 bug。
   *    轮询 1s 一次、do_seek 只要 600~1400ms，完全可能整段 pending 落在两次采样之间，
   *    所以不能只依赖 sawPending。
   *    落点未知（target<=0，即 seekBy 还没返回）时**不算完成** —— 那种窗口里
   *    old 帧数增长 + 落点未知，正好是最容易误判的组合。 */
  if (target > 0 && grew && nearTarget) return true;

  /* B. 亲眼见过 pending=true，且它已落回 false → do_seek 真的跑完了。
   *    此时只要帧数已越过基线、或位置已靠近落点，就算完成。
   *    （do_seek 收尾会 pb2_probe_reset() 把 frames 归零，重建后的累计帧数可能长期
   *      小于 seek 前的值，所以不能只要求 grew。） */
  if (sawPending === true && (grew || nearTarget)) return true;

  return false;
}

function defaultSleep(ms) {
  return new Promise(function (resolve) {
    setTimeout(resolve, ms);
  });
}

function defaultNow() {
  return Date.now();
}

function pickMs(v, dflt) {
  return typeof v === 'number' && isFinite(v) && v > 0 ? v : dflt;
}

// 等落点 → {ok, landing, drift, waitedMs, samples, sawPending, lastPos, frames, error}
//   ok=true   : landing=实测位置，drift=landing-target（可正可负，正常 0~数百 ms）
//   ok=false  : error='TIMEOUT' | 'NO_FRAME'? | 播放错误原文；drift=null（调用方打 n/a，
//               不要打 0ms —— 没落点就没有偏差可言）
// opts: {near, deadlineMs, pollMs, requireSettled(默认 true), sleep, now}
export async function waitLanded(read, targetMs, opts) {
  const o = opts || {};
  const target = Math.floor(Number(targetMs) || 0);
  const near = pickMs(o.near, SEEK_LAND_NEAR_MS);
  const deadlineMs = pickMs(o.deadlineMs, SEEK_LAND_DEADLINE_MS);
  const pollMs = pickMs(o.pollMs, SEEK_LAND_POLL_MS);
  const requireSettled = o.requireSettled !== false;
  const failOnNewError = o.failOnNewError === true;
  const sleep = typeof o.sleep === 'function' ? o.sleep : defaultSleep;
  const now = typeof o.now === 'function' ? o.now : defaultNow;
  const t0 = now();
  let samples = 0;
  let sawPending = false;
  let pendingAt = -1;
  let pos = 0;
  let frames = 0;
  let lastErr = '';
  let baseErr = null; // null = 还没采到第一帧基线
  const pendingMs = function () {
    return pendingAt >= 0 ? now() - pendingAt : -1;
  };
  for (;;) {
    await sleep(pollMs);
    samples++;
    let st = null;
    try {
      st = await read();
    } catch (e) {
      lastErr = (e && e.message) || String(e);
      st = null;
    }
    if (st && st.ok === false) {
      /* 真错误态（native 报 error / status 读失败）：立刻失败并带上原因，
       * 不要空转满 deadline 才打一个什么都没说明的 TIMEOUT。 */
      if (st.state === 'error') {
        return {
          ok: false,
          error: st.message || '播放错误',
          landing: 0,
          drift: null,
          waitedMs: now() - t0,
          samples: samples,
          sawPending: sawPending,
          pendingMs: pendingMs(),
          lastPos: pos,
          frames: frames
        };
      }
    } else if (st) {
      pos = num(st.positionMs, 0);
      frames = num(st.frames, 0);
      /* native 的「seek 没落点」不会把 state 置 error，只 patch_error(PB2_E_SEEK=10,
       * "seek did not land") 并保留真实位置（session.c 的 P0-3 修复）。这种情形下
       * 光等位置永远不会到位，只能靠 lastError 变化识别 —— 基线取第一次采样的值，
       * 之后变了就说明这次 seek 报了新错。默认关闭（保持纯函数语义稳定），
       * 探针/页面可以按需打开。 */
      if (failOnNewError) {
        const le = numSigned(st.lastError, 0);
        const msg = typeof st.lastErrorMsg === 'string' ? st.lastErrorMsg : '';
        if (baseErr === null) baseErr = { code: le, msg: msg };
        /* 只把「新出现的**非零**错误」当失败。为什么必须排除 0：
         * native 的自愈成功路径会 patch_error(PB2_E_NONE(0), "") 把错误清空
         * （session.c 的 self-heal / open 收尾），那是"变好了"，不是"失败了"。
         * 若把清零也当变化，会把一次成功的自愈 seek 误判成失败。 */
        else if (le !== 0 && (le !== baseErr.code || msg !== baseErr.msg)) {
          return {
            ok: false,
            error: 'SEEK ' + (msg || 'native 报错 errno=' + le) + '（落点 ' + pos + 'ms，目标 ' + target + 'ms）',
            landing: 0,
            drift: null,
            waitedMs: now() - t0,
            samples: samples,
            sawPending: sawPending,
            pendingMs: pendingMs(),
            lastPos: pos,
            frames: frames
          };
        }
      }
      if (st.seekPending === true) {
        if (!sawPending) {
          sawPending = true;
          pendingAt = now();
        }
      }
      const settled = !requireSettled || st.seekPending !== true;
      const onTarget = (pos > 0 || target === 0) && Math.abs(pos - target) <= near;
      if (settled && onTarget) {
        return {
          ok: true,
          error: '',
          landing: pos,
          drift: pos - target,
          waitedMs: now() - t0,
          samples: samples,
          sawPending: sawPending,
          /* 从"第一次看到 pending"到"看到落点"的时长 = native 侧 debounce+do_seek 的实耗，
           * 这才是排查 seek 慢的可用数字；waitedMs 含前面的轮询粒度，不是它。 */
          pendingMs: pendingMs(),
          lastPos: pos,
          frames: frames
        };
      }
    }
    if (now() - t0 >= deadlineMs) break;
  }
  return {
    ok: false,
    error: lastErr ? 'READ ' + lastErr : 'TIMEOUT',
    landing: 0,
    drift: null,
    waitedMs: now() - t0,
    samples: samples,
    sawPending: sawPending,
    pendingMs: pendingMs(),
    lastPos: pos,
    frames: frames
  };
}

// 等首帧 → {ok, frameMs, frames, samples, error}
// seek 后 native 会把 frames 归零（core/queue.c 的 pb2_probe_reset），所以
// frames>0 第一次为真 = 新管线真的出帧了，这才是"到首帧耗时"的真值。
// 软判据：超时也算一种结果（frameMs=-1），不否定落点结论。
export async function waitFirstFrame(read, opts) {
  const o = opts || {};
  const deadlineMs = pickMs(o.deadlineMs, SEEK_FRAME_DEADLINE_MS);
  const pollMs = pickMs(o.pollMs, SEEK_LAND_POLL_MS);
  const sleep = typeof o.sleep === 'function' ? o.sleep : defaultSleep;
  const now = typeof o.now === 'function' ? o.now : defaultNow;
  const t0 = now();
  let samples = 0;
  let frames = 0;
  let firstErr = '';
  for (;;) {
    await sleep(pollMs);
    samples++;
    let st = null;
    try {
      st = await read();
    } catch (e) {
      st = null;
    }
    /*
     * 【容忍自愈中间态】native 在「首次 seek 不出首帧」时会
     * 自动重试一次（2s 内无 video buffer 就重试）。重试期间
     * `state` 可能一直是 error，但重试完成后会回到 playing 并出帧。
     * 旧写法一看到 error 就立即返回失败，于是永远测不到自愈后的首帧
     * （真机实测：native 已经 `SEEK first frame +1032ms OK`，JS 仍报 TIMEOUT）。
     * 现在改成：记下第一个错误但**继续等**，直到 deadline；
     * 期间一旦出帧就算成功。真失败（重试也救不回）会在 deadline 后报错。
     */
    if (st && st.ok === false && st.state === 'error') {
      if (!firstErr) firstErr = st.message || '播放错误';
    }
    if (st && st.ok !== false) {
      frames = num(st.frames, 0);
      if (frames > 0) {
        return { ok: true, error: '', frameMs: now() - t0, frames: frames, samples: samples };
      }
    }
    if (now() - t0 >= deadlineMs) break;
  }
  return { ok: false, error: firstErr || 'NO_FRAME', frameMs: -1, frames: frames, samples: samples };
}

// 幂等停止 + 释放 fb（stop/release 自身幂等）
export async function closeSession(ctx) {
  try {
    await ctx.player.stop();
    await ctx.player.release();
  } catch (e) {
    if (ctx.log) ctx.log('[bili] closeSession 异常: ' + (e && e.message));
  }
  return { ok: true };
}

// 错误码/阶段 → 用户一行话
export function describePlayError(res) {
  if (!res || res.ok) return '';
  if (res.stage === 'cancel') return '';
  // 直播未开播：接口明确给了 live_status != 1，直接给用户一行话
  if (res.stage === 'offline') return res.message || '主播未开播';
  const stageText = {
    param: '参数错误',
    view: '稿件信息失败',
    wbi: '签名密钥失败',
    playurl: '取流失败',
    transport: '网络失败',
    http: 'HTTP ' + (res.status || ''),
    parse: '响应解析失败',
    open: '播放器启动失败'
  }[res.stage] || '失败';
  return stageText + (res.message ? '：' + res.message : '');
}
