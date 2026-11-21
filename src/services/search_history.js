// 搜索记录（本地 schema，version 化；给搜索页空态展示）
// 与播放历史（history.js）同款：脏数据/旧版本一律归一成空列表，不让 UI 拿到坏结构。
import { KEYS, getJson, setJson } from './storage.js';

export const SEARCH_HISTORY_VERSION = 1;
export const MAX_SEARCH_HISTORY = 10;

function cleanKw(v) {
  return typeof v === 'string' ? v.replace(/\s+/g, ' ').trim().slice(0, 30) : '';
}

// 容错：非对象/旧版本/脏条目 → 空或裁剪后的干净列表（newest-first、去重）
export function normalizeSearchHistory(raw) {
  const empty = { version: SEARCH_HISTORY_VERSION, items: [] };
  if (!raw || typeof raw !== 'object') return empty;
  if (raw.version !== SEARCH_HISTORY_VERSION) return empty;
  if (!Array.isArray(raw.items)) return empty;
  const items = [];
  const seen = {};
  for (let i = 0; i < raw.items.length && items.length < MAX_SEARCH_HISTORY; i++) {
    const it = raw.items[i];
    const kw = cleanKw(it && typeof it === 'object' ? it.kw : it);
    if (!kw || seen[kw]) continue;
    seen[kw] = 1;
    items.push({ kw: kw, at: Number(it && it.at) || 0 });
  }
  return { version: SEARCH_HISTORY_VERSION, items: items };
}

// 新关键词置顶、去重、封顶（纯函数；返回 {version, items}）
export function pushSearchHistory(list, kw, now) {
  const base = normalizeSearchHistory(
    list && list.version !== undefined ? list : { version: SEARCH_HISTORY_VERSION, items: list || [] }
  );
  const k = cleanKw(kw);
  if (!k) return base;
  const at = Number(now) || Date.now();
  const rest = base.items.filter((x) => x.kw !== k);
  return normalizeSearchHistory({
    version: SEARCH_HISTORY_VERSION,
    items: [{ kw: k, at: at }].concat(rest)
  });
}

export async function loadSearchHistory() {
  return normalizeSearchHistory(await getJson(KEYS.searchHistory, null));
}

export function saveSearchHistory(history) {
  return setJson(KEYS.searchHistory, normalizeSearchHistory(history));
}
