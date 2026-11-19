// B 站弹幕（seg.so protobuf）：手写解析 / 按 idStr 去重 / 分片缓存 / 按位置取窗口 / 喂给 native 的紧凑文本。
//
// 为什么手写 protobuf 解析：设备侧是 QuickJS（falcon 20200705），既没有 protobufjs，也没有
//   Node 的 Buffer / TextDecoder。引 runtime 会撑大 .amr 还要过语法裁剪；而这里只需解一个扁平的
//   DmSegMobileReply（顶层 1 个 repeated 字段 + 每条约 12 个标量），手写 varint + length-delimited
//   扫描更小更快（实测单片 5863 条 / 1236855 B，解析 29~39ms）。
//
// 为什么按 idStr 去重、绝不按 content 去重：「同一句话被不同用户刷」是弹幕生态，不是重复数据。
//   单片实测 1404 条里 id 全唯一（1404/1404），content 只有 1338 个唯一值 —— 那 66 条重复是不同
//   用户真的刷了同一句话（例如「哈哈哈哈」），按 content 去重会把真实弹幕吃掉。idStr 是 B 站给的
//   十进制字符串（例 "34576873522987015"），超出 JS 数字精度，必须按字符串比较。
//
// 为什么不需要解压：设备侧 native httpjson.c 里 CURLOPT_ACCEPT_ENCODING 是注释掉的，服务器因此
//   返回未压缩明文 protobuf（首字节 0x0A = 顶层 field 1 / wire type 2）。所以这里不写任何解压
//   代码，也不设 Accept-Encoding —— 设了反而可能拿到压缩流。
//
// 本文件全部是纯函数 + 一个可注入 client 的取数，便于 Node 单测。

import { logWarn } from '../storage.js';

/* 切换式开关：off → full → half → quarter → off */
export const DM_MODES = ['off', 'full', 'half', 'quarter'];
export const DM_MODE_LABELS = { off: '弹幕关', full: '弹幕全屏', half: '弹幕 1/2', quarter: '弹幕 1/4' };
/* 【3.5.19】默认开启弹幕（全屏模式）。
 * 之前默认 off，用户反馈「装上就是没弹幕，还以为没做」。
 * 脏数据也回到这个默认值（而不是回到 off）。 */
export const DM_MODE_DEFAULT = 'full';
export const DM_SETTINGS_KEY = 'bili_dm_settings';
export const DM_SETTINGS_VERSION = 2;

/*
 * 字号档位。这里是**屏幕逻辑像素**（960x266 的设计稿坐标），native 侧会按
 * 平面缓冲区的实际宽度再放大一次（见 session.c 的「字号换算」）。
 * 三档而不是滑杆：falcon 没有原生滑杆控件，轮转是最省事也最不容易误触的形态。
 */
export const DM_SIZES = ['small', 'mid', 'large'];
export const DM_SIZE_LABELS = { small: '小', mid: '中', large: '大' };
/*
 * 【3.5.16】用户 m22717 第 2 条：
 *   「弹幕中字号应该是大字号的大小，中字号就取现在的小和中的平均值」
 * 按「整体上调一档」落地：
 *   中 = 上一版的「大」（22）—— 用户要的就是这个大小；
 *   小 = 上一版「小 14」与「中 18」的平均（16）—— 三档仍保持上下可辨；
 *   大 = 26（在中之上再留一档，否则「大」与「中」同值，选项就退化成两档）。
 * 这三个数是**屏幕逻辑像素**，与设计稿 960x266 同坐标系；native 会按缓冲区宽度
 * 等比放大，kmssink 再缩回矩形，两个系数互为倒数，所以这里写多少就是屏幕上多高。
 */
export const DM_SIZE_PX = { small: 16, mid: 22, large: 26 };
/* 【3.5.19】默认字号改为「小」。首次装上的人看到的就是最小档，
 * 嫌小自己去「我的 → 弹幕字号」调到中/大；反过来默认大字会遮画面。 */
export const DM_SIZE_DEFAULT = 'small';

/* 每片 360 秒：第 n 片（n 从 1 起）覆盖 [(n-1)*360000, n*360000) 毫秒 */
export const SEG_SECONDS = 360;
export const SEG_MS = SEG_SECONDS * 1000;

export const DM_SEG_URL = 'https://api.bilibili.com/x/v2/dm/web/seg.so';
export const DM_FETCH_TIMEOUT_MS = 15000;
/* 存活窗口：滚动类 8s；底部(4)/顶部(5) 固定弹幕 4s */
export const DM_SCROLL_LIFE_MS = 8000;
export const DM_FIXED_LIFE_MS = 4000;
/* 单行文本里 content 的最大字符数（按码点算，别切坏 emoji） */
export const DM_FEED_TEXT_MAX = 80;

/* seg.so 明文 protobuf 的魔数：顶层 field 1 / wire type 2 */
const MAGIC_0A = 0x0a;

/* ArrayBuffer / Uint8Array / 其它视图 / 普通数组 → Uint8Array；拿不到返回 null。
 * QuickJS 跨桥接偶尔给 { buffer, byteOffset, byteLength } 的视图壳，一并兜住。 */
function toU8(buf) {
  if (!buf) return null;
  if (typeof Uint8Array !== 'undefined' && buf instanceof Uint8Array) return buf;
  if (typeof ArrayBuffer !== 'undefined' && buf instanceof ArrayBuffer) return new Uint8Array(buf);
  if (buf.buffer && typeof buf.byteLength === 'number') {
    try { return new Uint8Array(buf.buffer, buf.byteOffset || 0, buf.byteLength); } catch (e) { return null; }
  }
  const n = typeof buf.length === 'number' ? buf.length : -1;
  if (n > 0 && typeof buf[0] === 'number') {
    const out = new Uint8Array(n);
    for (let i = 0; i < n; i++) out[i] = buf[i] & 0xff;
    return out;
  }
  return n === 0 ? new Uint8Array(0) : null;
}

function toInt(v) {
  const n = Number(v);
  return !isFinite(n) || n < 0 ? 0 : Math.floor(n);
}

/* 带边界读 varint：最多 10 字节（64 位），越界 / 超长 / 截断一律 null。
 * 用乘法累加而不是 <<，避免 32 位位移把大 id 截断。 */
function varintAt(u8, off, end) {
  let pos = off;
  if (!(pos >= 0) || pos >= end) return null;
  let value = 0;
  let shift = 1;
  for (let i = 0; i < 10; i++) {
    if (pos >= end) return null;
    const b = u8[pos++];
    value += (b & 0x7f) * shift;
    if ((b & 0x80) === 0) return { value: value, off: pos };
    shift *= 128;
  }
  return null;
}

/* 导出给单测/复用：buf 可以是 ArrayBuffer 或 Uint8Array，off 从 0 起 */
export function parseVarint(buf, off) {
  const u8 = toU8(buf);
  if (!u8) return null;
  const o = toInt(off);
  return varintAt(u8, o, u8.length);
}

/* 按 wire type 跳过未知字段；返回下一个字段偏移，非法返回 -1。
 * 0=varint 1=fixed64 2=length-delimited 5=fixed32；3/4（group）不支持 → -1 交给上层停止。 */
function skipField(u8, off, wire) {
  const lim = u8.length;
  if (wire === 0) {
    const r = varintAt(u8, off, lim);
    return r ? r.off : -1;
  }
  if (wire === 1) return off + 8 <= lim ? off + 8 : -1;
  if (wire === 5) return off + 4 <= lim ? off + 4 : -1;
  if (wire !== 2) return -1;
  const r = varintAt(u8, off, lim);
  if (!r) return -1;
  const next = r.off + r.value;
  return next >= r.off && next <= lim ? next : -1;
}

/* length-delimited 子串：→ { start, end, off } | null（长度非法 / 越界） */
function readDelimited(u8, off, end) {
  const r = varintAt(u8, off, end);
  if (!r) return null;
  const start = r.off;
  if (!(r.value >= 0) || start + r.value > end) return null;
  return { start: start, end: start + r.value, off: start + r.value };
}

/* 手写 UTF-8 解码（不能用 TextDecoder）：非法序列用 U+FFFD 顶掉，且保证偏移前进不死循环 */
function utf8Decode(u8, start, end) {
  let s = '';
  let i = start;
  while (i < end) {
    const b0 = u8[i++];
    let cp = 0;
    let need = 0;
    if (b0 < 0x80) s += String.fromCharCode(b0);
    else if (b0 >= 0xc2 && b0 <= 0xdf) {
      cp = b0 & 0x1f;
      need = 1;
    } else if (b0 >= 0xe0 && b0 <= 0xef) {
      cp = b0 & 0x0f;
      need = 2;
    } else if (b0 >= 0xf0 && b0 <= 0xf4) {
      cp = b0 & 0x07;
      need = 3;
    } else s += '\ufffd';
    if (!need) continue;
    if (i + need > end) {
      s += '\ufffd';
      break;
    }
    let bad = false;
    for (let k = 0; k < need; k++) {
      const b = u8[i + k];
      if ((b & 0xc0) !== 0x80) bad = true;
      else cp = cp * 64 + (b & 0x3f);
    }
    if (bad) {
      s += '\ufffd';
      continue;
    }
    i += need;
    if (cp < 0x80 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
      s += '\ufffd';
      continue;
    }
    if (cp > 0xffff) {
      const v = cp - 0x10000;
      s += String.fromCharCode(0xd800 + (v >> 10), 0xdc00 + (v & 0x3ff));
    } else s += String.fromCharCode(cp);
  }
  return s;
}

/* 在 [start, end) 里解一条弹幕；字段越界 → null（上层继续下一条）。
 * 字段表（0=varint 2=length-delimited 5=32bit）：
 *   1 id / 2 progress / 3 mode / 4 fontsize / 5 color / 6 midHash / 7 content /
 *   8 ctime / 9 weight / 12 idStr；其它（11/13/15/18/20/21/24/25/26/27…）按 wire type 跳过。 */
function parseElem(u8, start, end) {
  let off = start;
  let id = 0;
  let progress = 0;
  let mode = 1;
  let color = 16777215;
  let fontsize = 25;
  let text = '';
  let idStr = '';
  while (off < end) {
    const tagR = varintAt(u8, off, end);
    if (!tagR) return null;
    const field = Math.floor(tagR.value / 8);
    const wire = tagR.value & 7;
    off = tagR.off;
    if (wire === 2) {
      const d = readDelimited(u8, off, end);
      if (!d) return null;
      if (field === 7) text = utf8Decode(u8, d.start, d.end);
      else if (field === 12) idStr = utf8Decode(u8, d.start, d.end);
      off = d.off;
    } else if (wire === 0) {
      const v = varintAt(u8, off, end);
      if (!v) return null;
      off = v.off;
      if (field === 1) id = v.value;
      else if (field === 2) progress = v.value;
      else if (field === 3) mode = v.value;
      else if (field === 4) fontsize = v.value;
      else if (field === 5) color = v.value;
    } else if (wire === 1 || wire === 5) {
      off += wire === 1 ? 8 : 4;
      if (off > end) return null;
    } else {
      return null;
    }
  }
  /* idStr 缺失时退回 id 的十进制串：宁可精度差一点，也不能让同一条反复进队列 */
  return {
    idStr: idStr || (id > 0 ? String(id) : ''),
    progress: progress > 0 ? progress : 0,
    mode: mode > 0 ? mode : 1,
    color: color >= 0 ? color : 16777215,
    fontsize: fontsize > 0 ? fontsize : 25,
    text: text
  };
}

/* 解一整片 DmSegMobileReply → { ok, list, count, message }
 * 严格容错：任何截断 / 非法长度都立即停止并返回已解析部分，绝不抛异常。 */
export function parseSegReply(buf) {
  const u8 = toU8(buf);
  if (!u8 || u8.length === 0) return { ok: false, list: [], count: 0, message: '响应为空' };
  if (u8[0] !== MAGIC_0A) {
    return { ok: false, list: [], count: 0, message: '响应魔数不是 0x0A（可能是错误页）' };
  }
  const list = [];
  const len = u8.length;
  let off = 0;
  let stopped = false;
  while (off < len) {
    const tagR = varintAt(u8, off, len);
    if (!tagR) { stopped = true; break; }
    const field = Math.floor(tagR.value / 8);
    const wire = tagR.value & 7;
    off = tagR.off;
    if (field === 1 && wire === 2) {
      const d = readDelimited(u8, off, len);
      if (!d) { stopped = true; break; }
      const item = parseElem(u8, d.start, d.end);
      if (item) list.push(item);
      off = d.off;
      continue;
    }
    const next = skipField(u8, off, wire);
    if (next < 0) {
      stopped = true;
      break;
    }
    off = next;
  }
  const message = stopped ? '响应截断，已解析 ' + list.length + ' 条' : '';
  return { ok: list.length > 0, list: list, count: list.length, message: message };
}

/* 脏数据一律退回默认值（DM_MODE_DEFAULT = full）。
 * 3.5.19 之前这里退回 off，结果「设置文件损坏」就等于永远没弹幕。 */
export function normalizeDmSettings(raw) {
  let r = raw;
  if (typeof r === 'string') {
    try {
      r = JSON.parse(r);
    } catch (e) {
      r = null;
    }
  }
  if (!r || typeof r !== 'object') {
    return { version: DM_SETTINGS_VERSION, mode: DM_MODE_DEFAULT, size: DM_SIZE_DEFAULT };
  }
  return {
    version: DM_SETTINGS_VERSION,
    mode: DM_MODES.indexOf(r.mode) >= 0 ? r.mode : DM_MODE_DEFAULT,
    size: DM_SIZES.indexOf(r.size) >= 0 ? r.size : DM_SIZE_DEFAULT
  };
}

/* 轮转：off → full → half → quarter → off；脏值直接给 DM_MODE_DEFAULT */
export function nextDmMode(mode) {
  const i = DM_MODES.indexOf(mode);
  if (i < 0) return DM_MODE_DEFAULT;
  return DM_MODES[(i + 1) % DM_MODES.length];
}

/* 字号轮转：small → mid → large → small；脏值按 mid 处理 */
export function nextDmSize(size) {
  const i = DM_SIZES.indexOf(size);
  return DM_SIZES[i < 0 ? 1 : (i + 1) % DM_SIZES.length];
}

/* 档位 → 传给 native 的逻辑像素。脏值回落 mid */
export function dmSizePx(size) {
  const px = DM_SIZE_PX[size];
  return px > 0 ? px : DM_SIZE_PX[DM_SIZE_DEFAULT];
}

/* 播放位置（毫秒）→ 分片序号（1 起）。负数 / 脏值按 0 处理 */
export function segmentIndexAt(posMs) {
  const p = Number(posMs);
  const q = isFinite(p) && p > 0 ? Math.floor(p) : 0;
  return Math.floor(q / SEG_MS) + 1;
}

/* 按 idStr 去重：seen 是跨片共享的 { idStr: 1 } 字典，就地登记，返回过滤后的新数组。
 * 没有 id 的条目无法判重 → 保留（丢一条真实弹幕比多渲染一条更糟）。 */
export function dedupeByKey(list, seen) {
  const src = Array.isArray(list) ? list : [];
  const bag = seen && typeof seen === 'object' ? seen : {};
  const out = [];
  for (let i = 0; i < src.length; i++) {
    const it = src[i];
    if (!it) continue;
    const key = String(it.idStr || it.id || '');
    if (!key) {
      out.push(it);
      continue;
    }
    if (bag[key]) continue;
    bag[key] = 1;
    out.push(it);
  }
  return out;
}

/* 单条在 posMs 是否该出现在屏幕上：滚动类 [progress, +8000)，顶部/底部 [progress, +4000) */
export function danmakuVisibleAt(item, posMs) {
  if (!item) return false;
  const p = Number(item.progress);
  const q = Number(posMs);
  if (!isFinite(p) || !isFinite(q)) return false;
  const m = toInt(item.mode);
  const life = m === 4 || m === 5 ? DM_FIXED_LIFE_MS : DM_SCROLL_LIFE_MS;
  return q >= p && q < p + life;
}

/* 纯函数：从（多片合并的）数组里取「此刻可见」的一批，不排序也不改原数组 */
export function pickWindow(list, posMs) {
  const src = Array.isArray(list) ? list : [];
  const out = [];
  for (let i = 0; i < src.length; i++) if (danmakuVisibleAt(src[i], posMs)) out.push(src[i]);
  return out;
}

/* 迷你分片缓存：按 index 存已解析的片，超出容量丢最旧的（顺序播放，几片就够当前+预取）。 */
export function createSegCache(limit) {
  const max = Math.max(1, toInt(limit) || 8);
  const map = {};
  const order = [];
  return {
    has: (i) => !!map[i],
    get: (i) => map[i] || null,
    put: (i, list) => {
      if (!map[i]) {
        order.push(i);
        if (order.length > max) delete map[order.shift()];
      }
      map[i] = Array.isArray(list) ? list : [];
    },
    keys: () => order.slice(0),
    clear: () => {
      for (let i = 0; i < order.length; i++) delete map[order[i]];
      order.length = 0;
    }
  };
}

/* 按码点截断，避免把 emoji 的代理对切成半个 */
function cutByCodePoint(s, max) {
  if (s.length <= max) return s;
  let n = 0;
  for (let i = 0; i < s.length; i++) {
    if (n + 1 > max) return s.slice(0, i);
    n += 1;
    const c = s.charCodeAt(i);
    if (c >= 0xd800 && c <= 0xdbff) i += 1;
  }
  return s;
}

function cleanFeedText(s) {
  let t = String(s == null ? '' : s);
  /* 竖线是字段分隔符、换行会把一条弹幕拆成两行 —— 这两个必须清掉 */
  t = t.replace(/[|\r\n]/g, '').replace(/[\u0000-\u001f\u007f]/g, ' ');
  return cutByCodePoint(t.replace(/\s+/g, ' ').trim(), DM_FEED_TEXT_MAX);
}

/* 每行 `id|progress|mode|color|fontsize|content`，行间 \n。
 * id 用 idStr（字符串，超 2^53 也不丢精度），native 侧按它做队列去重与回收。 */
export function buildFeedText(list) {
  const src = Array.isArray(list) ? list : [];
  const lines = [];
  for (let i = 0; i < src.length; i++) {
    const it = src[i];
    if (!it) continue;
    const text = cleanFeedText(it.text);
    if (!text) continue;
    lines.push(
      String(it.idStr || it.id || '') +
        '|' + toInt(it.progress) +
        '|' + toInt(it.mode) +
        '|' + toInt(it.color) +
        '|' + toInt(it.fontsize) +
        '|' + text
    );
  }
  return lines.join('\n');
}

/* client 由调用方注入（src/services/bili/client.js），只要求有 getBinary(url, { timeout })。
 * 返回 { ok, list, count, index, message, stage }；stage: param/client/transport/http/parse/ok */
export async function fetchSegment(client, opts) {
  const o = opts || {};
  const aid = toInt(o.aid);
  const cid = toInt(o.cid);
  const index = toInt(o.index);
  if (!client || typeof client.getBinary !== 'function') {
    return { ok: false, list: [], count: 0, index: index, message: 'client.getBinary 不可用', stage: 'client' };
  }
  if (!(aid > 0) || !(cid > 0) || !(index > 0)) {
    return { ok: false, list: [], count: 0, index: index, message: 'aid/cid/segment_index 非法', stage: 'param' };
  }
  const url = DM_SEG_URL + '?type=1&oid=' + cid + '&pid=' + aid + '&segment_index=' + index;
  let res;
  try {
    res = await client.getBinary(url, { timeout: toInt(o.timeout) || DM_FETCH_TIMEOUT_MS });
  } catch (e) {
    const msg = (e && e.message) || String(e);
    logWarn('[danmaku] 分片 ' + index + ' 取数异常: ' + msg);
    return { ok: false, list: [], count: 0, index: index, message: msg, stage: 'transport' };
  }
  if (!res || res.ok !== true) {
    const stage = (res && res.stage) || 'http';
    const msg = (res && res.message) || '取弹幕分片失败';
    logWarn('[danmaku] 分片 ' + index + ' 失败 stage=' + stage + ' msg=' + msg);
    return { ok: false, list: [], count: 0, index: index, message: msg, stage: stage };
  }
  const parsed = parseSegReply(res.body);
  if (!parsed.ok) {
    logWarn('[danmaku] 分片 ' + index + ' 解析失败: ' + parsed.message);
    return { ok: false, list: [], count: 0, index: index, message: parsed.message, stage: 'parse' };
  }
  return { ok: true, list: parsed.list, count: parsed.count, index: index, message: '', stage: 'ok' };
}
