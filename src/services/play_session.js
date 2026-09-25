// 播放会话编排（可注入、可取消；页面只消费稳定接口）
//
// 视频链：**无条件 view**（权威 cid/时长/标题/显示宽高）→ WBI 密钥 → playurl html5 qn 阶梯
//         → player.open(按真实宽高信箱适配的居中矩形 + UA/Referer)。
// 控制：toggle / seekBy(±20s) / readStatus / closeSession，全部幂等、结构化错误。
// 直播播放已从 UI 移除（2026-09-23 用户决策：直播 tab 换成视频搜索）。技术留档：
//       getRoomPlayInfo 实测唯一 avc 档 accept_qn=[10000,250]=720p（720x1280），
//       本机 720p 软解 0.76x 不实时（profile 基准）→ 即便恢复入口也不可播。
import { DASH_QN_LADDER, QN_LADDER, HTML5_FPS, buildPlayurlQuery, parseHtml5Response, parseDashResponse, validateStreamUrl } from './bili/playurl.js';
import { deriveMixinKey } from './bili/wbi.js';
import { playVideoRects } from './screen.js';

export const SEEK_STEP_MS = 20000;

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
  // DASH 只走 [16]=640x360（dash 的 id32 是 480p，A53 软解击穿实时线——见 DASH_QN_LADDER 注释）
  for (let i = 0; i < DASH_QN_LADDER.length; i++) {
    const qn = DASH_QN_LADDER[i];
    const built = buildPlayurlQuery({ bvid: item.bvid, cid: cid, qn: qn, fnval: 16 }, mixin, nowSec());
    if (!built.ok) return { ok: false, stage: 'param', message: built.message };
    const res = await ctx.client.fetchPlayurl(built.query);
    if (cancelled(ctx)) return { ok: false, stage: 'cancel', message: '已取消' };
    const parsed = parseDashResponse(res, qn);
    if (!parsed.ok) {
      last = { stage: parsed.stage === 'api' ? 'playurl' : parsed.stage, message: parsed.message };
      if (ctx.log) ctx.log('[bili] dash 尝试失败 qn' + qn + ': ' + parsed.message);
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
        '[bili] stream DASH qn=' + parsed.qn + ' vb=' + parsed.bandwidth + ' ' + parsed.codecs +
          ' ab=' + parsed.audioBandwidth + ' host=' + vv.host
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
    input2: rs.audioUrl || '', // DASH 音频轨第二输入（空 = durl 回退路径）
    startMs: 0,
    durationMs: rs.durationMs,
    fps: HTML5_FPS,
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
        (rs.audioUrl ? ' DASH' : ' durl')
    );
  }
  return {
    ok: true,
    session: {
      kind: 'video',
      bvid: item.bvid,
      cid: rs.cid,
      title: rs.title || item.title || '',
      up: item.up || '',
      cover: item.cover || '',
      qn: rs.qn,
      aid: rs.aid || 0, /* 评论 oid */
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
    gateActive: !!st.gateActive
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
