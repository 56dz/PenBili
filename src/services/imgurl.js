// 图片 URL 归一（X6 Pro 真机网络实测，2026-10-29）
//
// 事实：
//   1. 设备上 i0/i1/i2.hdslb.com **无法解析**（name resolution failure；
//      nslookup 223.5.5.5 超时；直连 IP 亦超时 → 疑似 SNI/域名级封锁）。
//      旧版封面因此全是灰块。
//   2. archive.biliimg.com 在同一设备上可正常访问，且提供**完全相同**的
//      /bfs/archive/<hash>.jpg、/bfs/article/<hash>.jpg 路径
//      （实测 http/https 均 200，1311x737 JPEG）。
//   3. 框架的 ImageLoader 基于 curl，https 未必带回环 CA 证书库 → 统一降 http。
//
// 因此：凡 *.hdslb.com 的图片地址 → archive.biliimg.com，保留 path/query，
// 协议一律 http://。其它域名原样返回（不动外链）。

export const IMG_CDN_HOST = 'archive.biliimg.com';

// 匹配 i0.hdslb.com / s1.hdslb.com / archive.biliimg.com 这类 B 站图片域名
const HDSLB_HOST_RE = /^(?:[a-z0-9-]+\.)*hdslb\.com(:\d+)?$/i;

/**
 * 归一单个图片 URL。
 * @param {string} url 可能为 //host/path、https://host/path、http://host/path
 * @returns {string} 归一后的地址；空输入返回 ''
 */
export function normalizeImageUrl(url) {
  if (typeof url !== 'string') return '';
  const u = url.trim();
  if (!u) return '';
  // 协议相对
  if (u.indexOf('//') === 0) {
    const rest = u.slice(2);
    const slash = rest.indexOf('/');
    const host = slash < 0 ? rest : rest.slice(0, slash);
    const path = slash < 0 ? '' : rest.slice(slash);
    if (HDSLB_HOST_RE.test(host)) return 'http://' + IMG_CDN_HOST + path;
    return 'http:' + u;
  }
  const m = /^(https?):\/\/([^/]+)(\/[^?#]*)?([?#].*)?$/i.exec(u);
  if (m) {
    if (HDSLB_HOST_RE.test(m[2])) {
      return 'http://' + IMG_CDN_HOST + (m[3] || '') + (m[4] || '');
    }
    return u;
  }
  return u;
}
