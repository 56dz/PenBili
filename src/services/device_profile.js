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

import { getString, setString, logWarn } from './storage.js';
import { fitLogicalRect, logicalRectToPhysicalRect } from './screen.js';
import { buildLayout } from './layout.js';

export const PROFILE_KEY = 'bili_device_profile';

// CSS 设计画布**基准**（960x266，X6 Pro 的原生画布）。
// 3.6.1 起画布不再写死：app.js 用 profile.cfg.width 调 setViewPort，页面按
// layout.js 算出的弹性尺寸布局。这个常量只剩两个用途：
//   ① 拿不到任何设备信息时的兜底画布；
//   ② 单测/文档里对照「X6 基线」。
export const DESIGN = { width: 960, height: 266 };
// DRM 面板物理尺寸（GPU 分支文档：RK3562 单模式 480x960@59.88）
export const PANEL = { width: 480, height: 960 };

// 两个已知机型的兜底档。
//
// 【planeId / swScale 的三次真机回归，别再改回去】
//   3.6.0  X7P 用 planeId=-1（kmssink 自选）：画面出来了，但被压成一条
//          贴在底部 —— 挑到了没有缩放能力的 Smart 平面（1:1 出图，
//          720p 的 buffer 塞不进 253x450 的窗口，driver 裁一块贴上去）。
//   3.6.1  改成 X6 验证过的 76（Esmart1-win0，带缩放器）：**整片不动了**。
//          真机日志：start gate 4s 内 vbranch 有、frames 一直是 0，
//          set_state(PLAYING) 返回 0（FAILURE），管线停在 PAUSED。
//          => 76 这个 DRM plane 对象号在 X7 Pro 的固件上不可用。
//   3.6.3  结论：**不要再赌平面号**。planeId=-1 让 kmssink 自选，同时
//          swScale=1 在解码后插一级 videoscale，把 buffer 缩到窗口大小。
//          sink 拿到的画面与 render-rectangle 等大，挑到没有缩放能力的
//          平面也能 1:1 正确上屏，几何完全由矩形决定。
//          native 侧还有一道兜底：显式 plane-id 起不来时自动切
//          auto+videoscale 重试一次（每会话一次）。
//
// X6 Pro 保持原样：76 + 硬件缩放（0 拷贝，且早已验证）。
// 真机要覆盖时用 storage：
//   bili_device_profile = {"planeId": 90, "swScale": 0}   // 换显式 id
//   bili_device_profile = {"planeId": -1}                  // 交给 kmssink 自选
//   bili_device_profile = {"swScale": 2}                   // 固件真旋转了 buffer
//
// 【3.5.6 比例修复】swScale=1 时 native 会把 videoscale 的目标宽高按 rotate
// 互换。原因：mppvideodec rotation=270 只是旋转意图，videoscale 看到的仍是
// 横向帧；直接缩到物理竖矩形会把 16:9 拉成 9:16（真机「视频被压扁，不同
// 比例压扁程度不一样」）。若某固件真的把输出 buffer 转成了竖帧，设 2 关掉
// 互换即可，不用重新打包。
export const X6P_DEFAULTS = { width: 960, height: 266, planeId: 76, swScale: 0 };
/*
 * 【3.5.6 第四轮：四档截图定案】
 *
 * 真机把 A0/B0/C1/D2 各截了一张：四张的 `playing out=253x450` **完全相同**，
 * 画面也都只占视频区的一小块（A/C/D 黑边在左/上，B 在左/下）。这说明前四档
 * 的设计本身就不对题：它们只改「缩放方式 / 面板列偏移」，而
 *   render-rectangle 的宽高、起点、以及送到平面的 buffer 尺寸**一像素没变**。
 * 用户要的是「占满」，靠换缩放方式永远试不出来。
 *
 * 这一轮从两个确定的错位点下手：
 *   ① 自检页挖洞区是 584x280（左整块），probe 却按首页播放区
 *      （x=208 起、457x253）算矩形 → 画面天生偏到一边，且比洞小一圈。
 *      现在洞改成 16:9（X7 = 498x280），probe 用洞本身作为 box，矩形与洞
 *      严格同源，640x360 恰好铺满、零黑边。
 *   ② X7 自选平面可能只做 1:1 贴 buffer（A0 那张窄条就是证据：640x360
 *      横帧被按窗口裁了一块）。改 render-rectangle 对这种平面无效，必须改
 *      **buffer 像素尺寸**。新增 bufScale：videoscale 目标在矩形基准上按
 *      百分比放大，矩形不动。平面按矩形缩放时画面不变，按 buffer 1:1 时
 *      画面随之变大 —— 两种驱动行为都覆盖到。
 *
 * 默认回到 swScale=1（软件缩放）：A0（硬件缩放）那张是四张里最差的窄条，
 * C1/D2 至少有完整画面。X6 Pro 仍是 0（硬件缩放，已验证）。
 *
 * 【3.5.6 第五轮：panelXOffset 必须是 0，不是 100】
 *
 * 第四轮把四档全改成「几何档」时，把唯一验证过列偏移的 B0（off=0）删掉了，
 * 四档统一回落到公式值 (480-280)/2=100。真机四张新截图：缓冲从 498x280
 * 变到 946x532、横帧/竖帧都试了，**画面一模一样地贴在屏幕下方**，
 * 生效矩形全是 <100,438,280,498> —— 用户「没有正常的，全部靠下」。
 *
 * 回头重看第三轮 B0（off=0）那张：画面是四张里最大的，上方黑边几乎没了，
 * 用户当时说的「左侧和下方一小条」是**剩下的少量黑边**，不是画面小。
 * 也就是说 rect.x 不该再加 100：JS 产出的 x 已经是面板绝对列坐标，
 * native 再加一次 (480-280)/2 等于把整块画面往面板列 +100 推，
 * 超出 280 宽逻辑带的部分被裁掉 → 只剩下方约 180 高、贴底。
 *
 * X6 Pro 保持 (480-266)/2=107：那台机器上 107 是验证过的正确值（3.5.5
 * 起 native 就硬编码它），X7 不能跟着套公式。
 */
export const X7P_DEFAULTS = {
  width: 936, height: 280, planeId: -1, swScale: 1, panelXOffset: 0
};

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

  /*
   * 【env 可信度闸门】3.6.1 起画布宽 = env 宽，所以 env 一旦报的是「面板尺寸」
   * 而不是「Falcon 视口」，整页画布就会按 480 宽去排（真机上等于满屏错位）。
   *
   * SDK 文档只说 deviceWidth/deviceHeight 是「设备分辨率」，没有说明它是旋转前
   * 还是旋转后。已知真机事实：
   *   X6 Pro 视口 960x266（宽高比 3.61），X7 Pro 936x280（3.34），
   *   物理面板都是 480x960（横向 480x960 / 纵向 960x480，宽高比 ≤2）。
   * 所以判据取「宽高比 ≥ 2.2 且宽 ≥ 600」才认为是可用视口；
   * 否则**尺寸整体丢弃**（不能只丢宽：480x960 会把高度当成 960），
   * 退回按机型预设。此时机型只能靠 deviceModel 认（X6/X7 的物理面板同为
   * 480x960，面板尺寸本身不含信息），认不出就按 X6 基线走。
   */
  const envW = env && env.width ? env.width : 0;
  const envH = env && env.height ? env.height : 0;
  const envUsable = envW >= 600 && envH > 0 && envW / envH >= 2.2;
  const modelText = String((env && env.model) || '');
  const modelX7 = /x7/i.test(modelText);

  // 视口：override > env（可信时） > 机型预设（认不出机型时按 X6 Pro）
  const height = clampInt(
    ov.height,
    envUsable ? envH : (modelX7 ? X7P_DEFAULTS.height : X6P_DEFAULTS.height),
    120,
    1200
  );
  const isX7 = envUsable ? height >= 272 : modelX7 ||
    String(ov.name || '').toLowerCase() === 'x7p';
  const preset = isX7 ? X7P_DEFAULTS : X6P_DEFAULTS;
  const width = clampInt(ov.width, envUsable ? envW : preset.width, 320, 1920);

  const panelWidth = clampInt(ov.panelWidth, PANEL.width, 240, 4096);
  // 面板行数：逻辑宽度映射过去（X6P 960 行、X7P 936 行），不是固定的 960。
  const panelHeight = clampInt(ov.panelHeight, width, 240, 4096);
  /*
   * 面板列偏移：JS 产出的物理矩形 x 已经是面板绝对列坐标，native 会原样
   * 加到这里的值上（do_open: g_w.rect = <panel_x_offset + req->x, ...>）。
   *
   * X6P = 107（(480-266)/2，3.5.5 起 native 硬编码、真机验证过）。
   * X7P = **0**（不是 (480-280)/2=100）：第三轮 B0 档真机证明 0 时画面最大、
   * 上方黑边最少；第四轮四档统一回落到 100 后「全部靠下」。详见 X7P_DEFAULTS
   * 注释。override 仍可显式覆盖，STORAGE 里写 {"panelXOffset":100} 即可回到旧值。
   */
  const centeredOffset = Math.round((panelWidth - height) / 2);
  const offsetDefault = preset.panelXOffset !== undefined ? preset.panelXOffset : centeredOffset;
  const panelXOffset = clampInt(ov.panelXOffset, offsetDefault, 0, panelWidth - 1);
  const panelYOffset = clampInt(ov.panelYOffset, 0, 0, panelHeight - 1);
  /*
   * rowOffset：UI 合成带在「面板行方向」的起始行。
   *
   * 为什么会有这个量：面板固定 480x960，而视口只有 936 行（X6 是 960 行，
   * 正好占满）。多出来的 24 行怎么分，取决于合成器是「把 936 行居中放进
   * 960 行」还是「从第 0 行开始贴」。视频平面走的是面板绝对坐标，一旦
   * 猜错，画面相对 UI 挖洞就会整体偏移（真机表现：画面贴边/上下被裁）。
   *
   * X6 Pro 上 936→960 差值为 0，默认 0 与原行为逐像素一致；
   * X7 Pro 若取证发现 UI 平面 dst 的 y 是 12，就把 rowOffset 设成 12
   * （storage: bili_device_profile = {"rowOffset":12}），不用重新打包。
   */
  const rowOffset = clampInt(ov.rowOffset, 0, -64, 256);
  const direction = clampInt(ov.direction, 270, 0, 270);
  const transpose = clampInt(ov.transpose, 2, 1, 3);
  const planeId = Math.round(num(ov.planeId, preset.planeId));
  /*
   * swScale：解码后是否插 videoscale（见上面 X6P/X7P 的注释）。
   * 显式 plane-id 时默认不开（走硬件缩放）；planeId<0（自选平面）默认开，
   * 因为自选很可能挑到没有缩放器的 Smart 平面。override 可显式关掉。
   */
  /* X7 Pro 默认 0（硬件缩放）；只有「X6 Pro 被手动覆盖成自选平面」时才
   * 自动开 1（自选平面可能没有缩放器）。X7 不能跟着这条规则走。 */
  const swDefault = isX7 ? preset.swScale : (planeId < 0 ? 1 : preset.swScale);
  const swScale = clampInt(ov.swScale, swDefault, 0, 2);
  /*
   * 【3.5.6 第四轮】bufScale：videoscale 输出相对 render-rectangle 的百分比。
   * 100 = 与矩形等大（原行为）。X7 Pro 自选平面若只做 1:1 贴 buffer，
   * 改矩形完全不起作用，必须把 buffer 本身放大才能「占满」。
   * 只在 swScale 非 0 时生效；范围 50..400，超出会被 native 钳住。
   */
  const bufScale = clampInt(ov.bufScale, 100, 50, 400);

  // setViewPort(cfg.width) 让 1 个设计像素 = 1 个物理像素。
  // 【3.6.1 改动】以前 setViewPort(960) + scale = width/960，X7 Pro 上整页被
  // 缩到 0.975，底部留 21px 黑边、网格第 5 列被挤掉（右侧 184px 黑边）。
  // 现在画布 = 视口，scale 恒 1；所有尺寸由 layout.js 按视口重算。
  const scale = 1;
  const layout = buildLayout(width, height);

  return {
    name: String(ov.name || (isX7 ? 'x7p' : 'x6p')),
    source: ov.width || ov.height ? 'override' : envUsable ? 'env' : 'default',
    env: env ? { width: env.width, height: env.height, model: env.model } : null,
    design: { width: DESIGN.width, height: DESIGN.height },
    // cfg.json screen 语义：width = 视口宽（面板行方向），height = 视口高（面板列方向）
    cfg: { width: width, height: height, xoffset: panelYOffset, yoffset: panelXOffset, direction: direction },
    scale: scale,
    // 页面布局（物理像素）—— .vue 里所有容器宽高都从这里来
    layout: layout,
    logical: { width: width, height: height },
    // logicalRectToPhysicalRect 用的 physical：height 必须是「面板行数」
    physical: { width: height, height: panelHeight },
    panelXOffset: panelXOffset,
    panelYOffset: panelYOffset,
    rowOffset: rowOffset,
    panelWidth: panelWidth,
    panelHeight: panelHeight,
    direction: direction,
    transpose: transpose,
    planeId: planeId,
    swScale: swScale,
    bufScale: bufScale,
    videoArea: { x: layout.play.left, y: 0, width: layout.play.videoW, height: layout.play.videoH },
    liveVideoArea: { x: layout.play.left, y: 0, width: layout.play.videoW, height: height }
  };
}

/*
 * 按 profile 算 native 需要的物理矩形。
 *
 * 视频区（p.videoArea / p.liveVideoArea）已经是 layout.js 按视口算出的
 * **物理像素**，所以这里不再乘 scale —— 3.6.1 起画布就是视口，1:1 不缩放。
 * X6 Pro 上 layout.play = 213/469/278 + 240/266，与旧常量逐像素相同，
 * 因此本函数在 X6 上的输出与 screen.js:playVideoRects 完全一致（单测守护）。
 *
 * 【3.5.6 第四轮】第 5 参数 box：调用方可以直接给「自己挖的洞」。
 * 自检页的洞是左侧 16:9 整块（X7 = 498x280），与首页三列布局里的中列
 * （x=208 起、457x253）完全不同；以前 probe 借首页的 area 算，画面自然
 * 偏出洞外、还小一圈。现在两边都用各自的洞，矩形与洞严格同源。
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
  const physical = logicalRectToPhysicalRect(logical, p.logical, p.physical);
  /* 行方向原点补偿（见 buildProfile 里 rowOffset 的注释）。默认 0。 */
  physical.y += Math.round(p.rowOffset || 0);
  return { logical: logical, physical: physical };
}

// 只读摘要，日志/诊断页用（不泄露无关字段）
export function describeProfile(p) {
  if (!p) return 'profile=(none)';
  const L = p.layout || {};
  return 'profile=' + p.name + '/' + p.source +
    ' viewport=' + p.cfg.width + 'x' + p.cfg.height +
    ' canvas=' + (L.width || '?') + 'x' + (L.height || '?') +
    ' cols=' + (L.grid ? L.grid.cols + 'x' + L.grid.card : '?') +
    ' panelOffset=' + p.panelXOffset +
    ' rowOffset=' + (p.rowOffset || 0) +
    ' planeId=' + (p.planeId < 0 ? 'auto' : p.planeId) +
    ' swScale=' + (p.swScale || 0) +
    ' bufScale=' + (p.bufScale || 100) +
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

/*
 * 【3.5.6 第四轮】自检页「比例」按钮 = **几何档**。
 *
 * 旧四档（A0/B0/C1/D2）只改缩放方式与列偏移，四张真机截图里
 * `playing out=253x450` 一模一样 —— 所以「没一个能占满」是必然的：
 * 矩形和 buffer 尺寸从没变过。这一轮四档全部真正改变送到平面的像素：
 *   A 横帧 ×1.0   videoscale 按 rect 互换，基准尺寸
 *   B 竖帧 ×1.0   videoscale 不互换（解码器真转了 buffer 的固件）
 *   C 横帧 ×1.9   buffer 放大 → 1:1 平面也能铺满窗口
 *   D 竖帧 ×1.9   同上 + 不互换
 *
 * bufScale 只放大 **buffer**，render-rectangle 保持与挖洞区等大且不出面板；
 * 所以放大档不会让 kmssink 因为矩形越界而拒绝上屏。
 */
export const RATIO_MODES = [
  { label: 'A', swScale: 1, bufScale: 100, desc: '横帧 x1.0' },
  { label: 'B', swScale: 2, bufScale: 100, desc: '竖帧 x1.0' },
  { label: 'C', swScale: 1, bufScale: 190, desc: '横帧 x1.9' },
  { label: 'D', swScale: 2, bufScale: 190, desc: '竖帧 x1.9' }
];

export function currentRatioMode(override) {
  const i = Number(override && override.ratioMode);
  return Number.isInteger(i) && i >= 0 && i < RATIO_MODES.length ? i : 0;
}

export async function cycleRatioMode() {
  let ov = {};
  try {
    const raw = await getString(PROFILE_KEY, '');
    if (raw) ov = JSON.parse(raw) || {};
  } catch (e) { /* 解析失败就当空覆盖 */ }
  const next = (currentRatioMode(ov) + 1) % RATIO_MODES.length;
  /* 清掉上一档（含旧版四档）写过的字段，避免 B0 的 panelXOffset=0、旧
   * A0 的 swScale=0 之类的残留跨档污染。 */
  const clean = Object.assign({}, ov);
  delete clean.swScale;
  delete clean.bufScale;
  delete clean.panelXOffset;
  delete clean.label;
  delete clean.desc;
  const m = RATIO_MODES[next];
  clean.ratioMode = next;
  clean.swScale = m.swScale;
  clean.bufScale = m.bufScale;
  setString(PROFILE_KEY, JSON.stringify(clean));
  /* 让下次 ensureProfile() 重新读，不要复用缓存 */
  cached = null;
  loading = null;
  logWarn('[device] ratio mode -> ' + m.label + ' ' + JSON.stringify(clean));
  return m;
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
