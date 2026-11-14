// 互动视频（Steins;Gate / stein）节点解析、隐藏变量条件求值与连播判定。
//
// 上游接口（bilibili-API-collect docs/video/interact_video.md，2026-11 实测）：
//   GET https://api.bilibili.com/x/stein/edgeinfo_v2
//       ?bvid=BV..&graph_version=<来自 x/player/v2 的 data.interaction.graph_version>
//       &edge_id=<模块编号，0 或留空为起始模块>
//   data:
//     title / edge_id / is_leaf / no_backtracking / no_evaluation
//     story_list[]      : { node_id, edge_id, title, cid, start_pos, cursor, is_current }
//                         ⚠️ 这是「进度回溯」链，未登录只有起始模块；is_current 指向
//                         用户已经走过的最后一段，**不是**本次 edge_id 的当前段。
//     edges.questions[] : { type, duration(毫秒, -1=不限时), pause_video, start_time_r, choices[] }
//       type=2/1 : 选择题，pause_video=1，start_time_r=300（上游未写明；
//                  按 1/100 秒解释 = 片尾前 3s 出题，作为题面提前量）
//       type=0   : 自动跳转片段，只有一个 option="A " 的空文本选项，pause_video=0
//     edges.questions[].choices[] :
//       { id, cid, option, condition, native_action, is_default, is_hidden, x, y,
//         platform_action: "JUMP <edge_id> <cid>" }
//     hidden_vars[]     : { id, id_v2, value, type(1 普通 / 2 随机), name, is_show }
//     preload.video[]   : { aid, cid }
//
// 播放模型（本项目）：
//   片段 cid 由「正在播的那一段」给出（起播 cid / 选项 cid / 回退快照），
//   绝不从 story_list 反推；选项的 id 是**下一模块**的 edge_id。
//   点选项 → 用 choice.cid 重新取流播放，并以 choice.id 拉下一节点的选项。
//   这就是「每个片段的选项」+「互动视频连播」：不回列表、不重进播放页，
//   只重建播放会话，选项与隐藏变量在同一个互动会话里连续演进。
//
// 变量表达式（condition / native_action）不能用 eval：QuickJS 里既能注入也难保证
// 安全，且上游语法固定为「$变量 + 比较/算术/&&/||」的极小子集。这里写一个
// 纯递归下降解析器；解析不了的条件一律判 true（宁可多显示选项，也不能藏掉
// 用户本来能点的分支）。

export const EDGE_INFO_PATH = '/x/stein/edgeinfo_v2';
/* 片段结束前多久把选项摆出来（毫秒）。上游 questions[].start_time_r 实测恒为 300
 * （= 3.00 秒），但笔端 2.8 寸屏 3 秒太紧；
 * 这里以 start_time_r 为准，解析时钳到 [2s, 15s]，缺失时才用这个兜底值。 */
export const CHOICE_LEAD_MS = 8000;
export const MIN_LEAD_MS = 2000;
export const MAX_LEAD_MS = 15000;
/* 片段结束后，默认选项自动连播前的等待（毫秒）——给用户一点反悔/手点的余地。
 * type=0（自动跳转片段）用更短的 AUTO_JUMP_DELAY_MS，避免空档。 */
export const AUTO_CHAIN_DELAY_MS = 900;
export const AUTO_JUMP_DELAY_MS = 350;
/* 题面倒计时（questions[].duration > 0，单位毫秒）到点后走默认项。 */
export const MIN_CHOICE_COUNTDOWN_MS = 3000;
export const MAX_CHOICE_COUNTDOWN_MS = 120000;

export function parsePlatformAction(s) {
  const m = /JUMP\s+(\d+)\s+(\d+)/i.exec(String(s || ''));
  if (!m) return { edgeId: 0, cid: 0 };
  return { edgeId: Math.floor(Number(m[1])), cid: Math.floor(Number(m[2])) };
}

export function normalizeChoice(c) {
  if (!c || typeof c !== 'object') return null;
  const jump = parsePlatformAction(c.platform_action);
  const cid = Math.floor(Number(c.cid) || 0) || jump.cid;
  const id = Math.floor(Number(c.id) || 0) || jump.edgeId;
  if (!(cid > 0)) return null;
  const text = String(c.option == null ? '' : c.option).trim();
  return {
    id: id,
    cid: cid,
    option: text || ('选项 ' + (id || '?')),
    /* type=0 的「自动跳转」片段经常只给一个 option="A " 的空选项。
     * auto=true 表示这条选项不是给人点的，页面不要把它渲染成可点行。 */
    auto: !text,
    isDefault: Number(c.is_default) === 1,
    isHidden: Number(c.is_hidden) === 1,
    condition: String(c.condition == null ? '' : c.condition),
    nativeAction: String(c.native_action == null ? '' : c.native_action),
    x: Number(c.x) || 0,
    y: Number(c.y) || 0,
    textAlign: Number(c.text_align) || 0
  };
}

export function parseStoryList(list) {
  const src = Array.isArray(list) ? list : [];
  return src
    .map((s) => ({
      nodeId: Math.floor(Number(s && s.node_id) || 0),
      edgeId: Math.floor(Number(s && s.edge_id) || 0),
      title: String((s && s.title) || ''),
      cid: Math.floor(Number(s && s.cid) || 0),
      startPosMs: Math.floor(Number(s && s.start_pos) || 0),
      cursor: Math.floor(Number(s && s.cursor) || 0),
      isCurrent: Number(s && s.is_current) === 1
    }))
    .filter((s) => s.cid > 0);
}

export function hiddenVarsToMap(list) {
  const out = {};
  const src = Array.isArray(list) ? list : [];
  for (let i = 0; i < src.length; i++) {
    const v = src[i];
    if (!v || typeof v !== 'object') continue;
    const key = String(v.id_v2 || v.id || '').trim();
    const num = Number(v.value);
    if (key && isFinite(num)) out[key] = num;
  }
  return out;
}

/* 服务器每个节点都会带一份 hidden_vars：
 *   - type=2 随机值 → 每次都用服务器新值（同一互动会话里随机数按节点刷新）；
 *   - 普通变量     → 只有在本地还没见过时才采纳，否则保留 native_action 累积的结果。 */
export function mergeHiddenVars(vars, list) {
  const out = {};
  const base = vars && typeof vars === 'object' ? vars : {};
  for (const k of Object.keys(base)) {
    if (isFinite(Number(base[k]))) out[k] = Number(base[k]);
  }
  const src = Array.isArray(list) ? list : [];
  for (let i = 0; i < src.length; i++) {
    const v = src[i];
    if (!v || typeof v !== 'object') continue;
    const key = String(v.id_v2 || v.id || '').trim();
    const num = Number(v.value);
    if (!key || !isFinite(num)) continue;
    if (Number(v.type) === 2 || !(key in out)) out[key] = num;
  }
  return out;
}

/* ---------------- 极小表达式解析器（不使用 eval / Function） ---------------- */
function tokenize(s) {
  const str = String(s == null ? '' : s);
  const out = [];
  let i = 0;
  while (i < str.length) {
    const ch = str.charAt(i);
    if (/\s/.test(ch)) { i++; continue; }
    if (ch === '$') {
      let j = i + 1;
      while (j < str.length && /[A-Za-z0-9_]/.test(str.charAt(j))) j++;
      if (j === i + 1) { out.push({ t: 'bad', v: ch }); i++; continue; }
      out.push({ t: 'var', v: str.slice(i, j) });
      i = j;
      continue;
    }
    if (/[0-9.]/.test(ch)) {
      let j = i + 1;
      while (j < str.length && /[0-9.]/.test(str.charAt(j))) j++;
      const n = parseFloat(str.slice(i, j));
      if (!isFinite(n)) { out.push({ t: 'bad', v: str.slice(i, j) }); }
      else out.push({ t: 'num', v: n });
      i = j;
      continue;
    }
    const two = str.slice(i, i + 2);
    if (two === '>=' || two === '<=' || two === '==' || two === '!=' || two === '&&' || two === '||') {
      out.push({ t: 'op', v: two });
      i += 2;
      continue;
    }
    if ('+-*/><=()'.indexOf(ch) >= 0) {
      out.push({ t: 'op', v: ch });
      i++;
      continue;
    }
    out.push({ t: 'bad', v: ch });
    i++;
  }
  return out;
}

function peek(p) { return p.i < p.t.length ? p.t[p.i] : null; }
function eat(p, v) {
  const k = peek(p);
  if (k && k.t === 'op' && k.v === v) { p.i++; return true; }
  return false;
}
function valueOf(tok, vars) {
  if (tok.t === 'num') return tok.v;
  if (tok.t === 'var') {
    const v = vars && vars[tok.v];
    return isFinite(Number(v)) ? Number(v) : 0;
  }
  throw new Error('bad token');
}
function parsePrimary(p, vars) {
  const k = peek(p);
  if (!k) throw new Error('eof');
  if (eat(p, '(')) {
    const v = parseOr(p, vars);
    if (!eat(p, ')')) throw new Error('unclosed paren');
    return v;
  }
  p.i++;
  return valueOf(k, vars);
}
function parseUnary(p, vars) {
  if (eat(p, '-')) return -parseUnary(p, vars);
  if (eat(p, '+')) return parseUnary(p, vars);
  return parsePrimary(p, vars);
}
function parseMul(p, vars) {
  let v = parseUnary(p, vars);
  for (;;) {
    if (eat(p, '*')) v *= parseUnary(p, vars);
    else if (eat(p, '/')) {
      const d = parseUnary(p, vars);
      v = d === 0 ? 0 : v / d;
    } else return v;
  }
}
function parseAdd(p, vars) {
  let v = parseMul(p, vars);
  for (;;) {
    if (eat(p, '+')) v += parseMul(p, vars);
    else if (eat(p, '-')) v -= parseMul(p, vars);
    else return v;
  }
}
function parseCmp(p, vars) {
  let v = parseAdd(p, vars);
  const k = peek(p);
  if (k && k.t === 'op' && ['>=', '<=', '==', '!=', '>', '<'].indexOf(k.v) >= 0) {
    p.i++;
    const r = parseAdd(p, vars);
    if (k.v === '>=') v = v >= r ? 1 : 0;
    else if (k.v === '<=') v = v <= r ? 1 : 0;
    else if (k.v === '==') v = v === r ? 1 : 0;
    else if (k.v === '!=') v = v !== r ? 1 : 0;
    else if (k.v === '>') v = v > r ? 1 : 0;
    else v = v < r ? 1 : 0;
  }
  return v;
}
function parseAnd(p, vars) {
  let v = parseCmp(p, vars);
  while (eat(p, '&&')) {
    const r = parseCmp(p, vars);
    v = v !== 0 && r !== 0 ? 1 : 0;
  }
  return v;
}
function parseOr(p, vars) {
  let v = parseAnd(p, vars);
  while (eat(p, '||')) {
    const r = parseAnd(p, vars);
    v = v !== 0 || r !== 0 ? 1 : 0;
  }
  return v;
}

// → number；语法错误抛异常（调用方决定降级策略）
export function evalExpression(expr, vars) {
  const toks = tokenize(expr);
  if (!toks.length) throw new Error('empty expression');
  if (toks.some((t) => t.t === 'bad')) throw new Error('unsupported token');
  const p = { t: toks, i: 0 };
  const v = parseOr(p, vars || {});
  if (p.i !== toks.length) throw new Error('trailing token');
  if (!isFinite(v)) throw new Error('non-finite');
  return v;
}

/* 选项出现条件：空条件 / 无法解析 → true（不藏选项）。 */
export function evalCondition(expr, vars) {
  const s = String(expr == null ? '' : expr).trim();
  if (!s) return true;
  try {
    return evalExpression(s, vars) !== 0;
  } catch (e) {
    return true;
  }
}

/* native_action：形如 "$a=1;$b=$b+1"，返回**新的**变量表（不改入参）。
 * 真实抓包都是 `$var=expr` 与 `$var=$var+1.00`；顺手兼容 `$var+=1` 这种复合赋值，
 * 避免上游改写法时静默丢变量。 */
export function applyNativeAction(action, vars) {
  const out = {};
  const base = vars && typeof vars === 'object' ? vars : {};
  for (const k of Object.keys(base)) out[k] = Number(base[k]) || 0;
  const stmts = String(action == null ? '' : action).split(';');
  for (let i = 0; i < stmts.length; i++) {
    const toks = tokenize(stmts[i]);
    if (!toks.length || toks[0].t !== 'var') continue;
    let op = '';
    let rhsFrom = 0;
    if (toks[1] && toks[1].t === 'op' && toks[1].v === '=') {
      op = '=';
      rhsFrom = 2;
    } else if (
      toks[1] && toks[1].t === 'op' && ['+', '-', '*', '/'].indexOf(toks[1].v) >= 0 &&
      toks[2] && toks[2].t === 'op' && toks[2].v === '='
    ) {
      op = toks[1].v + '=';
      rhsFrom = 3;
    } else {
      continue;
    }
    const rhs = toks.slice(rhsFrom);
    if (!rhs.length) continue;
    try {
      const p = { t: rhs, i: 0 };
      const v = parseOr(p, out);
      if (p.i !== rhs.length || !isFinite(v)) continue;
      const key = toks[0].v;
      if (op === '=') out[key] = v;
      else if (op === '+=') out[key] = (Number(out[key]) || 0) + v;
      else if (op === '-=') out[key] = (Number(out[key]) || 0) - v;
      else if (op === '*=') out[key] = (Number(out[key]) || 0) * v;
      else if (op === '/=') out[key] = v === 0 ? 0 : (Number(out[key]) || 0) / v;
    } catch (e) { /* 单条失败不影响其它赋值 */ }
  }
  return out;
}

/* ---------------- 节点归一化 ---------------- */

/* questions[].duration 的单位上游文档写明是**毫秒**（-1 = 不限时）。
 * 仍保留一个下限：有些稿会给 500ms 这种来不及点的值，钳到 3s。 */
export function questionDurationMs(v) {
  const n = Number(v);
  if (!isFinite(n) || n <= 0) return 0;
  return Math.max(MIN_CHOICE_COUNTDOWN_MS, Math.min(MAX_CHOICE_COUNTDOWN_MS, Math.round(n)));
}

/* start_time_r 实测 300（= 3.00 秒）。按 1/100 秒解释；未来若上游改成毫秒
 * （>1000）也兼容。最终钳到 [2s, 15s]，避免极短片段一开播就把选项糊上来。 */
export function questionLeadMs(startTimeR) {
  const n = Number(startTimeR);
  if (!isFinite(n) || n <= 0) return CHOICE_LEAD_MS;
  const ms = n > 1000 ? n : n * 10;
  return Math.max(MIN_LEAD_MS, Math.min(MAX_LEAD_MS, Math.round(ms)));
}

/* → { type, durationMs, pauseVideo, startTimeR, leadMs, autoJump, hasChoices }
 *   type=2 / 1 : 有选项，片尾暂停等用户点（实测 pause_video=1）
 *   type=0     : 自动跳转片段，只有一个空文本选项，片尾直接连播 */
export function normalizeQuestion(q) {
  if (!q || typeof q !== 'object') return null;
  const type = Number(q.type) || 0;
  return {
    type: type,
    durationMs: questionDurationMs(q.duration),
    pauseVideo: Number(q.pause_video) === 1,
    startTimeR: Number(q.start_time_r) || 0,
    leadMs: questionLeadMs(q.start_time_r),
    autoJump: type === 0,
    hasChoices: type !== 0
  };
}

/* 片段播完后该怎么走（纯函数，页面只负责执行）：
 *   { kind:'leaf' }                                    结局，留回退/重播
 *   { kind:'auto', choice, delayMs }                   直接连播（type=0 或默认项）
 *   { kind:'wait', choice, countdownMs }               片尾停住等用户点；countdownMs>0 到点走默认项
 * 「暂停等选择」优先于「默认项自动跳」：真实互动稿的 pause_video 恒为 1，
 * 若仍然 900ms 后自动跳，用户根本来不及读完选项。 */
export function segmentEndPlan(node, choices) {
  const n = node && typeof node === 'object' ? node : {};
  const list = Array.isArray(choices) ? choices.filter((c) => c) : [];
  if (Number(n.isLeaf) === 1) return { kind: 'leaf' };
  const q = n.question && typeof n.question === 'object' ? n.question : null;
  const def = defaultChoice(list);
  if (q && q.autoJump) {
    return { kind: 'auto', choice: list[0] || def || null, delayMs: AUTO_JUMP_DELAY_MS };
  }
  if (q && q.pauseVideo) {
    return { kind: 'wait', choice: def, countdownMs: q.durationMs > 0 ? q.durationMs : 0 };
  }
  if (def) return { kind: 'auto', choice: def, delayMs: AUTO_CHAIN_DELAY_MS };
  return { kind: 'wait', choice: null, countdownMs: 0 };
}

/* ---------------- 节点归一化 ---------------- */
// → { title, edgeId, cid, isLeaf, noBacktracking, noEvaluation,
//     storyList, question, choices, hiddenVars, preload }
export function parseEdgeInfo(data, fallbackCid) {
  const d = data && typeof data === 'object' ? data : {};
  const edges = d.edges && typeof d.edges === 'object' ? d.edges : {};
  const qs = Array.isArray(edges.questions) ? edges.questions : [];
  const choices = [];
  for (let i = 0; i < qs.length; i++) {
    const cs = qs[i] && Array.isArray(qs[i].choices) ? qs[i].choices : [];
    for (let j = 0; j < cs.length; j++) {
      const c = normalizeChoice(cs[j]);
      if (c) choices.push(c);
    }
  }
  const question = normalizeQuestion(qs.length ? qs[0] : null);
  /* type=0 的自动跳转项常写成 "A "（有字母但没人会点），不能靠空文本判断；
   * 题型本身就是「自动跳」，把这个节点下的选项统一标成 auto。 */
  if (question && question.autoJump) {
    for (let i = 0; i < choices.length; i++) choices[i].auto = true;
  }
  const storyList = parseStoryList(d.story_list);
  const current = storyList.find((s) => s.isCurrent);
  /* 【不要相信 story_list.is_current】真实抓包（av73267982）：
   * 用 edge_id=48360986 请求「1 第一循环」时，story_list 仍返回
   * 「0 片头 / cid=245682070 / is_current=1」—— 它是**进度回溯**链，
   * 不是当前模块。播放中的 cid 永远由调用方给（起播 cid / 选项 cid / 历史快照），
   * 只有调用方没有时才退回 story_list。搞反会一直重播上一段。 */
  const fb = Math.floor(Number(fallbackCid) || 0);
  const cid = fb > 0
    ? fb
    : Math.floor(Number(d.cid) || 0) || Math.floor(Number(current && current.cid) || 0);
  const preload = d.preload && Array.isArray(d.preload.video)
    ? d.preload.video.map((v) => ({ aid: Number(v && v.aid) || 0, cid: Number(v && v.cid) || 0 })).filter((v) => v.cid > 0)
    : [];
  return {
    title: String(d.title || ''),
    edgeId: Math.floor(Number(d.edge_id) || 0),
    cid: cid,
    isLeaf: Number(d.is_leaf) === 1,
    noBacktracking: Number(d.no_backtracking) === 1,
    noEvaluation: Number(d.no_evaluation) === 1,
    storyList: storyList,
    question: question,
    choices: choices,
    hiddenVars: Array.isArray(d.hidden_vars) ? d.hidden_vars : [],
    preload: preload
  };
}

// → { ok:true, node } | { ok:false, stage, message, code }
export async function fetchEdgeInfo(client, bvid, graphVersion, edgeId) {
  if (!client || typeof client.request !== 'function') {
    return { ok: false, stage: 'param', message: 'client 不可用' };
  }
  if (!/^BV[0-9A-Za-z]{10}$/.test(String(bvid || ''))) {
    return { ok: false, stage: 'param', message: 'bvid 不合法' };
  }
  const gv = Math.floor(Number(graphVersion) || 0);
  if (!(gv > 0)) return { ok: false, stage: 'param', message: 'graph_version 不合法' };
  const q = { bvid: bvid, graph_version: gv };
  const eid = Math.floor(Number(edgeId) || 0);
  if (eid > 0) q.edge_id = eid;
  const r = await client.request(EDGE_INFO_PATH, q, {});
  if (!r || !r.ok) {
    return {
      ok: false,
      stage: r ? r.stage : 'transport',
      code: r ? r.code : undefined,
      message: (r && r.message) || '互动节点请求失败'
    };
  }
  return { ok: true, node: parseEdgeInfo(r.data, 0) };
}

/* 条件过滤后的可点选项。全被过滤掉时退回「所有非隐藏选项」，
 * 避免解析偏差把用户锁死在一个没有出口的片段。 */
export function visibleChoices(node, vars) {
  const all = (node && Array.isArray(node.choices) ? node.choices : []).filter((c) => !c.isHidden);
  if (!all.length) return [];
  const ok = all.filter((c) => evalCondition(c.condition, vars));
  return ok.length ? ok : all;
}

export function defaultChoice(choices) {
  const list = Array.isArray(choices) ? choices : [];
  return list.find((c) => c.isDefault) || null;
}

/* 片段是否该把选项摆出来。durMs<=0（时长未知）时交给末尾判据兜底。 */
export function shouldShowChoices(posMs, durMs, leadMs) {
  const dur = Number(durMs) || 0;
  if (!(dur > 0)) return false;
  const lead = leadMs === undefined ? CHOICE_LEAD_MS : Math.max(0, Number(leadMs) || 0);
  return Number(posMs) >= dur - lead;
}

/* 下一个片段的播放 item：cid 覆盖让 resolveVideoUrl 不再用 view 的第一段 cid。 */
export function buildSegmentItem(baseItem, node, choice) {
  const b = baseItem && typeof baseItem === 'object' ? baseItem : {};
  const c = choice || {};
  const n = node || {};
  return {
    kind: 'video',
    bvid: b.bvid,
    cid: Math.floor(Number(c.cid) || 0),
    useCid: true,
    title: b.title || '',
    up: b.up || '',
    cover: b.cover || '',
    durationSec: 0,
    nodeTitle: String(n.title || ''),
    edgeId: Math.floor(Number(c.id) || 0)
  };
}
