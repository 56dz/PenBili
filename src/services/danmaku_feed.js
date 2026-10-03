// 弹幕数据服务（分段拉取 + 游标推进 + 去重）
//
// 与 v2.7.0 之前的弹幕实现的根本区别：**不在视频帧上烧字**（那是 native drawtext，
// 占解码预算 ~20%，弱稿件软解跌破实时 → 已被移除）。本模块只产出「当前该显示哪几条」，
// 由 Falcon UI 层在右栏渲染成普通文本列表 —— 与评论列表同一条渲染路径，成本极低。
//
// 数据来源：`https://api.bilibili.com/x/v2/dm/web/seg.so`（protobuf，6 分钟一包）。
// 解析在 bili/danmaku.js；本模块负责分段调度、排序、去重、游标推进。
//
// 设计约束（真机是单核 A53 + QuickJS，重绘敏感）：
//   · 不引入新的定时器 —— 由播放轮询 tick 驱动 advance()
//   · 显示列表有硬上限（MAX_LINES），永不增长
//   · 单 tick 产出有上限（MAX_PER_TICK），密集段落按时间均匀取样，避免整屏跳动
//   · 分段缓存有上限（MAX_CACHE_SEGS），长视频不会累积内存

import { SEG_DURATION_MS, segUrl, segCount, toBytes, isDanmakuBytes, parseDanmakuSeg } from './bili/danmaku.js';

export const DM_MAX_LINES = 11; /* 右栏可见行数上限（254px 高 / 实测每行约 21px + 上下 padding） */
export const DM_MAX_PER_TICK = 2; /* 单次推进最多产出条数：密集段落取样，防整屏跳动 */
export const DM_MAX_CACHE_SEGS = 3; /* 分段缓存上限 */
export const DM_PREFETCH_MS = 45000; /* 距本段结束不足此值 → 预取下一段 */

// 播放位置 → 分包序号（1 起；每 6 分钟一包）
export function segIndexOf(positionMs, segMs) {
  const span = Number(segMs) || SEG_DURATION_MS;
  const p = Number(positionMs) || 0;
  return Math.max(1, Math.floor(Math.max(0, p) / span) + 1);
}

// 分包内的弹幕按出现时间去重 + 排序（原始包内顺序不保证递增，见 danmaku.js 头注释）
export function normalizeSeg(items) {
  const seen = new Set();
  const out = [];
  for (let i = 0; i < (items ? items.length : 0); i++) {
    const it = items[i];
    if (!it || !it.text) continue;
    const key = it.id || it.p + ':' + it.text;
    if (seen.has(key)) continue;
    seen.add(key);
    out.push({ id: key, p: Number(it.p) || 0, text: it.text });
  }
  out.sort((a, b) => (a.p - b.p) || (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
  return out;
}

// 在已排序数组中找第一个 p >= t 的下标（二分）
export function lowerBound(arr, t) {
  let lo = 0;
  let hi = arr ? arr.length : 0;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (arr[mid].p < t) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

// 从 pending 中取至多 k 条（保持时间顺序；超量时按时间均匀取样，而不是简单截断，
// 这样 1 秒内涌入的弹幕会铺满整屏而不是只显示开头几条）
export function sampleEven(items, k) {
  const n = items ? items.length : 0;
  if (n <= k) return items ? items.slice() : [];
  const out = [];
  const step = n / k;
  for (let i = 0; i < k; i++) {
    out.push(items[Math.min(n - 1, Math.floor(i * step))]);
  }
  return out;
}

/* 创建弹幕流。
 * opts.fetchSeg(idx) → Promise<{ok, items} | {ok:false, message}>   （idx 从 1 起）
 * opts.log(msg)       → 可选日志
 * opts.maxLines / maxPerTick / maxCacheSegs → 可选覆盖（测试用）
 */
export function createDanmakuFeed(opts) {
  const o = opts || {};
  const fetchSeg = o.fetchSeg;
  const log = o.log || function () {};
  const maxLines = o.maxLines || DM_MAX_LINES;
  const maxPerTick = o.maxPerTick || DM_MAX_PER_TICK;
  const maxCacheSegs = o.maxCacheSegs || DM_MAX_CACHE_SEGS;

  let gen = 0;
  let cid = 0;
  let aid = 0;
  let durationMs = 0;
  let segMs = SEG_DURATION_MS;
  let totalSegs = 0;

  let segs = {}; /* idx → 已排序条目数组 */
  let segState = {}; /* idx → 'loading' | 'ok' | 'fail' */
  let curSeg = 0;
  let curIdx = 0;
  let lastPos = -1;
  let lines = []; /* 显示列表（有上限） */
  let shown = 0; /* 累计产出条数（诊断用） */
  let failed = 0;
  let loadedSegs = 0; /* 成功拉取过的分段数（用于空状态判定） */
  let itemsTotal = 0; /* 累计解析出的弹幕条数（用于"本视频无弹幕"判定） */

  function reset() {
    gen++;
    segs = {};
    segState = {};
    curSeg = 0;
    curIdx = 0;
    lastPos = -1;
    lines = [];
    shown = 0;
    failed = 0;
    loadedSegs = 0;
    itemsTotal = 0;
  }

  function start(session) {
    reset();
    const s = session || {};
    cid = Number(s.cid) || 0;
    aid = Number(s.aid) || 0;
    durationMs = Number(s.durationMs) || 0;
    totalSegs = cid > 0 ? segCount(durationMs) : 0;
    if (cid > 0) log('[bili] dm start cid=' + cid + ' aid=' + aid + ' dur=' + durationMs + 'ms segs=' + totalSegs);
  }

  // 分段条目（未加载返回 null）
  function itemsOf(idx) {
    return segs[idx] || null;
  }

  // 缓存裁剪：只保留离当前段最近的 maxCacheSegs 个
  function trim() {
    const keys = Object.keys(segs).map(Number);
    if (keys.length <= maxCacheSegs) return;
    keys.sort((a, b) => Math.abs(a - curSeg) - Math.abs(b - curSeg));
    for (let i = maxCacheSegs; i < keys.length; i++) {
      delete segs[keys[i]];
      delete segState[keys[i]];
    }
  }

  // 拉取一个分段（幂等；已在加载/已成功则跳过）
  function ensure(idx) {
    if (!(idx >= 1) || (totalSegs > 0 && idx > totalSegs)) return;
    if (segState[idx]) return;
    if (!fetchSeg) return;
    segState[idx] = 'loading';
    const myGen = gen;
    Promise.resolve()
      .then(() => fetchSeg(idx))
      .then((r) => {
        if (myGen !== gen) return; /* 已换视频：丢弃 */
        if (r && r.ok && r.items) {
          segs[idx] = normalizeSeg(r.items);
          segState[idx] = 'ok';
          loadedSegs++;
          itemsTotal += segs[idx].length;
          log('[bili] dm seg ' + idx + ' ok n=' + segs[idx].length);
        } else {
          segState[idx] = 'fail';
          failed++;
          log('[bili] dm seg ' + idx + ' fail: ' + ((r && r.message) || 'unknown'));
        }
        trim();
      })
      .catch((e) => {
        if (myGen !== gen) return;
        segState[idx] = 'fail';
        failed++;
        log('[bili] dm seg ' + idx + ' error: ' + (e && e.message));
      });
  }

  // 是否已在屏。两级判定：
  //   · id 相同 → 同一条弹幕（后退重看/分段重复拉取时会出现）→ 绝不重复进屏
  //   · text 相同 → 不同用户发的同一句话。B 站上极常见（"前方高能"/"我同意"），
  //     但在 174px 的小面板里并排出现两条一模一样的文字，观感就是 bug。
  //     故在当前**可见窗口内**按文本去重（滚出窗口后允许再次出现，不损失信息）。
  function inDisplay(id, text) {
    for (let i = 0; i < lines.length; i++) {
      if (lines[i].id === id) return true;
      if (lines[i].text === text) return true;
    }
    return false;
  }

  /* 推进到 positionMs，返回本次新增条数。
   * 同步：分段未就绪时只触发后台拉取并返回 0，绝不阻塞轮询。 */
  function advance(positionMs) {
    if (!cid) return 0;
    const cur = Math.max(0, Number(positionMs) || 0);
    const seg = segIndexOf(cur, segMs);

    // 换段 / 后退 / 首次 → 重置游标到"当前时间附近"
    const backward = lastPos >= 0 && cur < lastPos;
    const segChanged = seg !== curSeg;
    if (segChanged) {
      curSeg = seg;
      curIdx = 0;
    }
    const needSeek = segChanged || backward || lastPos < 0;
    lastPos = cur;

    const arr = itemsOf(seg);
    if (!arr) {
      ensure(seg); /* 后台拉取，本次不产出 */
      return 0;
    }
    if (needSeek) {
      // 起点 = 第一个 p >= cur 的条目：前进跳段时跳过已过期弹幕，后退重看时从当前点重新开始
      curIdx = lowerBound(arr, cur);
    }

    const pending = [];
    while (curIdx < arr.length && arr[curIdx].p <= cur) {
      pending.push(arr[curIdx]);
      curIdx++;
    }

    // 预取下一段
    const segEnd = seg * segMs;
    if (segEnd - cur < DM_PREFETCH_MS) ensure(seg + 1);

    if (!pending.length) return 0;

    const picked = sampleEven(pending, maxPerTick);
    let added = 0;
    for (let i = 0; i < picked.length; i++) {
      const it = picked[i];
      if (inDisplay(it.id, it.text)) continue; /* 去重（id 全局 / text 限可见窗口） */
      lines.push({ id: it.id, text: it.text });
      added++;
    }
    if (added > 0) {
      shown += added;
      if (lines.length > maxLines) lines = lines.slice(lines.length - maxLines);
    }
    return added;
  }

  /* 空状态提示文案。空面板若什么都不显示，看起来像界面坏了 —— 这里给出唯一的一行说明。
   * 返回 '' 表示"有内容可显示"或"数据已就绪、只是播放头还没走到第一条弹幕"。 */
  function hintText() {
    if (!cid) return '';
    if (lines.length) return '';
    if (!loadedSegs) return '弹幕加载中…';
    if (itemsTotal === 0) return '本视频无弹幕';
    return '';
  }

  /* 显示窗口的廉价指纹（长度 + 末条 id）：用于页面判断是否需要重新赋值触发渲染。
   * 列表只会在尾部追加、超限时从头部丢弃 → 这两个量足以唯一标识当前窗口。 */
  function signature() {
    return lines.length + ':' + (lines.length ? lines[lines.length - 1].id : '');
  }

  return {
    start: start,
    reset: reset,
    advance: advance,
    ensure: ensure,
    getLines: () => lines.slice(),
    lineCount: () => lines.length,
    hintText: hintText,
    signature: signature,
    stats: () => ({
      cid: cid,
      seg: curSeg,
      totalSegs: totalSegs,
      shown: shown,
      failed: failed,
      cached: Object.keys(segs).length,
      loadedSegs: loadedSegs,
      itemsTotal: itemsTotal
    })
  };
}

// 默认抓取器：cid/aid → seg.so 字节 → 解析条目。
// 走 client.getBinary（native httpjson，ArrayBuffer 保字节）；魔数校验兜住错误响应。
export function makeSegFetcher(client, cid, aid) {
  return async function fetchSeg(idx) {
    const r = await client.getBinary(segUrl(cid, aid, idx));
    if (!r || !r.ok) return { ok: false, message: (r && r.message) || 'HTTP 失败' };
    const bytes = toBytes(r.body);
    if (!isDanmakuBytes(bytes)) return { ok: false, message: '非弹幕数据（魔数不符）' };
    const parsed = parseDanmakuSeg(bytes);
    if (!parsed.ok) return { ok: false, message: '解析失败: ' + parsed.reason };
    return { ok: true, items: parsed.items };
  };
}
