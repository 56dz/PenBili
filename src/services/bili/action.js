// 互动（点赞 / 投币 / 收藏夹列表 / 收藏）
// 事实来源：xieren58/bilibili-API-collect 契约 + 本项目 client.postForm 约定
//           （form 串 + Cookie + csrf=bili_jct，原样 x-www-form-urlencoded）。
//
// 读  GET  /x/web-interface/view ?aid=  → data.stat.{like,coin,favorite,view}（**全站总数**）
//      GET  /x/web-interface/archive/has/like ?aid=  → data（1=已赞 0=未赞），未登录 -101
//      GET  /x/web-interface/archive/coins   ?aid=  → data.multiply（本人已投枚数）
//      GET  /x/v2/fav/video/favoured         ?aid=  → data.favoured（本人是否已收藏）
//      GET  /x/v3/fav/folder/created/list-all ?up_mid=<mid> →
//           data.list[]: {id, fid, mid, attr, title, media_count, fav_state, is_default}
//
// 注意：老的 `/x/v2/view?aid=` 已于 2026 年前后下线，现在返回的是 HTML 错误页
//       （不是 JSON）→ 解析必然失败、左栏计数全空。必须改用 web-interface 系列。
// 写  POST /x/web-interface/archive/like   aid=&like=<1|2>&csrf=   （1=点赞 2=取消赞）
//      POST /x/web-interface/coin/add      aid=&multiply=<1|2>&select_like=<0|1>&csrf=
//      POST /x/v3/fav/resource/deal        rid=<aid>&type=2&add_media_ids=&del_media_ids=&csrf=
//      POST /x/v3/fav/resource/batch-deal  resources=<aid:2,...>&add_media_ids=&del_media_ids=&csrf=
//
// 返回一律 {ok:true,...} | {ok:false, stage, code?, message}
// 日志卫生：不记 Cookie / csrf，只记 aid、状态码、收藏夹条数。

export const VIEW_URL = '/x/web-interface/view';
export const HAS_LIKE_URL = '/x/web-interface/archive/has/like';
export const COINS_URL = '/x/web-interface/archive/coins';
export const FAVOURED_URL = '/x/v2/fav/video/favoured';
export const FAV_FOLDERS_URL = '/x/v3/fav/folder/created/list-all';
export const LIKE_URL = '/x/web-interface/archive/like';
export const COIN_URL = '/x/web-interface/coin/add';
export const FAV_DEAL_URL = '/x/v3/fav/resource/deal';

function fail(stage, message, code) {
  return { ok: false, stage: stage, code: code, message: message };
}

// 稿件互动状态 → {like:0|1, coin:0..2, favorite:bool, likeCount, coinCount, favCount, viewCount}
//
// 两路数据合并：
//   · **全站总数**（左栏徽标）来自 `/x/web-interface/view` 的 `data.stat.*` —— 未登录也能拿；
//   · **本人状态**（图标是否变粉）来自三个独立接口 —— 未登录统一 -101，此时一律按未操作处理。
// 老实现只读 `/x/v2/view`（已下线 → 返回 HTML），导致两个都没有。
export function parseViewState(res, mine) {
  if (!res || res.ok === false) {
    return fail(res ? res.stage : 'transport', (res && res.message) || '状态获取失败', res ? res.code : undefined);
  }
  const d = res.data || {};
  const stat = d.stat || {};
  // mine = {like, coin, favoured} | null（未登录 / 请求失败）
  const m = mine || {};
  const liked = Number(m.like) > 0 ? 1 : 0;
  const coin = Math.min(Math.max(Math.floor(Number(m.coin) || 0), 0), 2);
  return {
    ok: true,
    like: liked,
    coin: coin,
    favorite: !!m.favoured,
    // 左栏计数徽标：stat 里的全是「总数」（所有人点赞/投币/收藏的累计），不是本人状态
    likeCount: Number(stat.like) || 0,
    coinCount: Number(stat.coin) || 0,
    favCount: Number(stat.favorite) || 0,
    viewCount: Number(stat.view) || 0,
    share: Number(stat.share) || 0,
    // 简介：用户手绘稿 m11950 要求播放页左列上方显示「视频标题和简介」。
    // /x/web-interface/view 的 data.desc 就是稿件简介（未登录也能拿）。
    desc: typeof d.desc === 'string' ? d.desc : ''
  };
}

// 三个「本人状态」接口并行拉；任一失败（多为未登录 -101）就当未操作，不阻塞计数徽标。
async function fetchMine(client, aid) {
  const q = { aid: aid };
  const [likeR, coinR, favR] = await Promise.all([
    client.request(HAS_LIKE_URL, q, { acceptCodes: [-101, 0] }),
    client.request(COINS_URL, q, { acceptCodes: [-101, 0] }),
    client.request(FAVOURED_URL, q, { acceptCodes: [-101, 0] })
  ]);
  // has/like 成功时 data 是 0/1（老版本是 {is_like:bool}，两种都认）
  let like = 0;
  if (likeR && likeR.ok) {
    const dd = likeR.data;
    like = dd && typeof dd === 'object' ? (dd.is_like ? 1 : 0) : Number(dd) || 0;
  }
  let coin = 0;
  if (coinR && coinR.ok && coinR.data && typeof coinR.data === 'object') {
    coin = Number(coinR.data.multiply) || 0;
  }
  let favoured = false;
  if (favR && favR.ok && favR.data && typeof favR.data === 'object') {
    favoured = !!favR.data.favoured;
  }
  return { like: like, coin: coin, favoured: favoured };
}

export async function fetchViewState(client, aid) {
  const id = Math.floor(Number(aid) || 0);
  if (!(id > 0)) return fail('param', '缺少稿件 id');
  const [base, mine] = await Promise.all([
    client.request(VIEW_URL, { aid: id }, {}),
    fetchMine(client, id)
  ]);
  return parseViewState(base, mine);
}

// 收藏夹列表 → {ok, items:[{id,title,count,isDefault,has}], count}
export function parseFavFolders(res) {
  if (!res || res.ok === false) {
    return fail(res ? res.stage : 'transport', (res && res.message) || '收藏夹获取失败', res ? res.code : undefined);
  }
  // list-all 未登录时 data 为 null（code 0）
  const d = res.data;
  const list = (d && d.list) || [];
  const items = [];
  for (const f of list) {
    const id = Number(f && f.id) || 0;
    if (!(id > 0)) continue;
    items.push({
      id: id,
      title: typeof f.title === 'string' && f.title ? f.title : '未命名收藏夹',
      count: Number(f.media_count) || 0,
      isDefault: !!f.is_default || Number(f.attr) === 1,
      has: Number(f.fav_state) > 0
    });
  }
  return { ok: true, items: items, count: items.length };
}

export function fetchFavFolders(client, mid) {
  const up = Math.floor(Number(mid) || 0);
  if (!(up > 0)) return Promise.resolve(fail('param', '缺少用户 id（需登录）'));
  return client.request(FAV_FOLDERS_URL, { up_mid: up }, { acceptCodes: [-101, 0] })
    .then(function (r) {
      // -101 未登录：list-all 在未登录时会直接返回 code 0 + data:null，
      // 但个别版本回 -101，这里统一当「无收藏夹」处理，不当作错误刷屏。
      if (r && r.ok === false && r.code === -101) {
        return { ok: true, items: [], count: 0, needLogin: true };
      }
      const p = parseFavFolders(r);
      if (p.ok) p.needLogin = false;
      return p;
    });
}

// 点赞 / 取消 → {ok, like}
// 契约：like=1 点赞、like=2 取消赞（**不是 0**；发 0 服务端按"未点赞"处理，取消不掉）。
export function setLike(client, aid, on, csrf) {
  const id = Math.floor(Number(aid) || 0);
  if (!(id > 0)) return Promise.resolve(fail('param', '缺少稿件 id'));
  if (!csrf) return Promise.resolve(fail('param', '需要登录'));
  const form = 'aid=' + id + '&like=' + (on ? 1 : 2) + '&csrf=' + encodeURIComponent(csrf);
  return client.postForm(LIKE_URL, form, { acceptCodes: [0] }).then(function (r) {
    if (!r || r.ok === false) {
      return fail(r ? r.stage : 'transport', (r && r.message) || '点赞失败', r ? r.code : undefined);
    }
    return { ok: true, like: on ? 1 : 0 };
  });
}

// 投币 → {ok, coin}（multiply=2 时顺带点赞）
export function addCoin(client, aid, multiply, alsoLike, csrf) {
  const id = Math.floor(Number(aid) || 0);
  if (!(id > 0)) return Promise.resolve(fail('param', '缺少稿件 id'));
  if (!csrf) return Promise.resolve(fail('param', '需要登录'));
  const n = Number(multiply) === 2 ? 2 : 1;
  const form =
    'aid=' + id +
    '&multiply=' + n +
    '&select_like=' + (alsoLike ? 1 : 0) +
    '&csrf=' + encodeURIComponent(csrf);
  return client.postForm(COIN_URL, form, { acceptCodes: [0] }).then(function (r) {
    if (!r || r.ok === false) {
      return fail(r ? r.stage : 'transport', (r && r.message) || '投币失败', r ? r.code : undefined);
    }
    return { ok: true, coin: n };
  });
}

// 收藏 / 取消收藏 → {ok, added}
// addIds / delIds 为收藏夹 id 数组；bilibili 单次请求同时支持加与删。
export function dealFav(client, aid, addIds, delIds, csrf) {
  const id = Math.floor(Number(aid) || 0);
  if (!(id > 0)) return Promise.resolve(fail('param', '缺少稿件 id'));
  if (!csrf) return Promise.resolve(fail('param', '需要登录'));
  const add = (addIds || []).map(Number).filter(function (v) { return v > 0; }).join(',');
  const del = (delIds || []).map(Number).filter(function (v) { return v > 0; }).join(',');
  if (!add && !del) return Promise.resolve(fail('param', '未选择收藏夹'));
  const form =
    'rid=' + id +
    '&type=2' +
    '&add_media_ids=' + encodeURIComponent(add) +
    '&del_media_ids=' + encodeURIComponent(del) +
    '&csrf=' + encodeURIComponent(csrf);
  return client.postForm(FAV_DEAL_URL, form, { acceptCodes: [0] }).then(function (r) {
    if (!r || r.ok === false) {
      return fail(r ? r.stage : 'transport', (r && r.message) || '收藏失败', r ? r.code : undefined);
    }
    return { ok: true, added: !!add, addIds: add ? add.split(',').map(Number) : [] };
  });
}
