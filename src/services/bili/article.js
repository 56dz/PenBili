// 图文（专栏）详情 adapter
// 事实来源：xieren58/bilibili-API-collect 契约 + 2026-10-04 探针（tools/probe_search.mjs）+ dsh bilibili_x5 实证：
//   读 GET /x/article/view?id=<aid> —— 需 WBI 签名（无签名 -509/-352 风控，见 fetchArticle）
//     data: { id, title, type(ftype: 0=HTML正文 3=富文本), author:{name}, content(HTML/ops JSON),
//             opus:{content:{paragraphs[]}, article:{cover[]}}（type3 新版）, image_urls[], words, stats ... }
//   正文三形态优先级（对齐 dsh bilibili_x5 实证）：opus 富文本段 > content.ops（type3 老） > content HTML（type0）
//   评论：/x/v2/reply 族 **type=12** oid=<aid>（探针实测；17 旧文档值 -404）
// 图片渲染关键（dsh 实证）：**falcon image 以样式定尺寸**——image 元素必须带 inline style 宽高，
//   仅属性不足以撑开（.thumb 能显示是因其宽高来自 CSS）。图块统一 https 直链 + 每图精确 dw/dh。
// 日志卫生：不记正文内容。

import { buildSignedQuery } from './wbi.js';
import { httpGetTextNative } from '../net.js';

const ART_W = 436; /* 图文正文列显示宽（art-scroll 452 - 左右 padding 8） */
const IMG_DEFAULT_H = 258; /* 无尺寸元数据时的 16:9 兜底高 */

// 协议相对地址 → https（dsh bilibili_x5 实证：专栏图 https 直链笔上可加载）
function toHttps(u) {
  if (typeof u !== 'string' || !u) return '';
  if (u.indexOf('//') === 0) return 'https:' + u;
  return u;
}

function clampH(h) {
  return Math.min(620, Math.max(60, Math.round(Number(h) || IMG_DEFAULT_H)));
}

// opus 富文本段落 → 混排块[]：{t:'text', text} | {t:'img', src, dw, dh}
export function opusToBlocks(opus) {
  const paras = (opus && opus.content && opus.content.paragraphs) || [];
  const out = [];
  paras.forEach((p) => {
    if (!p || out.length >= 120) return;
    if (p.para_type === 2 && p.pic && Array.isArray(p.pic.pics) && p.pic.pics[0]) {
      const pic = p.pic.pics[0];
      const w = Number(pic.width) || 0;
      const h = Number(pic.height) || 0;
      const src = toHttps(pic.url || '');
      if (src) {
        out.push({
          t: 'img',
          src: src,
          dw: ART_W,
          dh: w > 0 && h > 0 ? clampH((ART_W * h) / w) : IMG_DEFAULT_H
        });
      }
      return;
    }
    const nodes = (p.text && p.text.nodes) || [];
    let line = '';
    nodes.forEach((nd) => {
      const w = nd && nd.word && typeof nd.word.words === 'string' ? nd.word.words : '';
      if (w) line += w;
    });
    line = line.trim();
    if (line) out.push({ t: 'text', text: line });
  });
  return out;
}

// content.ops（type3 老版富文本，Quill deltas）→ 混排块[]；insert 字符串的 \n 为软换行合并
export function opsToBlocks(opsJson) {
  if (typeof opsJson !== 'string' || !opsJson) return [];
  let ops;
  try {
    const obj = JSON.parse(opsJson);
    ops = (obj && obj.ops) || [];
  } catch (e) {
    return [];
  }
  const out = [];
  ops.forEach((op) => {
    const ins = op && op.insert;
    if (typeof ins === 'string') {
      ins.split('\n').forEach((s) => {
        if (!s) return;
        const last = out[out.length - 1];
        if (last && last.t === 'text') last.text += s;
        else out.push({ t: 'text', text: s });
      });
      return;
    }
    if (ins && typeof ins === 'object') {
      if (ins['native-image']) {
        const nw = Number(ins['native-image'].width) || 0;
        const nh = Number(ins['native-image'].height) || 0;
        const src = toHttps(ins['native-image'].url || '');
        if (src) {
          out.push({ t: 'img', src: src, dw: ART_W, dh: nw > 0 && nh > 0 ? clampH((ART_W * nh) / nw) : IMG_DEFAULT_H });
        }
        return;
      }
      out.push({ t: 'text', text: ins['video-card'] ? '[视频卡片]' : '[卡片]' });
    }
  });
  return out;
}

const ENTITIES = {
  '&amp;': '&',
  '&lt;': '<',
  '&gt;': '>',
  '&quot;': '"',
  '&#39;': "'",
  '&nbsp;': ' ',
  '&copy;': '©',
  '&middot;': '·',
  '&hellip;': '…',
  '&ldquo;': '“',
  '&rdquo;': '”',
  '&mdash;': '—',
  '&ensp;': ' ',
  '&emsp;': ' '
};
export function decodeEntities(s) {
  return String(s == null ? '' : s)
    .replace(/&#(\d+);/g, (m, n) => {
      const c = Number(n);
      return c > 0 && c < 65536 ? String.fromCharCode(c) : m;
    })
    .replace(/&(amp|lt|gt|quot|#39|nbsp|copy|middot|hellip|ldquo|rdquo|mdash|ensp|emsp);/g, (m) => ENTITIES[m] || m);
}

// HTML → 混排块[]：<img> 原位成图块（无尺寸元数据 → 默认 16:9）；
// 文本按块级标签切段（每段一个块），段内 <br> 为软换行（空格续接）
export function htmlToBlocks(html, maxBlocks) {
  const cap = Math.max(1, Math.floor(Number(maxBlocks) || 120));
  if (typeof html !== 'string' || !html) return [];
  const blocks = [];
  const imgRe = /<img[^>]*>/gi;
  let last = 0;
  let m;
  const pushText = (seg) => {
    decodeParagraphs(seg, cap).forEach((t) => {
      if (blocks.length < cap) blocks.push({ t: 'text', text: t });
    });
  };
  while ((m = imgRe.exec(html)) !== null) {
    pushText(html.slice(last, m.index));
    last = imgRe.lastIndex;
    const src = toHttps((m[0].match(/src\s*=\s*"([^"]+)"/i) || [])[1] || '');
    if (src && blocks.length < cap) {
      blocks.push({ t: 'img', src: src, dw: ART_W, dh: IMG_DEFAULT_H });
    }
  }
  pushText(html.slice(last));
  return blocks.slice(0, cap);
}

function decodeParagraphs(html, cap) {
  let s = String(html == null ? '' : html)
    .replace(/<(script|style)[\s\S]*?<\/\1>/gi, '')
    // <br> = 段内软换行（同一文本块内以空格续接）；块级标签结束 = 段落硬边界（切块）
    // —— 两者必须区分：若都当软换行，整篇正文会合并成**单个**文本块，
    //    而 .art-p 是 lines:30 截断，长正文（HTML 兜底路径的旧专栏）会被吃掉半篇。
    .replace(/<br\s*\/?>/gi, '\n')
    .replace(/<\/(p|div|h[1-6]|li|blockquote|figure|figcaption|section)>/gi, '\n\n')
    .replace(/<[^>]+>/g, '');
  s = decodeEntities(s);
  const out = [];
  let buf = '';
  s.split('\n').forEach((rawLine) => {
    const line = rawLine.trim();
    if (!line) {
      if (buf) {
        out.push(buf);
        buf = '';
      }
      return;
    }
    buf = buf ? buf + ' ' + line : line;
  });
  if (buf) out.push(buf);
  return out.slice(0, cap);
}

// /x/article/view 响应 → {ok, article} | {ok:false, stage, code?, message}
// 正文三形态优先级：opus 富文本段 > content.ops（type3 老） > content HTML（type0）——对齐 dsh 实证
export function parseArticle(res) {
  if (!res || res.ok === false) {
    return {
      ok: false,
      stage: res ? res.stage : 'transport',
      code: res ? res.code : undefined,
      message: (res && res.message) || '图文请求失败'
    };
  }
  const d = res.data || {};
  const aid = Number(d.id || d.aid) || 0;
  const title = typeof d.title === 'string' && d.title.trim() ? d.title.trim() : '未命名图文';
  if (!(aid > 0)) return { ok: false, stage: 'parse', message: '图文数据缺少 id' };
  const ftype = Number(d.type) || 0;
  let blocks = opusToBlocks(d.opus);
  if (!blocks.length && ftype === 3) blocks = opsToBlocks(d.content || '');
  if (!blocks.length) blocks = htmlToBlocks(d.content || '', 120);
  if (!blocks.length) {
    if (d.summary) blocks.push({ t: 'text', text: String(d.summary) });
    else return { ok: false, stage: 'parse', message: '图文正文为空' };
  }
  const authorObj = d.author || {};
  const stats = d.stats || {};
  // 封面：opus.article.cover[0]（带原始宽高）> image_urls[0]，https 直链 + 比例高
  let cover = '';
  let coverDh = 140;
  const opusArt = (d.opus && d.opus.article) || {};
  let coverRaw = '';
  let cw = 0;
  let ch = 0;
  if (opusArt.cover && opusArt.cover[0] && opusArt.cover[0].url) {
    coverRaw = opusArt.cover[0].url;
    cw = Number(opusArt.cover[0].width) || 0;
    ch = Number(opusArt.cover[0].height) || 0;
  }
  if (!coverRaw && Array.isArray(d.image_urls) && d.image_urls[0]) coverRaw = d.image_urls[0];
  cover = toHttps(coverRaw);
  if (cover) coverDh = cw > 0 && ch > 0 ? clampH((ART_W * ch) / cw) : 140;
  return {
    ok: true,
    article: {
      aid: aid,
      title: title,
      author: typeof authorObj.name === 'string' ? authorObj.name : '',
      cover: cover,
      coverDh: coverDh,
      blocks: blocks,
      read: Number(stats.view) || Number(d.read) || 0,
      like: Number(stats.like) || Number(d.like) || 0,
      words: Number(d.words) || 0
    }
  };
}

// 拉取图文详情（WBI 签名必需）→ parseArticle 结果
// transport：native libcurl（httpGetTextNative）+ 完整浏览器头——jsapi.http 瘦头会被
// article/view 风控判 -352/-509（2026-10-04 笔上对拍实测，见当天 memory）。
export function fetchArticle(client, mixinKey, aid) {
  const id = Math.floor(Number(aid) || 0);
  if (!(id > 0)) {
    return Promise.resolve({ ok: false, stage: 'param', message: '图文 id 无效' });
  }
  if (!/^[0-9a-f]{32}$/.test(String(mixinKey || ''))) {
    return Promise.resolve({ ok: false, stage: 'wbi', message: 'WBI 密钥无效' });
  }
  const query = buildSignedQuery({ id: id }, mixinKey, Math.floor(Date.now() / 1000));
  const base = typeof client.getHeaders === 'function' ? client.getHeaders() : {};
  const headers = Object.assign({}, base, {
    'Accept-Language': 'zh-CN,zh;q=0.9',
    Origin: 'https://www.bilibili.com',
    'Sec-Fetch-Dest': 'empty',
    'Sec-Fetch-Mode': 'cors',
    'Sec-Fetch-Site': 'same-site'
  });
  return client
    .request('/x/article/view', query, {
      get: function (url, o) {
        return httpGetTextNative(url, { headers: headers, timeout: o && o.timeout });
      }
    })
    .then(parseArticle);
}
