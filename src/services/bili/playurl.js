// playurl 参数、响应解析与流地址校验。
// 事实（profiles/youdao-x5.md）：
//   - 匿名 html5 单文件（fnval=1, platform=html5）：qn=32/16 → 640x360 AVC+AAC，免 Referer，只需浏览器 UA
//   - qn=64 → 720p，本机软解 0.76x 实时，**禁止提供**
//   - 流地址有效期约 120 分钟；native open() 的 input 上限 1024 字符
import { buildSignedQuery } from './wbi.js';

// 取流阶梯：
//   durl（html5 单文件）：先 360p(qn=32)，失败退 360p 低码率(qn=16)
//   dash 的 qn 语义不同：id32 = **852x480(480p)**、id16 = 640x360(360p) —— 与 html5 的
//   qn32=640x360 不同！真机实测（2h 长片）：dash 取 480p 源让 A53 全线滑行 0.7x 实时
//   （pos/ab/frames 统一 70%、aplay underrun 1.4s 循环 = "一直卡"）→ **DASH 只走 id16**。
export const QN_LADDER = [32, 16];
export const DASH_QN_LADDER = [16];

// html5 单文件实测输出帧率（30fps；设备观测 90 帧 / 3 秒）
export const HTML5_FPS = 30;

export const MAX_STREAM_URL_LEN = 1000;

// B 站系 CDN 域后缀（命中与否都记录在探测详情里，非命中不作为硬失败——
// URL 本身已通过 https + 无控制字符 + 长度校验，且以 argv 数组传给 ffmpeg，无 shell 注入面）
export const ALLOWED_HOST_SUFFIXES = [
  'bilivideo.com',
  'bilivideo.cn',
  'bilivideo.net',
  'hdslb.com',
  'bilibili.com',
  'biliapi.net',
  'solseed.cn',
  'akamaized.net'
];

export function buildPlayurlParams(target) {
  const t = target || {};
  const dash = Number(t.fnval) === 16;
  const p = {
    bvid: typeof t.bvid === 'string' ? t.bvid : '',
    cid: Math.floor(Number(t.cid) || 0),
    qn: Number(t.qn) || 32,
    fnver: 0,
    // fnval=16 → DASH 分离流（主路径：音频轨独立 66kbps 恒供）；
    // fnval=1  → durl 单文件（回退保底）。
    fnval: dash ? 16 : 1,
    fourk: 0
  };
  // 关键：**platform=html5 与 fnval=16 互斥**——服务端见 html5 强制降级 durl（实测回退日志坐实）。
  // DASH 请求按标准播放器形态（开发机探测成功的参数集：无 platform/high_quality）。
  if (!dash) {
    p.platform = 'html5';
    p.high_quality = 1;
  }
  return p;
}

// → { ok, query, qn } | { ok:false, message }
export function buildPlayurlQuery(target, mixinKey, wts) {
  const params = buildPlayurlParams(target);
  if (!/^BV[0-9A-Za-z]{10}$/.test(params.bvid)) {
    return { ok: false, message: 'bvid 不合法: ' + params.bvid };
  }
  if (!(params.cid > 0)) {
    return { ok: false, message: 'cid 不合法: ' + params.cid };
  }
  if (!/^[0-9a-f]{32}$/.test(String(mixinKey || ''))) {
    return { ok: false, message: 'mixinKey 不是 32 位 hex' };
  }
  return { ok: true, query: buildSignedQuery(params, mixinKey, wts), qn: params.qn };
}

export function hostOf(url) {
  const m = /^https?:\/\/([^\/?#]+)/i.exec(String(url || ''));
  return m ? m[1] : '';
}

export function isAllowedHost(host) {
  const bare = String(host || '')
    .toLowerCase()
    .split(':')[0];
  if (!bare) return false;
  for (let i = 0; i < ALLOWED_HOST_SUFFIXES.length; i++) {
    const s = ALLOWED_HOST_SUFFIXES[i];
    if (bare === s) return true;
    if (bare.length > s.length + 1 && bare.slice(-(s.length + 1)) === '.' + s) return true;
  }
  return false;
}

// 流地址硬校验：https + 无空白/控制字符 + 无 '..' + 长度 ≤1000（native input 上限 1024 留余量）
// 返回 { ok, host, allowlisted, length } | { ok:false, reason }
export function validateStreamUrl(url) {
  if (typeof url !== 'string' || !url) return { ok: false, reason: 'url 为空' };
  if (url.length > MAX_STREAM_URL_LEN) {
    return { ok: false, reason: '超长 ' + url.length + ' chars > ' + MAX_STREAM_URL_LEN };
  }
  if (/[\s\u0000-\u001f]/.test(url)) return { ok: false, reason: '含空白/控制字符' };
  if (url.indexOf('..') >= 0) return { ok: false, reason: '含 ..' };
  if (!/^https:\/\//i.test(url)) return { ok: false, reason: '非 https' };
  const host = hostOf(url);
  if (!host) return { ok: false, reason: 'host 解析失败' };
  return { ok: true, host: host, allowlisted: isAllowedHost(host), length: url.length };
}

// playurl 成功响应 → { ok, url, sizeBytes, quality, acceptQuality } | { ok:false, stage, code, message }
export function parseHtml5Response(res) {
  if (!res || res.ok === false) {
    return {
      ok: false,
      stage: res ? res.stage : 'transport',
      code: res ? res.code : undefined,
      message: (res && res.message) || 'playurl 请求失败'
    };
  }
  const data = res.data || {};
  const durl = data.durl;
  if (!Array.isArray(durl) || durl.length === 0) {
    return { ok: false, stage: 'api', code: res.code, message: 'playurl 无 durl（地区/版权不可播）' };
  }
  const d = durl[0] || {};
  const url = typeof d.url === 'string' ? d.url : '';
  if (!url) return { ok: false, stage: 'api', code: res.code, message: 'durl[0].url 为空' };
  return {
    ok: true,
    url: url,
    sizeBytes: typeof d.length === 'number' ? d.length : 0,
    quality: typeof data.quality === 'number' ? data.quality : 0,
    acceptQuality: Array.isArray(data.accept_quality) ? data.accept_quality : []
  };
}

// DASH 条目 URL 候选打分（baseUrl + backupUrl 同内容镜像，选实测快的）：
//   实测（设备端 curl，2026-09-24）：bilivideo.com:443 ≈0.3s；mcdn.bilivideo.cn:8082 与
//   *.edge.mountaintoys.cn:4483 ≈5-6s/请求 —— 烂端口高延迟让 ffmpeg 双输入首轮探测
//   超过 12s（零帧零 stderr 的根因）。非白名单域（mountaintoys 不在 ALLOWED_HOST_SUFFIXES）
//   直接淘汰。分数越小越优先；-1 = 淘汰。
function scoreStreamUrl(u) {
  const host = hostOf(u);
  if (!host) return -1;
  if (!isAllowedHost(host)) return -1;
  const parts = host.split(':');
  const bare = parts[0];
  const port = parts[1] || '';
  if (bare.slice(-14) === '.bilivideo.com' && (port === '' || port === '443')) return 0; // 快域
  if (bare.slice(-21) === '.bilivideo.cn' || bare.slice(-13) === '.bilivideo.cn') return 2; // mcdn:8082 慢
  return 1;
}

// 从 [baseUrl, ...backupUrl] 挑最优；全被淘汰时回退任何有效 https 候选（保底可播）
function pickStreamUrl(entry) {
  const cands = [entry.baseUrl || entry.base_url || ''];
  const bu = entry.backupUrl || entry.backup_url || [];
  if (Array.isArray(bu)) bu.forEach((u) => cands.push(u));
  const valid = cands.filter((u) => u && /^https:\/\//.test(u));
  const ranked = valid
    .map((u) => ({ u: u, s: scoreStreamUrl(u) }))
    .sort((a, b) => a.s - b.s);
  const good = ranked.filter((x) => x.s >= 0);
  if (good.length) return good[0].u;
  return ranked.length ? ranked[0].u : '';
}

// DASH(fnval=16) 响应 → {ok, videoUrl, audioUrl, qn, durationMs, bandwidth, codecs, audioBandwidth}
//   | {ok:false, stage, message, hasDurl?}
// 选流规则（实测 fixture：同 id 有 avc1 与 hvc1 双条目）：
//   1) id === 请求 qn 的池子优先 → 池内 **avc1 优先**（HEVC 软解在 A53 上≈解不动，是
//      "按稿件分化卡顿"的真凶之一）→ 同池按带宽升序取最省带宽的一条。
//   2) 条目内 URL 按 scoreStreamUrl 挑（快域 bilivideo.com 优先）。
//   3) 音频取最低档（实测 66kbps ≈ 设备实测带宽 646kbps 的 1/10 → 分离后声音供给恒稳）。
//   4) 无 dash 结构 → hasDurl 信号，调用方回退 fnval=1 单文件路径。
export function parseDashResponse(res, wantQn) {
  if (!res || res.ok === false) {
    return {
      ok: false,
      stage: res ? res.stage : 'transport',
      code: res ? res.code : undefined,
      message: (res && res.message) || 'playurl 请求失败'
    };
  }
  const data = res.data || {};
  const dash = data.dash;
  if (!dash) {
    return { ok: false, stage: 'parse', hasDurl: !!(data.durl && data.durl.length), message: '响应无 dash 结构' };
  }
  const vids = dash.video || [];
  if (!vids.length) return { ok: false, stage: 'parse', hasDurl: false, message: 'dash.video 为空' };
  const qn = Number(wantQn) || 0;
  let pool = qn ? vids.filter((x) => Number(x.id) === qn) : [];
  if (!pool.length) pool = vids.slice();
  const byBw = (a, b) => (Number(a.bandwidth) || 0) - (Number(b.bandwidth) || 0);
  const avc = pool.filter((x) => String(x.codecs || '').indexOf('avc1') === 0);
  const pick = (avc.length ? avc : pool).slice().sort(byBw)[0];
  const videoUrl = pickStreamUrl(pick);
  if (!videoUrl) return { ok: false, stage: 'parse', hasDurl: false, message: 'dash.video 无可用 URL' };
  const auds = (dash.audio || []).slice().sort(byBw);
  const audioUrl = auds.length ? pickStreamUrl(auds[0]) : '';
  return {
    ok: true,
    videoUrl: videoUrl,
    audioUrl: audioUrl,
    qn: Number(pick.id) || qn,
    bandwidth: Number(pick.bandwidth) || 0,
    codecs: String(pick.codecs || ''),
    audioBandwidth: auds.length ? Number(auds[0].bandwidth) || 0 : 0,
    durationMs: Math.max(0, Math.floor(Number(data.timelength) || 0))
  };
}

// qn → 显示尺寸（profile：html5 qn32/16 实测 640x360；qn64=1280x720 仅作日志说明，不提供）
export function dimsForQn(qn) {
  if (qn === 64) return { width: 1280, height: 720 };
  return { width: 640, height: 360 };
}
