// 屏幕几何（纯函数，Node 可单测）
//
// 设备 profile（profiles/youdao-x5.md，真机截屏 + cfg.json 证实）：
//   logical  : 800 x 254  （Falcon UI 合成面，setViewPort(800)）
//   physical : 254 x 800  （/dev/fb0，direction=270）
//   触控     : direction=270, tp_xoffset=113
//
// 本项目布局：视频洞放**逻辑左列** {0,0,452,254}，右列 348px 给步骤面板。
// native 直接写 /dev/fb0，因此 JS 必须给出**物理像素矩形**。

export const LOGICAL = { width: 800, height: 254 };
export const PHYSICAL = { width: 254, height: 800 };
export const DIRECTION = 270;

// 左列视频洞的逻辑宽度（16:9 信箱适配 800x254 → 452x254）
export const VIDEO_COL_WIDTH = 452;

function num(v, fallback) {
  const n = typeof v === 'number' ? v : parseFloat(v);
  return isFinite(n) && n > 0 ? Math.round(n) : fallback;
}

// 逻辑空间信箱适配：返回 { x, y, width, height }（居中）
export function fitLogicalRect(videoWidth, videoHeight, logical) {
  const box = logical || LOGICAL;
  const vw = num(videoWidth, 640);
  const vh = num(videoHeight, 360);
  const boxAspect = box.width / box.height;
  const aspect = vw / vh;
  let w;
  let h;
  if (aspect >= boxAspect) {
    w = box.width;
    h = Math.round(box.width / aspect);
  } else {
    h = box.height;
    w = Math.round(box.height * aspect);
  }
  if (w > box.width) w = box.width;
  if (h > box.height) h = box.height;
  return {
    x: Math.round((box.width - w) / 2),
    y: Math.round((box.height - h) / 2),
    width: w,
    height: h
  };
}

// 本项目的视频逻辑矩形：信箱适配后靠左放在视频列内（右侧面板不被盖住）
export function probeLogicalVideoRect(videoWidth, videoHeight) {
  const fit = fitLogicalRect(videoWidth, videoHeight, LOGICAL);
  return { x: 0, y: fit.y, width: fit.width, height: fit.height };
}

// 逻辑矩形 → 物理矩形（旋转 90/270 时长宽互换，居中）
export function logicalRectToPhysicalRect(rect, logical, physical) {
  const box = logical || LOGICAL;
  const panel = physical || PHYSICAL;
  const w = num(rect.width, box.height);
  const h = num(rect.height, box.width);
  const pw = h;
  const ph = w;
  return {
    x: Math.round((panel.width - pw) / 2),
    y: Math.round((panel.height - ph) / 2),
    width: pw,
    height: ph
  };
}

// native 需要的物理视频矩形（含偶数对齐，避免行字节错位）
export function probeVideoRects(videoWidth, videoHeight) {
  const logical = probeLogicalVideoRect(videoWidth, videoHeight);
  const physical = logicalRectToPhysicalRect(logical, LOGICAL, PHYSICAL);
  return { logical: logical, physical: physical };
}

// 左侧选项栏宽度（浏览态与播放态共用同一列）
export const BAR_WIDTH = 174;

// 播放态视频矩形：**必须信箱适配到视频列 452x254**（不能用全屏 800 作 box：
// 21:9 等比 16:9 更宽的视频在全屏 box 下按高适配得 w=593>452 → blit 越出列、
// 溢出到左右栏（真机实测 bug）；16:9 恰好 452 所以历史未暴露）。
// 列内居中与全屏居中数学恒等（174+(452-w)/2 ≡ (800-w)/2），logicalRectToPhysicalRect
// 的"居中=真实旋转变换"前提（video_player 真机证据）继续成立。
export function playVideoRects(videoWidth, videoHeight) {
  const fit = fitLogicalRect(videoWidth, videoHeight, { width: VIDEO_COL_WIDTH, height: LOGICAL.height });
  const logical = {
    x: Math.round((LOGICAL.width - fit.width) / 2),
    y: fit.y,
    width: fit.width,
    height: fit.height
  };
  const physical = logicalRectToPhysicalRect(logical, LOGICAL, PHYSICAL);
  return { logical: logical, physical: physical };
}

// 逻辑点 → 物理点（四种 direction 的纯函数；触控/调试对拍用）
export function logicalToPhysicalPoint(point, direction, logical, physical) {
  const box = logical || LOGICAL;
  const panel = physical || PHYSICAL;
  const x = Math.round(typeof point.x === 'number' ? point.x : 0);
  const y = Math.round(typeof point.y === 'number' ? point.y : 0);
  const cx = Math.min(Math.max(x, 0), box.width - 1);
  const cy = Math.min(Math.max(y, 0), box.height - 1);
  const dir = ((Math.round(direction / 90) * 90) % 360 + 360) % 360;
  if (dir === 0) return { x: cx, y: cy };
  if (dir === 90) return { x: cy, y: box.width - 1 - cx };
  if (dir === 180) return { x: box.width - 1 - cx, y: box.height - 1 - cy };
  // 270
  return { x: box.height - 1 - cy, y: cx };
}

// 物理矩形是否落在面板内（native 侧入参校验的前置检查）
export function isRectInsidePanel(rect, panel) {
  const p = panel || PHYSICAL;
  if (!rect) return false;
  const x = Math.round(rect.x);
  const y = Math.round(rect.y);
  const w = Math.round(rect.width);
  const h = Math.round(rect.height);
  if (w <= 0 || h <= 0) return false;
  if (x < 0 || y < 0) return false;
  if (x + w > p.width || y + h > p.height) return false;
  return true;
}
