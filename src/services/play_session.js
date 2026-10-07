// 播放会话编排（可注入、可取消；页面只消费稳定接口）
//
// 视频链：**无条件 view**（权威 cid/时长/标题/显示宽高）→ WBI 密钥 → playurl html5 qn 阶梯
//         → player.open(按真实宽高信箱适配的居中矩形 + UA/Referer)。
// 直播链：搜索 kind=live → 服务端 /live?...（转码，见 server/live_proxy.py v5 HLS）→
//         302 → index.m3u8 → 同一个 player.open（单输入，durationMs=0）。
// 控制：toggle / seekBy(±20s) / readStatus / closeSession，全部幂等、结构化错误。
// 直播技术留档：getRoomPlayInfo 唯一 avc 档是 720p，本机软解 0.76x 不实时 → **必须经服务端转码**；
//         服务端 v5 以 HLS 1s 分片回传，笔端实测 480p30 解 1.2x 余量（2026-10-07 真机）。
import { DASH_QN_LADDER, QN_LADDER, HTML5_FPS, buildPlayurlQuery, parseHtml5Response, parseDashResponse, validateStreamUrl } from './bili/playurl.js';
import { deriveMixinKey } from './bili/wbi.js';
import { playVideoRects } from './screen.js';

export const SEEK_STEP_MS = 20000;

// 解码预算（v2.6.0）：实测 640×360(230K px) 软解吞吐 1.1~1.9x（4×A53，无弹幕）；2 倍像素即
// CPU 100% 吞吐 93%（2026-10-01 soak 实证）。预算卡 310K（≈640×480 / 480×640 竖屏），超出 → durl。
export const MAX_DECODE_PIXELS = 310000;

// ---- 直播（v2.9.0）：服务端以 HLS 分片回传，笔端只把 /live?... 当**点播地址**消费 ----
//   链路：搜索 kind=live → http://<addr>/live?room&ck&res&bv&trans&buf → 302 →
//         .../hls/<sid>/index.m3u8（ffmpeg 自动跟随 302；终点 .m3u8 → hls demuxer）；
//         单输入（input2=''）走 durl/直播那条 native 路径。
//   durationMs=0：native 以"时长未知"处理 → 不判播完、seek 无上界（正是直播想要的语义）。
//   地址取自「我的 → 直播设置」；未填时这里直接给出可读错误，页面据此提示。
//   （真机实证：笔端 ffmpeg 4.4 支持 hls demuxer + hls/http 协议；480p30 实时解码 ~1.2x 余量）
export const LIVE_FPS = 30; /* 服务端 HLS 输出固定 30fps（-vf fps=30 与 -g 30 对齐分片） */
export const LIVE_VIDEO_RECT = { width: 640, height: 360 }; /* 服务端 scale=-2:480 的 16:9 输出 */

// 纯函数（可测）：服务端 /live 地址。addr 为空 / room 非法 → ''
export function buildLiveUrl(addr, roomid, opts) {
  const a = String(addr == null ? '' : addr).trim().replace(/\/+$/, '');
  const room = Math.floor(Number(roomid) || 0);
  if (!a || !(room > 0)) return '';
  const o = opts || {};
  const buf = Math.max(2000, Math.min(20000, Math.floor(Number(o.bufMs) || 6000)));
  const res = [360, 480, 540].indexOf(Number(o.res)) >= 0 ? Number(o.res) : 480;
  const bv = /^\d+[km]?$/i.test(String(o.bv || '')) ? String(o.bv).toLowerCase() : '700k';
  const q = ['room=' + room, 'buf=' + buf, 'res=' + res, 'bv=' + bv, 'trans=' + (o.trans === 0 ? 0 : 1)];
  if (o.ck) q.push('ck=' + encodeURIComponent(String(o.ck)));
  return a + '/live?' + q.join('&');
}

// → {ok, live:true, url, audioUrl:'', qn:0, cid, roomid, aid:0, durationMs:0, title, width, height, host, fps}
export function resolveLiveUrl(ctx, item) {
  const cfg = (ctx && ctx.live) || {};
  const addr = String(cfg.addr == null ? '' : cfg.addr).trim().replace(/\/+$/, '');
  const roomid = Number(item && item.roomid) || 0;
  if (!(roomid > 0)) return { ok: false, stage: 'param', message: '直播间号无效' };
  if (!addr) return { ok: false, stage: 'liveaddr', message: '未填写转码服务器地址' };
  const url = buildLiveUrl(addr, roomid, cfg);
  if (!url) return { ok: false, stage: 'liveaddr', message: '转码服务器地址不合法' };
  return {
    ok: true,
    live: true,
    url: url,
    audioUrl: '',
    qn: 0,
    cid: roomid,
    roomid: roomid,
    aid: 0, /* 直播不接评论（reply 无 oid） */
    durationMs: 0, /* 时长未知：不判播完、seek 不设上界 */
    title: (item && item.title) || '',
    width: LIVE_VIDEO_RECT.width,
    height: LIVE_VIDEO_RECT.height,
    host: addr,
    fps: LIVE_FPS
  };
}

// WBI 密钥会话级缓存（约每日轮换，单次 app 会话内复用安全）
export async function ensureMixin(ctx) {
  if (ctx.mixinMemo && ctx.mixinMemo.key) return ctx.mixinMemo.key;
  const r = await ctx.client.fetchWbiKeys();
  if (!r.ok) return null;
  const key = deriveMixinKey(r.imgKey, r.subKey);
  if (!/^[0-9a-f]{32}$/.test(key)) return null;
  if (ctx.mixinMemo) {
    ctx.mixinMemo.key = key;
    ctx.mixinMemo.at = Date.now();
  }
  return key;
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
  // DASH 阶梯 [6,16]：先试 240p（解码像素减半，254px 屏上只需 6% 上采样），没有则 360p。
  // 最后一档允许非 avc1 兜底（保住"能播"）；前面的档位要求 avc1，避免退到 HEVC（软解解不动）。
  const lastIdx = DASH_QN_LADDER.length - 1;
  for (let i = 0; i < DASH_QN_LADDER.length; i++) {
    const qn = DASH_QN_LADDER[i];
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
    // 解码预算硬校验（v2.6.0）：实测 230K 像素(360p) 软解吞吐 1.1~1.9x；部分稿件 rendition
    // 实为 720p/1080p（High@L5.1，真机 CPU 100% 吞吐仅 93% 实证）→ 按实际像素拒载，
    // 落到 durl 真 360p 单文件路径（DASH_QN_LADDER 走完后自然回退）。
    const px = (parsed.width || 0) * (parsed.height || 0);
    if (px > MAX_DECODE_PIXELS) {
      last = { stage: 'playurl', message: 'DASH 分辨率超解码预算' };
      if (ctx.log) {
        ctx.log('[bili] dash qn' + parsed.qn + ' ' + parsed.width + 'x' + parsed.height +
          ' (' + px + 'px > ' + MAX_DECODE_PIXELS + ') 超解码预算 → 回退 durl 360p');
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
  const openRes = await ctx.player.openSession({
    input: rs.url,
    input2: rs.audioUrl || '', // DASH 音频轨第二输入（空 = durl 回退 / 直播单流路径）
    startMs: 0,
    durationMs: rs.durationMs, // 直播 = 0（时长未知）
    fps: rs.fps || HTML5_FPS, // 直播 = 30（服务端 HLS 输出帧率）
    audio: true,
    transpose: 2,
    rect: rects.physical,
    audioDevice: '',
    userAgent: ctx.mediaUa,
    referer: ctx.mediaReferer
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
  // 直播未配服务器：给出用户指定的完整提示（页面直接用这一行）
  if (res.stage === 'liveaddr') return (res.message || '未填写转码服务器地址') + '，无法播放';
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
