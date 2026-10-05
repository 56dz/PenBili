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

// 图文（专栏）条目 → 同形 VideoItem（kind:'article'；列表展示，点开详情超出当前范围）
//   实测 search_type=article：result 为数组，条目 {id(cvid), title, desc, author, image_urls[], view, ...}
export function normalizeSearchArticle(raw) {
  if (!raw || typeof raw !== 'object') return null;
  const id = Number(raw.id) || 0;
  if (!(id > 0)) return null;
  return {
    kind: 'article',
    bvid: 'article_' + id, /* vrow :key 唯一（非真 bvid，onPlayItem 按 kind 分流不进播放） */
    cid: 0,
    title: stripHtml(raw.title) || '未命名专栏',
    cover: normalizeCover((raw.image_urls && raw.image_urls[0]) || ''),
    up: stripHtml(raw.author) || '',
    category: stripHtml(raw.category_name) || '',
    cvid: id,
    durationSec: 0,
    view: Number(raw.view) || 0,
    source: 'search'
  };
}

// 直播间条目 → 同形（kind:'live'；collect 对象类型3 live_room）
//   实测 search_type=live：result 为对象 {live_room[], live_user[]}——取 live_room（直播间列表）
export function normalizeSearchLive(raw) {
  if (!raw || typeof raw !== 'object') return null;
  const roomid = Number(raw.roomid) || 0;
  if (!(roomid > 0)) return null;
  return {
    kind: 'live',
    bvid: 'room_' + roomid,
    roomid: roomid, /* ★必须透传：resolveLiveUrl 依赖（漏存 → 'roomid 无效' 实测 bug） */
    cid: 0,
    title: stripHtml(raw.title) || '直播间',
    cover: normalizeCover(raw.cover || raw.user_cover || ''),
    up: stripHtml(raw.uname) || '',
    category: stripHtml(raw.cate_name) || '',
    durationSec: 0,
    view: Number(raw.online) || 0, /* online 可能是字符串 */
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

// 分类搜索（WBI 签名；mixinKey 由 play_session.ensureMixin 提供会话级缓存）
// search_type：video(视频) / article(图文·专栏) / live(直播间) —— 三者同端点同签名，仅换参数。
// → {ok, items, page, noMore, total} | {ok:false, stage, message}
export async function searchByType(client, mixinKey, keyword, page, searchType) {
  const kw = String(keyword == null ? '' : keyword).trim();
  if (!kw) return { ok: false, stage: 'param', message: '关键词为空' };
  if (!/^[0-9a-f]{32}$/.test(String(mixinKey || ''))) {
    return { ok: false, stage: 'wbi', message: 'WBI 密钥无效' };
  }
  const st = searchType === 'article' || searchType === 'live' ? searchType : 'video';
  const pageNum = Math.max(1, Math.floor(Number(page) || 1));
  const params = {
    search_type: st,
    keyword: kw,
    order: 'totalrank',
    page: pageNum
  };
  const query = buildSignedQuery(params, mixinKey, Math.floor(Date.now() / 1000));
  const res = await client.request(SEARCH_PATH, query);
  if (!res.ok) return failFrom(res);
  const d = res.data || {};
  const rawRes = d.result || [];
  /* video/article 的 result=数组；live 的 result={live_room[], live_user[]}（取直播间列表） */
  const arr = st === 'live' && !Array.isArray(rawRes) ? (rawRes.live_room || []) : rawRes;
  const items = [];
  arr.forEach((raw) => {
    let it = null;
    if (st === 'article') it = normalizeSearchArticle(raw);
    else if (st === 'live') it = normalizeSearchLive(raw);
    else it = normalizeSearch(raw);
    if (it) items.push(it);
  });
  if (items.length === 0) {
    return { ok: false, stage: 'parse', message: pageNum > 1 ? '没有更多结果' : '无搜索结果' };
  }
  const numPages = Number(d.numPages) || 0;
  return {
    ok: true,
    items: items,
    page: pageNum,
    /* numPages 缺失时按空页兜底（article/live 响应不保证带 numPages） */
    noMore: numPages > 0 ? pageNum >= numPages : false,
    total: Number(d.numResults) || 0
  };
}

// 兼容包装：视频搜索（既有调用点/测试不变）
export async function searchVideos(client, mixinKey, keyword, page) {
  return searchByType(client, mixinKey, keyword, page, 'video');
}

/* ---------------- 图文（专栏）正文 ----------------
 * GET x/article/view?id=<cvid>（collect docs/article/view.md；-509 限流 → 调用方降级）
 * type=0：content 为 HTML（含 <img>）；type=3：content 为 JSON {ops:[{insert}]}。
 * 统一转 **blocks 交错数组**（保序）：{type:'text',text} / {type:'img',url,w,h} → panel 图文混排。 */

function toHttps(u) {
  if (typeof u !== 'string' || !u) return '';
  if (u.indexOf('//') === 0) return 'https:' + u;
  return u;
}

const IMG_MARK = '@@IMG@@';

export function htmlToBlocks(html) {
  if (typeof html !== 'string' || !html) return [];
  const prepared = String(html)
    .replace(/<br\s*\/?>/gi, '\n')
    .replace(/<\/p>/gi, '\n')
    /* <img src=…> → 行内标记保序；HTML 图无尺寸 → 16:9 默认 460×258 */
    .replace(/<img[^>]*?src=["']([^"']+)["'][^>]*?>/gi, (m, u) => '\n' + IMG_MARK + u + '\n')
    .replace(/<[^>]+>/g, '')
    .replace(/&nbsp;/g, ' ')
    .replace(/&amp;/g, '&')
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&#39;/g, "'");
  const blocks = [];
  prepared.split(/\n/).forEach((line) => {
    const s = line.trim();
    if (!s) return;
    if (s.indexOf(IMG_MARK) === 0) {
      const url = toHttps(s.slice(IMG_MARK.length));
      if (url) blocks.push({ type: 'img', url: url, w: 460, h: 258 });
      return;
    }
    /* HTML 模式每个换行行独立成块：<p> 是主段落结构（合并会吞掉段界 → 排版粘连）；
     * ops 模式不同——insert 内 \n 是软换行才合并（见 opsToBlocks）。 */
    blocks.push({ type: 'text', text: s });
  });
  return blocks;
}

export function opsToBlocks(opsJson) {
  if (typeof opsJson !== 'string' || !opsJson) return [];
  let ops;
  try {
    const obj = JSON.parse(opsJson);
    ops = (obj && obj.ops) || [];
  } catch (e) {
    return [];
  }
  const blocks = [];
  ops.forEach((op) => {
    const ins = op && op.insert;
    if (typeof ins === 'string') {
      ins.split('\n').forEach((s) => {
        if (!s) return;
        const last = blocks[blocks.length - 1];
        if (last && last.type === 'text') last.text += s;
        else blocks.push({ type: 'text', text: s });
      });
      return;
    }
    if (ins && typeof ins === 'object') {
      if (ins['native-image']) {
        const nw = Number(ins['native-image'].width) || 0;
        const nh = Number(ins['native-image'].height) || 0;
        blocks.push({
          type: 'img',
          url: toHttps(ins['native-image'].url || ''),
          w: 460,
          h: nw > 0 && nh > 0 ? Math.round((460 * nh) / nw) : 258
        });
        return;
      }
      blocks.push({ type: 'text', text: ins['video-card'] ? '[视频卡片]' : '[卡片]' });
    }
  });
  return blocks.filter((b) => b.type !== 'img' || b.url);
}

/* opus 富文本（type=3 新版正文，collect article/view.md：图在 paragraphs[].para_type=2 的 pic.pics[]）
 * —— 实测线索：content.ops 里可能没图（正文图片只在 opus 里）→ fetchArticle 优先走本分支。 */
export function opusToBlocks(opus) {
  try {
    const paras = (opus && opus.content && opus.content.paragraphs) || [];
    const blocks = [];
    paras.forEach((p) => {
      if (p && p.para_type === 2 && p.pic && p.pic.pics && p.pic.pics[0]) {
        const pic = p.pic.pics[0];
        const w = Number(pic.width) || 0;
        const h = Number(pic.height) || 0;
        const url = toHttps(pic.url || '');
        if (url) blocks.push({ type: 'img', url: url, w: 460, h: w > 0 && h > 0 ? Math.round((460 * h) / w) : 258 });
        return;
      }
      const nodes = (p && p.text && p.text.nodes) || [];
      let s = '';
      nodes.forEach((n) => {
        if (n && n.word && typeof n.word.words === 'string') s += n.word.words;
      });
      s = s.trim();
      if (s) blocks.push({ type: 'text', text: s });
    });
    return blocks.filter((b) => b.type !== 'img' || b.url);
  } catch (e) {
    return [];
  }
}

// 拉专栏正文 → {ok, title, author, blocks, words, ftype} | {ok:false, stage, message}
export async function fetchArticle(client, cvid) {
  const id = Math.floor(Number(cvid) || 0);
  if (!(id > 0)) return { ok: false, stage: 'param', message: 'cvid 无效' };
  const res = await client.request('/x/article/view', { id: id });
  if (!res.ok) return failFrom(res);
  const d = res.data || {};
  const ftype = Number(d.type) || 0;
  /* 正文三形态优先级：opus 富文本(type3 新版，图在 paragraphs) > content.ops(type3 老) > content HTML(type0) */
  let blocks = [];
  if (d.opus && d.opus.content && d.opus.content.paragraphs) blocks = opusToBlocks(d.opus);
  if (blocks.length === 0) blocks = ftype === 3 ? opsToBlocks(d.content || '') : htmlToBlocks(d.content || '');
  if (blocks.length === 0) {
    if (d.summary) blocks.push({ type: 'text', text: String(d.summary) });
    else return { ok: false, stage: 'parse', message: '正文为空' };
  }
  return {
    ok: true,
    title: d.title || '',
    author: (d.author && d.author.name) || '',
    blocks: blocks,
    words: Number(d.words) || 0,
    ftype: ftype
  };
}

/* ---------------- 直播流 ----------------
 * GET api.live.bilibili.com/room/v1/Room/playUrl?cid=<roomid>&platform=pc（collect live_stream 同族；
 * 开发机实测：quality 请求常回落 current_qn=250 超清（accept=['4']）；durl[0].url 为 https flv 直链、
 * length=0（持续流）。高码率直播对 A53 软解有压力 → 音频钟消费丢帧自保（见 play_session 头注释）。 */
export async function resolveLiveUrl(client, roomid) {
  const cid = Math.floor(Number(roomid) || 0);
  if (!(cid > 0)) return { ok: false, stage: 'param', message: 'roomid 无效' };
  const res = await client.request('https://api.live.bilibili.com/room/v1/Room/playUrl', {
    cid: cid,
    platform: 'pc',
    quality: 0,
    ptype: 16
  });
  if (!res.ok) return failFrom(res);
  const d = res.data || {};
  const durl = d.durl || [];
  const url = durl[0] && durl[0].url;
  if (!url || !/^https:\/\//.test(url)) return { ok: false, stage: 'parse', message: '直播流地址缺失' };
  return { ok: true, url: url, qn: Number(d.current_qn) || 0 };
}

// 四段错误 → 面板一行文案
export function describeListError(res) {
  if (!res || res.ok) return '';
  const stage = res.stage || '';
  if (stage === 'transport') return '网络失败：' + (res.message || '');
  if (stage === 'http') return 'HTTP ' + (res.status || '') + '：' + (res.message || '');
  if (stage === 'parse') return res.message || '数据解析失败';
  /* 直播 playUrl 房间级线路异常（collect 未收录；真机复现：cid=1967217283 带/不带 cookie 必现、
   * 其它房间正常，message 含 bvc-play-url-one）→ 人话提示换房间，而非裸码 */
  if (Number(res.code) === 19001012) return '该直播间暂无可用线路（B站 19001012），换个直播间试试';
  return 'B站 code=' + (res.code != null ? res.code : '?') + '：' + (res.message || '接口错误');
}

export { logWarn };
