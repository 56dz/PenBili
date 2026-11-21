// 搜索/图文/评论接口真值探针（开发机 node 直连，取证用）
// 用法：node tools/probe_search.mjs [关键词]
// 取证目标：
//   1) search_type=article 结果条目的真实字段名（封面/评论数字段）
//   2) search_type=live 结果条目的真实字段名
//   3) /x/article/view 正文 content 的结构（img 标签形态）
//   4) /x/v2/reply 对专栏 aid 的正确 type/oid 组合（-404 根因）
import crypto from 'node:crypto';

const UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36';
const KW = process.argv[2] || '旅行';

const MIXIN_TABLE = [
  46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35,
  27, 43, 5, 49, 33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
  37, 48, 7, 16, 24, 55, 40, 61, 26, 17, 0, 1, 60, 51, 30, 4,
  22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36, 20, 34, 44, 52
];
function mixinKey(img, sub) {
  const s = img + sub;
  return MIXIN_TABLE.map((i) => s.charAt(i)).join('').slice(0, 32);
}
function md5(s) {
  return crypto.createHash('md5').update(s).digest('hex');
}
function sign(params, mk) {
  const all = { ...params, wts: Math.floor(Date.now() / 1000) };
  const q = Object.keys(all)
    .sort()
    .map((k) => encodeURIComponent(k) + '=' + encodeURIComponent(String(all[k]).replace(/[!'()*]/g, '')))
    .join('&');
  return q + '&w_rid=' + md5(q + mk);
}
async function getJSON(url) {
  const r = await fetch(url, {
    headers: { 'User-Agent': UA, 'Referer': 'https://www.bilibili.com/' }
  });
  return r.json();
}

const nav = await getJSON('https://api.bilibili.com/x/web-interface/nav');
const wi = nav.data.wbi_img;
const mk = mixinKey(wi.img_url.split('/').pop().split('.')[0], wi.sub_url.split('/').pop().split('.')[0]);
console.log('== mixin key ok:', mk.slice(0, 8) + '...');

const S = 'https://api.bilibili.com/x/web-interface/wbi/search/type';
for (const st of ['article', 'live']) {
  const res = await getJSON(S + '?' + sign({ search_type: st, keyword: KW, order: 'totalrank', page: 1 }, mk));
  const arr = (res.data && res.data.result) || [];
  console.log('\n== search_type=' + st + ' code=' + res.code + ' n=' + arr.length);
  if (arr.length) {
    const first = arr[0];
    const keys = Object.keys(first);
    console.log('fields:', keys.join(','));
    const slim = {};
    for (const k of keys) {
      const v = first[k];
      if (typeof v === 'string' || typeof v === 'number') slim[k] = String(v).slice(0, 60);
    }
    console.log('first:', JSON.stringify(slim, null, 1).slice(0, 1200));
    if (st === 'article') {
      globalThis.__aid = Number(first.aid || first.id) || 0;
    }
  }
}

const aid = globalThis.__aid;
if (aid > 0) {
  console.log('\n== article/view id=' + aid);
  const av = await getJSON('https://api.bilibili.com/x/article/view?id=' + aid);
  const d = av.data || {};
  console.log('code=' + av.code, 'keys:', Object.keys(d).slice(0, 30).join(','));
  console.log('title=', String(d.title || '').slice(0, 40));
  console.log('banner_url=', String(d.banner_url || '').slice(0, 80));
  console.log('content head=', String(d.content || '').slice(0, 600).replace(/\n/g, ' '));
  const imgs = String(d.content || '').match(/<img[^>]+src="([^"]+)"/) || [];
  console.log('img count=', imgs.length, 'first src=', (imgs[1] || '').slice(0, 100));

  for (const t of [17, 12, 11]) {
    const rp = await getJSON('https://api.bilibili.com/x/v2/reply?type=' + t + '&oid=' + aid + '&pn=1&ps=10&sort=2');
    const n = rp.data && rp.data.replies ? rp.data.replies.length : -1;
    console.log('reply type=' + t + ' oid=' + aid + ' → code=' + rp.code + ' count=' + (rp.data && rp.data.page ? rp.data.page.count : '?') + ' replies=' + n + (rp.message ? ' msg=' + rp.message : ''));
  }
}
