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

/* 选轨优先级：用户指定 lan > 中文（zh-Hans / ai-zh / zh） > AI 中文 > 第一条。
 * 返回 null 表示没有轨道。 */
export function pickSubtitleTrack(tracks, preferredLan) {
  const list = Array.isArray(tracks) ? tracks.filter((t) => t && t.url) : [];
  if (!list.length) return null;
  const want = String(preferredLan || '').trim();
  if (want) {
    const hit = list.find((t) => t.lan === want);
    if (hit) return hit;
  }
  const zh = list.find((t) => /^zh([-_]|$)/i.test(t.lan)) || list.find((t) => /^ai-zh/i.test(t.lan));
  if (zh) return zh;
  return list[0];
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
