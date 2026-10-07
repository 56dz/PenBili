// 存储 adapter（真机证据 youdao-x5-fw3.4.6，见 profiles/youdao-x5.md）——
//   getStorage({key}) → Promise<{data:string}>；setStorage({key, data}) 才落盘
//   （传 value/content 会落盘为空串；字符串形态 setStorage(k,v) 的 promise 永不 settle）
// 统一走对象形态 + 超时保护；失败时保留内存态。高频写入串行化。

const MEMORY = {};

export const KEYS = {
  // 会话痕迹：buvid3/buvid4（B 站风险控制所需的匿名身份，非登录凭据）
  session: 'bvp_session',
  // 目标稿件覆盖（默认有内置值，可在 storage 里改）
  settings: 'bvp_settings',
  // 播放历史（给「我的」页；schema 见 history.js）
  history: 'bili_history',
  // 自检开关（tools/seed_settings.js 预置，真机自动化用）
  autotest: 'bili_autotest',
  // 直播设置 + 重定向解析缓存（schema 见下）
  live: 'bvp_live'
};

export const SESSION_SCHEMA_VERSION = 2;
export const SETTINGS_SCHEMA_VERSION = 1;
export const LIVE_SCHEMA_VERSION = 1;

function getSetPair(key) {
  const f = typeof $falcon !== 'undefined' ? $falcon : null;
  const s = f && f.jsapi && f.jsapi.storage;
  if (!s) return null;
  if (typeof s.getStorage === 'function' && typeof s.setStorage === 'function') {
    return {
      get: () => s.getStorage({ key: key }),
      set: (v) => s.setStorage({ key: key, data: v })
    };
  }
  return null;
}

export function normalizeStoredValue(result) {
  if (result == null) return '';
  if (typeof result === 'object') {
    if (typeof result.data === 'string') return result.data;
    if (typeof result.value === 'string') return result.value;
    return '';
  }
  return typeof result === 'string' ? result : '';
}

function withTimeout(p, ms) {
  let timer = 0;
  const timeout = new Promise((resolve) => {
    timer = setTimeout(() => resolve({ __timeout: true }), ms);
  });
  return Promise.race([p, timeout]).then((r) => {
    clearTimeout(timer);
    return r;
  });
}

async function rawGet(key) {
  const pair = getSetPair(key);
  if (!pair) return typeof MEMORY[key] === 'string' ? MEMORY[key] : '';
  try {
    const r = await withTimeout(pair.get(), 5000);
    if (r && r.__timeout) {
      logWarn('[storage] get 超时: ' + key);
      return typeof MEMORY[key] === 'string' ? MEMORY[key] : '';
    }
    const v = normalizeStoredValue(r);
    if (v) {
      // 读到的值必须同步进内存缓存，否则后续读取会拿到空值并反过来覆盖存储
      MEMORY[key] = v;
      return v;
    }
    return typeof MEMORY[key] === 'string' ? MEMORY[key] : '';
  } catch (e) {
    return typeof MEMORY[key] === 'string' ? MEMORY[key] : '';
  }
}

let writeChain = Promise.resolve();

function rawSet(key, value) {
  const run = async () => {
    MEMORY[key] = value;
    const pair = getSetPair(key);
    if (!pair) return;
    try {
      await withTimeout(pair.set(value), 3000);
      const back = await withTimeout(pair.get(), 3000);
      if (!(back && !back.__timeout && normalizeStoredValue(back) === value)) {
        logWarn('[storage] 写入未持久化: ' + key);
      }
    } catch (e) {
      logWarn('[storage] set 异常: ' + (e && e.message));
    }
  };
  writeChain = writeChain.then(run, run);
  return writeChain;
}

export function logWarn(msg) {
  // 设备日志只落 console.warn/error（console.log 不落盘）
  try {
    console.warn(msg);
  } catch (e) {
    /* 忽略 */
  }
}

export async function getJson(key, fallback) {
  const raw = await rawGet(key);
  if (!raw) return fallback;
  try {
    return JSON.parse(raw);
  } catch (e) {
    logWarn('[storage] JSON 损坏: ' + key);
    return fallback;
  }
}

export function setJson(key, obj) {
  return rawSet(key, JSON.stringify(obj));
}

export async function getString(key, fallback) {
  const raw = await rawGet(key);
  return raw || fallback;
}

export function setString(key, value) {
  return rawSet(key, String(value == null ? '' : value));
}

export function memoryPeek(key) {
  return typeof MEMORY[key] === 'string' ? MEMORY[key] : '';
}

// ---- 会话 schema（version 化，读取时处理空值/损坏/旧版本）----
// v1：匿名触点 {buvid3, buvid4}
// v2：+ login（扫码登录态持久化：cookie 四件套 + 有效期 + 用户档案）
//     任何 cookie 值只进 storage，**绝不进日志**
// SESSION_SCHEMA_VERSION 见文件头
export function normalizeLogin(lg) {
  if (!lg || typeof lg !== 'object') return null;
  const se = typeof lg.SESSDATA === 'string' ? lg.SESSDATA : '';
  if (!se) return null;
  return {
    SESSDATA: se,
    bili_jct: typeof lg.bili_jct === 'string' ? lg.bili_jct : '',
    DedeUserID: typeof lg.DedeUserID === 'string' ? lg.DedeUserID : '',
    DedeUserID__ckMd5: typeof lg['DedeUserID__ckMd5'] === 'string' ? lg['DedeUserID__ckMd5'] : '',
    expiresAt: Number(lg.expiresAt) || 0,
    mid: Number(lg.mid) || 0,
    uname: typeof lg.uname === 'string' ? lg.uname : '',
    level: Number(lg.level) || 0,
    face: typeof lg.face === 'string' ? lg.face : '',
    loggedAt: Number(lg.loggedAt) || 0
  };
}

export function normalizeSession(raw) {
  const empty = { version: SESSION_SCHEMA_VERSION, buvid3: '', buvid4: '', updatedAt: 0, login: null };
  if (!raw || typeof raw !== 'object') return empty;
  if (raw.version === 1) {
    // v1 → v2 平移（v1 无登录概念）
    return {
      version: SESSION_SCHEMA_VERSION,
      buvid3: typeof raw.buvid3 === 'string' ? raw.buvid3 : '',
      buvid4: typeof raw.buvid4 === 'string' ? raw.buvid4 : '',
      updatedAt: typeof raw.updatedAt === 'number' ? raw.updatedAt : 0,
      login: null
    };
  }
  if (raw.version !== SESSION_SCHEMA_VERSION) return empty;
  return {
    version: SESSION_SCHEMA_VERSION,
    buvid3: typeof raw.buvid3 === 'string' ? raw.buvid3 : '',
    buvid4: typeof raw.buvid4 === 'string' ? raw.buvid4 : '',
    updatedAt: typeof raw.updatedAt === 'number' ? raw.updatedAt : 0,
    login: normalizeLogin(raw.login)
  };
}

export async function loadSession() {
  return normalizeSession(await getJson(KEYS.session, null));
}

export function saveSession(session) {
  return setJson(KEYS.session, normalizeSession(session));
}

// ---- 目标稿件设置 ----

export const DEFAULT_TARGET = { bvid: 'BV1ZCeb6NEyM' };

export function normalizeSettings(raw) {
  const empty = {
    version: SETTINGS_SCHEMA_VERSION,
    bvid: DEFAULT_TARGET.bvid
  };
  if (!raw || typeof raw !== 'object') return empty;
  if (raw.version !== SETTINGS_SCHEMA_VERSION) return empty;
  const bvid = typeof raw.bvid === 'string' ? raw.bvid.trim() : '';
  return {
    version: SETTINGS_SCHEMA_VERSION,
    bvid: /^BV[0-9A-Za-z]{10}$/.test(bvid) ? bvid : DEFAULT_TARGET.bvid
  };
}

export async function loadSettings() {
  return normalizeSettings(await getJson(KEYS.settings, null));
}

// ---- 直播设置 + 重定向解析缓存（LIVE_SCHEMA_VERSION 见文件头）----
//   addr         用户填的「入口地址」（可以是固定域名，如 http://penbili.560726.best）
//   resolvedAddr 重定向解析结果（如 http://penbili.560726.xyz:1728）。外网端口每次打洞都会变，
//                所以只把它当缓存：app 启动时探测它，2s 无响应即重新走入口地址解析（见 services/live.js）
//   bufMs        笔端想握的缓冲时长（服务端据此定 HLS 播放列表窗口长度）
//   res/bv/trans 画质高度 / 视频码率 / 是否转码
export const LIVE_LIMITS = {
  bufMinMs: 2000,
  bufMaxMs: 20000,
  bufDefaultMs: 6000,
  RES: [360, 480, 540]
};

// 地址归一：补 scheme、去尾斜杠（用户常直接填 "192.168.5.224:2050"）
export function normalizeLiveAddr(v) {
  let s = typeof v === 'string' ? v.trim() : '';
  if (!s) return '';
  if (!/^https?:\/\//i.test(s)) s = 'http://' + s;
  return s.replace(/\/+$/, '');
}

export function normalizeLive(raw) {
  const empty = {
    version: LIVE_SCHEMA_VERSION,
    addr: '',
    bufMs: LIVE_LIMITS.bufDefaultMs,
    res: 480,
    bv: '700k',
    trans: 1,
    resolvedAddr: '',
    resolvedAt: 0
  };
  if (!raw || typeof raw !== 'object') return empty;
  if (raw.version !== LIVE_SCHEMA_VERSION) return empty;
  const buf = Math.floor(Number(raw.bufMs) || LIVE_LIMITS.bufDefaultMs);
  const res = Number(raw.res);
  const bvRaw = typeof raw.bv === 'string' ? raw.bv.trim().toLowerCase() : '';
  return {
    version: LIVE_SCHEMA_VERSION,
    addr: normalizeLiveAddr(raw.addr),
    bufMs: Math.max(LIVE_LIMITS.bufMinMs, Math.min(LIVE_LIMITS.bufMaxMs, buf)),
    res: LIVE_LIMITS.RES.indexOf(res) >= 0 ? res : 480,
    bv: /^\d+[km]?$/.test(bvRaw) ? bvRaw : '700k',
    trans: raw.trans === 0 ? 0 : 1,
    resolvedAddr: normalizeLiveAddr(raw.resolvedAddr),
    resolvedAt: Number(raw.resolvedAt) || 0
  };
}

export async function loadLive() {
  return normalizeLive(await getJson(KEYS.live, null));
}

export function saveLive(live) {
  return setJson(KEYS.live, normalizeLive(live));
}
