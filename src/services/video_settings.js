// 视频/直播的画质与帧率偏好（「我的」菜单 → 视频调节 / 直播）
//
// 为什么需要这一层：硬解落地后画质与帧率不再是「只能取一个安全值」的硬约束
// （见 docs/x6pro-video-playback.md），但不同用户/不同网络下的最优解不同：
//   · 面板 480x960@59.88Hz，硬解实测 720p60 实时 57.95fps、1080p60 实时 56.20fps，
//     所以 60fps 内容能真正呈现，不是白费；
//   · 但 60fps 档的码率约为同分辨率 30fps 的两倍，弱网下反而更容易缓冲；
//   · 直播原画（1080p60）实测 4754kbps，设备外网吞吐约 10Mbps，能拉但没必要。
// 因此把「分辨率」「帧率」做成显式设置项，默认值保持「自动 = 尽量高」。
import { getJson, setJson } from './storage.js';

export const VIDEO_SETTINGS_KEY = 'bili_video_settings';
export const VIDEO_SETTINGS_VERSION = 1;

/* ---- 视频分辨率 ----
 * qn 编码（B 站）：116=1080P60、80=1080P、74=720P60、64=720P、32=480P、16=360P。
 * ladder 是**从高到低的尝试顺序**：先请求上限档，服务端不给就自动降级，
 * 所以 cap 只需要把高于上限的档剔掉，剩下的仍按原顺序保留（保底可播）。 */
export const VIDEO_QUALITY_OPTIONS = [
  {
    id: 'auto',
    label: '自动（最高可用）',
    ladder: [116, 80, 74, 32, 16],
    maxPixels: 1920 * 1080
  },
  { id: '1080p', label: '1080p', ladder: [116, 80, 32, 16], maxPixels: 1920 * 1080 },
  { id: '720p', label: '720p', ladder: [74, 64, 32, 16], maxPixels: 1280 * 720 },
  { id: '480p', label: '480p', ladder: [32, 16], maxPixels: 852 * 480 },
  { id: '360p', label: '360p', ladder: [16], maxPixels: 640 * 360 }
];

/* ---- 视频帧率 ----
 * 'auto' / '60'：不限制，能 60 就 60（fpsCap=0 表示不插 videorate）。
 * '30'：优先请求 30fps 档（116/74 是 60fps 变体，30fps 时先试 80/64），
 *       并给 native 传 fpsCap=30 兜底 —— 因为有些稿件**只有** 60fps 档，
 *       没有兜底就只能二选一：要么卡在 60fps，要么干脆不播。
 * 30fps 的两种实现必须同时存在：只靠请求档位（源没有 30fps 档就失效），
 * 或只靠 videorate（白拉两倍码率再丢一半，弱网更糟）。 */
export const VIDEO_FPS_OPTIONS = [
  { id: 'auto', label: '自动', fpsCap: 0, prefer30: false },
  { id: '60', label: '60fps', fpsCap: 0, prefer30: false },
  { id: '30', label: '30fps', fpsCap: 30, prefer30: true }
];

/* ---- 直播分辨率 ----
 * 直播 qn 与点播不同：150=480p（高清）、250=720p（超清）、400=蓝光、
 * 10000=原画。未登录时服务端最多给 480p，qn 传再大也只回落 150（实测过
 * 150/250/400/10000 四个值，未登录返回完全相同）。 */
export const LIVE_QUALITY_OPTIONS = [
  { id: '480p', label: '480p（省流）', qn: 150 },
  { id: '720p', label: '720p', qn: 250 },
  { id: 'auto', label: '自动（最高可用）', qn: 10000 }
];

/* ---- 直播刷新率 ----
 * 直播源常见 30fps，部分房间给 60fps。'auto' 不限制；'30' 用 videorate 限帧，
 * 省电且让 mppvideodec 少一半负载（rotation 在 MPP 里做，负载不低）。 */
export const LIVE_FPS_OPTIONS = [
  { id: 'auto', label: '自动', fpsCap: 0 },
  { id: '30', label: '30fps', fpsCap: 30 },
  { id: '60', label: '60fps', fpsCap: 60 }
];

const findById = (list, id) => list.find((x) => x.id === id) || list[0];

export const DEFAULT_VIDEO_SETTINGS = {
  version: VIDEO_SETTINGS_VERSION,
  videoQuality: 'auto',
  videoFps: 'auto',
  liveQuality: '480p',
  liveFps: 'auto'
};

/* 反序列化：任何脏数据都退回默认值，绝不因为一条坏记录让播放起不来。 */
export function normalizeVideoSettings(raw) {
  const base = Object.assign({}, DEFAULT_VIDEO_SETTINGS);
  if (!raw || typeof raw !== 'object') return base;
  return {
    version: VIDEO_SETTINGS_VERSION,
    videoQuality: findById(VIDEO_QUALITY_OPTIONS, raw.videoQuality).id,
    videoFps: findById(VIDEO_FPS_OPTIONS, raw.videoFps).id,
    liveQuality: findById(LIVE_QUALITY_OPTIONS, raw.liveQuality).id,
    liveFps: findById(LIVE_FPS_OPTIONS, raw.liveFps).id
  };
}

export async function loadVideoSettings() {
  return normalizeVideoSettings(await getJson(VIDEO_SETTINGS_KEY, null));
}

export function saveVideoSettings(s) {
  return setJson(VIDEO_SETTINGS_KEY, normalizeVideoSettings(s));
}

/* 点「分辨率」行时轮转到下一个选项（falcon 没有原生下拉，逐项轮转最省事，
 * 且状态永远可见，不需要弹层）。 */
export function nextOptionId(list, id) {
  const arr = Array.isArray(list) && list.length ? list : [{ id: id }];
  const i = arr.findIndex((x) => x.id === id);
  return arr[(i < 0 ? 0 : i + 1) % arr.length].id;
}

/* 点播的 qn 尝试阶梯。
 * prefer30=true 时把 30fps 变体（80/64/32/16）提到前面：116/74 是 60fps 专档，
 * 在「就要 30fps」的偏好下先试它们等于白拉双倍码率再丢一半。 */
export function dashLadderFor(settings) {
  const s = normalizeVideoSettings(settings);
  const q = findById(VIDEO_QUALITY_OPTIONS, s.videoQuality);
  const f = findById(VIDEO_FPS_OPTIONS, s.videoFps);
  const base = q.ladder.slice();
  if (!f.prefer30) return base;
  const is60 = (qn) => qn === 116 || qn === 74;
  const thirty = base.filter((qn) => !is60(qn));
  const sixty = base.filter(is60);
  // 30fps 档全部排在前面，60fps 档仍留在末尾保底（稿件只有 60fps 时才用得上）
  return thirty.concat(sixty);
}

/* 点播的渲染帧率上限（0 = 不限制）。质量档选 360p 时源基本只有 30fps，
 * 仍按用户选择返回，native 侧 videorate 对 30fps 源是直通，不会掉帧。 */
export function videoFpsCapFor(settings) {
  const s = normalizeVideoSettings(settings);
  return findById(VIDEO_FPS_OPTIONS, s.videoFps).fpsCap;
}

/* 点播的解码像素预算 = 所选分辨率档的上限。
 * 为什么必须按实际宽高再卡一次：parseDashResponse 在 qn 池内按像素**降序**取最大，
 * 而池为空（服务端没有该 qn 条目）时会退回全量池 —— 于是「选 480p」仍可能拿到
 * 1080p。这里用所选档的 maxPixels 兜住，超了就 continue 到下一档阶梯。 */
export function pixelBudgetFor(settings) {
  const s = normalizeVideoSettings(settings);
  return findById(VIDEO_QUALITY_OPTIONS, s.videoQuality).maxPixels;
}

/* 直播的 qn。 */
export function liveQnFor(settings) {
  const s = normalizeVideoSettings(settings);
  return findById(LIVE_QUALITY_OPTIONS, s.liveQuality).qn;
}

/* 直播的渲染帧率上限（0 = 不限制）。 */
export function liveFpsCapFor(settings) {
  const s = normalizeVideoSettings(settings);
  return findById(LIVE_FPS_OPTIONS, s.liveFps).fpsCap;
}

/* 菜单里显示的文字（模板不能直接调模块函数，由页面转发）。 */
export function videoQualityLabel(settings) {
  const s = normalizeVideoSettings(settings);
  return findById(VIDEO_QUALITY_OPTIONS, s.videoQuality).label;
}

export function videoFpsLabel(settings) {
  const s = normalizeVideoSettings(settings);
  return findById(VIDEO_FPS_OPTIONS, s.videoFps).label;
}

export function liveQualityLabel(settings) {
  const s = normalizeVideoSettings(settings);
  return findById(LIVE_QUALITY_OPTIONS, s.liveQuality).label;
}

export function liveFpsLabel(settings) {
  const s = normalizeVideoSettings(settings);
  return findById(LIVE_FPS_OPTIONS, s.liveFps).label;
}
