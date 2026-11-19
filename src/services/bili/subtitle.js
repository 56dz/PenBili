// CC 字幕：轨道选择 / 字幕文件下载 / 时轴解析 / 按位置取当前句。
//
// 上游接口（bilibili-API-collect）：
//   player/v2 → data.subtitle.subtitles[] → subtitle_url（常为 //aisubtitle.hdslb.com/...json）
//   字幕文件内容（**没有 code 字段**）：
//     { font_size, font_color, background_alpha, background_color, Stroke,
//       body: [ { from: 秒, to: 秒, location: 2, content: "字幕文本" }, ... ] }
//
// 设备侧关键约束：X6/X7 Pro 的视频是 KMS overlay 平面（zpos 高于 Falcon UI），
// **UI 文字画不到视频上面**，所以字幕必须占一段真实 UI 像素。
// 3.5.6 第十轮起这一段是顶部悬浮条（CHROME_TOP_H=18，单行 11px），
// **不再有独立的底部字幕带**：收缩视频矩形会改 render-rectangle，而 X7 的
// videoscale 输出 caps 是起播时定死的，运行期改矩形会让画面被拉伸/推到下方。
// 于是开关字幕只换 DOM 文字，视频矩形一像素都不动。
//
// 本文件全部是纯函数 + 一个可注入 client 的取数，便于 Node 单测。

import { CC_BAND_H } from '../play_chrome.js';

export const SUBTITLE_SETTINGS_KEY = 'bili_cc_settings';
export const SUBTITLE_SETTINGS_VERSION = 1;
/* 字幕带高度（逻辑像素）。3.5.6 起唯一来源是 play_chrome.js 的 CC_BAND_H：
 * 中列被切成「顶部悬浮条 / 视频洞 / 字幕带 / 控制条」四段，四段之和恒等于视口高。
 * 这里保留 CC_BAR_H 这个名字给老调用方（与单测）用。 */
export const CC_BAR_H = CC_BAND_H;
/* 字幕 URL 允许域：B 站字幕 CDN。禁止任意外链（取数走设备网络，别开敞口）。 */
export const SUBTITLE_HOSTS = ['hdslb.com', 'bilibili.com', 'biliapi.net', 'akamaized.net'];

export function normalizeSubtitleSettings(raw) {
  const r = raw && typeof raw === 'object' ? raw : {};
  return {
    version: SUBTITLE_SETTINGS_VERSION,
    on: r.on === true,
    lan: typeof r.lan === 'string' ? r.lan : '',
    /* 自动连播互动视频时是否保留字幕：默认开，换片段会重新按 cid 拉字幕 */
    keepOnSegment: r.keepOnSegment !== false
  };
}

/* 选轨优先级：用户指定 lan > 中文（zh-Hans / ai-zh / zh） > AI 中文 > 其余按原序。
 * 3.5.16：改成返回**有序候选表**而不是单条。
 *   player/v2 的 subtitle.subtitles 是灰度分桶的动态结果（实测同一 bvid+cid 连
 *   续 6 次：0 条 / A / 0 条 / B / C / 0 条，四个互不相同的 AI 产物），单个 cid
 *   可能同时挂着好几条互不对应的 AI 字幕。只取第一条时，一旦拿到的是另一条
 *   产物，用户看到的就是「字幕和视频完全对不上」。
 *   于是这里把候选全排出来，由页面按「时长吻合度」逐条试。 */
export function rankSubtitleTracks(tracks, preferredLan) {
  const list = Array.isArray(tracks) ? tracks.filter((t) => t && t.url) : [];
  if (!list.length) return [];
  const want = String(preferredLan || '').trim();
  const out = [];
  const push = (t) => {
    if (!t) return;
    if (out.some((x) => x.url === t.url)) return;
    out.push(t);
  };
  if (want) list.filter((t) => t.lan === want).forEach(push);
  list.filter((t) => /^zh([-_]|$)/i.test(t.lan)).forEach(push);
  list.filter((t) => /^ai-zh/i.test(t.lan)).forEach(push);
  list.forEach(push);
  return out;
}

/* 兼容老调用方：仍返回单条（= 候选表首项），返回 null 表示没有轨道。 */
export function pickSubtitleTrack(tracks, preferredLan) {
  const out = rankSubtitleTracks(tracks, preferredLan);
  return out.length ? out[0] : null;
}

/* 字幕与视频是否对得上（3.5.16 新增，纯函数）。
 *
 * 动机：上游会返回「同一 cid 下属于另一条音轨/另一版剪辑」的 AI 字幕，内容与
 * 当前画面毫无关系。判定不需要理解语义，只看两条硬指标：
 *   ① 覆盖度：最后一条 cue 的结束时间 / 视频时长。对得上的字幕基本铺满全片
 *      （实测正常值 0.95~1.1），错片通常差一个数量级或只剩零头。
 *   ② 起点：第一条 cue 应落在片头附近（AI 字幕一般 0~5s 起）。实测错片会出现
 *      首句在 10 分钟视频的第 5 分钟才开始。
 * 没有视频时长时无法判断，一律放行（ok=false 会让用户彻底没字幕）。
 * → { ok, ratio, reason } */
export const SUBTITLE_FIT_MIN = 0.55;
export const SUBTITLE_FIT_MAX = 2.1;
/* 判定「与视频对得上」的容差：|末条结束时间/时长 - 1| <= TOL。
 *
 * 真机实测（BV1j2sx6PE8p，73s）同一 cid 的三个候选分别是
 *   ratio 0.78 / 1.56 / 4.19 —— 只有 0.78 是这条音轨的字幕。
 * 所以不能只设一个「上下限」然后取第一条落在区间里的：要全量打分取
 * 离 1.0 最近的那条。1.56 这类轻微超尾是 AI 字幕把末句拉长的常见现象，
 * 依然比错的候选好得多，因此不判死、只降分。 */
export const SUBTITLE_FIT_TOL = 0.55;
/* 偏差小于这个值就不必再试下一条（省一次下载，起播更快）。
 *
 * 定 0.10 而不是更宽：真机 BV1qenS6REgT 上，正确那条的偏差是 0.04，而
 * 「内容完全属于另一条音轨」的候选偏差能低到 0.24（ratio 0.76）——
 * 阈值放 0.25 会让它提前收工，又退回错字幕。宁可多下一次文件。 */
export const SUBTITLE_FIT_GOOD = 0.1;

export function subtitleFit(cues, durationMs) {
  const list = Array.isArray(cues) ? cues : [];
  const dur = Math.floor(Number(durationMs) || 0);
  if (!list.length) return { ok: false, ratio: 0, dev: 9, reason: 'empty' };
  if (!(dur > 0)) return { ok: true, ratio: 1, dev: 0, reason: 'no-duration' };
  const last = list[list.length - 1].toMs;
  const ratio = last / dur;
  const dev = Math.abs(ratio - 1);
  let reason = 'ok';
  if (ratio < SUBTITLE_FIT_MIN) reason = 'too-short';
  else if (ratio > SUBTITLE_FIT_MAX) reason = 'too-long';
  /* 起点太晚（超过全片 1/4 且超过 30s）说明这不是从片头开始的字幕。 */
  const head = list[0].fromMs;
  if (reason === 'ok' && head > 30000 && head > dur * 0.25) reason = 'late-start';
  return {
    ok: reason === 'ok' && dev <= SUBTITLE_FIT_TOL,
    ratio: ratio,
    dev: dev,
    reason: reason
  };
}

/* 上游给的是协议相对 URL（//i0.hdslb.com/...）或带 auth_key 的绝对地址。
 * → { ok, url } | { ok:false, reason } */
export function normalizeSubtitleUrl(raw) {
  let u = String(raw || '').trim();
  if (!u) return { ok: false, reason: '字幕地址为空' };
  if (u.slice(0, 2) === '//') u = 'https:' + u;
  else if (/^http:\/\//i.test(u)) u = 'https://' + u.slice(7);
  if (!/^https:\/\//i.test(u)) return { ok: false, reason: '字幕地址非 https' };
  if (/[\s\u0000-\u001f]/.test(u)) return { ok: false, reason: '字幕地址含控制字符' };
  const m = /^https:\/\/([^\/?#]+)/i.exec(u);
  if (!m) return { ok: false, reason: '字幕地址无 host' };
  const host = m[1].toLowerCase().split(':')[0];
  const allowed = SUBTITLE_HOSTS.some((s) => host === s || host.slice(-(s.length + 1)) === '.' + s);
  if (!allowed) return { ok: false, reason: '字幕域不在白名单: ' + host };
  if (u.length > 1000) return { ok: false, reason: '字幕地址超长' };
  return { ok: true, url: u, host: host };
}

function cleanText(s) {
  return String(s == null ? '' : s)
    .replace(/<br\s*\/?>/gi, ' ')
    .replace(/<[^>]{1,40}>/g, '')
    .replace(/[\r\n]+/g, ' ')
    .replace(/\s+/g, ' ')
    .trim();
}

// → { ok, cues:[{fromMs,toMs,text}], count } | { ok:false, message }
export function parseSubtitleBody(raw) {
  const d = raw && typeof raw === 'object' ? raw : null;
  if (!d || !Array.isArray(d.body)) {
    return { ok: false, message: '字幕 JSON 无 body 数组' };
  }
  const cues = [];
  for (let i = 0; i < d.body.length; i++) {
    const it = d.body[i];
    if (!it || typeof it !== 'object') continue;
    const fromMs = Math.round(Number(it.from) * 1000);
    const toMs = Math.round(Number(it.to) * 1000);
    const text = cleanText(it.content);
    if (!isFinite(fromMs) || !isFinite(toMs) || fromMs < 0 || !text) continue;
    /* 上游偶见 to<=from（AI 字幕切分异常），给 1.2s 最小窗口，避免整句闪没。 */
    cues.push({ fromMs: fromMs, toMs: Math.max(toMs, fromMs + 1200), text: text });
  }
  cues.sort((a, b) => a.fromMs - b.fromMs || a.toMs - b.toMs);
  return { ok: cues.length > 0, cues: cues, count: cues.length, message: cues.length ? '' : '字幕内容为空' };
}

/* 二分查找「最后一条 from<=pos」，再确认没过 to。O(log n)，每秒/每 250ms 调一次无压力。 */
export function cueAt(cues, posMs) {
  const list = Array.isArray(cues) ? cues : [];
  const p = Math.max(0, Math.floor(Number(posMs) || 0));
  let lo = 0;
  let hi = list.length - 1;
  let idx = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (list[mid].fromMs <= p) {
      idx = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  if (idx < 0) return '';
  const c = list[idx];
  return p < c.toMs ? c.text : '';
}

/* client.getRawJson 由 client.js 提供（字幕 JSON 没有 code 字段）。
 * → { ok:true, cues, count } | { ok:false, stage, message } */
export async function fetchSubtitleCues(client, rawUrl) {
  const u = normalizeSubtitleUrl(rawUrl);
  if (!u.ok) return { ok: false, stage: 'param', message: u.reason };
  if (!client || typeof client.getRawJson !== 'function') {
    return { ok: false, stage: 'param', message: 'client 不支持 getRawJson' };
  }
  const r = await client.getRawJson(u.url, { timeout: 12000 });
  if (!r || !r.ok) {
    return { ok: false, stage: r ? r.stage : 'transport', message: (r && r.message) || '字幕下载失败' };
  }
  const parsed = parseSubtitleBody(r.data);
  if (!parsed.ok) return { ok: false, stage: 'parse', message: parsed.message };
  return { ok: true, cues: parsed.cues, count: parsed.count, host: u.host };
}
