// 真实弹幕（protobuf 二进制）
//
// 接口（事实来源：github.com/xieren58/bilibili-API-collect docs/danmaku/ + 开发机实测 2026-09-25）：
//   GET https://api.bilibili.com/x/v2/dm/web/seg.so?type=1&oid=<cid>&pid=<aid>&segment_index=N
//     · **oid = 视频 cid**（不是 aid！传 aid 返回空段——12 次全空实测踩坑）
//     · 认证=半匿名（无 SESSDATA 只返回部分弹幕；登录后全量——headers 自动带 Cookie）
//     · 6 分钟一包，progress 值域 [0, 360000) 每包；6000 条/包上限
//     · 实测该端点 200 application/octet-stream；老 list.so 端点已下线(HTML 404)
//
// proto（DmSegMobileReply / DanmakuElem，字段表见 collect danmaku_proto.md）：
//   外层 field1 = repeated DanmakuElem（LEN）
//   elem: 1=id(int64) 2=progress(ms) 3=mode 4=fontsize 5=color(RGB888)
//         6=midHash(str) 7=content(utf-8 str) 8=ctime 9=weight 10=action 11=pool 12=idStr 13=attr 14=animation
//   ★ key 是 varint（field≥16 的新字段 key 为多字节——单字节读会错位到非法 wt7；
//     实测老视频 elem 含 field20/21/26+，外层含 field4）→ key/len 全部 varint 读、未知字段按 wire 跳过。
//   解码金标准：fixtures/dm_seg.bin（炮姐 av810872/cid=1176840 分包1，1.2MB/5863条）。
//
// mode 过滤：1/2/3=普通(滚动) 4=底部 5=顶部 → 显示；
//   6=逆向 7=高级(定位指令文本语义不明) 8=代码 → 跳过（宁缺勿乱）。

export const SEG_URL = 'https://api.bilibili.com/x/v2/dm/web/seg.so';
export const SEG_DURATION_MS = 360000; /* 6 分钟一包 */
const SHOW_MODES = { 1: 1, 2: 1, 3: 1, 4: 1, 5: 1 };

export function segUrl(cid, aid, segmentIndex) {
  return SEG_URL + '?type=1&oid=' + Math.floor(Number(cid) || 0) +
    (aid ? '&pid=' + Math.floor(Number(aid)) : '') +
    '&segment_index=' + Math.max(1, Math.floor(Number(segmentIndex) || 1));
}

export function segCount(durationMs) {
  return Math.max(1, Math.ceil((Number(durationMs) || 60000) / SEG_DURATION_MS));
}

// jsapi body（可能 ArrayBuffer / TypedArray / 字符串）→ 字节
// 字符串按 latin1 逐字符取低 8 位（若 jsapi 用 UTF-8 解码破坏字节 → 魔数 0x0A 校验兜出，
// 调用方据此降级保留种子弹幕）。
export function toBytes(body) {
  if (!body) return null;
  if (body instanceof ArrayBuffer) return new Uint8Array(body);
  if (ArrayBuffer.isView && ArrayBuffer.isView(body)) return new Uint8Array(body.buffer, body.byteOffset, body.byteLength);
  if (typeof body === 'string') {
    const a = new Uint8Array(body.length);
    for (let i = 0; i < body.length; i++) a[i] = body.charCodeAt(i) & 0xff;
    return a;
  }
  return null;
}

// 魔数判定放宽（实测 2026-09-25）：段1 可能 elems 为空、以 field4 状态字段开头（0x22）——
// 首字节的低3位须是合法 wire type {0,1,2,5} 且 tag≥8（field≥1）：
//   0x0A(elems)/0x22(field4) 放行；'<'(0x3c wire4)/'{'(0x7b wire3) 自动排除 HTML/JSON。
//   0x22 开头的 JSON 字符串字面量会漏进来 → parseDanmakuSeg 解析失败兜出（双保险）。
export function isDanmakuBytes(a) {
  if (!a || a.length === 0) return false;
  const first = a[0];
  const wt = first & 7;
  return first >= 8 && (wt === 0 || wt === 1 || wt === 2 || wt === 5);
}

function readVarint(a, p) {
  let v = 0;
  let s = 0;
  for (;;) {
    if (p >= a.length) throw new Error('varint oob@' + p);
    const b = a[p++];
    v += (b & 0x7f) * Math.pow(2, s);
    if (!(b & 0x80)) break;
    s += 7;
    if (s > 70) throw new Error('varint too long@' + p);
  }
  return [v, p];
}

// UTF-8 字节 → 字符串（QuickJS 无 TextDecoder 依赖 → 手写 1-4 字节序列）
function utf8Decode(bytes) {
  let out = '';
  let i = 0;
  while (i < bytes.length) {
    const b = bytes[i++];
    let cp = b;
    if (b >= 0xf0) {
      cp = (b & 0x07) << 18 | (bytes[i++] & 0x3f) << 12 | (bytes[i++] & 0x3f) << 6 | (bytes[i++] & 0x3f);
    } else if (b >= 0xe0) {
      cp = (b & 0x0f) << 12 | (bytes[i++] & 0x3f) << 6 | (bytes[i++] & 0x3f);
    } else if (b >= 0xc0) {
      cp = (b & 0x1f) << 6 | (bytes[i++] & 0x3f);
    }
    out += String.fromCodePoint(cp);
  }
  return out;
}

// bytes → [{p, text}]（按 mode 过滤；key/len 全 varint、未知字段按 wire 跳过）
// 返回 {ok, items} | {ok:false, reason}
// 入口与 isDanmakuBytes 同一判定：**不强制 0x0A**（段1 elems 可空、以 field4 状态包开头）。
export function parseDanmakuSeg(a) {
  if (!a || !a.length || !isDanmakuBytes(a)) return { ok: false, reason: 'bad head' };
  const out = [];
  let p = 0;
  try {
    while (p < a.length) {
      let key;
      [key, p] = readVarint(a, p);
      const field = key >> 3;
      const wt = key & 7;
      if (field === 1 && wt === 2) {
        let elen;
        [elen, p] = readVarint(a, p);
        const end = p + elen;
        let progress = 0;
        let mode = 1;
        let content = '';
        while (p < end) {
          let k2;
          [k2, p] = readVarint(a, p);
          const f = k2 >> 3;
          const w = k2 & 7;
          if (w === 0) {
            let v;
            [v, p] = readVarint(a, p);
            if (f === 2) progress = v;
            else if (f === 3) mode = v;
          } else if (w === 2) {
            let l;
            [l, p] = readVarint(a, p);
            if (f === 7) content = utf8Decode(a.subarray(p, p + l));
            p += l;
          } else if (w === 5) {
            p += 4;
          } else if (w === 1) {
            p += 8;
          } else {
            return { ok: false, reason: 'elem wt ' + w + ' f ' + f };
          }
        }
        if (p !== end) return { ok: false, reason: 'misalign' };
        if (SHOW_MODES[mode] && content) out.push({ p: Math.floor(progress), text: content });
      } else if (wt === 2) {
        let l;
        [l, p] = readVarint(a, p);
        p += l;
      } else if (wt === 0) {
        let v;
        [v, p] = readVarint(a, p);
      } else if (wt === 5) {
        p += 4;
      } else if (wt === 1) {
        p += 8;
      } else {
        return { ok: false, reason: 'outer wt ' + wt + ' f ' + field };
      }
    }
  } catch (e) {
    return { ok: false, reason: String(e && e.message) };
  }
  return { ok: true, items: out };
}
