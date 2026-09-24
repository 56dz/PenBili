// 播放历史（本地 schema，version 化；给「我的」页用）
import { KEYS, getJson, setJson } from './storage.js';

export const HISTORY_VERSION = 1;
export const MAX_HISTORY = 10;
const BV_RE = /^BV[0-9A-Za-z]{10}$/;

// 容错：非对象/旧版本/脏条目 → 空或裁剪后的干净列表
export function normalizeHistory(raw) {
  const empty = { version: HISTORY_VERSION, items: [] };
  if (!raw || typeof raw !== 'object') return empty;
  if (raw.version !== HISTORY_VERSION) return empty;
  if (!Array.isArray(raw.items)) return empty;
  const items = [];
  const seen = {};
  for (let i = 0; i < raw.items.length && items.length < MAX_HISTORY; i++) {
    const it = raw.items[i];
    if (!it || typeof it !== 'object') continue;
    const bvid = typeof it.bvid === 'string' ? it.bvid : '';
    if (!BV_RE.test(bvid) || seen[bvid]) continue;
    seen[bvid] = 1;
    items.push({
      kind: 'video',
      bvid: bvid,
      cid: Number(it.cid) || 0,
      title: typeof it.title === 'string' && it.title ? it.title : '未命名',
      up: typeof it.up === 'string' ? it.up : '',
      cover: typeof it.cover === 'string' ? it.cover : '',
      durationSec: Number(it.durationSec) || 0,
      view: Number(it.view) || 0,
      source: 'history',
      at: Number(it.at) || 0
    });
  }
  return { version: HISTORY_VERSION, items: items };
}

// 新条目置顶、去重、封顶（纯函数）
export function pushHistory(list, entry, now) {
  const base = normalizeHistory(list && list.version !== undefined ? list : { version: HISTORY_VERSION, items: list || [] });
  const clean = normalizeHistory({ version: HISTORY_VERSION, items: (entry ? [entry] : []).concat(base.items) });
  let at = Number(now) || Date.now();
  // 置顶条目打时间戳；保持 newest-first
  if (clean.items.length > 0) {
    const topBvid = clean.items[0].bvid;
    const eBvid = entry && entry.bvid;
    if (eBvid && topBvid === eBvid) {
      clean.items[0].at = at;
      const rest = clean.items.slice(1).filter((x) => x.bvid !== eBvid);
      clean.items = [clean.items[0]].concat(rest);
    }
  }
  return clean;
}

export async function loadHistory() {
  return normalizeHistory(await getJson(KEYS.history, null));
}

export function saveHistory(history) {
  return setJson(KEYS.history, normalizeHistory(history));
}

// 展示文案：同天 → HH:MM，昨天 → 昨天，更早 → MM-DD
export function formatHistoryTime(at, now) {
  const t = Number(at) || 0;
  if (!t) return '';
  const d = new Date(t);
  const n = new Date(Number(now) || Date.now());
  const p2 = (x) => (x < 10 ? '0' + x : String(x));
  const sameDay = (a, b) => a.getFullYear() === b.getFullYear() && a.getMonth() === b.getMonth() && a.getDate() === b.getDate();
  if (sameDay(d, n)) return p2(d.getHours()) + ':' + p2(d.getMinutes());
  const yesterday = new Date(n.getTime() - 86400000);
  if (sameDay(d, yesterday)) return '昨天';
  return p2(d.getMonth() + 1) + '-' + p2(d.getDate());
}
