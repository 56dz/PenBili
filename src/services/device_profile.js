// 设备显示 profile —— 把 X6 Pro 写死的屏幕/DRM 假设变成运行时可探测、可覆盖的参数。
//
// 背景（docs/x7pro-adaptation.md）：
//   PenBili 3.5.5 只支持 X6 Pro。native 里写死了三样东西：
//     ① 面板列偏移 107（= (480-266)/2）
//     ② kmssink plane-id=76（Esmart1-win0）
//     ③ 逻辑画布 960x266 / 面板 266x960
//   X7 Pro 的视口是 936x280（cfg.json screen；部分固件报 960x280），逻辑带
//   280 宽落在 480 宽面板里偏移是 100，而且 plane 76 不一定存在/不一定空闲。
//   三个值全错 → kmssink 抢不到平面或矩形落在屏幕外 → 播放页黑屏。
//
// 这个模块只做「算参数」，不碰播放器：
//   - 权威来源是 $falcon.env.deviceWidth/deviceHeight（设备视口，物理像素）；
//   - 允许 storage 里的 bili_device_profile 覆盖任意字段（真机取证后用
//     tools/probe-x7p.sh 得到的精确值写进去，不用改代码重新打包）；
//   - X6 Pro 上算出来的值与旧硬编码逐字节一致（见 test/run.js 回归断言）。
//
// 坐标系（direction=270，与 screen.js 保持一致）：
//   设计坐标（CSS 960x266） --setViewPort(960) 铺满视口--> 实际像素坐标
//   实际 (x,y) -> 面板列 yoffset + y，面板行 (cfg.width - (x + w))
//   native 收到的是「逻辑带内坐标」，会自己再加 panelOffset。

import { getString, logWarn } from './storage.js';
import { fitLogicalRect, logicalRectToPhysicalRect } from './screen.js';

export const PROFILE_KEY = 'bili_device_profile';

// CSS 设计画布（base.less / app.js 的 setViewPort 值）。改动它等于重排全站 UI。
export const DESIGN = { width: 960, height: 266 };
// 播放态视频区（设计坐标，与 screen.js VIDEO_AREA / LIVE_VIDEO_AREA 一致）
export const VIDEO_DESIGN = { x: 213, y: 0, width: 469, height: 240 };
export const LIVE_VIDEO_DESIGN = { x: 213, y: 0, width: 469, height: 266 };
// DRM 面板物理尺寸（GPU 分支文档：RK3562 单模式 480x960@59.88）
export const PANEL = { width: 480, height: 960 };

// 两个已知机型的兜底档。注意 X7 Pro 的 planeId=-1 是刻意的：
// 不写 plane-id 时 kmssink 会自己挑空闲 overlay 平面，比猜一个 id 稳。
export const X6P_DEFAULTS = { width: 960, height: 266, planeId: 76 };
export const X7P_DEFAULTS = { width: 936, height: 280, planeId: -1 };

function num(v, fallback) {
  const n = typeof v === 'number' ? v : parseFloat(v);
  return isFinite(n) ? n : fallback;
}

function clampInt(v, fallback, lo, hi) {
  const n = Math.round(num(v, fallback));
  if (!isFinite(n)) return fallback;
  return n < lo ? lo : n > hi ? hi : n;
}

// 安全读取运行时环境。Node 单测 / 浏览器预览里没有 $falcon，不能抛。
export function readFalconEnv() {
  try {
    const e = typeof $falcon !== 'undefined' && $falcon.env ? $falcon.env : null;
    if (!e) return null;
    const w = Math.round(num(e.deviceWidth, 0));
    const h = Math.round(num(e.deviceHeight, 0));
    return {
      width: w > 0 ? w : 0,
      height: h > 0 ? h : 0,
      model: String(e.deviceModel || e.model || ''),
      platform: String(e.platform || '')
    };
  } catch (err) {
    return null;
  }
}

/*
 * 纯函数：由 env + override 算出完整 profile。
 * override 可含（全部可选）：
 *   name, width, height, panelWidth, panelHeight, panelXOffset, panelYOffset,
 *   planeId, direction, transpose
 *   width/height 即 cfg.json 的 screen.width/height（= 视口物理像素）。
 */
export function buildProfile(input) {
  const o = input || {};
  const env = o.env || null;
  const ov = o.override || {};

  // 视口：override > env > 按机型兜底（拿不到 env 时保守按 X6 Pro）
  const height = clampInt(ov.height, env && env.height ? env.height : X6P_DEFAULTS.height, 120, 1200);
  const isX7 = height >= 272 || /x7/i.test(String((env && env.model) || '')) ||
    String(ov.name || '').toLowerCase() === 'x7p';
  const preset = isX7 ? X7P_DEFAULTS : X6P_DEFAULTS;
  const width = clampInt(ov.width, env && env.width ? env.width : preset.width, 320, 1920);

  const panelWidth = clampInt(ov.panelWidth, PANEL.width, 240, 4096);
  // 面板行数：逻辑宽度映射过去（X6P 960 行、X7P 936 行），不是固定的 960。
  const panelHeight = clampInt(ov.panelHeight, width, 240, 4096);
  // 逻辑带在 480 宽面板里居中；X6P=107、X7P=100。
  const panelXOffset = clampInt(ov.panelXOffset, Math.round((panelWidth - height) / 2), 0, panelWidth - 1);
  const panelYOffset = clampInt(ov.panelYOffset, 0, 0, panelHeight - 1);
  const direction = clampInt(ov.direction, 270, 0, 270);
  const transpose = clampInt(ov.transpose, 2, 1, 3);
  const planeId = Math.round(num(ov.planeId, preset.planeId));

  // setViewPort(960) 把 960 个设计像素铺满 width 个物理像素。
  const scale = width / DESIGN.width;

  return {
    name: String(ov.name || (isX7 ? 'x7p' : 'x6p')),
    source: ov.width || ov.height ? 'override' : env && env.width ? 'env' : 'default',
    env: env ? { width: env.width, height: env.height, model: env.model } : null,
    design: { width: DESIGN.width, height: DESIGN.height },
    // cfg.json screen 语义：width = 视口宽（面板行方向），height = 视口高（面板列方向）
    cfg: { width: width, height: height, xoffset: panelYOffset, yoffset: panelXOffset, direction: direction },
    scale: scale,
    logical: { width: width, height: height },
    // logicalRectToPhysicalRect 用的 physical：height 必须是「面板行数」
    physical: { width: height, height: panelHeight },
    panelXOffset: panelXOffset,
    panelYOffset: panelYOffset,
    panelWidth: panelWidth,
    panelHeight: panelHeight,
    direction: direction,
    transpose: transpose,
    planeId: planeId,
    videoArea: { x: VIDEO_DESIGN.x, y: VIDEO_DESIGN.y, width: VIDEO_DESIGN.width, height: VIDEO_DESIGN.height },
    liveVideoArea: { x: LIVE_VIDEO_DESIGN.x, y: LIVE_VIDEO_DESIGN.y, width: LIVE_VIDEO_DESIGN.width, height: LIVE_VIDEO_DESIGN.height }
  };
}

/*
 * 按 profile 算 native 需要的物理矩形。
 * 与 screen.js:playVideoRects 的差别只有两点：
 *   ① 设计坐标乘 scale（X7 Pro 上 UI 被 setViewPort 缩放，hole 也跟着缩小）；
 *   ② physical.height 用视口宽（X7P 936 而不是 960）。
 * X6 Pro 上 scale=1、physical.height=960，结果与旧函数完全相同。
 */
export function playVideoRectsFor(videoWidth, videoHeight, profile, live) {
  const p = profile || buildProfile({});
  const area = live ? p.liveVideoArea : p.videoArea;
  const s = p.scale;
  const box = {
    x: Math.round(area.x * s),
    y: Math.round(area.y * s),
    width: Math.round(area.width * s),
    height: Math.round(area.height * s)
  };
  const fit = fitLogicalRect(videoWidth, videoHeight, { width: box.width, height: box.height });
  const logical = {
    x: box.x + fit.x,
    y: box.y + fit.y,
    width: fit.width,
    height: fit.height
  };
  const physical = logicalRectToPhysicalRect(logical, p.logical, p.physical);
  return { logical: logical, physical: physical };
}

// 只读摘要，日志/诊断页用（不泄露无关字段）
export function describeProfile(p) {
  if (!p) return 'profile=(none)';
  return 'profile=' + p.name + '/' + p.source +
    ' viewport=' + p.cfg.width + 'x' + p.cfg.height +
    ' scale=' + p.scale.toFixed(3) +
    ' panelOffset=' + p.panelXOffset +
    ' planeId=' + (p.planeId < 0 ? 'auto' : p.planeId) +
    ' transpose=' + p.transpose;
}

let cached = null;
let loading = null;

async function readOverride() {
  try {
    const raw = await getString(PROFILE_KEY, '');
    if (!raw) return {};
    const v = JSON.parse(raw);
    return v && typeof v === 'object' ? v : {};
  } catch (e) {
    logWarn('[device] bili_device_profile 解析失败，忽略覆盖: ' + (e && e.message));
    return {};
  }
}

// 幂等预热：created() 里调一次，播放热路径再 await 也只是拿同一个 Promise。
export function ensureProfile() {
  if (!loading) {
    loading = readOverride()
      .then((ov) => {
        cached = buildProfile({ env: readFalconEnv(), override: ov });
        logWarn('[device] ' + describeProfile(cached));
        return cached;
      })
      .catch((e) => {
        cached = buildProfile({});
        logWarn('[device] profile 读取失败，回退 X6 Pro 默认: ' + (e && e.message));
        return cached;
      });
  }
  return loading;
}

// 同步读取（ensureProfile 尚未完成时返回 X6 Pro 默认，绝不抛异常）
export function currentProfile() {
  return cached || buildProfile({});
}

// 供诊断页/取证脚本刷新（换 profile 后重置缓存）
export function resetProfileCache() {
  cached = null;
  loading = null;
}
