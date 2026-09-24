// 网络 adapter（真机证据 youdao-x5-fw3.4.6，见 profiles/youdao-x5.md）：
//   - jsapi 只有 http（无 net 命名空间）：http.request / downloadFile / ...
//   - 自定义请求头字段是 header（单数，对象）；query 走 urlParams
//   - 成功 {result:<body>}（无 statusCode）；非 2xx {error:3,result:{errorMessage:'curl ... resCode:N'}}
//     → 非 2xx 时 body 不可得，错误文本就是全部信息
//   - 响应头永远不可见
// 本项目只需要带自定义头的 GET JSON（B 站 api 必须自带浏览器 UA 过风控，见 profile）。

import { logWarn } from './storage.js';

function httpApi() {
  const f = typeof $falcon !== 'undefined' ? $falcon : null;
  const jsapi = f && f.jsapi;
  if (!jsapi) return null;
  if (jsapi.http && typeof jsapi.http.request === 'function') return jsapi.http;
  if (jsapi.net && typeof jsapi.net.request === 'function') return jsapi.net;
  return null;
}

export function netAvailable() {
  const api = httpApi();
  return !!(api && typeof api.request === 'function');
}

export function normalizeError(e) {
  if (e instanceof Error) return e;
  let msg = '';
  if (e && typeof e === 'object') msg = e.message || e.msg || e.error || '';
  else if (typeof e === 'string') msg = e;
  const err = new Error(msg || 'NET_UNKNOWN: 未知网络错误');
  if (e && typeof e === 'object') err.raw = e;
  return err;
}

// 把运行时返回包装归一化成 { statusCode, body, errorMessage }
export function unwrapResponse(res) {
  if (res == null || typeof res !== 'object') return { statusCode: 0, body: null, raw: res };
  const hasError = res.error !== undefined && res.error !== 0 && res.error !== null;
  if (hasError) {
    const info = res.result !== undefined ? res.result : res.error;
    let msg = '';
    let statusCode = 0;
    if (info && typeof info === 'object') {
      msg = info.errorMessage || info.message || '';
      if (typeof info.statusCode === 'number') statusCode = info.statusCode;
    } else if (typeof info === 'string') {
      msg = info;
    }
    const m = typeof msg === 'string' ? msg.match(/resCode:(\d+)/) : null;
    if (m) statusCode = parseInt(m[1], 10);
    const body = info && info.data !== undefined ? info.data : null;
    return { statusCode: statusCode, body: body, raw: res, errorMessage: msg };
  }
  let body = null;
  if (res.result !== undefined) body = res.result;
  else if (res.data !== undefined) body = res.data;
  else if (res.body !== undefined) body = res.body;
  else if (res.text !== undefined) body = res.text;
  const statusCode =
    typeof res.statusCode === 'number' ? res.statusCode : typeof res.status === 'number' ? res.status : 200;
  return { statusCode: statusCode, body: body, raw: res };
}

export function tryParseJson(body) {
  if (typeof body !== 'string') return body;
  const t = body.replace(/^\uFEFF/, '').trim();
  if (!t) return null;
  if (t.charAt(0) !== '{' && t.charAt(0) !== '[') return body;
  try {
    return JSON.parse(t);
  } catch (e) {
    return body;
  }
}

// 把 {headers:{...}} 归一化成运行时需要的 header（单数）对象；
// 只保留字符串值，键大小写保持服务器契约要求的写法。
export function toRuntimeHeader(headers) {
  const out = {};
  if (!headers || typeof headers !== 'object') return null;
  let n = 0;
  for (const k of Object.keys(headers)) {
    const v = headers[k];
    if (typeof v === 'string' && v.length > 0) {
      out[k] = v;
      n++;
    }
  }
  return n > 0 ? out : null;
}

// GET 文本；返回 { statusCode, body(string), errorMessage }
// opts: { headers?: Object, timeout?: number }
export function httpGet(url, opts) {
  return new Promise((resolve, reject) => {
    const api = httpApi();
    if (!api) {
      reject(new Error('NET_UNAVAILABLE: http.jsapi 不可用'));
      return;
    }
    const o = opts || {};
    let settled = false;
    let guard = 0;
    const limit = o.timeout || 15000;
    const done = (isErr, arg) => {
      if (settled) return;
      settled = true;
      clearTimeout(guard);
      if (isErr) reject(arg);
      else resolve(arg);
    };
    try {
      const options = { url: url, method: 'GET', timeout: limit };
      const header = toRuntimeHeader(o.headers);
      if (header) options.header = header;
      const ret = api.request(options, (res, err) => {
        if (err) done(true, normalizeError(err));
        else done(false, unwrapResponse(res));
      });
      if (ret && typeof ret.then === 'function') {
        ret.then(
          (res) => done(false, unwrapResponse(res)),
          (e) => done(true, normalizeError(e))
        );
      }
    } catch (e) {
      done(true, normalizeError(e));
      return;
    }
    guard = setTimeout(() => done(true, new Error('NET_TIMEOUT: 请求超时(' + limit + 'ms)')), limit + 500);
  });
}

// ---- 原生 form POST（native httpjson）----
// B 站 TV 登录接口 GET=405、且必须原样 x-www-form-urlencoded body；
// jsapi http 的 data 会被二次 JSON 序列化（deepseek-x5 实测）发不了 form → 只走 native。
// 模块：libs/libjsapi_httpjson.so（同固件已验证；startPost 异步 + takeResult 轮询，不卡 JS）

export function headersToLines(headers) {
  const lines = [];
  if (!headers) return '';
  Object.keys(headers).forEach((k) => {
    const v = headers[k];
    if (typeof v === 'string' && v) lines.push(k + ': ' + v);
  });
  return lines.join('\r\n');
}

async function loadHttpJson() {
  try {
    const m = await import('httpjson');
    const mod = m && (typeof m.startPost === 'function' ? m : m.default);
    return mod || null;
  } catch (e) {
    return null;
  }
}

// POST 原样 form body；opts: { headers, timeout } → {statusCode, body, errorMessage}
export async function httpPostForm(url, body, opts) {
  const o = opts || {};
  const limit = o.timeout || 15000;
  const mod = await loadHttpJson();
  if (!mod) {
    throw new Error('NET_NATIVE_MISSING: 缺少 libjsapi_httpjson.so（form POST 仅支持 native）');
  }
  const headerText = headersToLines(o.headers);
  let id;
  try {
    id = mod.startPost(url, headerText, String(body == null ? '' : body), limit);
  } catch (e) {
    throw new Error('NET_NATIVE_START: ' + ((e && e.message) || e));
  }
  const t0 = Date.now();
  return new Promise((resolve, reject) => {
    const timer = setInterval(() => {
      let raw = null;
      try {
        raw = mod.takeResult(id);
      } catch (e) {
        clearInterval(timer);
        reject(new Error('NET_NATIVE_TAKE: ' + ((e && e.message) || e)));
        return;
      }
      if (raw === null || raw === undefined) {
        if (Date.now() - t0 > limit + 2000) {
          clearInterval(timer);
          reject(new Error('NET_TIMEOUT: form POST 超时(' + limit + 'ms)'));
        }
        return;
      }
      clearInterval(timer);
      try {
        const r = typeof raw === 'string' ? JSON.parse(raw) : raw;
        if (!r) {
          reject(new Error('NET_NATIVE_BAD_RESULT: 空结果'));
          return;
        }
        if (!r.status && r.error) {
          reject(new Error('NET_NATIVE: ' + r.error));
          return;
        }
        resolve({
          statusCode: Number(r.status) || 0,
          body: typeof r.body === 'string' ? r.body : '',
          errorMessage: r.error ? String(r.error) : ''
        });
      } catch (e) {
        reject(new Error('NET_NATIVE_BAD_JSON: ' + ((e && e.message) || e)));
      }
    }, 150);
  });
}

// 页面卸载时丢弃在途 native 请求（幂等）
export async function cancelNativePosts() {
  const mod = await loadHttpJson();
  if (mod && typeof mod.cancelAll === 'function') {
    try {
      mod.cancelAll();
    } catch (e) {
      /* 幂等忽略 */
    }
  }
}

// 二进制 GET（弹幕 protobuf 等）：native httpjson getBinary → ArrayBuffer 原样返回。
// jsapi http 实测把 1.2MB body 截成 2 字节（0x22…）不可用——只有 native curl 能保字节。
// opts: { headers, timeout } → { statusCode, body:ArrayBuffer, errorMessage }（失败 throw 带 stage）
export async function httpGetBinary(url, opts) {
  const o = opts || {};
  const limit = o.timeout || 15000;
  const mod = await loadHttpJson();
  if (!mod || typeof mod.getBinary !== 'function') {
    const err = new Error('NET_NATIVE_MISSING: libjsapi_httpjson.so 无 getBinary（需重编升级）');
    err.stage = 'transport';
    throw err;
  }
  let r;
  try {
    r = mod.getBinary(url, headersToLines(o.headers), limit);
  } catch (e) {
    const err = new Error('NET_NATIVE_GET: ' + ((e && e.message) || e));
    err.stage = 'transport';
    throw err;
  }
  if (!r || !r.ok) {
    const err = new Error('HTTP ' + (r && r.status ? r.status : 0) + (r && r.error ? ': ' + r.error : ''));
    err.stage = 'http';
    throw err;
  }
  return { statusCode: r.status, body: r.buf, errorMessage: '' };
}
