// 设备显示 profile —— **写死的两档**（X7 Pro / X6 Pro），不再做任何运行时自适应。
//
// ==================== 3.5.6 第二十四轮：删掉自适应分辨率 ====================
// 前二十三轮这里是一个"探测 + 覆盖 + 档位"的小系统：
//   · 从 $falcon.env.deviceWidth/deviceHeight 猜视口（还要排除"报成面板尺寸"的固件）；
//   · storage 里的 bili_device_profile 可覆盖任意字段；
//   · 自检页 A/B/C/D 四档现场改 swScale / bufScale / panelXOffset。
//
// 用户明确要求「删除所有与自适应分辨率有关的代码，把 x7p 和 x6p 写死」——
// 这套机制本身就是 bug 来源：同一份包在不同固件上算出的矩形不同，真机上出现
// "这次正常、下次变扁"时根本不知道用的是哪一档；而且 storage 里一次误点会永久
// 留下覆盖值（第十六轮 A/C 档的 swScale=1 让整片绿幕，就是这条路径）。
//
// 现在只剩两张常量表。分辨率与参考系统播放器一致：
//   X7 Pro  936 x 280（面板 480x960，逻辑带列偏移 0）
//   X6 Pro  960 x 266（面板 480x960，逻辑带列偏移 107）
//
// 没有 env 探测、没有 storage 覆盖、没有比例档位。要改机型就改这张表。
// ==========================================================================

import { fitLogicalRect, logicalRectToPhysicalRect } from './screen.js';
import { buildLayout } from './layout.js';

// 设计画布**基准**（X6 Pro 的原生画布，单测/文档对照用）
export const DESIGN = { width: 960, height: 266 };
// DRM 面板物理尺寸（RK3562 单模式 480x960@59.88）
export const PANEL = { width: 480, height: 960 };

/*
 * 两台机器的全部参数。**这就是全部** —— 没有第三处定义。
 *
 * planeId / swScale 的取值来自真机定案（docs/x7pro-adaptation.md）：
 *   · X7 Pro：planeId=-1（kmssink 自选）+ swScale=2（videoscale 不互换宽高）。
 *     这台机器的 mppvideodec 会把 buffer 真的转成竖帧；swScale=1 的"互换"
 *     路线会拿横 buffer 去贴竖窗口，只剩一部分行，其余是未初始化 YUV = 绿幕。
 *     别再改回 1，也别再写死 76（X7 固件上那个 plane 对象号不可用，
 *     set_state(PLAYING) 直接 FAILURE）。
 *   · X6 Pro：planeId=76（Esmart1-win0）+ swScale=0（硬件缩放，0 拷贝）。
 *     这套是 3.5.5 起就在真机验证过的，逐字节不要动。
 */
export const X7P = {
  name: 'x7p',
  width: 936,
  height: 280,
  panelWidth: PANEL.width,
  panelHeight: PANEL.height,
  panelXOffset: 0,
  panelYOffset: 0,
  rowOffset: 0,
  direction: 270,
  transpose: 2,
  planeId: -1,
  swScale: 2,
  bufScale: 100,
  /* 控制条浮在画面上（参考系统播放器的做法）：视频矩形不扣控制条高度，
   * 底部 32px 由半透明控制条覆盖。详见 play_chrome.js 文件头。 */
  barOverlay: true
};

export const X6P = {
  name: 'x6p',
  width: 960,
  height: 266,
  panelWidth: PANEL.width,
  panelHeight: PANEL.height,
  panelXOffset: 107,
  panelYOffset: 0,
  rowOffset: 0,
  direction: 270,
  transpose: 2,
  planeId: 76,
  swScale: 0,
  bufScale: 100,
  /* 【X6 保持旧几何】X6 走 swScale=0 的硬件缩放路径，没有 videoscale 的
   * add-borders 兜底，矩形一变就可能被 kmssink 拉伸。所以 X6 不启用"控制条
   * 浮在画面上"，视频矩形仍然扣掉控制条那一段 —— 与 3.5.5 起的行为逐像素一致。 */
  barOverlay: false
};

// 兼容旧名字（测试/文档里还在用）
export const X7P_DEFAULTS = X7P;
export const X6P_DEFAULTS = X6P;

/* 本机构型。默认 X7 Pro —— 这是当前唯一在做真机验收的机器。
 * 想跑 X6 基线：buildProfile({ name: 'x6p' })；页面里不传就是 X7。 */
export const DEFAULT_DEVICE = 'x7p';

function pick(name) {
  const n = String(name || '').toLowerCase();
  if (n === 'x6p' || n === 'x6' || n.indexOf('x6') >= 0) return X6P;
  return X7P;
}

/*
 * 纯函数：由机型名算出完整 profile。
 *
 * input 只认一个字段：
 *   name  'x7p'（缺省）| 'x6p'
 *
 * 旧调用点传的 { env } / { override } 会被**忽略**（不报错、不生效）。
 * 保留这个宽容是为了让老测试与自检页不用整段重写；真正的取值只有上面两张表。
 */
export function buildProfile(input) {
  const o = input || {};
  const d = pick(o.name || DEFAULT_DEVICE);
  const width = d.width;
  const height = d.height;
  const layout = buildLayout(width, height);
  const scale = 1;

  return {
    name: d.name,
    source: 'fixed',
    env: null,
    design: { width: DESIGN.width, height: DESIGN.height },
    // cfg.json screen 语义：width = 视口宽（面板行方向），height = 视口高（面板列方向）
    cfg: {
      width: width,
      height: height,
      xoffset: d.panelYOffset,
      yoffset: d.panelXOffset,
      direction: d.direction
    },
    scale: scale,
    // 页面布局（物理像素）—— .vue 里所有容器宽高都从这里来
    layout: layout,
    logical: { width: width, height: height },
    // logicalRectToPhysicalRect 用的 physical：height 必须是「逻辑带行数」
    // （= 视口宽 936/960），不是面板物理 960 —— 旋转后矩形落在逻辑带里。
    physical: { width: height, height: width },
    panelXOffset: d.panelXOffset,
    panelYOffset: d.panelYOffset,
    rowOffset: d.rowOffset,
    panelWidth: d.panelWidth,
    panelHeight: d.panelHeight,
    direction: d.direction,
    transpose: d.transpose,
    planeId: d.planeId,
    swScale: d.swScale,
    bufScale: d.bufScale,
    barOverlay: d.barOverlay,
    videoArea: {
      x: layout.play.left,
      y: 0,
      width: layout.play.videoW,
      height: layout.play.videoH
    },
    liveVideoArea: {
      x: layout.play.left,
      y: 0,
      width: layout.play.videoW,
      height: height
    }
  };
}

/*
 * 按 profile 算 native 需要的物理矩形（信箱适配版）。
 *
 * 视频区（p.videoArea / p.liveVideoArea）已经是 layout.js 按视口算出的
 * **物理像素**，所以这里不再乘 scale —— 画布就是视口，1:1 不缩放。
 *
 * 第 5 参数 box：调用方可以直接给「自己挖的洞」（自检页用）。
 * 主页播放页已经改用 playFrameFor（不 fit），这个函数留给诊断/探针。
 */
export function playVideoRectsFor(videoWidth, videoHeight, profile, live, box) {
  const p = profile || buildProfile({});
  const area = box || (live ? p.liveVideoArea : p.videoArea);
  const fitBox = {
    x: Math.round(area.x),
    y: Math.round(area.y),
    width: Math.round(area.width),
    height: Math.round(area.height)
  };
  const fit = fitLogicalRect(videoWidth, videoHeight, { width: fitBox.width, height: fitBox.height });
  const logical = {
    x: fitBox.x + fit.x,
    y: fitBox.y + fit.y,
    width: fit.width,
    height: fit.height
  };
  let physical = logicalRectToPhysicalRect(logical, p.logical, p.physical);
  physical.y += Math.round(p.rowOffset || 0);
  physical = alignPlane16(physical, p);
  return { logical: logical, physical: physical };
}

/*
 * 【3.5.6 第二十九轮 v3：把物理矩形整体落到 16 网格上】
 *
 * 真机三版迭代：
 *   只对齐偶数        → 绿线原样（456 仍是 16 的半格，plane 会被撑大）
 *   只对齐尺寸 16     → 横屏绿线从左侧跑到上下（起点 x=12 也被驱动圆整）
 *   四边全落 16 网格  → 横屏竖屏都消失
 *
 * 驱动（VOP2）会把 render-rectangle 的 x/y/w/h 一起圆整到 16 边界；
 * 圆整多出来的区域没有 buffer 写入 = YUV 全 0 = 绿。
 *
 * 尺寸不能各自向下取整（280x158 → 272x144 会把 1.772 拉成 1.889），
 * 必须以高度为基准逐格试，取比例误差 ≤1% 的第一组。
 *
 * 只对 X7（swScale != 0，走 videoscale + 自选 overlay 平面）生效；
 * X6 是 kmssink 硬件缩放，rect 给多大填多大，收边只会白缩画面。
 *
 * 代价：竖屏画面缩到 91%、横屏高度 -1.8%。这是目前唯一**能播放**的消绿边
 * 方案 —— v4 试过用 videobox 补黑来保住尺寸，真机上管线协商失败，
 * 直接播放失败且退不出应用，已整段回退（2026-11-14 20:45）。
 */
function alignPlane16(physical, profile) {
  if (!(profile && profile.swScale)) return physical;
  if (!(physical && physical.width > 0 && physical.height > 0)) return physical;
  const snap = (v) => Math.round(v / 16) * 16;
  const size = fit16Size(physical.width, physical.height);
  const dx = Math.round((physical.width - size.width) / 2);
  const dy = Math.round((physical.height - size.height) / 2);
  return {
    x: Math.max(0, snap(physical.x + dx)),
    y: Math.max(0, snap(physical.y + dy)),
    width: size.width,
    height: size.height
  };
}

/*
 * 找一对都是 16 倍数、比例与原尺寸接近的尺寸。
 *
 * 【第二十九轮 v5：竖屏放大】
 * 起点规则：原高度离「上一个 16 倍数」只差 ≤4px 时向上取整，否则向下。
 *   竖屏 158 → 160（差 2）→ 288x160，比原来的 256x144 大 25%
 *   横屏 456 → 448（差 8，不向上）→ 256x448，保持不变
 * 这样竖屏能拿回大部分被 16 网格吃掉的画幅，横屏不会因为多 8px 顶出洞。
 *
 * 阈值 3%：16 网格下竖屏只有 256x144（误差 0.6%）和 288x160（误差 2.8%）
 * 两个候选，前者太小、后者大 25% 但会横向拉 2.8%。用户要求「放大竖屏」，
 * 取后者；288 比洞宽 280 多 8px，右侧压住 UI 一点点（画面内容，不是黑边）。
 * 从大往小试、第一个满足误差的就用 —— 不会掉进「误差最小但缩水 12%」的坑。
 */
function fit16Size(w, h) {
  const ratio = w / h;
  const up = Math.ceil(h / 16) * 16;
  const start = (up - h) <= 4 ? up : (h & ~15);
  for (let hh = start; hh >= 16; hh -= 16) {
    const ww = Math.round((hh * ratio) / 16) * 16;
    if (ww < 16) continue;
    if (Math.abs(ww / hh - ratio) <= 0.03) return { width: ww, height: hh };
  }
  return { width: Math.max(16, w & ~15), height: Math.max(16, h & ~15) };
}


/*
 * 【3.5.6 第二十四轮】只换坐标系、**不做信箱适配**的矩形。
 *
 * 和 playVideoRectsFor 的区别就是少一次 fitLogicalRect：传进来的 box 被原样
 * 当作视频平面（旋转后）的窗口。为什么需要它 —— 参考系统播放器把信箱交给 sink 的
 * force-aspect-ratio，自己只给一个外框；这里对应的是"不 fit、只换坐标系"。
 *
 * 这样一来「画面比例」不再依赖 JS 算的 fit 是否精确：矩形短了几像素、源尺寸
 * 报错、切字幕改了高度，videoscale 都会按片源 DAR 重新补黑边，永远不会拉伸。
 * 过去二十多轮反复出现的"方形压成扁方形"在物理上就不可能再发生。
 */
export function playFrameFor(profile, live, box) {
  const p = profile || buildProfile({});
  const area = box || (live ? p.liveVideoArea : p.videoArea);
  const logical = {
    x: Math.round(Number(area.x) || 0),
    y: Math.round(Number(area.y) || 0),
    width: Math.max(2, Math.round(Number(area.width) || 16)),
    height: Math.max(2, Math.round(Number(area.height) || 16))
  };
  let physical = logicalRectToPhysicalRect(logical, p.logical, p.physical);
  physical.y += Math.round(p.rowOffset || 0);
  physical = alignPlane16(physical, p);
  return { logical: logical, physical: physical };
}

/*
 * 【3.5.6 第二十六轮】全屏 cover：画面等比放大到铺满整个视口，再裁掉多余部分。
 *
 * 为什么需要：936x280 的屏是 3.34:1，16:9 片源按 contain 只能占到 498x280，
 * 无论视频区给多宽都是 498 —— 用户一直说「视频还是小 / 全屏要占满屏幕」。
 * 用 `videocrop` 做 cover 也是这个思路。
 *
 * 算式（物理坐标，rotation=270 后横帧变竖帧）：
 *   窗口 tw x th = 逻辑框高 x 逻辑框宽（如 280 x 936）
 *   解码后帧比例 r = vh / vw（16:9 → 0.5625）
 *   tw/th < r → 按高度铺满，scaleW = th*r，左右各裁 (scaleW-tw)/2
 *   否则      → 按宽度铺满，scaleH = tw/r，上下各裁 (scaleH-th)/2
 * native 拿到 cropX/cropY 后：videoscale 输出 (tw+2cx)x(th+2cy)，
 * 再用 videocrop 裁回 tw x th，kmssink 1:1 贴窗口 —— 不拉伸、不需要平面缩放。
 *
 * 竖屏片源不启用（裁掉 80% 太惨），退回 contain。
 */
export function coverFrameFor(videoWidth, videoHeight, profile, box) {
  const p = profile || buildProfile({});
  const b = box || { x: 0, y: 0, width: p.cfg.width, height: p.cfg.height };
  const vw = Math.max(1, Number(videoWidth) || 640);
  const vh = Math.max(1, Number(videoHeight) || 360);
  const contain = () => ({
    physical: playVideoRectsFor(vw, vh, p, false, b).physical,
    cropX: 0, cropY: 0
  });
  if (!(vw > vh)) return contain();

  const frame = playFrameFor(p, false, b);
  const tw = frame.physical.width;
  const th = frame.physical.height;
  const r = vh / vw;
  let cropX = 0;
  let cropY = 0;
  if (tw / th < r) {
    cropX = Math.max(0, (th * r - tw) / 2);
  } else {
    cropY = Math.max(0, (tw / r - th) / 2);
  }
  cropX = Math.round(cropX) & ~1;
  cropY = Math.round(cropY) & ~1;
  if (cropX + cropY < 2) return contain();
  return { physical: frame.physical, cropX: cropX, cropY: cropY };
}

// 只读摘要，日志/诊断页用（不泄露无关字段）
export function describeProfile(p) {
  if (!p) return 'profile=(none)';
  const L = p.layout || {};
  return 'profile=' + p.name + '/' + p.source +
    ' viewport=' + p.cfg.width + 'x' + p.cfg.height +
    ' canvas=' + (L.width || '?') + 'x' + (L.height || '?') +
    ' panelOffset=' + p.panelXOffset +
    ' rowOffset=' + (p.rowOffset || 0) +
    ' planeId=' + (p.planeId < 0 ? 'auto' : p.planeId) +
    ' swScale=' + (p.swScale || 0) +
    ' bufScale=' + (p.bufScale || 100) +
    ' barOverlay=' + (p.barOverlay === false ? 0 : 1) +
    ' transpose=' + p.transpose;
}

let cached = null;

// 幂等：第一次调用算一次，之后返回同一个对象（没有 IO，没有 storage）
export function ensureProfile() {
  if (!cached) {
    cached = buildProfile({});
    // eslint-disable-next-line no-console
    console.log('[device] ' + describeProfile(cached));
  }
  return Promise.resolve(cached);
}

export function currentProfile() {
  return cached || buildProfile({});
}

export function resetProfileCache() {
  cached = null;
}
