// 最小验证链路编排（本项目唯一业务流程）：
//   net → WBI 密钥 → 匿名触点 → 稿件信息 → html5 取流(360p 阶梯) → native 模块 → 起播 → 5 秒回读
//
// 契约：
//   ctx = {
//     client    : createClient(...) 实例（可注入 fake）
//     player    : { openSession, status, stop, release }（可注入 fake）
//     hasNet    : () => boolean
//     onStep    : (u) => void，u = { id, state: running|ok|fail|skip, detail, ms }
//     cancelled : () => boolean，true 时立即终止且**不再回调 onStep**
//     target    : { bvid }
//     nowSec    : () => number（WBI wts）
//     sleep     : (ms) => Promise
//     onSession : (session) => void（触点落 storage + 回填 Cookie）
//   }
// 返回 { ok:true, ... } | { ok:false, failedId } | { cancelled:true }

import { buildPlayurlQuery, parseHtml5Response, validateStreamUrl, dimsForQn, QN_LADDER, HTML5_FPS } from './playurl.js';
import { deriveMixinKey } from './wbi.js';
import { probeVideoRects } from '../screen.js';

export const STEPS = [
  { id: 'net', label: '网络通道' },
  { id: 'wbi', label: 'WBI密钥' },
  { id: 'finger', label: '匿名触点' },
  { id: 'view', label: '稿件信息' },
  { id: 'playurl', label: '取流' },
  { id: 'native', label: '播放器' },
  { id: 'play', label: '起播' },
  { id: 'watch', label: '回读5s' }
];

export const WATCH_ROUNDS = 5;
export const WATCH_INTERVAL_MS = 1000;

function shortTitle(t) {
  const s = String(t == null ? '' : t);
  return s.length > 10 ? s.slice(0, 10) + '…' : s;
}

export async function runProbe(ctx) {
  const c = ctx || {};
  const guard = () => !!(c.cancelled && c.cancelled());
  const emit = (u) => {
    if (guard()) return false;
    if (c.onStep) c.onStep(u);
    return true;
  };

  const stepState = {};
  const stepById = (id) => STEPS.filter((s) => s.id === id)[0];

  const stepFail = (id, message) => {
    const st = stepState[id];
    if (st) {
      st.state = 'fail';
      st.detail = String(message == null ? '' : message).slice(0, 120);
      emit({ id: id, state: 'fail', detail: st.detail });
    }
    // 其余未跑步骤标记 skip
    let seen = false;
    for (let i = 0; i < STEPS.length; i++) {
      if (STEPS[i].id === id) {
        seen = true;
        continue;
      }
      if (seen && stepState[STEPS[i].id] && stepState[STEPS[i].id].state === 'pending') {
        stepState[STEPS[i].id].state = 'skip';
        stepState[STEPS[i].id].detail = '前序失败';
        emit({ id: STEPS[i].id, state: 'skip', detail: '前序失败' });
      }
    }
    return { ok: false, failedId: id };
  };

  const begin = (id) => {
    const st = stepState[id];
    if (st) {
      st.state = 'running';
      st.detail = '';
    }
    return emit({ id: id, state: 'running', detail: '' });
  };

  const ok = (id, detail) => {
    const st = stepState[id];
    const d = String(detail == null ? '' : detail).slice(0, 120);
    if (st) {
      st.state = 'ok';
      st.detail = d;
    }
    emit({ id: id, state: 'ok', detail: d });
  };

  for (let i = 0; i < STEPS.length; i++) {
    stepState[STEPS[i].id] = { state: 'pending', detail: '' };
  }

  const t0 = Date.now();
  const elapsed = () => ' ' + ((Date.now() - t0) / 1000).toFixed(1) + 's';

  // 1) 网络通道
  if (guard()) return { cancelled: true };
  begin('net');
  if (!(c.hasNet && c.hasNet())) return stepFail('net', 'jsapi.http 不可用');
  ok('net', 'jsapi.http ✓' + elapsed());

  // 2) WBI 密钥（nav 匿名 code=-101 也算通过）
  if (guard()) return { cancelled: true };
  begin('wbi');
  const wbi = await c.client.fetchWbiKeys();
  if (guard()) return { cancelled: true };
  if (!wbi.ok) return stepFail('wbi', (wbi.stage ? '[' + wbi.stage + '] ' : '') + wbi.message);
  const mixinKey = deriveMixinKey(wbi.imgKey, wbi.subKey);
  if (!/^[0-9a-f]{32}$/.test(mixinKey)) return stepFail('wbi', 'mixin_key 派生失败');
  ok('wbi', '密钥32位✓ code=' + wbi.code + ' ' + wbi.ms + 'ms');

  // 3) 匿名触点（软步骤：失败不阻断，Cookie 置空继续）
  if (guard()) return { cancelled: true };
  begin('finger');
  const finger = await c.client.fetchFinger();
  if (guard()) return { cancelled: true };
  if (finger.ok) {
    const session = {
      version: 1,
      buvid3: finger.buvid3,
      buvid4: finger.buvid4,
      updatedAt: Date.now()
    };
    if (c.onSession) c.onSession(session);
    ok('finger', 'buvid3 ' + finger.buvid3.length + '字符✓');
  } else {
    emit({ id: 'finger', state: 'skip', detail: '触点失败，匿名继续' });
    stepState.finger.state = 'skip';
    stepState.finger.detail = '触点失败，匿名继续';
  }

  // 4) 稿件信息
  if (guard()) return { cancelled: true };
  begin('view');
  const bvid = (c.target && c.target.bvid) || '';
  const view = await c.client.fetchView(bvid);
  if (guard()) return { cancelled: true };
  if (!view.ok) return stepFail('view', (view.stage ? '[' + view.stage + '] ' : '') + view.message);
  const durationMs = Math.max(0, Math.floor(view.duration * 1000));
  ok('view', '「' + shortTitle(view.title) + '」 cid=' + view.cid + ' ' + view.duration + 's');

  // 5) 取流：qn 阶梯 32→16
  if (guard()) return { cancelled: true };
  begin('playurl');
  let stream = null;
  let lastMsg = '未取到';
  for (let qi = 0; qi < QN_LADDER.length; qi++) {
    const qn = QN_LADDER[qi];
    const built = buildPlayurlQuery({ bvid: bvid, cid: view.cid, qn: qn }, mixinKey, c.nowSec ? c.nowSec() : Math.floor(Date.now() / 1000));
    if (!built.ok) {
      lastMsg = built.message;
      if (c.log) c.log('[bili] playurl qn' + qn + ' 参数失败: ' + lastMsg);
      break;
    }
    const res = await c.client.fetchPlayurl(built.query);
    if (guard()) return { cancelled: true };
    const parsed = parseHtml5Response(res);
    if (!parsed.ok) {
      lastMsg = 'qn' + qn + ': ' + (parsed.message || '失败');
      if (c.log) c.log('[bili] playurl 尝试失败 ' + lastMsg);
      // 稿件级错误换 qn 也没用
      if (res && res.stage === 'api' && (res.code === -404 || res.code === 62002 || res.code === 62004)) break;
      continue;
    }
    const v = validateStreamUrl(parsed.url);
    if (!v.ok) {
      lastMsg = 'qn' + qn + ': URL ' + v.reason;
      if (c.log) c.log('[bili] playurl 尝试失败 ' + lastMsg);
      continue;
    }
    stream = {
      url: parsed.url,
      qn: parsed.quality || qn,
      sizeBytes: parsed.sizeBytes,
      host: v.host,
      allowlisted: v.allowlisted,
      length: v.length
    };
    break;
  }
  if (!stream) return stepFail('playurl', lastMsg);
  // 注意：只输出 host / 长度 / 大小，完整签名 URL 严禁进日志（日志落盘可见）
  ok(
    'playurl',
    'qn' + stream.qn + ' ' + dimsForQn(stream.qn).width + 'x' + dimsForQn(stream.qn).height +
      ' ' + (stream.sizeBytes ? (stream.sizeBytes / 1048576).toFixed(1) + 'MB ' : '') +
      stream.host + (stream.allowlisted ? '' : '(表外)') + ' ' + stream.length + 'c'
  );

  // 6) native 模块
  if (guard()) return { cancelled: true };
  begin('native');
  const hasMod = c.player && c.player.hasModule ? await c.player.hasModule() : false;
  if (guard()) return { cancelled: true };
  if (!hasMod) return stepFail('native', 'libjsapi_player.so 未加载');
  ok('native', 'player.so ✓');

  // 7) 起播（真正 open）
  if (guard()) return { cancelled: true };
  const dims = dimsForQn(stream.qn);
  const rects = probeVideoRects(dims.width, dims.height);
  const openRes = await c.player.openSession({
    input: stream.url,
    startMs: 0,
    durationMs: durationMs,
    fps: HTML5_FPS,
    audio: true,
    transpose: 2,
    rect: rects.physical,
    audioDevice: '',
    userAgent: c.mediaUa,
    referer: c.mediaReferer
  });
  if (guard()) {
    // 页面已取消：把刚拉起的播放停掉，避免孤儿进程
    await c.player.stop();
    await c.player.release();
    return { cancelled: true };
  }
  if (!openRes || openRes.ok !== true) {
    return stepFail('play', (openRes && (openRes.code + ': ' + openRes.error)) || 'open 失败');
  }
  ok('play', 'playing out=' + openRes.outWidth + 'x' + openRes.outHeight + ' ' + openRes.fps + 'fps');

  // 8) 5 秒回读：帧数增长即证明真实解码渲染
  if (guard()) {
    await c.player.stop();
    await c.player.release();
    return { cancelled: true };
  }
  begin('watch');
  let maxFrames = 0;
  let lastPos = 0;
  let playState = 'playing';
  let playErr = '';
  for (let r = 0; r < WATCH_ROUNDS; r++) {
    await (c.sleep ? c.sleep(WATCH_INTERVAL_MS) : Promise.resolve());
    if (guard()) {
      await c.player.stop();
      await c.player.release();
      return { cancelled: true };
    }
    const st = await c.player.status();
    if (st && st.ok !== false) {
      if (typeof st.frames === 'number' && st.frames > maxFrames) maxFrames = st.frames;
      if (typeof st.positionMs === 'number') lastPos = st.positionMs;
      if (st.state) playState = st.state;
      if (st.error) playErr = String(st.error).slice(0, 80);
    } else if (st && st.ok === false) {
      playState = 'error';
      playErr = st.error || st.code || 'status 失败';
    }
    if (playState === 'error') break;
  }
  if (guard()) {
    await c.player.stop();
    await c.player.release();
    return { cancelled: true };
  }
  if (playState === 'error' || !(maxFrames > 0)) {
    // 失败路径不留播放进程；成功路径保持播放（停止责任在页面：重跑/切后台/离页）
    await c.player.stop();
    await c.player.release();
  }
  if (playState === 'error') return stepFail('watch', '播放错误: ' + playErr);
  if (!(maxFrames > 0)) return stepFail('watch', '5s 无帧输出（state=' + playState + '）');
  ok('watch', '帧=' + maxFrames + ' 位=' + (lastPos / 1000).toFixed(1) + 's ✓ 播放保持');

  return {
    ok: true,
    qn: stream.qn,
    frames: maxFrames,
    positionMs: lastPos,
    durationMs: durationMs,
    title: view.title,
    bvid: bvid,
    host: stream.host
  };
}
