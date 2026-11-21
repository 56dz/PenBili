// 扫码登录（**TV 变体**，登录信息持久化）
//
// 为什么是 TV 变体（2026-09-23 开发机取证，见 tools/qr_login_diag.js）：
//   web 变体 poll 成功 → data.url=.../crossDomain?ticket=...，cookie **只在每跳 Set-Cookie 响应头**
//     （hop0-2 三跳 302 各下发 SESSDATA/bili_jct/DedeUserID/ckMd5/sid）→ jsapi 读不到响应头，死路。
//   TV 变体 poll 成功 → **cookie_info.cookies[] 明文就在 body 里**（native body 可读）。
//
// 契约（xieren58/bilibili-API-collect docs/login/login_action/QR.md + 实测）：
//   POST passport.bilibili.com/x/passport-tv-login/qrcode/auth_code   {appkey, local_id, ts, sign}
//     → code:0 {url, auth_code}（GET=405 必须 POST；原样 form body 需 native httpjson）
//   POST passport.bilibili.com/x/passport-tv-login/qrcode/poll        {appkey, auth_code, local_id, ts, sign}
//     状态码在**外层 code**：0 成功 / 86039 未扫未确认 / 86090 已扫未确认 / 86038 失效 / -3 签名错误
//   APP sign = md5( 按 key 排序的 k=v&… + APPSEC )（sign 不参与自身计算）
//     实测与文档示例逐字符一致：local_id=0,ts=0 → e134154ed6add881d28fbdf68653cd9c
//
// nav 验证：GET x/web-interface/nav（带 Cookie）code:0→档案 / -101→未登录或过期。
// 日志卫生：绝不输出 cookie 值 / token / 完整二维码 url（含 auth_code）。

import { md5Hex } from './md5.js';

export const APPKEY_TV = '4409e2ce8ffd12b8'; // 云视听小电视(TV版)
export const APPSEC_TV = '59b43e04ad6965f34319062b478f83dd';
export const TV_GEN_URL = 'https://passport.bilibili.com/x/passport-tv-login/qrcode/auth_code';
export const TV_POLL_URL = 'https://passport.bilibili.com/x/passport-tv-login/qrcode/poll';

// APP sign：参数（不含 sign）按 key 字典序 k=v& + appsec → md5
// （TV 组参数值均为 hex/数字，encode 无差异；文档向量已验证）
export function appSign(params) {
  const keys = Object.keys(params)
    .filter((k) => k !== 'sign')
    .sort();
  const base = keys.map((k) => k + '=' + params[k]).join('&');
  return md5Hex(base + APPSEC_TV);
}

// 构造带 sign 的 form body（排序 + encode，与签名逐字一致的顺序）
export function tvForm(extra) {
  const p = Object.assign({ appkey: APPKEY_TV, local_id: '0', ts: String(Math.floor(Date.now() / 1000)) }, extra || {});
  p.sign = appSign(p);
  return Object.keys(p)
    .sort()
    .map((k) => k + '=' + encodeURIComponent(p[k]))
    .join('&');
}

// cookie_info.cookies[] → cookie 对象（缺 SESSDATA 视为失败；只暴露 name 列表）
export function extractCookies(cookieInfo) {
  const list = (cookieInfo && cookieInfo.cookies) || [];
  const map = {};
  list.forEach((c) => {
    if (c && c.name) map[c.name] = c;
  });
  if (!map.SESSDATA || !map.SESSDATA.value) {
    return { ok: false, reason: 'no_SESSDATA_in_cookie_info', keys: Object.keys(map).join(',') };
  }
  const exp = Number(map.SESSDATA.expires) || 0;
  return {
    ok: true,
    cookies: {
      SESSDATA: map.SESSDATA.value,
      bili_jct: (map.bili_jct && map.bili_jct.value) || '',
      DedeUserID: (map.DedeUserID && map.DedeUserID.value) || '',
      DedeUserID__ckMd5: (map['DedeUserID__ckMd5'] && map['DedeUserID__ckMd5'].value) || '',
      expiresAt: exp > 0 ? exp * 1000 : 0
    }
  };
}

// generate 响应（client.postForm 归一化形态 {ok, code, data} / {ok:false, stage, ...}）
export function parseGenerate(res) {
  if (!res || res.ok === false) {
    return {
      ok: false,
      stage: res ? res.stage : 'transport',
      code: res ? res.code : undefined,
      message: (res && res.message) || '申请二维码失败'
    };
  }
  const d = res.data || {};
  const url = typeof d.url === 'string' ? d.url : '';
  const auth = typeof d.auth_code === 'string' ? d.auth_code : '';
  if (!url || !/^[0-9a-f]{32}$/.test(auth)) {
    return { ok: false, stage: 'parse', message: 'generate 响应缺 url/auth_code' };
  }
  return { ok: true, url: url, authCode: auth };
}

export const QR_STATE = {
  WAITING: 'waiting', // 86039 未扫未确认
  SCANNED: 'scanned', // 86090 已扫未确认（手机上等确认）
  EXPIRED: 'expired', // 86038 失效/超时
  OK: 'ok',
  ERROR: 'error'
};

// poll 状态码在外层 code；调用方用此表放行后交本函数 switch
export const POLL_ACCEPT_CODES = [0, 86039, 86090, 86038];

export function parsePoll(res) {
  if (!res || res.ok === false) {
    if (res && typeof res.code === 'number' && POLL_ACCEPT_CODES.indexOf(res.code) < 0) {
      return { state: QR_STATE.ERROR, stage: 'api', code: res.code, message: 'code=' + res.code + ' ' + (res.message || '') };
    }
    return { state: QR_STATE.ERROR, stage: res ? res.stage : 'transport', message: (res && res.message) || '轮询失败' };
  }
  const code = Number(res.code);
  if (code === 0) {
    const ex = extractCookies(res.data && res.data.cookie_info);
    if (!ex.ok) {
      return {
        state: QR_STATE.ERROR,
        stage: 'cookie',
        message: '成功响应缺少 SESSDATA（' + ex.reason + (ex.keys ? ' names=' + ex.keys : '') + '）'
      };
    }
    const d = res.data || {};
    return {
      state: QR_STATE.OK,
      cookies: ex.cookies,
      mid: Number(d.mid) || 0,
      refreshToken: typeof d.refresh_token === 'string' ? d.refresh_token : ''
    };
  }
  if (code === 86039) return { state: QR_STATE.WAITING, message: res.message || '未扫码' };
  if (code === 86090) return { state: QR_STATE.SCANNED, message: res.message || '已扫码未确认' };
  if (code === 86038) return { state: QR_STATE.EXPIRED, message: res.message || '二维码已失效' };
  return { state: QR_STATE.ERROR, stage: 'api', code: code, message: 'code=' + code + ' ' + (res.message || '') };
}

// nav 响应 → 用户档案（登录验证兼个人页数据）
export function parseNavProfile(res) {
  // -101 两种形态都要吃到：client 未放行时 {ok:false,stage:'api',code:-101}；
  // acceptCodes 放行时 {ok:true,code:-101}
  if (res && typeof res.code === 'number' && res.code === -101) {
    return { ok: false, stage: 'unauthorized', message: '未登录或登录已过期' };
  }
  if (!res || res.ok === false) {
    return { ok: false, stage: res ? res.stage : 'transport', message: (res && res.message) || '获取用户信息失败' };
  }
  if (res.code !== 0) {
    return { ok: false, stage: 'api', code: res.code, message: res.message };
  }
  const d = res.data || {};
  if (!d.isLogin) return { ok: false, stage: 'unauthorized', message: '未登录或登录已过期' };
  const li = d.level_info || {};
  return {
    ok: true,
    mid: Number(d.mid) || 0,
    uname: typeof d.uname === 'string' ? d.uname : '',
    face: typeof d.face === 'string' ? d.face : '',
    level: Number(li.current_level) || 0,
    exp: Number(li.current_exp) || 0,
    money: Number(d.money) || 0,
    vipType: Number(d.vipType) || 0
  };
}

// 轮询间隔与总时长（auth_code 180s 过期，留余量）
export const POLL_INTERVAL_MS = 2000;
export const POLL_MAX_ROUNDS = 85; // ≈170s

// 登录态过期判断（expiresAt 毫秒时间戳，提前 60s 判过期；未知有效期为有效，由 nav 兜底）
export function loginExpired(cookies, nowMs) {
  if (!cookies || !cookies.SESSDATA) return true;
  if (!cookies.expiresAt) return false;
  const now = Number(nowMs) || Date.now();
  return now >= cookies.expiresAt - 60000;
}
