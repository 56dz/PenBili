// 离线缓存（「下载」页）：文件落在 /userdisk/PenBili/，元数据进 storage。
//
// 为什么文件不走 jsapi http.downloadFile：它的参数只有 {url,savePath,saveName}，
// **不能带请求头**；B 站 CDN 实测（设备 curl 对拍）无 UA/Referer 一律 403。
// 所以下载走 native httpjson 的 startDownload（libcurl，异步 worker，JS 轮询进度）。
//
// 文件命名：<bvid>_<cid>.mp4 —— 同一稿件不同分P（cid 不同）可各存一份。
// 落盘策略在 native 侧：先写 .part，成功后 rename（不会把半截文件当缓存）。
import { KEYS, getJson, setJson, logWarn } from './storage.js';
import { headersToLines } from './net.js';
import { coverThumbUrl } from './imgurl.js';

export const DL_DIR = '/userdisk/PenBili';
export const DL_VERSION = 1;
export const MAX_DOWNLOADS = 50;
const BV_RE = /^BV[0-9A-Za-z]{10}$/;

async function loadNative() {
  try {
    const m = await import('httpjson');
    const mod = m && (typeof m.startDownload === 'function' ? m : m.default);
    return mod || null;
  } catch (e) {
    return null;
  }
}

/** 建目录（幂等）。native 缺失时返回可读错误，由页面提示。 */
export async function ensureDlDir() {
  const mod = await loadNative();
  if (!mod || typeof mod.ensureDir !== 'function') {
    return { ok: false, message: 'native 缺少 ensureDir（请安装 3.8.0 及以上版本）' };
  }
  try {
    const r = mod.ensureDir(DL_DIR);
    if (r && r.ok) return { ok: true };
    return { ok: false, message: (r && r.error) || '创建缓存目录失败' };
  } catch (e) {
    return { ok: false, message: (e && e.message) || String(e) };
  }
}

/** 文件字节数；不存在返回 -1 */
export async function fileSize(path) {
  const mod = await loadNative();
  if (!mod || typeof mod.fileSize !== 'function') return -1;
  try {
    const n = Number(mod.fileSize(path));
    return isFinite(n) ? n : -1;
  } catch (e) {
    return -1;
  }
}

/** 删文件（不存在也算成功） */
export async function removeFile(path) {
  const mod = await loadNative();
  if (!mod || typeof mod.removeFile !== 'function') return { ok: false };
  try {
    return mod.removeFile(path) || { ok: false };
  } catch (e) {
    return { ok: false };
  }
}

export function dlFilePath(bvid, cid) {
  return DL_DIR + '/' + bvid + '_' + (Number(cid) || 0) + '.mp4';
}

/**
 * native 下载到文件（异步 + 进度回调）。Promise 只在终态 resolve，绝不 reject。
 * @param {{url:string, headers:Object, filePath:string, timeoutMs?:number, onProgress?:Function}} opts
 * @returns {Promise<{ok:boolean, bytes?:number, message?:string, status?:number}>}
 */
export async function downloadToFile(opts) {
  const o = opts || {};
  const mod = await loadNative();
  if (!mod || typeof mod.startDownload !== 'function') {
    return { ok: false, message: 'native 缺少 startDownload（请安装 3.8.0 及以上版本）' };
  }
  let id = 0;
  try {
    id = mod.startDownload(o.url, headersToLines(o.headers), o.filePath, o.timeoutMs || 600000);
  } catch (e) {
    return { ok: false, message: (e && e.message) || String(e) };
  }
  if (!(id > 0)) return { ok: false, message: '下载任务创建失败' };
  logWarn('[bili] dl task id=' + id + ' path=' + o.filePath);
  return await new Promise((resolve) => {
    let last = -1;
    const timer = setInterval(() => {
      let r = null;
      try {
        r = mod.downloadPoll(id);
      } catch (e) {
        clearInterval(timer);
        resolve({ ok: false, message: (e && e.message) || String(e) });
        return;
      }
      if (!r) {
        clearInterval(timer);
        resolve({ ok: false, message: '下载任务丢失' });
        return;
      }
      const bytes = Number(r.bytes) || 0;
      if (bytes !== last) {
        last = bytes;
        if (typeof o.onProgress === 'function') o.onProgress(bytes);
      }
      if (r.state === 1) {
        clearInterval(timer);
        resolve({ ok: true, bytes: bytes, status: Number(r.status) || 0 });
      } else if (r.state === 2) {
        clearInterval(timer);
        resolve({ ok: false, bytes: bytes, message: r.error || '下载失败' });
      }
    }, 500);
  });
}

// ---- 元数据（storage，version 化；与 history.js 同风格）----

export function normalizeDownloads(raw) {
  const empty = { version: DL_VERSION, items: [] };
  if (!raw || typeof raw !== 'object' || raw.version !== DL_VERSION || !Array.isArray(raw.items)) return empty;
  const items = [];
  const seen = {};
  for (let i = 0; i < raw.items.length && items.length < MAX_DOWNLOADS; i++) {
    const it = raw.items[i];
    if (!it || typeof it !== 'object') continue;
    const bvid = typeof it.bvid === 'string' ? it.bvid : '';
    const cid = Number(it.cid) || 0;
    if (!BV_RE.test(bvid) || !(cid > 0)) continue;
    const key = bvid + '_' + cid;
    if (seen[key]) continue;
    seen[key] = 1;
    items.push({
      kind: 'video',
      bvid: bvid,
      cid: cid,
      title: typeof it.title === 'string' && it.title ? it.title : '未命名',
      up: typeof it.up === 'string' ? it.up : '',
      cover: coverThumbUrl(typeof it.cover === 'string' ? it.cover : '', 480, 270),
      durationSec: Number(it.durationSec) || 0,
      sizeBytes: Number(it.sizeBytes) || 0,
      width: Number(it.width) || 640,
      height: Number(it.height) || 360,
      filePath: typeof it.filePath === 'string' && it.filePath.charAt(0) === '/' ? it.filePath : dlFilePath(bvid, cid),
      at: Number(it.at) || 0
    });
  }
  return { version: DL_VERSION, items: items };
}

export async function loadDownloads() {
  return normalizeDownloads(await getJson(KEYS.downloads, null));
}

// 兼容两种入参：条目数组，或 {version, items} 文档。
// （踩过的坑：旧实现只认数组，页面传 {version,items} 时被当成空列表存盘 →
//  下载完成后元数据永远丢失，文件在、列表空。）
export function saveDownloads(listOrDoc) {
  const items = Array.isArray(listOrDoc)
    ? listOrDoc
    : (listOrDoc && Array.isArray(listOrDoc.items) ? listOrDoc.items : []);
  return setJson(KEYS.downloads, normalizeDownloads({ version: DL_VERSION, items: items }));
}

// 新条目置顶、同 bvid+cid 覆盖
export function upsertDownload(list, entry) {
  const base = normalizeDownloads({ version: DL_VERSION, items: list || [] }).items;
  const key = entry && entry.bvid + '_' + entry.cid;
  const rest = base.filter((x) => x.bvid + '_' + x.cid !== key);
  return normalizeDownloads({ version: DL_VERSION, items: [entry].concat(rest) }).items;
}

export function removeDownloadEntry(list, bvid, cid) {
  const key = bvid + '_' + (Number(cid) || 0);
  return normalizeDownloads({ version: DL_VERSION, items: list || [] }).items.filter((x) => x.bvid + '_' + x.cid !== key);
}

export function formatBytes(n) {
  const v = Number(n) || 0;
  if (v >= 1048576) return (v / 1048576).toFixed(1) + 'MB';
  if (v >= 1024) return Math.round(v / 1024) + 'KB';
  return v > 0 ? v + 'B' : '';
}
