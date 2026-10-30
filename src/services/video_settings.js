// 视频/直播的画质偏好（「我的」菜单 → 视频调节 / 直播）
//
// 为什么需要这一层：硬解落地后画质与帧率不再是「只能取一个安全值」的硬约束
// （见 docs/x6pro-video-playback.md），但不同用户/不同网络下的最优解不同：
//   · 面板 480x960@59.88Hz，硬解实测 720p60 实时 57.95fps、1080p60 实时 56.20fps，
//     所以 60fps 内容能真正呈现，不是白费；
//   · 但 60fps 档的码率约为同分辨率 30fps 的两倍，弱网下反而更容易缓冲；
//   · 直播原画（1080p60）实测 4754kbps，设备外网吞吐约 10Mbps，能拉但没必要。
//
// **v3.3.0 起分辨率与帧率合并为单一「画质」设置**（用户要求）：
// B 站的 qn 编码本身就把两者绑在一起（116=1080P60、74=720P60 是 60fps 专档，
// 80/64/32/16 只有 30fps），而 360p/480p 根本没有 60fps 档。原先两个独立下拉
// 允许「360p + 60fps」这种服务端不存在的组合 —— 选流会先按 360p 卡死 qn=16，
// 再给 native 传 fpsCap=0，结果既拿不到 60fps 也白等一轮降级。
// 现在每个画质档自带 (ladder, maxPixels, fpsCap)，不存在非法组合。
import { getJson, setJson } from './storage.js';

export const VIDEO_SETTINGS_KEY = 'bili_video_settings';
export const VIDEO_SETTINGS_VERSION = 2;

/* ---- 点播画质 ----
 * qn 编码（B 站）：116=1080P60、80=1080P、74=720P60、64=720P、32=480P、16=360P。
 * ladder 是**从高到低的尝试顺序**：先请求上限档，服务端不给就自动降级，
 * 所以每个档位只需要把高于上限的 qn 剔掉，剩下的仍按原顺序保留（保底可播）。
 * fpsCap 是该档的渲染帧率上限（0 = 不限制，交给源）；30fps 档同时把 60fps
 * 专档（116/74）排在末尾 —— 先试 30fps 变体，拿不到才退回 60fps 再靠 videorate 限帧。
 * maxPixels 用于二次校验：parseDashResponse 的池为空时会退回全量池，
 * 只靠 qn 阶梯挡不住（详见 pixelBudgetFor 注释）。 */
export const VIDEO_QUALITY_OPTIONS = [
  {
    id: 'auto',
    label: '自动（最高可用）',
    ladder: [116, 80, 74, 64, 32, 16],
    maxPixels: 1920 * 1080,
    fpsCap: 0
  },
  {
    id: '1080p60',
    label: '1080p 60fps',
    ladder: [116, 80, 74, 64, 32, 16],
    maxPixels: 1920 * 1080,
    fpsCap: 0
  },
  {
    id: '1080p30',
    label: '1080p 30fps',
    ladder: [80, 116, 74, 64, 32, 16],
    maxPixels: 1920 * 1080,
    fpsCap: 30
  },
  {
    id: '720p60',
    label: '720p 60fps',
    ladder: [74, 64, 32, 16],
    maxPixels: 1280 * 720,
    fpsCap: 0
  },
  {
    id: '720p30',
    label: '720p 30fps',
    ladder: [64, 74, 32, 16],
    maxPixels: 1280 * 720,
    fpsCap: 30
  },
  {
    // 480p/360p 在 B 站只有 30fps 变体，不存在 60fps 档 —— 这正是合并设置的原因。
    id: '480p30',
    label: '480p 30fps',
    ladder: [32, 16],
    maxPixels: 852 * 480,
    fpsCap: 30
  },
  {
    id: '360p30',
    label: '360p 30fps',
    ladder: [16],
    maxPixels: 640 * 360,
    fpsCap: 30
  }
];

/* ---- 直播画质 ----
 * 直播 qn 与点播不同：150=480p（高清）、250=720p（超清）、400=蓝光、
 * 10000=原画。未登录时服务端最多给 480p，qn 传再大也只回落 150（实测过
 * 150/250/400/10000 四个值，未登录返回完全相同）。
 * 直播的 media_info 里**没有帧率字段**（只有 width/height/realtime_avg_bw），
 * 所以帧率只能靠 videorate 限帧，不能靠「请求 30fps 档」。 */
export const LIVE_QUALITY_OPTIONS = [
  { id: '480p30', label: '480p 30fps（省流）', qn: 150, fpsCap: 30 },
  { id: '720p30', label: '720p 30fps', qn: 250, fpsCap: 30 },
  { id: '720p60', label: '720p 60fps', qn: 250, fpsCap: 0 },
  { id: 'auto', label: '自动（最高可用）', qn: 10000, fpsCap: 0 }
];

const findById = (list, id) => list.find((x) => x.id === id) || list[0];

export const DEFAULT_VIDEO_SETTINGS = {
  version: VIDEO_SETTINGS_VERSION,
  videoQuality: 'auto',
  liveQuality: '480p30'
};

/* v1 → v2 迁移：老记录是 videoQuality('auto'|'1080p'|'720p'|'480p'|'360p')
 * + videoFps('auto'|'60'|'30') 两个独立字段，合并成单个画质 id。
 * 认不出来的组合一律落到 auto，宁可画质自动也不要因为一条旧记录起不来。 */
const LEGACY_VIDEO_MAP = {
  'auto|auto': 'auto',
  'auto|60': '1080p60',
  'auto|30': '1080p30',
  '1080p|auto': '1080p60',
  '1080p|60': '1080p60',
  '1080p|30': '1080p30',
  '720p|auto': '720p60',
  '720p|60': '720p60',
  '720p|30': '720p30',
  '480p|auto': '480p30',
  '480p|60': '480p30',
  '480p|30': '480p30',
  '360p|auto': '360p30',
  '360p|60': '360p30',
  '360p|30': '360p30'
};

const LEGACY_LIVE_MAP = {
  '480p|auto': '480p30',
  '480p|30': '480p30',
  '480p|60': '720p60',
  '720p|auto': '720p60',
  '720p|30': '720p30',
  '720p|60': '720p60',
  'auto|auto': 'auto',
  'auto|30': '720p30',
  'auto|60': 'auto'
};

function migrateLegacy(raw) {
  const v = LEGACY_VIDEO_MAP[String(raw.videoQuality) + '|' + String(raw.videoFps)];
  const l = LEGACY_LIVE_MAP[String(raw.liveQuality) + '|' + String(raw.liveFps)];
  return { videoQuality: v || 'auto', liveQuality: l || '480p30' };
}

/* 反序列化：任何脏数据都退回默认值，绝不因为一条坏记录让播放起不来。
 * 老版本（version<2 或还带着 videoFps 字段）走迁移表。 */
export function normalizeVideoSettings(raw) {
  const base = Object.assign({}, DEFAULT_VIDEO_SETTINGS);
  if (!raw || typeof raw !== 'object') return base;
  let src = raw;
  if (Number(raw.version) < 2 || raw.videoFps !== undefined || raw.liveFps !== undefined) {
    src = migrateLegacy(raw);
  }
  return {
    version: VIDEO_SETTINGS_VERSION,
    videoQuality: findById(VIDEO_QUALITY_OPTIONS, src.videoQuality).id,
    liveQuality: findById(LIVE_QUALITY_OPTIONS, src.liveQuality).id
  };
}

export async function loadVideoSettings() {
  return normalizeVideoSettings(await getJson(VIDEO_SETTINGS_KEY, null));
}

export function saveVideoSettings(s) {
  return setJson(VIDEO_SETTINGS_KEY, normalizeVideoSettings(s));
}

/* 点「画质」行时轮转到下一个选项（falcon 没有原生下拉，逐项轮转最省事，
 * 且状态永远可见，不需要弹层）。 */
export function nextOptionId(list, id) {
  const arr = Array.isArray(list) && list.length ? list : [{ id: id }];
  const i = arr.findIndex((x) => x.id === id);
  return arr[(i < 0 ? 0 : i + 1) % arr.length].id;
}

/* 点播的 qn 尝试阶梯（画质档自带顺序，60fps 专档已在 30fps 档里被排到末尾）。 */
export function dashLadderFor(settings) {
  const s = normalizeVideoSettings(settings);
  return findById(VIDEO_QUALITY_OPTIONS, s.videoQuality).ladder.slice();
}

/* 点播的渲染帧率上限（0 = 不限制）。
 * 30fps 档必须给 native 传 30：有些稿件**只有** 60fps 档，此时只靠请求档位
 * 拿不到 30fps 变体，只能拉 60fps 源再用 videorate 限帧（只封顶、不插帧）。 */
export function videoFpsCapFor(settings) {
  const s = normalizeVideoSettings(settings);
  return findById(VIDEO_QUALITY_OPTIONS, s.videoQuality).fpsCap;
}

/* 点播的解码像素预算 = 所选画质档的上限。
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
  return findById(LIVE_QUALITY_OPTIONS, s.liveQuality).fpsCap;
}

/* 菜单里显示的文字（模板不能直接调模块函数，由页面转发）。 */
export function videoQualityLabel(settings) {
  const s = normalizeVideoSettings(settings);
  return findById(VIDEO_QUALITY_OPTIONS, s.videoQuality).label;
}

export function liveQualityLabel(settings) {
  const s = normalizeVideoSettings(settings);
  return findById(LIVE_QUALITY_OPTIONS, s.liveQuality).label;
}
