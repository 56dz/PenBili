// 验证 parseArticle 的封面/块逻辑对真实签名响应的产出（开发机取证用）
import crypto from 'node:crypto';

const T = [46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35,
  27, 43, 5, 49, 33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
  37, 48, 7, 16, 24, 55, 40, 61, 26, 17, 0, 1, 60, 51, 30, 4,
  22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36, 20, 34, 44, 52];
const UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36';
const H = {
  'User-Agent': UA,
  Referer: 'https://www.bilibili.com/',
  Accept: 'application/json, text/plain, */*',
  'Accept-Language': 'zh-CN,zh;q=0.9',
  Origin: 'https://www.bilibili.com',
  'Sec-Fetch-Dest': 'empty',
  'Sec-Fetch-Mode': 'cors',
  'Sec-Fetch-Site': 'same-site'
};
const nav = await (await fetch('https://api.bilibili.com/x/web-interface/nav', { headers: { 'User-Agent': UA } })).json();
const wi = nav.data.wbi_img;
const mk = T.map((i) => (wi.img_url.split('/').pop().split('.')[0] + wi.sub_url.split('/').pop().split('.')[0]).charAt(i)).join('').slice(0, 32);

function signUrl(params) {
  const all = { ...params, wts: Math.floor(Date.now() / 1000) };
  const q = Object.keys(all)
    .sort()
    .map((k) => encodeURIComponent(k) + '=' + encodeURIComponent(all[k]))
    .join('&');
  return 'https://api.bilibili.com/x/article/view?' + q + '&w_rid=' + crypto.createHash('md5').update(q + mk).digest('hex');
}

const AID = Number(process.argv[2] || 19361153);
const res = await (await fetch(signUrl({ id: AID }), { headers: H })).json();
console.log('code=' + res.code);
const d = res.data || {};
console.log('opus:', !!d.opus, '| paragraphs:', d.opus && d.opus.content ? d.opus.content.paragraphs.length : '-');

let cover = '';
const opusArt = (d.opus && d.opus.article) || {};
if (opusArt.cover && opusArt.cover[0] && opusArt.cover[0].url) {
  const cw = Number(opusArt.cover[0].width) || 1490;
  const ch = Number(opusArt.cover[0].height) || 437;
  const dh = Math.min(400, Math.max(80, Math.round((436 * ch) / cw)));
  cover = opusArt.cover[0].url.replace(/^https:\/\//i, 'http://') + '@436w_' + dh + 'h.jpg';
}
console.log('cover=' + cover);

const paras = (d.opus && d.opus.content && d.opus.content.paragraphs) || [];
let txt = 0;
let img = 0;
let imgSample = '';
paras.forEach((p) => {
  if (p.para_type === 2 && p.pic && p.pic.pics && p.pic.pics[0]) {
    img++;
    if (!imgSample) imgSample = p.pic.pics[0].url.slice(0, 80) + ' w=' + p.pic.pics[0].width + ' h=' + p.pic.pics[0].height;
  }
  if (p.para_type === 1) txt++;
});
console.log('blocks: text=' + txt + ' img=' + img);
console.log('first img:', imgSample);
