// B 站 API 客户端（笔端直连 api.bilibili.com）。
// 事实来源：bilibili-API-collect；风险码语义与本机网络面见 profiles/youdao-x5.md。
//
// 职责：
//   1. 请求头装配（B 站要求浏览器 UA + Referer 才能过风控；Cookie=buvid3 匿名触点）
//   2. 错误语义归一成四段：transport / http / parse / api（非 2xx 时 body 不可得）
//   3. 风控码翻译成人话
// transport 可注入（get），单测不碰网络。

import { httpGet, httpPostForm, httpGetBinary, tryParseJson } from '../net.js';

export const BILI_API = 'https://api.bilibili.com';
export const DEFAULT_UA =
  'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36';
export const REFERER = 'https://www.bilibili.com/';
export const DEFAULT_TIMEOUT = 15000;

export function apiHeaders(cookie) {
  const h = {
    'User-Agent': DEFAULT_UA,
    Referer: REFERER,
    Accept: 'application/json, text/plain, */*'
  };
  if (cookie) h.Cookie = cookie;
  return h;
}

// session(v1/v2 皆可) → Cookie 请求头字符串：
//   匿名触点 buvid3/4 + 扫码登录态（v2 login 存在时）
export function cookieFromSession(session) {
  if (!session) return '';
  const parts = [];
  if (session.buvid3) parts.push('buvid3=' + session.buvid3);
  if (session.buvid4) parts.push('buvid4=' + session.buvid4);
  const lg = session.login;
  if (lg && lg.SESSDATA) {
    if (lg.DedeUserID) parts.push('DedeUserID=' + lg.DedeUserID);
    if (lg['DedeUserID__ckMd5']) parts.push('DedeUserID__ckMd5=' + lg['DedeUserID__ckMd5']);
    parts.push('SESSDATA=' + lg.SESSDATA);
    if (lg.bili_jct) parts.push('bili_jct=' + lg.bili_jct);
  }
  return parts.join('; ');
}

// 风控/业务码 → 人话（探测面板直接显示；未收录的码原样带出）
export function describeBiliCode(code, voucher) {
  let msg = null;
  switch (code) {
    case -101:
      msg = '未登录（匿名可用，属正常）';
      break;
    case -102:
      msg = '账号状态异常(-102)';
      break;
    case -352:
      msg = '风控(-352)：签名/UA 被识别异常';
      break;
    case -400:
      msg = '请求错误(-400)：参数或签名不合法';
      break;
    case -403:
      msg = '访问权限不足(-403)';
      break;
    case -404:
      msg = '资源不存在(-404)';
      break;
    case -412:
      msg = '风控(-412)：请求被拦截';
      break;
    case -509:
      msg = '接口限流(-509)';
      break;
    case 62002:
      msg = '稿件不可见(62002)';
      break;
    case 62004:
      msg = '稿件在审核(62004)';
      break;
    default:
      msg = null;
  }
  const base = msg || 'B站 code=' + code;
  return voucher ? base + '（v_voucher：需验证码）' : base;
}

function fail(stage, extra) {
  const out = { ok: false, stage: stage };
  if (extra) {
    for (const k of Object.keys(extra)) out[k] = extra[k];
  }
  return out;
}

// object → query 串；**字符串原样透传**（已是签名产物，不得二次编码）
export function buildQuery(q) {
  if (q == null) return '';
  if (typeof q === 'string') return q;
  const keys = Object.keys(q);
  return keys
    .map((k) => encodeURIComponent(k) + '=' + encodeURIComponent(String(q[k])))
    .join('&');
}

export function createClient(opts) {
  const o = opts || {};
  const get = o.get || ((url, o2) => httpGet(url, o2));
  const post = o.post || ((url, body, o2) => httpPostForm(url, body, o2));
  const now = o.now || (() => Date.now());
  const timeout = o.timeout || DEFAULT_TIMEOUT;
  let headers = o.headers || apiHeaders('');

  // JSON 响应四段归一化（GET request 与 form POST postForm 共用同一语义）
  function normalizeJson(res, ro, ms) {
    if (res.statusCode && (res.statusCode < 200 || res.statusCode >= 300)) {
      return fail('http', {
        status: res.statusCode,
        message: res.errorMessage || 'HTTP ' + res.statusCode,
        ms: ms
      });
    }
    const parsed = tryParseJson(res.body);
    if (!parsed || typeof parsed !== 'object') {
      return fail('parse', { message: '响应不是 JSON', ms: ms });
    }
    if (parsed.code !== 0) {
      const voucher = !!(parsed.data && parsed.data.v_voucher);
      const accept = (ro && ro.acceptCodes) || [];
      if (accept.indexOf(parsed.code) >= 0) {
        return { ok: true, code: parsed.code, data: parsed.data || null, httpStatus: res.statusCode, ms: ms };
      }
      return fail('api', {
        code: parsed.code,
        voucher: voucher,
        message: describeBiliCode(parsed.code, voucher),
        ms: ms
      });
    }
    return { ok: true, code: 0, data: parsed.data, httpStatus: res.statusCode, ms: ms };
  }

  async function request(path, query, reqOpts) {
    const ro = reqOpts || {};
    const qs = query != null && query !== '' ? '?' + buildQuery(query) : '';
    // 绝对地址原样透传（直播域 api.live.bilibili.com 不在主域前缀下）
    const url = /^https?:\/\//i.test(path) ? path + qs : BILI_API + path + qs;
    const t0 = now();
    let res;
    try {
      res = await get(url, { headers: headers, timeout: ro.timeout || timeout });
    } catch (e) {
      return fail('transport', { message: (e && e.message) || String(e), ms: now() - t0 });
    }
    return normalizeJson(res, ro, now() - t0);
  }

  // form POST（原样 x-www-form-urlencoded body；native httpjson 路径）
  // url 绝对地址；form 为**已签名 body 字符串**（调用方保证与 sign 的顺序/编码一致）
  async function postForm(url, form, reqOpts) {
    const ro = reqOpts || {};
    const t0 = now();
    let res;
    try {
      res = await post(url, form, { headers: headers, timeout: ro.timeout || timeout });
    } catch (e) {
      return fail('transport', { message: (e && e.message) || String(e), ms: now() - t0 });
    }
    return normalizeJson(res, ro, now() - t0);
  }

  function setCookie(cookie) {
    headers = apiHeaders(cookie || '');
  }

  // nav：匿名返回 code=-101，但 data.wbi_img 照常携带 —— 用它取 WBI 密钥
  async function fetchWbiKeys() {
    const r = await request('/x/web-interface/nav', null, { acceptCodes: [-101] });
    if (!r.ok) return r;
    const wi = r.data && r.data.wbi_img;
    if (!wi || !wi.img_url || !wi.sub_url) {
      return fail('api', { code: r.code, message: 'nav 缺少 wbi_img 字段' });
    }
    const imgKey = keyOf(wi.img_url);
    const subKey = keyOf(wi.sub_url);
    if (!/^[0-9a-f]{32}$/.test(imgKey) || !/^[0-9a-f]{32}$/.test(subKey)) {
      return fail('api', { code: r.code, message: 'wbi_img 不是 32 位 hex' });
    }
    return { ok: true, code: r.code, imgKey: imgKey, subKey: subKey, ms: r.ms };
  }

  // 匿名触点：x/frontend/finger/spi → data.b_3 / b_4（放进 Cookie buvid3/buvid4）
  async function fetchFinger() {
    const r = await request('/x/frontend/finger/spi', null, {});
    if (!r.ok) return r;
    const d = r.data || {};
    const b3 = typeof d.b_3 === 'string' ? d.b_3 : '';
    if (!b3) return fail('api', { code: 0, message: 'spi 缺少 b_3' });
    return { ok: true, code: 0, buvid3: b3, buvid4: typeof d.b_4 === 'string' ? d.b_4 : '', ms: r.ms };
  }

  // 二进制 GET（protobuf 弹幕 seg.so）：**native httpjson getBinary**（ArrayBuffer 保字节）。
  // jsapi http 实测把 1.2MB body 截成 2 字节 → 弃用；魔数 0x0A 校验在调用方兜残余错误。
  // getBinary 传输可注入（单测 fake；默认走 native）。
  async function getBinary(url, reqOpts) {
    const ro = reqOpts || {};
    const t0 = now();
    const gb = ro.getBinary || httpGetBinary;
    try {
      const res = await gb(url, { headers: headers, timeout: ro.timeout || timeout });
      return { ok: true, body: res.body, httpStatus: res.statusCode, ms: now() - t0 };
    } catch (e) {
      const stage = e.stage === 'http' ? 'http' : 'transport';
      return fail(stage, { message: (e && e.message) || String(e), ms: now() - t0 });
    }
  }

  // 稿件信息：权威 cid + 时长 + 标题 + **显示宽高**（dimension.rotate≠0 时宽高互换，
  // 与 ffmpeg autorotate 转正后的显示方向一致；缺失兑底 16:9）
  async function fetchView(bvid) {
    const r = await request('/x/web-interface/view', { bvid: bvid }, {});
    if (!r.ok) return r;
    const d = r.data || {};
    const cid = Number(d.cid) || 0;
    if (!(cid > 0)) return fail('api', { code: 0, message: 'view 缺少 cid' });
    const dim = d.dimension || (d.pages && d.pages[0] && d.pages[0].dimension) || {};
    let w = Number(dim.width) || 0;
    let h = Number(dim.height) || 0;
    const rot = Number(dim.rotate) || 0;
    if (rot === 90 || rot === 270) {
      const t = w;
      w = h;
      h = t;
    }
    if (!(w > 0 && h > 0)) {
      w = 640;
      h = 360;
    }
    return {
      ok: true,
      code: 0,
      cid: cid,
      aid: Number(d.aid) || 0,
      title: typeof d.title === 'string' ? d.title : '',
      duration: Number(d.duration) || 0,
      width: w,
      height: h,
      ms: r.ms
    };
  }

  // 取流：signedQuery 必须是 buildSignedQuery 的产物（字符串原样拼接）
  async function fetchPlayurl(signedQuery) {
    return request('/x/player/wbi/playurl', signedQuery, {});
  }

  return {
    request: request,
    postForm: postForm,
    getBinary: getBinary,
    setCookie: setCookie,
    getHeaders: () => headers,
    fetchWbiKeys: fetchWbiKeys,
    fetchFinger: fetchFinger,
    fetchView: fetchView,
    fetchPlayurl: fetchPlayurl
  };
}

function keyOf(url) {
  const m = /([0-9a-f]{32})/.exec(String(url || ''));
  if (m) return m[1];
  const tail = String(url || '').split('/').pop() || '';
  return tail.split('.')[0];
}
