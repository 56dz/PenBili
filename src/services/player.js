// native 播放器 adapter（libs/libjsapi_player.so，契约见 native/csrc/player.c）
//
// open(input, startMs, durationMs, fps, audio, transpose, x, y, w, h,
//      audioDevice?, userAgent?, referer?)
// 尾部两个可选参数是本项目对 native 的扩展（PLAYER_VERSION 1.1.0）：
// B 站 CDN 对 ffmpeg 默认 UA（Lavf/…）返回 403（profiles/youdao-x5.md 实测），
// 必须能给 ffmpeg 传 -user_agent / -headers 才能拉流。
//
// 职责：能力检测、入参校验、参数归一、JSON 归一化、错误翻译。页面只消费稳定接口。

import { logWarn } from './storage.js';

export const MAX_INPUT_LEN = 1024;
export const MAX_USER_AGENT_LEN = 255;
export const MAX_REFERER_LEN = 512;

let mod = null;
let tried = false;

export function resetPlayerModuleCache() {
  mod = null;
  tried = false;
}

// native 模块是否可用（probe 的“播放器”步骤只做存在性检查）
export async function hasModule() {
  const m = await getPlayerModule();
  return !!m;
}

export async function getPlayerModule() {
  if (tried) return mod;
  tried = true;
  try {
    const m = await import('player');
    let cand = null;
    if (m && typeof m.open === 'function') cand = m;
    else if (m && m.default && typeof m.default.open === 'function') cand = m.default;
    mod = cand;
    logWarn('[player] native 模块: ' + (cand ? '可用' : '未导出 open'));
  } catch (e) {
    mod = null;
    logWarn('[player] native 加载失败: ' + (e && e.message));
  }
  return mod;
}

export function isSafeInput(input) {
  if (typeof input !== 'string') return false;
  const s = input;
  if (!s || s.length > MAX_INPUT_LEN) return false;
  if (/[\s\u0000-\u001f]/.test(s)) return false;
  if (s.indexOf('..') >= 0) return false;
  if (/^https?:\/\/[^\s]+$/i.test(s)) return true;
  if (s.charAt(0) === '/') return true;
  return false;
}

// User-Agent：可打印 ASCII、长度受限（native 侧同规则；JS 先挡一道给可读错误）
export function isSafeUserAgent(ua) {
  if (ua == null || ua === '') return true;
  if (typeof ua !== 'string' || ua.length > MAX_USER_AGENT_LEN) return false;
  for (let i = 0; i < ua.length; i++) {
    const c = ua.charCodeAt(i);
    if (c < 0x20 || c > 0x7e) return false;
  }
  return true;
}

// Referer：http(s) URL，无空白/控制字符
export function isSafeReferer(r) {
  if (r == null || r === '') return true;
  if (typeof r !== 'string' || r.length > MAX_REFERER_LEN) return false;
  if (/[\s\u0000-\u001f]/.test(r)) return false;
  return /^https?:\/\/[^\s]+$/i.test(r);
}

export function parseNativeJson(raw) {
  if (typeof raw !== 'string' || !raw) return null;
  try {
    const v = JSON.parse(raw);
    return v && typeof v === 'object' ? v : null;
  } catch (e) {
    return null;
  }
}

function fail(code, error) {
  return { ok: false, code: code, error: error, state: 'error' };
}

function call(fnName, args) {
  if (!mod || typeof mod[fnName] !== 'function') {
    return fail('PLAYER_NATIVE_MISSING', '播放器模块不可用（未随包安装 libjsapi_player.so）');
  }
  try {
    const raw = mod[fnName].apply(mod, args || []);
    let parsed = raw;
    if (typeof raw === 'string') parsed = parseNativeJson(raw);
    if (!parsed || typeof parsed !== 'object') return fail('PLAYER_BAD_JSON', '播放器返回格式异常');
    return parsed;
  } catch (e) {
    logWarn('[player] ' + fnName + ' 抛错: ' + (e && e.message));
    return fail('PLAYER_CALL_FAILED', (e && e.message) || '播放器调用失败');
  }
}

// opts → native open() 位置参数（纯函数，单测覆盖顺序与钳制）
// opts: { input, input2, startMs, durationMs, fps, audio, transpose, rect, audioDevice, userAgent, referer }
// input2 = DASH 音频轨第二输入 URL（''/缺省 = 单文件）
export function buildOpenArgs(opts) {
  const o = opts || {};
  const rect = o.rect || { x: 0, y: 0, width: 0, height: 0 };
  return [
    o.input,
    Math.max(0, Math.round(o.startMs || 0)),
    Math.max(0, Math.round(o.durationMs || 0)),
    Math.max(1, Math.round(o.fps || 24)),
    o.audio === false ? 0 : 1,
    o.transpose === 2 ? 2 : 1,
    Math.round(rect.x),
    Math.round(rect.y),
    Math.round(rect.width),
    Math.round(rect.height),
    typeof o.audioDevice === 'string' ? o.audioDevice : '',
    typeof o.userAgent === 'string' ? o.userAgent : '',
    typeof o.referer === 'string' ? o.referer : '',
    typeof o.input2 === 'string' ? o.input2 : ''
  ];
}

// opts: { input, input2, startMs, durationMs, fps, audio, transpose, rect, audioDevice, userAgent, referer }
export async function openSession(opts) {
  const m = await getPlayerModule();
  if (!m) return fail('PLAYER_NATIVE_MISSING', '播放器模块不可用（未随包安装 libjsapi_player.so）');
  const o = opts || {};
  if (!isSafeInput(o.input)) return fail('PLAYER_BAD_INPUT', '播放地址不合法');
  if (o.input2 && !isSafeInput(o.input2)) return fail('PLAYER_BAD_INPUT', '音频地址不合法');
  if (!isSafeUserAgent(o.userAgent)) return fail('PLAYER_BAD_HEADERS', 'User-Agent 参数不合法');
  if (!isSafeReferer(o.referer)) return fail('PLAYER_BAD_HEADERS', 'Referer 参数不合法');
  const rect = o.rect || null;
  if (!rect || !(rect.width > 0) || !(rect.height > 0)) return fail('PLAYER_BAD_RECT', '视频矩形未计算');
  const res = call('open', buildOpenArgs(o));
  if (res && res.ok === false && !res.code) res.code = 'PLAYER_OPEN_FAILED';
  return res;
}

export async function pause() {
  await getPlayerModule();
  return call('pause', []);
}

export async function resume() {
  await getPlayerModule();
  return call('resume', []);
}

export async function seek(positionMs) {
  await getPlayerModule();
  return call('seek', [Math.max(0, Math.round(positionMs || 0))]);
}

export async function stop() {
  await getPlayerModule();
  return call('stop', []);
}

export async function release() {
  await getPlayerModule();
  return call('release', []);
}

export async function status() {
  await getPlayerModule();
  return call('status', []);
}

/* v1.7.0：显示权与弹幕泳道 */
export async function pauseRender() {
  await getPlayerModule();
  return call('pauseRender', []);
}

export async function resumeRender() {
  await getPlayerModule();
  return call('resumeRender', []);
}

// writeDm(lane 0..3, text)：写弹幕泳道（textfile reload 即时生效；空串=清空该泳道）
export async function writeDm(lane, text) {
  await getPlayerModule();
  return call('writeDm', [Math.max(0, Math.min(3, Math.round(lane || 0))), typeof text === 'string' ? text : '']);
}

export async function redraw() {
  await getPlayerModule();
  return call('redraw', []);
}

export async function info() {
  await getPlayerModule();
  return call('info', []);
}

export const PLAYER_STATE = {
  IDLE: 'idle',
  PLAYING: 'playing',
  PAUSED: 'paused',
  ENDED: 'ended',
  ERROR: 'error'
};

// 错误码 → 给用户看的中文提示
export function describeError(code, error) {
  const map = {
    PLAYER_NATIVE_MISSING: '播放器组件缺失（需重新安装完整 AMR 包）',
    PLAYER_BAD_INPUT: '播放地址不合法',
    PLAYER_BAD_HEADERS: '播放请求头参数不合法',
    PLAYER_BAD_RECT: '视频区域计算失败',
    PLAYER_BAD_JSON: '播放器内部错误（返回格式异常）',
    PLAYER_CALL_FAILED: '播放器调用失败',
    PLAYER_OPEN_FAILED: '播放器启动失败',
    PLAYER_FFMPEG_FAILED: '解码进程启动失败',
    PLAYER_FB_FAILED: '显示设备打开失败（/dev/fb0）',
    PLAYER_ALREADY_RUNNING: '已有播放会话在运行'
  };
  if (code && map[code]) return map[code] + (error ? '：' + error : '');
  return error || '播放失败';
}
