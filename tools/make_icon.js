// 生成 app_icon.png（192x192，纯 Node zlib 手写 PNG，无第三方依赖）
// 词典笔桌面在没有 icon 时不显示应用，因此这个文件是打包必需项。
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const SIZE = 192;

function crc32(buf) {
  let c;
  const table = [];
  for (let n = 0; n < 256; n++) {
    c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  let crc = 0xffffffff;
  for (let i = 0; i < buf.length; i++) crc = table[(crc ^ buf[i]) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length, 0);
  const typeBuf = Buffer.from(type, 'ascii');
  const body = Buffer.concat([typeBuf, data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body), 0);
  return Buffer.concat([len, body, crc]);
}

function makePng(size) {
  const raw = Buffer.alloc(size * (size * 4 + 1));
  const cx = size / 2;
  const cy = size / 2;
  const r = size * 0.42;
  for (let y = 0; y < size; y++) {
    const rowStart = y * (size * 4 + 1);
    raw[rowStart] = 0; // filter: none
    for (let x = 0; x < size; x++) {
      const o = rowStart + 1 + x * 4;
      // 背景：深蓝到紫的垂直渐变
      const t = y / (size - 1);
      let R = Math.round(18 + 30 * t);
      let G = Math.round(24 + 40 * t);
      let B = Math.round(40 + 90 * t);
      let A = 255;
      // 圆形遮罩外透明
      const dx = x - cx;
      const dy = y - cy;
      const dist = Math.sqrt(dx * dx + dy * dy);
      if (dist > r) {
        A = 0;
      } else if (dist > r - 3) {
        A = 160;
      }
      // 播放三角形（向右），位于圆心
      const inTri =
        x >= cx - size * 0.14 &&
        x <= cx + size * 0.20 &&
        Math.abs(y - cy) <= ((x - (cx - size * 0.14)) / (size * 0.34)) * size * 0.20;
      if (inTri && A > 0) {
        R = 240;
        G = 248;
        B = 255;
      }
      raw[o] = R;
      raw[o + 1] = G;
      raw[o + 2] = B;
      raw[o + 3] = A;
    }
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(size, 0);
  ihdr.writeUInt32BE(size, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 6; // RGBA
  ihdr[10] = 0;
  ihdr[11] = 0;
  ihdr[12] = 0;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0))
  ]);
}

const out = path.join(__dirname, '..', 'app_icon.png');
fs.writeFileSync(out, makePng(SIZE));
console.log('wrote ' + out + ' (' + fs.statSync(out).size + ' bytes, ' + SIZE + 'x' + SIZE + ')');
