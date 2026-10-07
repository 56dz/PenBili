// 直播服务器地址解析 + 重定向缓存
//
// 背景：外网入口是固定域名（如 http://penbili.560726.best），但它 302 到
// penbili.560726.xyz:<动态端口>——**端口每次打洞都会变**；而笔端播放地址必须能直连
// （且只能是 http://，笔端 TLS 栈不可用）。
// 策略（用户指定）：把「解析结果」缓存进 storage（live.resolvedAddr）；
//   app 启动时探测这个缓存地址，**2s 内无响应就重新走入口地址解析并更新缓存**。
//
// /health 返回的 host 字段 = "这次请求实际到达的地址"（服务端回显 Host 头），
// 因此它天然携带了 301 链的终点 → 是解析结果的权威来源。
//
// 全部函数通过注入的 get(url, opts) 走网络（默认 net.httpGet），便于单测。

import { httpGet, tryParseJson } from './net.js';
import { normalizeLiveAddr, logWarn } from './storage.js';

// 超时说明：外网打洞端口（penbili.560726.xyz:<动态>）从外地网络的 TCP connect 实测要
// 5s+（本地/内网 <0.1s），用户最初设的 2s 探测在内网成立、外网必然超时 → 会永久降级回入口域名。
// 故缓存探测 8s、入口解析 15s，既覆盖慢连接，又不至于在端口真的失效时干等太久。
export const PROBE_MS = 8000;   // 缓存地址存活探测超时
export const RESOLVE_MS = 15000; // 入口地址解析超时
export const HEALTH_PATH = '/health';

export function buildHealthUrl(addr) {
  const a = normalizeLiveAddr(addr);
  return a ? a + HEALTH_PATH : '';
}

// /health 响应体 → {ok, addr, host, lan, version, load} | null
export function parseHealth(text) {
  const j = tryParseJson(text);
  if (!j || typeof j !== 'object' || j.ok !== true) return null;
  const host = typeof j.host === 'string' ? j.host.trim() : '';
  return {
    ok: true,
    host: host,
    // host 形如 "penbili.560726.xyz:1728"（无 scheme）→ 统一补 http://
    addr: host ? normalizeLiveAddr(host) : '',
    lan: Array.isArray(j.lan) ? j.lan.filter((x) => typeof x === 'string') : [],
    version: typeof j.version === 'string' ? j.version : '',
    load: j.load && typeof j.load === 'object' ? j.load : null
  };
}

// 探测地址是否活着（顺带拿到它的 host）→ health | null（失败一律 null，不抛）
export async function probeAddr(get, addr, timeoutMs) {
  const url = buildHealthUrl(addr);
  if (!url) return null;
  try {
    const r = await get(url, { timeout: timeoutMs || PROBE_MS });
    return parseHealth(r && r.body);
  } catch (e) {
    return null;
  }
}

// 用入口地址解析真实地址（跟随 301；/health 的 host 即终点）
export async function resolveAddr(get, entry, timeoutMs) {
  const h = await probeAddr(get, entry, timeoutMs || RESOLVE_MS);
  if (!h) return { ok: false, message: '入口地址无响应' };
  return { ok: true, addr: h.addr || normalizeLiveAddr(entry), health: h };
}

// 决定本次使用哪个服务器地址。
// live = { addr: 入口地址, resolvedAddr: 上次解析结果 }
// → { ok, addr, source:'cache'|'resolved'|'entry', changed, stale, degraded?, health? }
//   changed=true 表示解析结果变了（调用方应回写 storage）
export async function ensureLiveAddr(get, live, opts) {
  const o = opts || {};
  const entry = normalizeLiveAddr(live && live.addr);
  const cached = normalizeLiveAddr(live && live.resolvedAddr);
  if (!entry && !cached) {
    return { ok: false, addr: '', source: 'none', changed: false, message: '未配置服务器地址' };
  }
  // ① 有缓存 → 先探缓存（2s）
  if (cached) {
    const h = await probeAddr(get, cached, o.probeMs || PROBE_MS);
    if (h) return { ok: true, addr: cached, source: 'cache', changed: false, stale: false, health: h };
    logWarn('[live] 缓存地址 ' + (o.probeMs || PROBE_MS) + 'ms 无响应 → 重新解析: ' + cached);
  }
  // ② 重解析（走入口地址；没有入口就用缓存地址当入口）
  const base = entry || cached;
  const r = await resolveAddr(get, base, o.resolveMs || RESOLVE_MS);
  if (r.ok && r.addr) {
    return { ok: true, addr: r.addr, source: 'resolved', changed: r.addr !== cached, stale: true, health: r.health };
  }
  // ③ 解析也失败 → 兜底用入口地址本身（播放时再暴露错误）
  return { ok: true, addr: base, source: 'entry', changed: base !== cached, stale: !!cached, degraded: true };
}
