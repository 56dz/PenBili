// 屏幕几何（纯函数，Node 可单测）
//
// 设备 profile（X6 Pro 真机截屏 + cfg.json 证实）：
//   logical  : 960 x 266  （Falcon UI 合成面）
//   physical : 266 x 960  （面板，direction=270）
//   触控     : direction=270, tp_xoffset=113
//
// 本项目布局（v3 定稿）：左栏 76 + 内容 884 = 960（主页）；
// 播放态 v4 布局（用户手绘稿，m11950）：左 2 : 中 4.4 : 右 2.6 ⇒ 213 : 469 : 278 = 960。
//   左列上 1.8 高（192）= 视频标题 + 简介；左列下 0.7 高（74）= 点赞/投币/收藏 三个按钮；
//   中列 469x240 = 视频，其下 26px = 可拖动进度条；
//   右列上 2.25 高（240）= 评论区；右列下 0.25 高（26）= 输入框 + 发送按钮。
//   高度基准：1.8 + 0.7 = 2.25 + 0.25 = 2.5 单位 ⇒ 1 单位 = 266/2.5 = 106.4px。
//
// 为什么视频区高度是 240 而不是 266：X6 Pro 的视频走 KMS overlay 平面，
// **盖在 Falcon UI 之上**，UI 画不上去。所以进度条必须落在视频矩形之外。

export const LOGICAL = { width: 960, height: 266 };
export const PHYSICAL = { width: 266, height: 960 };
export const DIRECTION = 270;

// 面板列偏移：266 宽的逻辑列在 480 宽面板里的起始列。
// native（player.c `#define PANEL_X_OFFSET 107`）在拼 render-rectangle 时加上它；
// screen.js 只产出 266 列内的坐标，所以 x 端是 0..266 而不是 107..373。
export const PANEL_X_OFFSET = 107;

// 主页左栏宽度（76 + 884 = 960）
export const BAR_WIDTH = 76;

// 主页视频网格列数：.grid 870 宽 / .vrow 174（166 + margin-right 8）⇒ 5 列。
// 用户要求 m11950-3：加载更多后条目数必须能被列数整除，否则最后一行填不满。
export const GRID_COLS = 5;

// 把条目列表裁到列数的整数倍（尾部余数丢掉），并报告是否发生了裁剪。
// 只裁尾部、不重排，保证已有卡片位置不变。
export function trimToGridRows(items, cols) {
  const list = Array.isArray(items) ? items : [];
  const n = Math.max(1, Math.round(Number(cols) || GRID_COLS));
  const rem = list.length % n;
  if (rem === 0) return { items: list, trimmed: 0 };
  return { items: list.slice(0, list.length - rem), trimmed: rem };
}

// 播放态三列宽度（用户手绘稿 m11950：左 2 : 中 4.4 : 右 2.6）
//   213 + 469 + 278 = 960
export const LEFT_COL_WIDTH = 213;
export const MID_COL_WIDTH = 469;
export const RIGHT_COL_WIDTH = 278;

// 左列上下两块高度（1.8 : 0.7 单位，1 单位 = 106.4px）
export const LEFT_INFO_H = 192;
export const LEFT_BTN_H = 74;

// 右列上下两块高度（2.25 : 0.25 单位）
export const RIGHT_BODY_H = 240;
export const RIGHT_INPUT_H = 26;

// 视频下方进度条高度
export const SEEK_H = 26;

// 播放态视频区（逻辑面坐标）：x 紧接左列，底部 26px 留给进度条
export const VIDEO_AREA = { x: LEFT_COL_WIDTH, y: 0, width: MID_COL_WIDTH, height: 240 };

// 旧名兼容（图文态仍用「图标栏」这个概念时不再需要，保留导出避免既有 import 报错）
export const ICON_BAR_WIDTH = LEFT_COL_WIDTH;
export const ICON_BAR_GAP = 0;

// 视频列逻辑宽度（= VIDEO_AREA.width，保留旧名给既有调用方）
export const VIDEO_COL_WIDTH = VIDEO_AREA.width;

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

// 本项目的视频逻辑矩形：信箱适配后居中放在视频区 VIDEO_AREA 内
export function probeLogicalVideoRect(videoWidth, videoHeight) {
  const fit = fitLogicalRect(videoWidth, videoHeight, {
    width: VIDEO_AREA.width,
    height: VIDEO_AREA.height
  });
  return { x: VIDEO_AREA.x + fit.x, y: VIDEO_AREA.y + fit.y, width: fit.width, height: fit.height };
}

// 逻辑矩形 → 物理矩形（**真·旋转变换**，不是"面板内居中"近似）
//
// 由真机像素测量反推（详见 docs/x6pro-video-playback.md §2.1）：
// 逻辑面上粉色 accent 的 bbox (10,0)-(932,170) 落在面板 (108,28)-(276,948)，
// 唯一自洽的映射是
//     panel_x = PANEL_X_OFFSET + logical_y      （逻辑 y 沿面板 +x）
//     panel_y = 960 − logical_x                 （逻辑 x 沿面板 −y，反向）
// C 侧的 PANEL_X_OFFSET=107 由 native 加，这里只给 266 宽列内的坐标，
// 所以 x 端就是 logical_y，矩形则是两端点取包围盒：
//     phys.x = rect.y
//     phys.y = panel.height − (rect.x + rect.width)
//     phys.w = rect.height,  phys.h = rect.width
//
// 旧实现是 x=(266-h)/2（恒 0）、y=(960-w)/2（面板内居中）。它在
// "视频本来就水平居中于 960 逻辑面"时与真变换重合（老布局 16:9 差 1px），
// 一旦左右栏宽度变化或视频不再占满整列高度，就会偏出去（新布局 16:9 偏 65px）。
export function logicalRectToPhysicalRect(rect, logical, physical) {
  const box = logical || LOGICAL;
  const panel = physical || PHYSICAL;
  const x = Math.round(num(rect.x, 0));
  const y = Math.round(typeof rect.y === 'number' ? rect.y : 0);
  const w = Math.round(num(rect.width, box.height));
  const h = Math.round(num(rect.height, box.width));
  return {
    x: y,
    y: panel.height - (x + w),
    width: h,
    height: w
  };
}

// native 需要的物理视频矩形（含偶数对齐，避免行字节错位）
export function probeVideoRects(videoWidth, videoHeight) {
  const logical = probeLogicalVideoRect(videoWidth, videoHeight);
  const physical = logicalRectToPhysicalRect(logical, LOGICAL, PHYSICAL);
  return { logical: logical, physical: physical };
}

// 播放态视频矩形：信箱适配到 VIDEO_AREA(448x240) 并在区内居中。
//
// 不能用全屏 960x266 作 box：21:9 等比 16:9 更宽的视频会按高适配得 w>448 →
// blit 越出视频区、溢出到左右栏（X5 真机实测 bug）。也不能只约束宽度：
// 视频区高度是 240（底部 26px 留给进度条，overlay 平面盖住 UI 画不上去）。
export function playVideoRects(videoWidth, videoHeight) {
  const fit = fitLogicalRect(videoWidth, videoHeight, {
    width: VIDEO_AREA.width,
    height: VIDEO_AREA.height
  });
  const logical = {
    x: VIDEO_AREA.x + fit.x,
    y: VIDEO_AREA.y + fit.y,
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
