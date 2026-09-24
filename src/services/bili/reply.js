// 评论（读 + 写）
// 事实来源：api-mock/fixtures/reply.json（开发机 2026-09-24 真响应）+ xieren58/bilibili-API-collect 契约。
//
// 读 GET /x/v2/reply?type=1&oid=<aid>&pn=&ps=10&sort=2   （匿名可用，实测 code:0）
//   data.replies[]: {rpid, mid, ctime, like, rcount,
//                    member:{uname, face}, content:{message}}   ← message 为纯文本（emoji 为 [文字] 形态）
//   data.page: {num, size, count, acount}   ← pn/ps 翻页（非 cursor 模式）
//   sort=2 热门排序；主楼每页 10 条（scroller 滑动浏览）、子楼每页 20 条（行内展开）。
//   （闪烁根因修复在根容器 page-live 透明——见 base.less，滚动回滚保留）。
//
// 写 POST https://api.bilibili.com/x/v2/reply/add   （form + Cookie + csrf=bili_jct）
//   参数: oid=<aid> type=1 message=<文本> csrf=<bili_jct> platform=web
//   成功 {code:0, data:{rpid}}；-101 未登录 / -111 csrf 不符 / -412 风控。
//   无 w_rid（web 表单契约）；若真机被要求签名，按 code 升级为 wbi（回退计划已注释在 client 侧）。
//   通过 client.postForm 走 native httpjson（原样 form body；jsapi POST 会二次 JSON 化不可用）。
//
// 日志卫生：只记条数/状态码，不记评论内容。

export const REPLY_PAGE_SIZE = 10; /* 主楼每页（滑动浏览） */
export const CHILD_PS = 20; /* 子楼每页（行内展开） */
export const REPLY_ADD_URL = 'https://api.bilibili.com/x/v2/reply/add';

// 原始回复 → 视图条目 | null（缺 rpid/content 丢弃）
export function normalizeReply(raw) {
  if (!raw || typeof raw !== 'object') return null;
  const rpid = Number(raw.rpid || raw.rpid_str) || 0;
  if (!(rpid > 0)) return null;
  const m = raw.member || {};
  const c = raw.content || {};
  const msg = typeof c.message === 'string' ? c.message.trim() : '';
  if (!msg) return null;
  return {
    rpid: rpid,
    name: typeof m.uname === 'string' && m.uname ? m.uname : '用户' + (raw.mid || ''),
    message: msg,
    likes: Number(raw.like) || 0,
    ctime: Number(raw.ctime) || 0,
    rcount: Number(raw.rcount) || 0
  };
}

// x/v2/reply 响应 → {ok, items, page, count, pn, noMore} | {ok:false, stage, code?, message}
export function parseReplies(res) {
  if (!res || res.ok === false) {
    return {
      ok: false,
      stage: res ? res.stage : 'transport',
      code: res ? res.code : undefined,
      message: (res && res.message) || '评论请求失败'
    };
  }
  const d = res.data || {};
  const list = d.replies || [];
  const items = [];
  list.forEach((raw) => {
    const it = normalizeReply(raw);
    if (it) items.push(it);
  });
  const pg = d.page || {};
  const pn = Number(pg.num) || 1;
  const size = Number(pg.size) || REPLY_PAGE_SIZE;
  const count = Number(pg.count) || 0;
  const shown = pn * size;
  return {
    ok: true,
    items: items,
    pn: pn,
    count: count,
    noMore: items.length === 0 || shown >= count
  };
}

// 读一页评论 → 同上 | {ok:false, stage, message}
export async function fetchReplies(client, aid, pn) {
  const page = Math.max(1, Math.floor(Number(pn) || 1));
  const res = await client.request('/x/v2/reply', {
    type: 1,
    oid: Math.floor(Number(aid) || 0),
    pn: page,
    ps: REPLY_PAGE_SIZE,
    sort: 2
  });
  if (!res.ok) return parseReplies(res);
  return parseReplies(res);
}

// 写评论（form 串：csrf=bili_jct，全部 URL 编码）→ {ok, rpid} | {ok:false, stage, code?, message}
export async function addReply(client, aid, message, csrf) {
  const oid = Math.floor(Number(aid) || 0);
  const text = String(message == null ? '' : message).trim();
  if (!(oid > 0)) return { ok: false, stage: 'param', message: '缺少稿件 id' };
  if (!text) return { ok: false, stage: 'param', message: '评论内容为空' };
  if (text.length > 1000) return { ok: false, stage: 'param', message: '评论超长（>1000）' };
  if (!csrf) return { ok: false, stage: 'param', message: '缺少 csrf（需要登录）' };
  const form =
    'oid=' + oid +
    '&type=1' +
    '&message=' + encodeURIComponent(text) +
    '&csrf=' + encodeURIComponent(csrf) +
    '&platform=web';
  const res = await client.postForm(REPLY_ADD_URL, form, { acceptCodes: [0] });
  if (!res || res.ok === false) {
    return {
      ok: false,
      stage: res ? res.stage : 'transport',
      code: res ? res.code : undefined,
      message: (res && res.message) || '发送失败'
    };
  }
  const rpid = Number(res.data && res.data.rpid) || 0;
  if (!(rpid > 0)) return { ok: false, stage: 'parse', message: '响应缺少 rpid' };
  return { ok: true, rpid: rpid };
}

// 子楼读取 GET /x/v2/reply/reply?oid=&type=1&root=<主楼rpid>&pn=&ps=20
//   响应结构与主接口同构（data.replies + data.page）→ 复用 parseReplies；
//   root=被展开评论的 rpid；匿名可用（与主接口同族）。
export async function fetchSubReplies(client, aid, rootRpid, pn) {
  const page = Math.max(1, Math.floor(Number(pn) || 1));
  const root = Math.floor(Number(rootRpid) || 0);
  if (!(root > 0)) return { ok: false, stage: 'param', message: '缺少 root rpid' };
  const res = await client.request('/x/v2/reply/reply', {
    type: 1,
    oid: Math.floor(Number(aid) || 0),
    root: root,
    pn: page,
    ps: CHILD_PS
  });
  return parseReplies(res);
}
