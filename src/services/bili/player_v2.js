// B 站播放器信息（x/player/v2）—— CC 字幕与互动视频的**共同入口**。
//
// 上游接口（bilibili-API-collect / 播放器信息、互动视频、字幕章节）：
//   GET https://api.bilibili.com/x/player/v2?bvid=..&cid=..
//     data.subtitle.subtitles[] : { lan, lan_doc, subtitle_url, ai_status, id, ... }
//     data.interaction         : { graph_version, id, ... }   ← 仅互动视频有
//   GET https://api.bilibili.com/x/player/wbi/v2?...&w_rid=.. （WBI 签名回退）
//   GET https://api.bilibili.com/x/stein/edgeinfo_v2?bvid=..&graph_version=..&edge_id=..
//
// 三点真机约束：
//   ① 字幕列表**匿名恒为空**（B 站要求 SESSDATA）；空列表不是错误，页面要提示登录。
//   ② /x/player/v2 偶发 -352/-403 风控，此时用项目既有 WBI 密钥签名后改走 wbi/v2。
//   ③ 字幕 JSON 是纯 body 数组，**没有 code 字段**，不能走 client.request()
//      （它按 code!==0 判失败）→ 用 client.getRawJson()。
//
// 本模块只做「请求 + 纯解析」，不碰 storage / 不碰播放器，全部可注入 client 单测。

import { buildSignedQuery } from './wbi.js';

export const PLAYER_V2_PATH = '/x/player/v2';
export const PLAYER_WBI_V2_PATH = '/x/player/wbi/v2';

// → { lan, lanDoc, url, ai, id }
export function normalizeSubtitleTrack(t) {
  if (!t || typeof t !== 'object') return null;
  const lan = typeof t.lan === 'string' ? t.lan : '';
  const url = typeof t.subtitle_url === 'string' ? t.subtitle_url : '';
  if (!lan && !url) return null;
  return {
    lan: lan,
    lanDoc: typeof t.lan_doc === 'string' && t.lan_doc ? t.lan_doc : lan,
    url: url,
    ai: Number(t.ai_status) === 1 || /^ai-/.test(lan),
    id: Number(t.id) || 0
  };
}

// → { subtitles, subtitleLan, allowSubmit, interactive, graphVersion, interactionId }
export function parsePlayerV2(data) {
  const d = data && typeof data === 'object' ? data : {};
  const sub = d.subtitle && typeof d.subtitle === 'object' ? d.subtitle : {};
  const list = Array.isArray(sub.subtitles) ? sub.subtitles : [];
  const subtitles = [];
  for (let i = 0; i < list.length; i++) {
    const t = normalizeSubtitleTrack(list[i]);
    if (t && t.url) subtitles.push(t);
  }
  const inter = d.interaction && typeof d.interaction === 'object' ? d.interaction : null;
  /* 互动视频的剧情图版本号：上游示例与 bilibili-api Python 客户端都取
   * data.interaction.graph_version；兼容几种历史别名，避免固件/接口灰度时漏判。 */
  const gv = inter
    ? Number(inter.graph_version || inter.graphVersion || inter.version || 0)
    : Number(d.graph_version || 0);
  return {
    subtitles: subtitles,
    subtitleLan: typeof sub.lan === 'string' ? sub.lan : '',
    allowSubmit: !!sub.allow_submit,
    interactive: Math.floor(gv) > 0,
    graphVersion: Math.floor(gv) > 0 ? Math.floor(gv) : 0,
    interactionId: inter ? Number(inter.id) || 0 : 0
  };
}

/* opts: { mixinKey?, nowSec? } —— 传了才启用 WBI 回退。
 * → { ok:true, subtitles, interactive, graphVersion, ... } | { ok:false, stage, message } */
export async function fetchPlayerV2(client, bvid, cid, opts) {
  if (!client || typeof client.request !== 'function') {
    return { ok: false, stage: 'param', message: 'client 不可用' };
  }
  if (!/^BV[0-9A-Za-z]{10}$/.test(String(bvid || ''))) {
    return { ok: false, stage: 'param', message: 'bvid 不合法' };
  }
  const c = Math.floor(Number(cid) || 0);
  if (!(c > 0)) return { ok: false, stage: 'param', message: 'cid 不合法' };

  let res = await client.request(PLAYER_V2_PATH, { bvid: bvid, cid: c }, {});
  /* 风控/旧接口下线时改走 WBI 签名版。签名串必须原样透传，不能再 buildQuery 一次。 */
  const risky = res && !res.ok && (
    res.code === -352 || res.code === -403 || res.code === -400 || res.code === -412 ||
    res.stage === 'http'
  );
  const o = opts || {};
  if (risky && o.mixinKey && typeof o.nowSec === 'function') {
    const q = buildSignedQuery({ bvid: bvid, cid: c }, o.mixinKey, o.nowSec());
    const wbi = await client.request(PLAYER_WBI_V2_PATH, q, {});
    if (wbi && wbi.ok) res = wbi;
  }
  if (!res || !res.ok) {
    return {
      ok: false,
      stage: res ? res.stage : 'transport',
      code: res ? res.code : undefined,
      message: (res && res.message) || 'player/v2 请求失败'
    };
  }
  return Object.assign({ ok: true }, parsePlayerV2(res.data));
}

/* 3.5.16：字幕列表是灰度分桶的，一次请求可能是空、也可能只给其中一版产物。
 * 实测同一 bvid+cid 连续 6 次 player/v2：
 *   0 条 / A(675cues) / 0 条 / B(38cues) / C(319cues) / 0 条 —— 四个互不相同的
 *   AI 产物，内容主题都不一样。用户反馈的「字幕和视频完全对不上」就是这个：
 *   页面只取首条，拿到哪版全看分桶。
 * 对策：把 `subtitles` 当**候选池**，多次请求按 url 去重合并，交页面逐条试。
 *
 * opts: { mixinKey?, nowSec?, tries?, gapMs?, shouldStop? }
 *   tries     请求次数上限（默认 3）
 *   gapMs     两次之间等待（默认 260ms；设备上次网络抖动时不至于卡太久）
 *   shouldStop(merged) 返回 true 可提前收工（默认凑够 wantTracks 条就停）
 *   wantTracks  期望候选数（默认 2：给页面留一次换轨机会）
 * → { ok:true, subtitles, tries, interactive, graphVersion, ... } | { ok:false, ... }
 */

/* 合并一批 player/v2 结果里的字幕候选（按 url 去重）。 */
function mergeTracks(merged, seen, list) {
  if (!Array.isArray(list)) return;
  for (let k = 0; k < list.length; k++) {
    const t = list[k];
    if (!t || !t.url || seen[t.url]) continue;
    seen[t.url] = 1;
    merged.push(t);
  }
}

/* 3.5.16：字幕列表是灰度分桶的，单次请求既可能空、也可能只给其中一版产物。
 *
 * 实测（BV1qenS6REgT / cid 43048174426 / 176s）连取 6 轮能凑到 4 个互不相同的
 * AI 产物，末条/时长偏差 0.04 / 0.08 / 0.12 / 0.76 —— 只取 1~2 条时经常两个
 * 都是错的，用户看到的就是「字幕和视频完全对不上」。
 *
 * 串行取 6 轮要 6.8s（每轮 1.1s），起播等不起；实测**并发**发 5 个请求只花
 * 0.56s 就能拿到 5 个不同分桶。所以策略是「第 1 发单跑探路，够数就收工；
 * 不够再把剩下的并发打出去」。这样正常稿件只花 1 次往返，异常稿件 ~0.6s。
 *
 * opts: { mixinKey?, nowSec?, tries?, wantTracks?, shouldStop? }
 * → { ok:true, subtitles, tries, interactive, graphVersion, ... } | { ok:false, ... }
 */
export const PLAYER_V2_TRIES = 5;
export const PLAYER_V2_WANT = 4;
/* 保留给调用方显式限速用（默认 0：并发发，不排队等）。 */
export const PLAYER_V2_GAP_MS = 0;

function sleepMs(ms) {
  return new Promise((resolve) => setTimeout(resolve, Math.max(0, ms | 0)));
}

export async function fetchPlayerV2Tracks(client, bvid, cid, opts) {
  const o = opts || {};
  const tries = Math.max(1, Math.floor(Number(o.tries) || PLAYER_V2_TRIES));
  const gapMs = Number.isFinite(Number(o.gapMs)) ? Number(o.gapMs) : PLAYER_V2_GAP_MS;
  const want = Math.max(1, Math.floor(Number(o.wantTracks) || PLAYER_V2_WANT));
  const merged = [];
  const seen = Object.create(null);

  /* 第 1 发探路：请求本身失败（风控/网络）就别再叠加并发，直接报给调用方。 */
  const first = await fetchPlayerV2(client, bvid, cid, o);
  if (!first || !first.ok) {
    return first || { ok: false, stage: 'transport', message: 'player/v2 请求失败' };
  }
  mergeTracks(merged, seen, first.subtitles);
  let last = first;
  let used = 1;
  if (typeof o.shouldStop === 'function' && o.shouldStop(merged)) {
    return Object.assign({}, last, { ok: true, subtitles: merged, tries: used });
  }
  if (merged.length >= want || tries <= 1) {
    return Object.assign({}, last, { ok: true, subtitles: merged, tries: used });
  }

  /* 剩下的一起打出去。任何一个成功都保留结果；只有全失败才维持 first。 */
  if (gapMs > 0) await sleepMs(gapMs);
  const rest = [];
  for (let i = 1; i < tries; i++) rest.push(fetchPlayerV2(client, bvid, cid, o));
  const got = await Promise.all(rest);
  used = tries;
  for (let i = 0; i < got.length; i++) {
    const r = got[i];
    if (!r || !r.ok) continue;
    last = r;
    mergeTracks(merged, seen, r.subtitles);
  }
  return Object.assign({}, last, { ok: true, subtitles: merged, tries: used });
}
