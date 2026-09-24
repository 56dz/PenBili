// make_icon_v2 第二步：favicon.svg(图形) × icon_colors.json(ICO 颜色) -> app_icon.png (256x256)
// 设计（用户定义）：**背景透明，小电视图形线条 = ICO 主色(#00a1d6 B站蓝)**
// - svg 的 fill="currentColor" 替换为 ICO 主色
// - 不铺背景矩形（透明底）
// - @resvg/resvg-js 矢量渲染（全抗锯齿）——替换旧的最近邻硬放大（"不够清晰"的根因）
const fs = require('fs');
const path = require('path');
const { Resvg } = require('@resvg/resvg-js');

const root = path.resolve(__dirname, '..');
const svgPath = path.join(root, 'favicon.svg');
const colors = JSON.parse(fs.readFileSync(path.join(root, 'tools', 'icon_colors.json'), 'utf8'));
const outPath = path.join(root, 'app_icon.png');
const SIZE = 256;

let svg = fs.readFileSync(svgPath, 'utf8');
// 图形色 = ICO 主色（B站蓝）；保持根 svg 的 fill="none" → 透明背景
svg = svg.replace(/fill="currentColor"/g, 'fill="' + colors.bg + '"');

const resvg = new Resvg(svg, { fitTo: { mode: 'width', value: SIZE } });
const rendered = resvg.render();
// asPng() 挂在 render() 的返回值上（挂错位置会写成裸 RGBA —— 自检 PNG 头即能戳穿）
const png = typeof rendered.asPng === 'function' ? rendered.asPng() : Buffer.from(rendered.pixels);
fs.writeFileSync(outPath, png);

// 自检输出（模型不能读图 → 用像素统计证明"透明底 + 蓝色图形"）：
//   transparent 占比应显著 >0（背景空）；blue 占比 = 图形面积
const raw = rendered.pixels; // RGBA Uint8Array
let transparent = 0;
let blue = 0;
const seen = new Set();
for (let i = 0; i < raw.length; i += 4) {
  const r = raw[i], g = raw[i + 1], b = raw[i + 2], a = raw[i + 3];
  if (a < 32) transparent++;
  if (a > 200 && Math.abs(r - 0) < 40 && Math.abs(g - 161) < 45 && Math.abs(b - 214) < 45) blue++;
  if (i % 64 === 0 && a > 0) seen.add((r << 16) | (g << 8) | b);
}
const totalPx = raw.length / 4;
console.log('icon_v2: ' + SIZE + 'x' + SIZE +
  ' transparent=' + (100 * transparent / totalPx).toFixed(1) + '%' +
  ' blueLines=' + (100 * blue / totalPx).toFixed(1) + '%' +
  ' visibleColors>=' + seen.size +
  ' bytes=' + png.length);
console.log('colors: stroke=' + colors.bg + '（ICO主色，透明底）');
