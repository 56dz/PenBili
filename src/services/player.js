// native 播放器 adapter —— 双实现开关（v1 = 旧 libjsapi_player，v2 = 新 libjsapi_player2）
//
// 为什么有两套（docs/design/playback-rewrite-adr.md §3 阶段 1）：
//   旧 player.c 完全不动，新模块以 JSAPI 名 `player2` 并行存在；JS 层用开关选择，
//   真机出问题可一行配置回退。开关来源：storage `bili_player_impl`（'v1'/'v2'），
//   缺省 = 'v2'。开关值由页面 created() 调 loadPlayerImpl() 预读并缓存，
//   因此**播放热路径上不做任何 storage 读取**（读盘最坏 5s，绝不能挡播放）。
//
// 硬约束（ADR §1.2）：
//   - JSAPI 零阻塞：所有方法都是 async，底层 native 调用本身立即返回；
//     JS 侧绝不 await 一个会阻塞 QuickJS 主线程的调用（旧 seek 实测阻塞 4961ms）。
//   - 暂停不冻结进程（禁止 SIGSTOP/SIGCONT）—— 由 native 保证，JS 只调 pause()。
//
// 契约（player2，详见 docs/design/player2-api.md）：
//   open(input, startMs, durationMs, fps, audio, transpose, x, y, w, h,
//        audioDevice, userAgent, referer, input2, fpsCap) → {ok, code?, error?}
//   pause()/resume()/seek(ms)/stop()/release() → {ok}   立即返回
//   status() → {ok, state, positionMs, durationMs, frames, fps, hasAudio, audioBytes,
//               videoStallMs, seekPending, seekTargetMs, lastError, lastErrorMsg,
//               planeActive, pipelineAlive, ...}
//   info()   → {ok, version, module, gstVersion, registryReady, planeId}
//   recover() → {ok}（**可选**，契约未固定 → 本文件防御式探测，缺则返回 PLAYER_NO_RECOVER）
//
// v1 契约见 native/csrc/player.c（open 尾部多两个可选参数是本项目扩展）。
//
// 职责：实现选择、能力检测、入参校验、参数归一、JSON 归一化、错误翻译。页面只消费稳定接口。

import { logWarn, getString } from './storage.js';

export const MAX_INPUT_LEN = 1024;
export const MAX_USER_AGENT_LEN = 255;
export const MAX_REFERER_LEN = 512;

// 实现开关：storage 键 + 缺省值（缺省走新实现；native 未就绪时自动回退 v1）
export const PLAYER_IMPL_KEY = 'bili_player_impl';
export const PLAYER_IMPL_DEFAULT = 'v2';

let mod = null;
let tried = false;
let implName = ''; // 实际生效的实现名（'v1' | 'v2'）；'' = 尚未加载
let wantedImpl = ''; // 期望实现（'' = 未显式指定，按 PLAYER_IMPL_DEFAULT）
let fallbackWarned = false;
let unsupportedWarned = false;

// ---- 实现开关（页面 created() 里 loadPlayerImpl 预读；热路径只读缓存）----
export function normalizeImpl(name) {
  const s = typeof name === 'string' ? name.trim().toLowerCase() : '';
  return s === 'v1' || s === 'v2' ? s : '';
}

export function setPlayerImpl(name) {
  const n = normalizeImpl(name);
  wantedImpl = n;
  // 期望实现变了 → 丢弃已加载模块，下次调用按新开关重新加载
  if (mod && implName && implName !== desiredImpl()) {
    mod = null;
    tried = false;
    implName = '';
  }
  return wantedImpl;
}

export function getPlayerImpl() {
  return implName || desiredImpl();
}

function desiredImpl() {
  return wantedImpl || PLAYER_IMPL_DEFAULT;
}

// 页面 created() 调一次：把 storage 里的开关预读进内存（异步，不挡播放）
//
// 来源纪律（P1-3 教训）：storage 里残留 `v1` 会让**所有真机测试都跑旧代码**，
// 排查时白测很久。所以这里必须把「值从哪来」打成一行日志：
//   [player] impl 来源=storage 值=v1
//   [player] impl 来源=default 值=v2
// 缺省值硬编码 v2（PLAYER_IMPL_DEFAULT），读不到/读不懂一律用 v2。
// 已经显式写成 v1 的**不自动覆盖**（那是用户/运维的显式选择），但要大声告警。
export async function loadPlayerImpl() {
  let source = 'default';
  let raw = '';
  try {
    raw = await getString(PLAYER_IMPL_KEY, '');
  } catch (e) {
    /* 读不到就用缺省值，绝不影响播放 */
    raw = '';
  }
  const n = normalizeImpl(raw);
  if (n) {
    wantedImpl = n;
    source = 'storage';
  }
  const val = desiredImpl();
  if (source === 'storage' && val === 'v1') {
    logWarn(
      '[player] !!! storage 显式指定 v1 —— 正在跑旧实现（player），性能与稳定性下降；' +
        '若真机测试请改回 v2 !!!（' + PLAYER_IMPL_KEY + '=v1）'
    );
  }
  if (source === 'default' && raw) {
    // 存储里有值但解析不了（拼错/旧版本残留）→ 明确说清走了缺省
    logWarn('[player] impl storage 值不可识别(raw=' + raw + ') → 用缺省 ' + PLAYER_IMPL_DEFAULT);
  }
  logWarn('[player] impl 来源=' + source + ' 值=' + val);
  return val;
}

export function resetPlayerModuleCache() {
  mod = null;
  tried = false;
  implName = '';
  fallbackWarned = false;
  unsupportedWarned = false;
}

// native 模块是否可用（probe 的"播放器"步骤只做存在性检查）
export async function hasModule() {
  const m = await getPlayerModule();
  return !!m;
}

function pickModule(m) {
  if (m && typeof m.open === 'function') return m;
  if (m && m.default && typeof m.default.open === 'function') return m.default;
  return null;
}

// 加载指定实现；返回 { cand, reason }
//   reason: '' = 成功；'模块加载失败: xxx' / '接口缺失（未导出 open）'
// 回退告警要写清原因（P1-3 要求 2），所以这里不能只返回 null。
async function loadImpl(name) {
  try {
    const m = await import(name);
    const cand = pickModule(m);
    if (cand) return { cand: cand, reason: '' };
    logWarn('[player] ' + name + ' 模块未导出 open');
    return { cand: null, reason: '接口缺失（' + name + ' 未导出 open）' };
  } catch (e) {
    const msg = (e && e.message) || String(e);
    logWarn('[player] ' + name + ' 加载失败: ' + msg);
    return { cand: null, reason: '模块加载失败（' + name + ': ' + msg + '）' };
  }
}

export async function getPlayerModule() {
  if (tried) return mod;
  tried = true;
  const want = desiredImpl();
  let r = await loadImpl(want === 'v2' ? 'player2' : 'player');
  let actual = want;
  let fallbackReason = '';
  // 期望实现不可用 → 回退另一实现（真机上线期 native 可能还没随包安装）。
  // 显式指定 v1 却缺 v1 时同样回退 v2，保证"开关任意值都不会把播放打死"。
  if (!r.cand) {
    const other = want === 'v2' ? 'v1' : 'v2';
    const reason = r.reason;
    r = await loadImpl(other === 'v2' ? 'player2' : 'player');
    if (r.cand) {
      actual = other;
      fallbackReason = reason;
      if (!fallbackWarned) {
        fallbackWarned = true;
        if (other === 'v1') {
          // 这是**必须大声**的一条：v1 回退会让所有真机测试跑旧代码，
          // 曾经因为这条静默回退白测很久（P1-3）。
          logWarn('[player] !!! v2 不可用，已回退 v1（性能与稳定性下降）!!! 原因=' + reason);
        } else {
          logWarn('[player] ' + want + ' 不可用 → 已回退 ' + other + ' 原因=' + reason);
        }
      }
    }
  }
  mod = r.cand;
  implName = r.cand ? actual : '';
  logWarn(
    '[player] 实现: ' +
      (r.cand
        ? actual + (fallbackReason ? '（回退，原因=' + fallbackReason + '）' : '')
        : '不可用（两个模块都没加载到）')
  );
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
    return fail('PLAYER_NATIVE_MISSING', '播放器模块不可用（未随包安装 libjsapi_player2.so / libjsapi_player.so）');
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

// 可选接口（player2 契约未固定）→ 缺失时返回明确错误码，由调用方决定降级
function callOptional(fnName, args) {
  if (!mod || typeof mod[fnName] !== 'function') {
    return fail('PLAYER_UNSUPPORTED', '当前播放器实现不支持 ' + fnName + '()');
  }
  return call(fnName, args);
}

// opts → native open() 位置参数（纯函数，单测覆盖顺序与钳制）
// opts: { input, input2, startMs, durationMs, fps, audio, transpose, rect, audioDevice, userAgent, referer, fpsCap, isLive }
// input2 = DASH 音频轨第二输入 URL（''/缺省 = 单文件）
// fpsCap = 渲染帧率上限（0/缺省 = 不限；「我的 → 视频调节 / 直播」的帧率设置）
// isLive = 1 直播 / 0 点播 / 缺省（undefined）→ native 收到 -1，回落严格启发式
// 注意：前 15 个位置参数的顺序是与 native 的硬契约，任何一位都不能动；
//       isLive 是第 16 位（追加，老 native 忽略多余实参不受影响）。
export function buildOpenArgs(opts) {
  const o = opts || {};
  const rect = o.rect || { x: 0, y: 0, width: 0, height: 0 };
  const cap = Math.round(Number(o.fpsCap) || 0);
  // 只有明确 true/false 才翻译成 1/0；其它（undefined/null/非布尔）保持 -1 语义，
  // 让 native 回落到「严格启发式」（durationMs==0 + http + .flv 才算直播）。
  let liveFlag = -1;
  if (o.isLive === true) liveFlag = 1;
  else if (o.isLive === false) liveFlag = 0;
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
    typeof o.input2 === 'string' ? o.input2 : '',
    cap > 0 ? cap : 0,
    liveFlag
  ];
}

// opts: { input, input2, startMs, durationMs, fps, audio, transpose, rect, audioDevice, userAgent, referer, fpsCap }
export async function openSession(opts) {
  const m = await getPlayerModule();
  if (!m) return fail('PLAYER_NATIVE_MISSING', '播放器模块不可用（未随包安装 libjsapi_player2.so / libjsapi_player.so）');
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

// seek：v2 侧立即返回，实际 seek 在 worker 线程做覆盖式 debounce；
// 因此 JS 层**不需要**也不能 await 等落点（旧实现阻塞主线程 4961ms）。
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

// status：原样透传 native 字段（v2 新增 seekPending/seekTargetMs/planeActive/
// pipelineAlive/lastError/lastErrorMsg/fps/hasAudio 都在这里自动带出，
// play_session.readStatus 再做归一化）。绝不在这里裁剪字段。
export async function status() {
  await getPlayerModule();
  return call('status', []);
}

// 原地自愈（任务 D）：优先 native recover()，缺失返回 PLAYER_UNSUPPORTED 由页面降级
export async function recover() {
  await getPlayerModule();
  return callOptional('recover', []);
}

/* v2 键盘避让：运行时改视频平面 render-rectangle（非阻塞，worker 空闲时执行）。
 * 坐标系与 open 的 x/y 相同（屏幕绝对坐标）；v1 无此能力 → PLAYER_UNSUPPORTED，
 * 调用方需容错（v1 下键盘避让做不了，只能不弹评论框或接受遮挡）。 */
export async function setVideoRect(x, y, w, h) {
  await getPlayerModule();
  return callOptional('setRect', [
    Math.round(Number(x) || 0),
    Math.round(Number(y) || 0),
    Math.round(Number(w) || 0),
    Math.round(Number(h) || 0)
  ]);
}

/* v1 专有：显示权与弹幕泳道（v2 无此概念 → PLAYER_UNSUPPORTED，调用方需容错） */
export async function pauseRender() {
  await getPlayerModule();
  return callOptional('pauseRender', []);
}

export async function resumeRender() {
  await getPlayerModule();
  return callOptional('resumeRender', []);
}

export async function redraw() {
  await getPlayerModule();
  return callOptional('redraw', []);
}

export async function info() {
  await getPlayerModule();
  return callOptional('info', []);
}

export function warnUnsupportedOnce(what) {
  if (unsupportedWarned) return;
  unsupportedWarned = true;
  logWarn('[player] ' + what);
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
    PLAYER_ALREADY_RUNNING: '已有播放会话在运行',
    PLAYER_UNSUPPORTED: '当前播放器实现不支持该操作'
  };
  if (code && map[code]) return map[code] + (error ? '：' + error : '');
  return error || '播放失败';
}
