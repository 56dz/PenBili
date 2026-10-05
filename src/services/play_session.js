// 播放会话编排（可注入、可取消；页面只消费稳定接口）
//
// 视频链：**无条件 view**（权威 cid/时长/标题/显示宽高）→ WBI 密钥 → playurl html5 qn 阶梯
//         → player.open(按真实宽高信箱适配的居中矩形 + UA/Referer)。
// 控制：toggle / seekBy(±20s) / readStatus / closeSession，全部幂等、结构化错误。
// 直播播放已恢复（2026-09-26）：search kind=live → playUrl flv 直链 → 单输入复用 durl 双输出
//       （音视频同流 → 主 ffmpeg 双输出 → 既有 ring/aplay/A-V 门全链生效；aid=0 → 评论/稿件弹幕自动跳过）。
//       风险留档：playUrl 档位常回落 qn250 超清（accept=['4']，远高于稿件 646kbps 基准）→
//       高码率直播 A53 软解可能丢帧（音频钟消费自保：画面不丝滑但音频连续）；720p 0.76x 结论为参考上限。
import { DASH_QN_LADDER, QN_LADDER, HTML5_FPS, buildPlayurlQuery, parseHtml5Response, parseDashResponse, validateStreamUrl } from './bili/playurl.js';
import { deriveMixinKey } from './bili/wbi.js';
import { resolveLiveUrl } from './feed.js';
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
  // 直播（search kind=live）：flv 直链单输入 → native 识别为 durl 模式（双输出：视频pipe+音频pipe）
  if (item && item.kind === 'live') {
    const lv = await resolveLiveUrl(ctx.client, item.roomid);
    if (!lv.ok) return lv;
    /* 服务器转码（直播设置开启+地址有效）：flv 直链改走 live_proxy → 服务端转 360p 回供 */
    if (ctx.liveProxy && ctx.liveProxy.on && ctx.liveProxy.addr) {
      /* room+lanes：服务端融合（转码同时 drawtext 烧弹幕；lanes=档位映射 0/1/2/4） */
      const lanes = Math.max(0, Math.min(4, ctx.danmakuLanes || 0));
      lv.url = String(ctx.liveProxy.addr).replace(/\/+$/, '') +
        '/live?u=' + encodeURIComponent(lv.url) +
        '&room=' + (item.roomid || 0) + '&lanes=' + lanes;
      if (ctx.log) ctx.log('[bili] live → 转码代理 ' + ctx.liveProxy.addr + ' room=' + item.roomid + ' lanes=' + lanes);
    }
    if (ctx.log) ctx.log('[bili] live stream qn=' + lv.qn + ' ' + lv.url.slice(0, 56));
    return {
      ok: true,
      url: lv.url,
      audioUrl: '', /* 单输入 flv 自带音视频 → 走既有 durl 双输出路径 */
      live: true,
      qn: lv.qn,
      cid: 0,
      aid: 0,
      durationMs: 0, /* 直播无时长 → seek 自动禁止、timeText 显示"直播中" */
      roomid: item.roomid || 0, /* 直播弹幕代理 /danmaku?room= 依赖 */
      title: item.title || '直播间',
      width: 1280,
      height: 720, /* playUrl 不带分辨率 → 16:9 信箱（竖屏直播会拉伸，留档边界） */
      host: ''
    };
  }
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

  // —— 主路径：durl 单文件（fnval=1，html5）=【音画合并，2026-09-27 定案】
  //   音视频来自**同一条流、同一个 ffmpeg、同一条源时间轴**：门期两侧 content 同步推进、
  //   门末清环后同点起步 → 音画天然对齐。实测对照：直播(单流)对齐成立(仅轻微慢)；
  //   点播原走 DASH 双流=**两个独立 ffmpeg 进程**，解码速度不同 → 门末清环后两侧 content
  //   不等 →【一进来音频就快几秒】。单流同时带来一个隐藏好处：若 A53 解码吃紧，音视频
  //   会被同一条链一起拖慢（表现为卡顿）而不是"音频独跑、越播越歪"。
  //   档位 QN_LADDER=[32,16] = 640x360 AVC+AAC（与 DASH 限制的 360p 同级，A53 可实时）。
  let last = null;
  let stop = false;
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
      if (fatal(res)) { stop = true; break; } /* 版权/不存在：阶梯与 DASH 回退都无意义 */
      continue;
    }
    const v2 = validateStreamUrl(parsed.url);
    if (!v2.ok) {
      last = { stage: 'param', message: 'URL ' + v2.reason };
      if (ctx.log) ctx.log('[bili] playurl 尝试失败 qn' + qn + ': URL ' + v2.reason);
      continue;
    }
    if (ctx.log) ctx.log('[bili] stream durl(单流合并) qn=' + (parsed.quality || qn) + ' ' + v2.length + 'c host=' + v2.host);
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

  // —— 回退：DASH 双流（fnval=16）。**仅当单流取不到时使用**：两进程两时间轴，音画可能渐进漂移
  //   （原主路径，正是点播"音频快几秒"的来源）；音频轨独立 ~66kbps 的抗带宽优势在此让位于同步。
  if (!stop) {
    // DASH 只走 [16]=640x360（dash 的 id32 是 852x480，A53 软解击穿实时线——见 DASH_QN_LADDER 注释）
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
        if (fatal(res)) break; /* 版权/不存在：阶梯已无意义 */
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
          if (ctx.log) ctx.log('[bili] dash audio URL 弃用: ' + av.reason); /* 坏 URL → 降级无音轨 */
          audioUrl = '';
        }
      }
      if (ctx.log) {
        ctx.log(
          '[bili] stream DASH(回退) qn=' + parsed.qn + ' vb=' + parsed.bandwidth + ' ' + parsed.codecs +
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
    referer: ctx.mediaReferer,
    startBufMs: Math.max(0, Math.round(ctx.startBufMs || 0)) /* 直播设置"缓冲时间"（0=默认800ms） */
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
      live: !!rs.live, /* 直播会话：timeText/idleText 特化、评论按钮隐藏（aid=0） */
      roomid: rs.roomid || 0, /* 直播弹幕轮询的房间号 */
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
    audioRingDrops: 0,
    writerBytes: 0,
    audioRate: 0
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
    /* 音画同步探针（2026-09-27）：外放位置(aps=写入量/设备率) vs 视频位置的差值由 JS tick 打印 */
    writerBytes: Number(st.writerBytes) || 0,
    audioRate: Number(st.audioRate) || 0,
    paced: !!st.paced,
    audioStarted: !!st.audioStarted,
    dueMs: Number(st.dueMs) || 0,
    aposMs: Number(st.aposMs) || 0,
    thWaited: Number(st.thWaited) || 0
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
