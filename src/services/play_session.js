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
import { playVideoRects } from './screen.js';
import { dashLadderFor, videoFpsCapFor, liveFpsCapFor, liveQnFor, pixelBudgetFor } from './video_settings.js';

export const SEEK_STEP_MS = 20000;

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

/* 直播：→ {ok, live:true, url, audioUrl:url, qn, cid, roomid, aid:0, durationMs:0,
 *          title, width, height, host, fps} | {ok:false, stage, code?, message}
 *
 * 每次播放都重新取直链（URL 带 expires，过期即 403）——转码时代的「服务器地址 +
 * 解析缓存」已随 server/live_proxy.py 一起删除。
 * 音频走**同一条 URL 的独立进程**（player.c 的 spawn_ffaudio）：单进程串行写两个输出时
 * 视频 pipe 背压会连带音频停摆（v2.9.4 真机 30 分钟长跑定位，X5 时代的结论，机制不变）。
 * 实测（2026-10-29 X6 Pro）：同一 FLV 直链并发跑 2~3 个 ffmpeg 全部成功。 */
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
    audioUrl: r.url,
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
  // 矩形按**稿件真实显示宽高**做信箱适配（竖屏 → 物理竖柱，不再被拉伸成横屏）
  const rects = playVideoRects(rs.width, rs.height);
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
    transpose: 2,
    rect: rects.physical,
    /* '' = 自动：已连蓝牙耳机走 bluealsa，否则内置扬声器（player.c 的 detect_bt_pcm） */
    audioDevice: '',
    userAgent: ctx.mediaUa,
    referer: ctx.mediaReferer,
    fpsCap: fpsCap
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
      outHeight: openRes.outHeight
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

// ±定位，钳制到 [0, duration-1s]（时长未知时不设上界）→ {ok, positionMs} | {ok:false, message}
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
  return { ok: true, positionMs: Number(r.positionMs) || 0, target: target };
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
    state: st.state || 'idle',
    frames: Number(st.frames) || 0,
    positionMs: Number(st.positionMs) || 0,
    audioBytes: Number(st.audioBytes) || 0,
    audioDropped: Number(st.audioDropped) || 0,
    audioUnderruns: Number(st.audioUnderruns) || 0,
    audioWrErrors: Number(st.audioWrErrors) || 0,
    audioRingDrops: Number(st.audioRingDrops) || 0,
    audioDead: !!st.audioDead,
    /* A/V 巡检（v1.7.1）：playing 态距最近帧的毫秒（>8000=视频停帧）；gateWaitMs=起播门时长；
     * gateActive=门进行中（此时巡检让位，防弱网首帧期被误判打断） */
    videoStallMs: Number(st.videoStallMs) || 0,
    gateWaitMs: Number(st.gateWaitMs) || 0,
    gateActive: !!st.gateActive,
    /* v1.8.0 音画对齐诊断：videoSkips=丢帧快进计数（追音频钟）；avDriftMs=可闻音频位置−画面位置
     * （正=声音超前；修复后应稳定在 ±100ms 内，持续增长=软解吞吐贴实时线）
     * v1.9.0：audioBufMs=可闻锚修正量（aplay 管道+ALSA 缓冲，恒定即正常）
     * v1.9.3：videoRestarts=视频断流重启次数（>0=发生过 CDN 断流，表现为短暂追赶后恢复） */
    videoSkips: Number(st.videoSkips) || 0,
    avDriftMs: Number(st.avDriftMs) || 0,
    audioBufMs: Number(st.audioBufMs) || 0,
    videoRestarts: Number(st.videoRestarts) || 0,
    /* v2.1.0 音画同步分层：resyncing=视频流正在强制重同步（首帧未产出）→ 页面显示"加载中…"；
     * resyncCount=漂移触发的强制重同步次数；avDriftNowMs=native 侧最近漂移（诊断） */
    resyncing: !!st.resyncing,
    resyncCount: Number(st.resyncCount) || 0,
    avDriftNowMs: Number(st.avDriftNowMs) || 0,
    resyncLeadMs: Number(st.resyncLeadMs) || 0,
    /* v2.2.4：本次重同步已持续毫秒（未重同步为 0）——页面据此只在"恢复确实慢"时才弹「加载中」 */
    resyncingMs: Number(st.resyncingMs) || 0
  };
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
