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

// 解码预算（v2.6.0）：实测 640×360(230K px) 软解吞吐 1.1~1.9x（4×A53，无弹幕）；2 倍像素即
// CPU 100% 吞吐 93%（2026-10-01 soak 实证）。预算卡 310K（≈640×480 / 480×640 竖屏），超出 → durl。
export const MAX_DECODE_PIXELS = 310000;

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

// ============ 直播（2026-10-07 恢复入口）============
// 链路：搜索(kind=live) → GET /live?room=N（转码代理）→ player 单输入打开。
// 三个真机实测约束决定了这个形态：
//   ① URL 必须短：native `char input[1024]` + MAX_INPUT_LEN=1024；带 CDN `u=` 的代理 URL 实测
//      超 1000 字符会撞上限 → **只传 room**，由服务端 getRoomPlayInfo v2 自解析线路 —— 顺带
//      天然没有"CDN 直链过期"问题（重启/重连永远拿新鲜地址）。
//   ② 不可直连播：getRoomPlayInfo 唯一 avc 档 720p，软解 0.76x 不实时（见文件头技术留档）
//      → 必须经服务端转码成 360p（服务端已强制 yuv420p / 44100 / 30fps）。
//   ③ 不能做 playUrl 预检：离线房 playUrl 同样返回 code=0 + durl 有 URL（2026-10-07 实测）
//      → 开播判据交给转码服务（离线 → 服务端 403/无线路 → 开流验证失败）。
//   ④ 隧道偶发 502/断口（实测 15s 1152KB 0 断口、25s 分段偶发 >1s 断口与 502）
//      → openVideo 内做"等首帧 + 换地址重试"。
// 同网直连优先（真机实测笔=192.168.5.119 与服务器同段）：省隧道缓冲与首字节延迟——
// 外网域名作为回退（离网漫游时内网地址会快速失败后落到它）
export const LIVE_SERVERS = ['http://192.168.5.224:2050', 'http://penbili.560726.best'];

// 纯函数（可测）：直播代理地址；room-only ≈ 48 字符
export function buildLiveUrl(roomid, server) {
  const room = Math.floor(Number(roomid) || 0);
  if (!(room > 0)) return '';
  return String(server || LIVE_SERVERS[0]) + '/live?room=' + room + '&lanes=0';
}

// attempt=0..N 用于开流重试时轮换服务器（域名优先，失败切内网直连）
export async function resolveLiveUrl(ctx, item, attempt) {
  const roomid = Number(item && item.roomid) || 0;
  if (!(roomid > 0)) return { ok: false, stage: 'param', message: '直播间 id 无效' };
  const idx = Math.max(0, Math.floor(Number(attempt) || 0));
  const server = LIVE_SERVERS[idx % LIVE_SERVERS.length];
  return {
    ok: true,
    live: true,
    url: buildLiveUrl(roomid, server),
    audioUrl: '', /* 单输入 → native durl 路径（单进程）；live 无 stream-resync，掉流由 JS 重连 */
    fps: 30, /* 转码输出固定 30fps（-vf fps=30 / -framerate 30）；VOD 才用 HTML5_FPS=24 */
    qn: 0,
    cid: roomid,
    roomid: roomid,
    aid: 0, /* 直播不接评论（reply 无 oid） */
    durationMs: 0, /* 时长未知 → 不钳制定位、不判"播完"（结束判定在 JS 侧转为重连） */
    title: (item && item.title) || '',
    width: 640, /* 服务端 scale=-2:360 的 16:9 输出 → 信箱矩形按此算（竖屏直播罕见，暂不支持） */
    height: 360,
    host: server
  };
}

function liveDelay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

// 直播开流确认：openSession 只 spawn（403/502 也返回 ok=true），**必须等到首帧**才证明链路通。
// 返回 'ok' | 'error'（native 已判错，等下去没意义）| 'timeout' | 'cancel'
async function waitForFirstFrame(ctx, maxMs) {
  const t0 = Date.now();
  for (;;) {
    if (cancelled(ctx)) return 'cancel';
    const st = await readStatus(ctx);
    if (cancelled(ctx)) return 'cancel';
    if (st.ok && st.frames > 0) return 'ok';
    if (!st.ok) return 'error';
    if (Date.now() - t0 >= maxMs) return 'timeout';
    await liveDelay(300);
  }
}

// → {ok, url, audioUrl, qn, cid, durationMs, title, width, height, host} | {ok:false, stage, message}
export async function resolveVideoUrl(ctx, item) {
  if (item && item.kind === 'live') return resolveLiveUrl(ctx, item, 0);
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
  const isLive = !!(item && item.kind === 'live');
  /* 直播：开流重试（换地址）。VOD 保持原语义——open 即返回，首帧由 poll 的「加载中」呈现。 */
  const attempts = isLive ? LIVE_SERVERS.length : 1;
  let rs = null;
  let openRes = null;
  let lastErr = '';
  let verified = false;
  for (let i = 0; i < attempts; i++) {
    rs = isLive ? await resolveLiveUrl(ctx, item, i) : await resolveVideoUrl(ctx, item);
    if (!rs.ok) return rs;
    if (cancelled(ctx)) return { ok: false, stage: 'cancel', message: '已取消' };
    // 矩形按**稿件真实显示宽高**做信箱适配（竖屏 → 物理竖柱，不再被拉伸成横屏）
    const rects = playVideoRects(rs.width, rs.height);
    openRes = await ctx.player.openSession({
      input: rs.url,
      input2: rs.audioUrl || '', // DASH 音频轨第二输入（空 = durl/直播 单输入路径）
      startMs: 0,
      durationMs: rs.durationMs,
      fps: rs.fps || HTML5_FPS, // 直播=30（转码输出帧率），VOD=HTML5_FPS
      audio: true,
      transpose: 2,
      rect: rects.physical,
      audioDevice: '',
      userAgent: ctx.mediaUa,
      referer: ctx.mediaReferer
    });
    if (!openRes || openRes.ok !== true) {
      lastErr = openRes ? (openRes.code + ': ' + openRes.error) : '播放器无响应';
      if (ctx.log) ctx.log('[bili] openSession fail #' + (i + 1) + ': ' + lastErr);
      if (i + 1 < attempts) {
        await liveDelay(1500);
        continue;
      }
      break;
    }
    if (!isLive) {
      verified = true;
      break;
    }
    /* 直播等首帧：403/502/隧道断/上游停顿在这里才暴露（openSession 恒 ok）→ 失败换下一个地址重开。
     * 窗口 20s：真机实测冷启动（上游拉流 + 转码 + 笔端探流解码）可达 7s+，v4 服务端"停顿换线"
     * 场景甚至更久；7s 会把"慢启动"误判成"不可达"（2026-10-05 autotest 实录）。 */
    const first = await waitForFirstFrame(ctx, 20000);
    if (first === 'ok') {
      verified = true;
      break;
    }
    if (first === 'cancel') return { ok: false, stage: 'cancel', message: '已取消' };
    lastErr = first === 'error' ? '转码服务返回错误（房间未开播？）' : '首帧超时（转码服务不可达？）';
    if (ctx.log) ctx.log('[bili] live verify #' + (i + 1) + ' → ' + first);
    await closeSession(ctx); /* 关掉不通的会话，换地址重开 */
    if (cancelled(ctx)) return { ok: false, stage: 'cancel', message: '已取消' };
    if (i + 1 < attempts) await liveDelay(1200);
  }
  if (!rs || !rs.ok) return rs || { ok: false, stage: 'open', message: '取流失败' };
  if (!openRes || openRes.ok !== true || !verified) {
    return {
      ok: false,
      stage: 'open',
      code: openRes ? openRes.code : undefined,
      message: isLive
        ? '直播间打不开（' + (lastErr || '未开播或转码服务不可达') + '）'
        : (lastErr || '播放器启动失败')
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
      kind: isLive ? 'live' : 'video',
      live: isLive,
      roomid: rs.roomid || 0,
      bvid: isLive ? ('live:' + rs.roomid) : item.bvid, /* 无 bvid（直播不入历史，日志用它占位） */
      cid: rs.cid,
      title: rs.title || item.title || '',
      up: item.up || '',
      cover: item.cover || '',
      qn: rs.qn,
      aid: rs.aid || 0, /* 评论 oid（直播=0 → 评论空转） */
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
