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
