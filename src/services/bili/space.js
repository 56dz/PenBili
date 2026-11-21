// UP 主主页解析（详情页点蓝色 UP 名进入）：
//   名片 GET /x/web-interface/card       —— 匿名可用（2026-11 开发机实测 code0）
//   投稿 GET /x/space/wbi/arc/search     —— 需 WBI 签名；匿名可能 -352/-799（风控）
// 投稿接口被风控时回退到「按 UP 名搜索 + author 精确过滤」（search/type 匿名可用）。
import { buildSignedQuery } from './wbi.js';
import { coverThumbUrl } from '../imgurl.js';
import { httpGetTextNative } from '../net.js';
import { logWarn } from '../storage.js';
import { searchBili } from '../feed.js';

export const SPACE_TIMEOUT_MS = 8000;
export const SPACE_PAGE_SIZE = 12;
const BV_RE = /^BV[0-9A-Za-z]{10}$/;

function fail(stage, message) {
  return { ok: false, stage: stage, message: message };
}

// 空间接口的指纹头：Referer 必须是该 UP 的 space 页（用主站 Referer 会被风控）
function spaceGet(referer) {
  return function (url, o) {
    return httpGetTextNative(url, {
      headers: Object.assign({}, (o && o.headers) || {}, {
        Referer: referer,
        Origin: 'https://space.bilibili.com'
      }),
      timeout: (o && o.timeout) || SPACE_TIMEOUT_MS
    });
  };
}

// 时长归一：arc/search 的 duration 可能是秒数，也可能是 "12:34" 字符串
function durationSec(v) {
  if (typeof v === 'string' && v.indexOf(':') >= 0) {
    const parts = v.split(':').map((x) => Number(x) || 0);
    let sec = 0;
    for (let i = 0; i < parts.length; i++) sec = sec * 60 + parts[i];
    return sec;
  }
  const n = Math.floor(Number(v) || 0);
  return n > 0 ? n : 0;
}

function normalizeSpaceVideo(raw, mid) {
  if (!raw || typeof raw !== 'object') return null;
  const bvid = typeof raw.bvid === 'string' ? raw.bvid : '';
  if (!BV_RE.test(bvid)) return null;
  return {
    kind: 'video',
    key: 'up:' + bvid,
    bvid: bvid,
    cid: 0,
    title: typeof raw.title === 'string' && raw.title ? raw.title : '未命名',
    cover: coverThumbUrl(raw.pic || '', 480, 270),
    up: typeof raw.author === 'string' ? raw.author : '',
    mid: Number(raw.mid) || mid,
    durationSec: durationSec(raw.duration),
    view: Number(raw.play) || 0,
    source: 'up'
  };
}

// 名片：{ok, card:{mid,name,face,sign,fans,level,archiveCount}, ms} | {ok:false, stage, message}
export async function fetchUpCard(client, mid) {
  const m = Math.floor(Number(mid) || 0);
  if (!(m > 0)) return fail('param', 'UP mid 无效');
  const referer = 'https://space.bilibili.com/' + m;
  let r = await client.request('/x/web-interface/card', { mid: m }, {
    get: spaceGet(referer),
    timeout: SPACE_TIMEOUT_MS
  });
  // native 栈失败才换 jsapi（服务端已明确应答时换栈无意义）
  if (!r.ok && r.stage === 'transport') {
    r = await client.request('/x/web-interface/card', { mid: m }, { timeout: SPACE_TIMEOUT_MS });
  }
  if (!r.ok) return r;
  const d = r.data || {};
  const c = d.card || {};
  if (!(Number(c.mid) > 0)) return fail('api', 'card 缺少 UP 信息');
  return {
    ok: true,
    card: {
      mid: Number(c.mid) || m,
      name: typeof c.name === 'string' ? c.name : '',
      face: coverThumbUrl(c.face || '', 120, 120),
      sign: typeof c.sign === 'string' ? c.sign : '',
      fans: Number(c.fans) || 0,
      level: Number(c.level_info && c.level_info.current_level) || 0,
      archiveCount: Number(d.archive_count) || 0
    },
    ms: r.ms
  };
}

// 空间投稿（WBI）。失败时往上抛 stage/message，由 fetchUpVideos 决定是否回退搜索。
async function fetchSpaceArc(client, mixinKey, mid, page) {
  const m = Math.floor(Number(mid) || 0);
  const pn = Math.max(1, Math.floor(Number(page) || 1));
  if (!(m > 0)) return fail('param', 'UP mid 无效');
  if (!/^[0-9a-f]{32}$/.test(String(mixinKey || ''))) return fail('wbi', 'WBI 密钥无效');
  const params = {
    mid: m,
    ps: SPACE_PAGE_SIZE,
    pn: pn,
    order: 'pubdate',
    platform: 'web'
  };
  const query = buildSignedQuery(params, mixinKey, Math.floor(Date.now() / 1000));
  const referer = 'https://space.bilibili.com/' + m + '/video';
  let r = await client.request('/x/space/wbi/arc/search', query, {
    get: spaceGet(referer),
    timeout: SPACE_TIMEOUT_MS
  });
  if (!r.ok && r.stage === 'transport') {
    r = await client.request('/x/space/wbi/arc/search', query, { timeout: SPACE_TIMEOUT_MS });
  }
  if (!r.ok) return r;
  const d = r.data || {};
  const vlist = (d.list && d.list.vlist) || [];
  const items = [];
  for (let i = 0; i < vlist.length; i++) {
    const it = normalizeSpaceVideo(vlist[i], m);
    if (it) items.push(it);
  }
  const total = Number(d.page && d.page.count) || 0;
  return {
    ok: true,
    items: items,
    page: pn,
    total: total,
    noMore: items.length === 0 || (total > 0 && pn * SPACE_PAGE_SIZE >= total),
    via: 'space'
  };
}

// 投稿列表：空间接口优先，风控/限流时回退「按 UP 名搜索（最新发布）+ 作者精确过滤」。
// name 只在回退时需要；没有名字时直接返回空间接口的原始错误。
export async function fetchUpVideos(client, mixinKey, mid, page, name) {
  const primary = await fetchSpaceArc(client, mixinKey, mid, page);
  if (primary.ok) return primary;
  const nm = String(name || '').trim();
  if (!nm) return primary;
  logWarn('[bili] up arc fail stage=' + primary.stage + ' code=' + (primary.code != null ? primary.code : '-') + ' → search fallback');
  const r = await searchBili(client, mixinKey, nm, page, 'video', 'pubdate');
  if (!r.ok) return primary; // 回退也失败时保留空间接口错误（更能说明根因）
  const items = (r.items || []).filter((x) => x && x.up === nm);
  if (!items.length && !r.noMore) return primary; // 本页没有该 UP 的稿件，不能当作「没更多」
  return { ok: true, items: items, page: page, total: 0, noMore: !!r.noMore || items.length === 0, via: 'search' };
}
