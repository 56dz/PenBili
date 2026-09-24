// B 站 Web 端 WBI 签名。
// 事实来源：bilibili-API-collect（docs/misc/sign/wbi.md）：
//   mixin_key = 64 项固定置换表作用于 img_key + sub_key，取前 32 字符；
//   w_rid = md5(按键排序的 urlencoded query + mixin_key)；
//   密钥约每日轮换，从 x/web-interface/nav 的 data.wbi_img 获取（匿名也返回）。
import { md5Hex } from './md5.js';

export const MIXIN_TABLE = [
  46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35,
  27, 43, 5, 49, 33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
  37, 48, 7, 16, 24, 55, 40, 61, 26, 17, 0, 1, 60, 51, 30, 4,
  22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36, 20, 34, 44, 52
];

// img_key/sub_key 均为 32 位 hex（nav 返回的 wbi_img 文件名去扩展名）
export function deriveMixinKey(imgKey, subKey) {
  const s = String(imgKey == null ? '' : imgKey) + String(subKey == null ? '' : subKey);
  let out = '';
  for (let i = 0; i < MIXIN_TABLE.length; i++) {
    const idx = MIXIN_TABLE[i];
    if (idx >= 0 && idx < s.length) out += s.charAt(idx);
  }
  return out.slice(0, 32);
}

// 签名编码约定：键按字符排序；值先剔除 !'()* 再 encodeURIComponent
export function encodeValue(v) {
  return encodeURIComponent(String(v == null ? '' : v).replace(/[!'()*]/g, ''));
}

// 返回可直接拼 URL 的 query 串：排序后的参数 + '&w_rid=' + md5(query + mixin)。
// 官方 worked example（可复现断言，见 test/run.js）：
//   params {foo:114, bar:514, zab:1919810}, wts=1702204169,
//   mixin=ea1db124af3c7062474693fa704f4ff8
//   → bar=514&foo=114&wts=1702204169&zab=1919810&w_rid=8f6f2b5b3d485fe1886cec6a0be8c5d4
export function buildSignedQuery(params, mixinKey, wts) {
  const all = {};
  const src = params || {};
  for (let i = 0; i < Object.keys(src).length; i++) {
    const k = Object.keys(src)[i];
    if (k === 'w_rid' || k === 'wts') continue;
    all[k] = src[k];
  }
  all.wts = Math.floor(Number(wts));
  const keys = Object.keys(all).sort();
  const query = keys
    .map((k) => encodeValue(k) + '=' + encodeValue(all[k]))
    .join('&');
  const wRid = md5Hex(query + String(mixinKey == null ? '' : mixinKey));
  return query + '&w_rid=' + wRid;
}
