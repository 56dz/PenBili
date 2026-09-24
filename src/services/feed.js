// 列表数据 adapter：推荐 / 热门 / **视频搜索** → 统一 item 形态。
// 事实来源：api-mock/fixtures（开发机 2026-09-22/23 探针，真响应裁剪）：
//   推荐 GET /x/web-interface/index/top/feed/rcmd?ps&pn → data.item[]（goto=av）
//     - ps=20 可用，ps=40 返回空 → 上限 20
//     - 匿名实测：同参 3 连拉 20/20 完全相同；pn=2==pn=1；refresh_type=1/2 无效
//       → 匿名不可翻页/换批（登录态交真机验证；换一批=清缓存重拉，列表耗尽用热门续底）
//   热门 GET /x/web-interface/popular?ps&pn → data.list[] + data.no_more（翻页可用 ✓）
//   搜索 GET /x/web-interface/wbi/search/type（WBI 签名，匿名 200 code0）
//       data.result[] 混排：视频卡(type=video,bvid,title带<em>标签,pic=//,duration="59:39")
//       与直播卡(roomid) → 用 roomid 滤除；结果**无 cid**（播放链的无条件 view 补齐）
//   封面 pic 原生 http://i*.hdslb.com（探针 200 image/jpeg）→ 统一归一为 http，
//     规避无 CA 库下 https ImageLoader 的未知风险。
import { logWarn } from './storage.js';
import { buildSignedQuery } from './bili/wbi.js';

export const RCMD_PAGE_SIZE = 20; // 实测上限
export const POPULAR_PAGE_SIZE = 12;
export const MAX_ITEMS = 60; // 列表长度上限（控内存）
export const SEARCH_PATH = '/x/web-interface/wbi/search/type';
export const SEARCH_PAGE_SIZE = 20;

const BV_RE = /^BV[0-9A-Za-z]{10}$/;

// //x → http://x；hdslb 域 https → http（CDN 实测 http 200）；其它原样
export function normalizeCover(url) {
  if (typeof url !== 'string' || !url) return '';
  let u = url.trim();
  if (u.indexOf('//') === 0) return 'http:' + u;
  if (/^https:\/\//i.test(u)) {
    const host = u.replace(/^https:\/\//i, '').split('/')[0].toLowerCase();
    if (/\.hdslb\.com(:\d+)?$/.test(host)) return 'http://' + u.slice(8);
  }
  return u;
}

// 时长秒 → "mm:ss" / "h:mm:ss"；非正数 → ''
export function formatDuration(sec) {
  const n = Math.floor(Number(sec) || 0);
  if (n <= 0) return '';
  const h = Math.floor(n / 3600);
  const m = Math.floor((n % 3600) / 60);
  const s = n % 60;
  const p2 = (x) => (x < 10 ? '0' + x : String(x));
  return h > 0 ? h + ':' + p2(m) + ':' + p2(s) : p2(m) + ':' + p2(s);
}

// "59:39" / "1:02:03" / 纯数字 → 秒（与 formatDuration 互逆）
export function parseDuration(v) {
  if (typeof v === 'number') return isFinite(v) && v > 0 ? Math.floor(v) : 0;
  if (typeof v !== 'string') return 0;
  const s = v.trim();
  if (!s) return 0;
  if (/^\d+$/.test(s)) return Number(s);
  const parts = s.split(':').map((x) => Number(x));
  if (parts.some((x) => !isFinite(x))) return 0;
  if (parts.length === 2) return parts[0] * 60 + parts[1];
  if (parts.length === 3) return parts[0] * 3600 + parts[1] * 60 + parts[2];
  return 0;
}

// 播放量：1.2万 / 10.3万（万位一位小数），其余原样
export function formatCount(n) {
  const v = Number(n) || 0;
  if (v >= 100000000) return (v / 100000000).toFixed(1) + '亿';
  if (v >= 10000) return (v / 10000).toFixed(1) + '万';
  return v > 0 ? String(v) : '';
}

// 搜索标题去 HTML 高亮标签与常见实体：<em class="keyword">测试</em> → 测试
const ENTITY_MAP = { '&amp;': '&', '&lt;': '<', '&gt;': '>', '&quot;': '"', '&#39;': "'", '&nbsp;': ' ' };
export function stripHtml(s) {
  return String(s == null ? '' : s)
    .replace(/<[^>]+>/g, '')
    .replace(/&(amp|lt|gt|quot|#39|nbsp);/g, (m) => ENTITY_MAP[m] || m);
}

// 推荐/热门原始条目 → VideoItem | null（bvid 不合规直接丢弃）
export function normalizeVideo(raw, source) {
  if (!raw || typeof raw !== 'object') return null;
  const bvid = typeof raw.bvid === 'string' ? raw.bvid : '';
  if (!BV_RE.test(bvid)) return null;
  const owner = raw.owner || {};
  const stat = raw.stat || {};
  const dur = Number(raw.duration);
  return {
    kind: 'video',
    bvid: bvid,
    cid: Number(raw.cid) || 0,
    title: typeof raw.title === 'string' && raw.title ? raw.title : '未命名',
    cover: normalizeCover(raw.pic || raw.cover || ''),
    up: typeof owner.name === 'string' && owner.name ? owner.name : '',
    durationSec: isFinite(dur) && dur > 0 ? Math.floor(dur) : 0,
    view: Number(stat.view) || 0,
    source: source || ''
  };
}

// 搜索条目 → VideoItem | null
//   - 无 cid（播放链会无条件 view 补齐）  - 直播卡（带 roomid）直接丢弃
//   - title 带 <em> 高亮标签 → 剥掉    - duration 为 "59:39" → parseDuration
export function normalizeSearch(raw) {
  if (!raw || typeof raw !== 'object') return null;
  const bvid = typeof raw.bvid === 'string' ? raw.bvid : '';
  if (!BV_RE.test(bvid)) return null;
  // 直播卡（roomid>0）滤除；普通视频卡常带 roomid:0，必须放行
  if (Number(raw.roomid) > 0) return null;
  return {
    kind: 'video',
    bvid: bvid,
    cid: 0,
    title: stripHtml(raw.title) || '未命名',
    cover: normalizeCover(raw.pic || raw.cover || ''),
    up: stripHtml(raw.author) || '',
    durationSec: parseDuration(raw.duration),
    view: Number(raw.play) || 0,
    source: 'search'
  };
}

// 按 bvid 去重追加；总长受 MAX_ITEMS 限制 → 返回 {items, added, capped}
export function appendDeduped(existing, incoming) {
  const seen = {};
  const out = [];
  (existing || []).forEach((x) => {
    if (x && x.bvid && !seen[x.bvid]) {
      seen[x.bvid] = 1;
      out.push(x);
    }
  });
  let added = 0;
  (incoming || []).forEach((x) => {
    if (x && x.bvid && !seen[x.bvid]) {
      seen[x.bvid] = 1;
      out.push(x);
      added++;
    }
  });
  const capped = out.length > MAX_ITEMS;
  return { items: capped ? out.slice(0, MAX_ITEMS) : out, added: added, capped: capped };
}

function failFrom(res) {
  return {
    ok: false,
    stage: res ? res.stage : 'transport',
    code: res ? res.code : undefined,
    message: (res && res.message) || '列表请求失败'
  };
}

// 推荐：单次 20 条（ps 上限，匿名不可翻页/换批——见文件头实测）
export async function fetchRecommended(client) {
  const res = await client.request('/x/web-interface/index/top/feed/rcmd', { ps: RCMD_PAGE_SIZE, pn: 1 });
  if (!res.ok) return failFrom(res);
  const arr = (res.data && res.data.item) || [];
  const items = [];
  arr.forEach((raw) => {
    if (raw && raw.bvid && (raw.goto === 'av' || raw.goto === undefined || raw.goto === '')) {
      const it = normalizeVideo(raw, 'rcmd');
      if (it) items.push(it);
    }
  });
  if (items.length === 0) return { ok: false, stage: 'parse', message: '推荐列表为空（接口变更？）' };
  return { ok: true, items: items, noMore: true };
}

// 热门：pn 翻页 + no_more（推荐耗尽时也用它续底）
export async function fetchPopular(client, pn) {
  const page = Math.max(1, Math.floor(Number(pn) || 1));
  const res = await client.request('/x/web-interface/popular', { ps: POPULAR_PAGE_SIZE, pn: page });
  if (!res.ok) return failFrom(res);
  const data = res.data || {};
  const arr = data.list || [];
  const items = [];
  arr.forEach((raw) => {
    const it = normalizeVideo(raw, 'hot');
    if (it) items.push(it);
  });
  return { ok: true, items: items, noMore: data.no_more === true || items.length === 0 };
}

// 视频搜索（WBI 签名；mixinKey 由 play_session.ensureMixin 提供会话级缓存）
// → {ok, items, page, noMore, total} | {ok:false, stage, message}
export async function searchVideos(client, mixinKey, keyword, page) {
  const kw = String(keyword == null ? '' : keyword).trim();
  if (!kw) return { ok: false, stage: 'param', message: '关键词为空' };
  if (!/^[0-9a-f]{32}$/.test(String(mixinKey || ''))) {
    return { ok: false, stage: 'wbi', message: 'WBI 密钥无效' };
  }
  const pageNum = Math.max(1, Math.floor(Number(page) || 1));
  const params = {
    search_type: 'video',
    keyword: kw,
    order: 'totalrank',
    page: pageNum
  };
  const query = buildSignedQuery(params, mixinKey, Math.floor(Date.now() / 1000));
  const res = await client.request(SEARCH_PATH, query);
  if (!res.ok) return failFrom(res);
  const d = res.data || {};
  const arr = d.result || [];
  const items = [];
  arr.forEach((raw) => {
    const it = normalizeSearch(raw);
    if (it) items.push(it);
  });
  if (items.length === 0) {
    return { ok: false, stage: 'parse', message: pageNum > 1 ? '没有更多结果' : '无搜索结果' };
  }
  const numPages = Number(d.numPages) || 1;
  return {
    ok: true,
    items: items,
    page: pageNum,
    noMore: pageNum >= numPages,
    total: Number(d.numResults) || 0
  };
}

// 四段错误 → 面板一行文案
export function describeListError(res) {
  if (!res || res.ok) return '';
  const stage = res.stage || '';
  if (stage === 'transport') return '网络失败：' + (res.message || '');
  if (stage === 'http') return 'HTTP ' + (res.status || '') + '：' + (res.message || '');
  if (stage === 'parse') return res.message || '数据解析失败';
  return 'B站 code=' + (res.code != null ? res.code : '?') + '：' + (res.message || '接口错误');
}

export { logWarn };
