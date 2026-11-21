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
import { coverThumbUrl } from './imgurl.js';
import { httpGetTextNative } from './net.js';

export const RCMD_PAGE_SIZE = 20; // 实测上限
export const POPULAR_PAGE_SIZE = 12;
export const MAX_ITEMS = 60; // 列表长度上限（控内存）
export const SEARCH_PATH = '/x/web-interface/wbi/search/type';
export const SEARCH_PAGE_SIZE = 20;

const BV_RE = /^BV[0-9A-Za-z]{10}$/;

// X6 Pro 真机网络实测（2026-10-29）：
//   i0/i1/i2.hdslb.com 在设备上**无法解析**（name resolution failure，
//   nslookup 223.5.5.5 超时，直连 IP 亦超时）→ 封面全部黑块。
//   archive.biliimg.com 可正常访问，且提供**完全相同**的 /bfs/archive/<hash>.jpg
//   路径（实测 http/https 均 200，1311x737 JPEG）。
// 归一逻辑收敛到 services/imgurl.js（feed / article / 头像共用一份）。
export function normalizeCover(url) {
  // 缩略图后缀：设备 BitmapFactory 解不了 >4MB 的原图（真机灰块根因）
  return coverThumbUrl(url, 480, 270);
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
    // UP mid：详情页「点蓝色 UP 名 → 主页」用；缺失时还可由 view 兜底补齐
    mid: Number(owner.mid) || 0,
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
    key: 'v:' + bvid,
    bvid: bvid,
    cid: 0,
    title: stripHtml(raw.title) || '未命名',
    cover: normalizeCover(raw.pic || raw.cover || ''),
    up: stripHtml(raw.author) || '',
    mid: Number(raw.mid) || 0,
    durationSec: parseDuration(raw.duration),
    view: Number(raw.play) || 0,
    source: 'search'
  };
}

// 图文（专栏）搜索条目（search_type=article）→ {kind:'article', id=专栏aid, ...} | null
//   字段（2026-10-04 探针 tools/probe_search.mjs 实测）：id（专栏aid）、title(<em>)、desc、
//   image_urls[]（封面数组，**无 pic 字段**）、author、view(阅读)、reply(评论数)、category_name
export function normalizeSearchArticle(raw) {
  if (!raw || typeof raw !== 'object') return null;
  const id = Number(raw.aid || raw.id) || 0;
  if (!(id > 0)) return null;
  const imgs = Array.isArray(raw.image_urls) ? raw.image_urls : [];
  return {
    kind: 'article',
    key: 'a:' + id,
    id: id,
    title: stripHtml(raw.title) || '未命名图文',
    cover: normalizeCover(imgs[0] || raw.pic || raw.cover || ''),
    up: stripHtml(raw.author) || '',
    summary: stripHtml(raw.desc).slice(0, 60),
    view: Number(raw.view) || 0,
    reply: Number(raw.reply) || 0,
    source: 'search'
  };
}

// 直播间搜索条目（search_type=live）→ {kind:'live', roomid, ...} | null
//   字段（2026-10-04 探针实测）：roomid、title(<em>)、cover/user_cover、uname、online、cate_name
export function normalizeSearchLive(raw) {
  if (!raw || typeof raw !== 'object') return null;
  const roomid = Number(raw.roomid) || 0;
  if (!(roomid > 0)) return null;
  return {
    kind: 'live',
    key: 'l:' + roomid,
    roomid: roomid,
    title: stripHtml(raw.title) || '未命名直播间',
    cover: normalizeCover(raw.cover || raw.user_cover || ''),
    up: stripHtml(raw.uname) || '',
    online: Number(raw.online) || 0,
    cate: stripHtml(raw.cate_name) || '',
    source: 'search'
  };
}

// 按 key 去重追加（三类条目统一 key）；总长受 MAX_ITEMS 限制 → 返回 {items, added, capped}
export function appendDeduped(existing, incoming) {
  const seen = {};
  const out = [];
  (existing || []).forEach((x) => {
    const k = x && (x.key || x.bvid);
    if (x && k && !seen[k]) {
      seen[k] = 1;
      out.push(x);
    }
  });
  let added = 0;
  (incoming || []).forEach((x) => {
    const k = x && (x.key || x.bvid);
    if (x && k && !seen[k]) {
      seen[k] = 1;
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

// 搜索（WBI 签名；mixinKey 由 play_session.ensureMixin 提供会话级缓存）
// searchType: 'video' | 'article'(图文/专栏) | 'live'(直播间)
// order: totalrank（默认）| pubdate（UP 主页回退取「最新发布」）
// → {ok, items, page, noMore, total} | {ok:false, stage, message}
export function searchBili(client, mixinKey, keyword, page, searchType, order) {
  const st = searchType === 'article' || searchType === 'live' ? searchType : 'video';
  return _searchWithFallback(client, mixinKey, keyword, page, st, order);
}

/* 两趟传输：先 native libcurl（httpjson.getBinary，同步 ≈0.3~0.5s），失败再回退 jsapi。
 *
 * 为什么不用 jsapi 优先：真机日志（2026-11-10 22:25 / 23:24 / 09:59）里每次
 * 「系统输入法返回后立刻搜索」的第一发都 `NET_TIMEOUT 15000ms`，重试才成功 ——
 * 15s 的等待就是用户说的「搜索慢」。native 栈不受 jsapi 请求队列/前后台切换影响；
 * 服务端已明确应答（api/http）时不再换栈重试，避免无意义双请求。 */
async function _searchWithFallback(client, mixinKey, keyword, page, st, order) {
  let r = null;
  try {
    r = await _searchType(client, mixinKey, keyword, page, st, {
      get: nativeSearchGet,
      timeout: SEARCH_TIMEOUT_MS
    }, order);
  } catch (e) {
    r = { ok: false, stage: 'transport', message: (e && e.message) || String(e) };
  }
  if (r && r.ok) return r;
  if (r && (r.stage === 'api' || r.stage === 'http')) return r;
  if (r && r.stage === 'transport') logWarn('[bili] search native fail → jsapi: ' + r.message);
  const r2 = await _searchType(client, mixinKey, keyword, page, st, { timeout: SEARCH_TIMEOUT_MS }, order);
  return r2 && r2.ok ? r2 : (r2 || r);
}

/* 搜索相关性守卫（v3.8.0，用户反馈「后几个结果与搜索内容不符」）。
 *
 * B 站 search/type 在关键词命中少时会用「相关推荐」把每页填满 20 条
 * （开发机对拍：搜 "PenBili" 只有第 1 条真含关键词，其余是 Apple Pencil /
 * Epic Pen / 字母积木之类；搜 "原神7.2" 末尾混兑换码广告）。
 * 这里只裁掉**末尾连续不相关**的条目：
 *   - 命中率高的关键词（原神/三国演义）一条不丢（末条也命中 → 不动）；
 *   - 整页都不命中（如「米哈游」，标题里很少出现这三个字）→ 原样保留，
 *     避免把合理的联想过早裁空；
 *   - 多词关键词按「所有词都出现」判定（不要求连续），单字/单 token 按整串。
 * 匹配前两边都去掉空白/标点/符号：标题里的「原神 7.2」「【原神】7.2」都能
 * 命中「原神7.2」。 */
export function normalizeForMatch(s) {
  return String(s == null ? '' : s).toLowerCase().replace(/[^0-9a-z\u4e00-\u9fa5]+/g, '');
}

export function searchHit(rawTitle, rawExtra, keyword) {
  const hay = normalizeForMatch(String(rawTitle || '') + String(rawExtra || ''));
  const whole = normalizeForMatch(keyword);
  if (!hay || !whole) return false;
  const parts = String(keyword || '').split(/[\s,，、|/]+/).map(normalizeForMatch).filter((t) => t.length >= 2);
  if (parts.length > 1) {
    for (let i = 0; i < parts.length; i++) if (hay.indexOf(parts[i]) < 0) return false;
    return true;
  }
  return hay.indexOf(whole) >= 0;
}

// 只裁尾部；返回 {items, trimmed, kept}
export function trimIrrelevantTail(items, hays, keyword) {
  const list = items || [];
  if (list.length < 2) return { items: list, trimmed: 0, kept: list.length };
  let lastHit = -1;
  for (let i = 0; i < list.length; i++) {
    if (searchHit('', (hays && hays[i]) || '', keyword)) lastHit = i;
  }
  if (lastHit < 0 || lastHit >= list.length - 1) return { items: list, trimmed: 0, kept: list.length };
  return { items: list.slice(0, lastHit + 1), trimmed: list.length - lastHit - 1, kept: lastHit + 1 };
}

// 搜索请求超时：jsapi.http 在系统输入法刚返回时会挂满 15s（真机日志
// 「load search 首次失败(transport) NET_TIMEOUT 15000ms」）；压到 8s 让重试更早发生。
export const SEARCH_TIMEOUT_MS = 8000;

// native libcurl 文本 GET（httpjson getBinary 同步实现，30KB JSON ≈0.4s）
function nativeSearchGet(url, o) {
  return httpGetTextNative(url, { headers: o && o.headers, timeout: (o && o.timeout) || SEARCH_TIMEOUT_MS });
}

function _searchType(client, mixinKey, keyword, page, searchType, reqOpts, order) {
  const kw = String(keyword == null ? '' : keyword).trim();
  if (!kw) return { ok: false, stage: 'param', message: '关键词为空' };
  if (!/^[0-9a-f]{32}$/.test(String(mixinKey || ''))) {
    return { ok: false, stage: 'wbi', message: 'WBI 密钥无效' };
  }
  const pageNum = Math.max(1, Math.floor(Number(page) || 1));
  const params = {
    search_type: searchType,
    keyword: kw,
    order: order === 'pubdate' ? 'pubdate' : 'totalrank',
    page: pageNum
  };
  const query = buildSignedQuery(params, mixinKey, Math.floor(Date.now() / 1000));
  return client
    .request(SEARCH_PATH, query, reqOpts || {})
    .then((res) => {
      if (!res.ok) return failFrom(res);
      const d = res.data || {};
      // video/article：result 为数组；live：result 为按子类型分组的对象（2026-10-04 探针实测）
      let arr = d.result || [];
      if (!Array.isArray(arr)) {
        arr = (searchType === 'live' && arr && arr.live_room) || [];
      }
      const items = [];
      const hays = [];
      arr.forEach((raw) => {
        let it = null;
        if (searchType === 'article') it = normalizeSearchArticle(raw);
        else if (searchType === 'live') it = normalizeSearchLive(raw);
        else it = normalizeSearch(raw);
        if (it) {
          items.push(it);
          hays.push(normalizeForMatch([raw.title, raw.description, raw.tag, raw.author, raw.uname].filter(Boolean).join(' ')));
        }
      });
      if (items.length === 0) {
        return { ok: false, stage: 'parse', message: pageNum > 1 ? '没有更多结果' : '无搜索结果' };
      }
      /* 相关性裁剪只对视频搜索做：图文/直播的结果标题经常不含关键词（主播自定义
       * 标题、专栏标题），按标题命中率裁会误删合理结果。 */
      const rel = searchType === 'video'
        ? trimIrrelevantTail(items, hays, kw)
        : { items: items, trimmed: 0, kept: items.length };
      if (rel.trimmed > 0) {
        logWarn('[bili] search relevance trim p' + pageNum + ' ' + items.length + '→' + rel.kept + ' (kw=' + kw.length + '字)');
      }
      const numPages = Number(d.numPages) || 1;
      return {
        ok: true,
        items: rel.items,
        page: pageNum,
        noMore: pageNum >= numPages,
        total: Number(d.numResults) || 0,
        trimmed: rel.trimmed
      };
    });
}

// 视频搜索（兼容入口；autotest 与历史调用使用）
export function searchVideos(client, mixinKey, keyword, page) {
  return searchBili(client, mixinKey, keyword, page, 'video');
}

// 相关推荐（播放页右栏「更多视频」）
//   GET /x/web-interface/archive/related?aid= | bvid=  → data[]（最多 40 条）
//   data[] 条目形态基本同 /x/v2/view 的 data：{aid,bvid,cid,pic,title,duration,owner.name,stat.view,dimension}
//   注：没有 cid 也能播（播放链有无条件 view 补 cid），这里 cid 缺失填 0 即可。
export async function fetchRelated(client, opts) {
  const o = opts || {};
  const bvid = typeof o.bvid === 'string' ? o.bvid : '';
  const aid = Math.floor(Number(o.aid) || 0);
  const q = {};
  if (BV_RE.test(bvid)) q.bvid = bvid;
  else if (aid > 0) q.aid = aid;
  else return { ok: false, stage: 'param', message: '缺少稿件标识' };
  const res = await client.request('/x/web-interface/archive/related', q);
  if (!res.ok) return failFrom(res);
  const arr = Array.isArray(res.data) ? res.data : (res.data && res.data.list) || [];
  const items = [];
  arr.forEach((raw) => {
    const it = normalizeVideo(raw, 'related');
    if (it) items.push(it);
  });
  return { ok: true, items: items, noMore: true };
}

// 四段错误 → 面板一行文案
export function describeListError(res) {
  if (!res || res.ok) return '';
  const stage = res.stage || '';
  if (stage === 'transport') return '网络失败：' + (res.message || '');
  if (stage === 'http') return 'HTTP ' + (res.status || '') + '：' + (res.message || '');
  if (stage === 'parse') return res.message || '数据解析失败';
  if (stage === 'wbi') return '签名密钥获取失败：' + (res.message || '请稍后重试');
  return 'B站 code=' + (res.code != null ? res.code : '?') + '：' + (res.message || '接口错误');
}

export { logWarn };
