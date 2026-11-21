// 直播「下播」判据的引擎级自证（autotest 探针 probeLiveEnd 的数据源）。
//
// 为什么需要它：native 侧 EOS 只发 `PB2_STATE_STOPPED`，`normalizeState` 归一成
// `'idle'`，页面靠 `isLiveEnded()` 判「直播已结束」。这条判据在 Node 里已被单测钉住，
// 但真机跑的是 QuickJS —— `typeof` / `Number()` / 正则行为与 Node 可能有细微差异
// （历史上踩过 `Number(undefined)` 一类坑）。真实下播又没法随时复现（要等主播自己
// 关播，或断网等 souphttpsrc 重试耗尽），所以这里把「判据 + 输入」全部静态化：
// 构造假的 status 对象喂给真函数，在真机引擎里断言结果。
// **不取流、不播放、不亮屏**，纯计算，几毫秒跑完。
//
// 边界：这里断言的是「判据本身的正确性」，不是「native 有没有正确上报 state」。
// 后者要靠真实下播或 native 日志验证。

import {
  isLiveEnded,
  isLiveReconnecting,
  LIVE_RECONNECT_ERR,
  num,
  numSigned
} from './play_session.js';

/**
 * 判据用例表。每个用例是一次「假 status → 期望结论」的断言。
 *
 * 字段：
 *   name        用例名（日志里出现，便于 grep）
 *   kind        'end' = isLiveEnded / 'reconnect' = isLiveReconnecting
 *   st          喂给判据的假 status 对象
 *   live        第二参（是否直播会话）
 *   everPlayed  第三参（本次会话有没有真的播起来过）
 *   want        期望返回值
 *   why         为什么要有这条（失败时打进日志，让看日志的人知道它在防什么）
 *
 * 假对象只给判据关心的字段：判据必须对「字段缺失」也给出确定结论，
 * 给全字段反而会掩盖 `num()` 归一化的坑。
 */
export const LIVE_END_CASES = [
  {
    name: '下播-state落回idle',
    kind: 'end',
    st: { state: 'idle', frames: 100, positionMs: 5000 },
    live: true,
    everPlayed: true,
    want: true,
    why: 'native EOS → STOPPED → normalizeState 归一成 idle；播过就判结束'
  },
  {
    name: '下播-frames兜底',
    kind: 'end',
    st: { state: 'idle', frames: 12, positionMs: 5000 },
    live: true,
    everPlayed: false,
    want: true,
    why: 'everPlayed 没置上时，出过帧也是播过的硬证据'
  },
  {
    name: '下播-state为ended',
    kind: 'end',
    st: { state: 'ended', frames: 30, positionMs: 8000 },
    live: true,
    everPlayed: true,
    want: true,
    why: '白名单里有 ended（点播用）；直播若被归一成 ended 同样判结束'
  },
  {
    name: '不误判-刚进页面未起播',
    kind: 'end',
    st: { state: 'idle', frames: 0, positionMs: 0 },
    live: true,
    everPlayed: false,
    want: false,
    why: '刚点进直播间就是 idle，不能立刻显示「直播已结束」'
  },
  {
    name: '不误判-墙钟位置有值但没出帧',
    kind: 'end',
    st: { state: 'idle', frames: 0, positionMs: 600000 },
    live: true,
    everPlayed: false,
    want: false,
    why: '★ 直播 positionMs 是墙钟（now - t_start），一帧没出也在涨，绝不能当证据'
  },
  {
    name: '不误判-播放中',
    kind: 'end',
    st: { state: 'playing', frames: 500, positionMs: 30000 },
    live: true,
    everPlayed: true,
    want: false,
    why: 'playing 时即使 everPlayed/frames 都满足也不能判结束'
  },
  {
    name: '不误判-暂停中',
    kind: 'end',
    st: { state: 'paused', frames: 500, positionMs: 30000 },
    live: true,
    everPlayed: true,
    want: false,
    why: '暂停不是结束；恢复播放要能继续'
  },
  {
    name: '不误判-错误态',
    kind: 'end',
    st: { state: 'error', frames: 0, positionMs: 0 },
    live: true,
    everPlayed: true,
    want: false,
    why: '真错误态由 readStatus 的 ok:false 早退路径处理，不走「已结束」'
  },
  {
    name: '不误判-点播会话',
    kind: 'end',
    st: { state: 'idle', frames: 100, positionMs: 5000 },
    live: false,
    everPlayed: true,
    want: false,
    why: '点播末尾走 isStreamEnded，isLiveEnded 对点播必须恒 false'
  },
  {
    name: '不误判-live字段缺失',
    kind: 'end',
    st: { state: 'idle', frames: 100, positionMs: 5000 },
    live: undefined,
    everPlayed: true,
    want: false,
    why: 'live 只有严格 === true 才算直播；undefined/字符串都不认'
  },
  {
    name: '重连-码17',
    kind: 'reconnect',
    st: { state: 'playing', lastError: 17, lastErrorMsg: '' },
    live: true,
    want: true,
    why: 'native2 约定的 PB2_E_LIVE_RECONNECT=17，state 保持 playing'
  },
  {
    name: '重连-码17字符串',
    kind: 'reconnect',
    st: { state: 'playing', lastError: '17' },
    live: true,
    want: true,
    why: 'JSAPI 有时把整数过成字符串，numSigned 要能归一'
  },
  {
    name: '重连-文案兜底',
    kind: 'reconnect',
    st: { state: 'playing', lastError: 99, lastErrorMsg: 'live reconnect attempt 2' },
    live: true,
    want: true,
    why: 'native 换码/换文案时的兜底：msg 含 reconnect 也提示'
  },
  {
    name: '重连-不抢真错误态',
    kind: 'reconnect',
    st: { state: 'error', lastError: 17 },
    live: true,
    want: false,
    why: 'ERROR 要交给页面失败路径，不能只显示「重连中…」把真错误吞掉'
  },
  {
    name: '重连-其它错误码不算',
    kind: 'reconnect',
    st: { state: 'playing', lastError: 12, lastErrorMsg: 'kms plane not active' },
    live: true,
    want: false,
    why: '平面丢失(12)等与重连无关，不能误挂「直播重连中…」'
  },
  {
    name: '重连-点播不算',
    kind: 'reconnect',
    st: { state: 'playing', lastError: 17 },
    live: false,
    want: false,
    why: '点播没有「重连」概念，提示会误导'
  }
];

/**
 * 在「当前引擎」里把用例表跑一遍。
 *
 * 返回 `{pass, fail, lines, failures}`：
 *   - `lines` 是给 `logWarn` 用的整行数组（含引擎指纹与每条用例的实测值）
 *   - `failures` 只装失败项，方便调用方判断要不要额外告警
 *
 * 为什么要先打一行引擎指纹：真机是 QuickJS、单测是 V8，
 * 出问题时第一件要确认的就是「引擎的 Number/typeof 行为是否一致」。
 *
 * @returns {{pass:number, fail:number, lines:string[], failures:string[]}}
 */
export function runLiveEndCases() {
  const lines = [];
  const failures = [];
  let pass = 0;
  let fail = 0;

  // 引擎指纹：一旦在真机与 Node 上不一致，说明判据依赖了引擎特性，需要重写
  lines.push(
    '[bili] LIVE-END PROBE engine typeof(Number)=' + typeof Number +
      " Number('17')=" + Number('17') +
      ' typeof(NaN)=' + typeof NaN +
      ' isFinite(1/0)=' + isFinite(1 / 0) +
      ' num(undefined,0)=' + num(undefined, 0) +
      ' numSigned(-5,0)=' + numSigned(-5, 0) +
      ' LIVE_RECONNECT_ERR=' + LIVE_RECONNECT_ERR
  );

  for (let i = 0; i < LIVE_END_CASES.length; i++) {
    const c = LIVE_END_CASES[i];
    let got;
    try {
      got =
        c.kind === 'end'
          ? isLiveEnded(c.st, c.live, c.everPlayed)
          : isLiveReconnecting(c.st, c.live);
    } catch (e) {
      // 判据抛错本身就是缺陷（必须是纯函数、对脏输入不崩），记为失败
      fail++;
      const msg = '#' + (i + 1) + ' ' + c.name + ' THROW ' + ((e && e.message) || e);
      lines.push('[bili] LIVE-END PROBE ' + msg);
      failures.push(msg);
      continue;
    }
    const ok = got === c.want;
    if (ok) pass++;
    else fail++;
    const row =
      '#' + (i + 1) + ' ' + c.name + ' ' + (ok ? 'PASS' : 'FAIL') +
      ' got=' + got + ' want=' + c.want +
      (ok ? '' : '  ← ' + c.why);
    lines.push('[bili] LIVE-END PROBE ' + row);
    if (!ok) failures.push(row);
  }

  lines.push(
    '[bili] LIVE-END PROBE ' + (fail === 0 ? 'PASS' : 'FAIL') +
      ' passed=' + pass + ' failed=' + fail + ' total=' + (pass + fail)
  );
  return { pass: pass, fail: fail, lines: lines, failures: failures };
}
