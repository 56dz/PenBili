// 纯 JS MD5（QuickJS 无 crypto 且 manifest bigNum=false；B 站 WBI 签名必需）。
// 输入按 UTF-8 编码为字节流后做 MD5，返回小写 hex。
// K 表直接内嵌标准常量（floor(abs(sin(i+1)) * 2^32)），避免 Math.sin 跨平台误差。
// 测试：test/run.js 对拍 node crypto 与官方向量。

const K = [
  0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
  0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
  0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
  0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
  0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
  0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
  0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
  0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
  0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
  0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
  0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
  0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
  0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
  0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
  0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
  0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
];

// 每轮的循环左移量：Round1 每列相同 7,12,17,22；Round2 5,9,14,20；Round3 4,11,16,23；Round4 6,10,15,21
const S = [
  7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
  5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
];

function utf8Bytes(str) {
  const out = [];
  for (let i = 0; i < str.length; i++) {
    let c = str.charCodeAt(i);
    if (c < 0x80) {
      out.push(c);
    } else if (c < 0x800) {
      out.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
    } else if (c >= 0xd800 && c <= 0xdbff && i + 1 < str.length) {
      const lo = str.charCodeAt(i + 1);
      const cp = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
      i++;
      out.push(
        0xf0 | (cp >> 18),
        0x80 | ((cp >> 12) & 0x3f),
        0x80 | ((cp >> 6) & 0x3f),
        0x80 | (cp & 0x3f)
      );
    } else {
      out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
    }
  }
  return out;
}

function rotl(x, n) {
  return ((x << n) | (x >>> (32 - n))) >>> 0;
}

function pushU32LE(arr, v) {
  arr.push(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff);
}

// MD5 digest = 每个 32 位字按**小端字节序**输出（d4 1d 8c d9 ← A=0xd98c1dd4）
function hex8(v) {
  const u = v >>> 0;
  let s = '';
  for (let i = 0; i < 4; i++) {
    const b = (u >>> (i * 8)) & 0xff;
    s += (b < 0x10 ? '0' : '') + b.toString(16);
  }
  return s;
}

export function md5Hex(input) {
  const str = String(input == null ? '' : input);
  const msg = utf8Bytes(str);
  const lo = (msg.length * 8) >>> 0;
  const hi = Math.floor(msg.length / 0x20000000) >>> 0;

  msg.push(0x80);
  while (msg.length % 64 !== 56) msg.push(0);
  pushU32LE(msg, lo);
  pushU32LE(msg, hi);

  let a = 0x67452301;
  let b = 0xefcdab89;
  let c = 0x98badcfe;
  let d = 0x10325476;

  for (let off = 0; off < msg.length; off += 64) {
    const x = new Array(16);
    for (let i = 0; i < 16; i++) {
      const p = off + i * 4;
      x[i] = (msg[p] | (msg[p + 1] << 8) | (msg[p + 2] << 16) | (msg[p + 3] << 24)) | 0;
    }
    let aa = a;
    let bb = b;
    let cc = c;
    let dd = d;
    for (let i = 0; i < 64; i++) {
      let f;
      let g;
      if (i < 16) {
        f = (bb & cc) | (~bb & dd);
        g = i;
      } else if (i < 32) {
        f = (dd & bb) | (~dd & cc);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        f = bb ^ cc ^ dd;
        g = (3 * i + 5) % 16;
      } else {
        f = cc ^ (bb | ~dd);
        g = (7 * i) % 16;
      }
      const tmp = dd;
      dd = cc;
      cc = bb;
      bb = (bb + rotl((aa + f + K[i] + x[g]) | 0, S[i])) | 0;
      aa = tmp;
    }
    a = (a + aa) | 0;
    b = (b + bb) | 0;
    c = (c + cc) | 0;
    d = (d + dd) | 0;
  }

  return hex8(a) + hex8(b) + hex8(c) + hex8(d);
}
