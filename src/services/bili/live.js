// B 站直播取流（本地直连 + 芯片硬解，不再经服务端转码）
//
// 背景（v3.0.0 / X6 Pro）：X5 时代 4×A53 软解 720p 只有 0.76x 实时，只能靠云端
// 转码把 480p30 HLS 推回来（旧 server/live_proxy.py）。X6 Pro 是 RK3562，有 MPP
// 硬件解码器（/dev/mpp_service + gst mppvideodec），720p 硬解 33x 实时、设备带宽
// 实测 10Mbps 而直播只要 1.2~2Mbps ⇒ 云端转码这一层整体删除，笔端直接拉 FLV。
//
// 接口：GET api.live.bilibili.com/xlive/web-room/v2/index/getRoomPlayInfo
//   响应 data.playurl_info.playurl.stream[] 每条含 protocol_name + format[] + codec[]
//   每个 codec 含 base_url + url_info[0].{host,extra} + current_qn + media_info
//   实测（2026-10-29，房间 22908869）6 种组合：
//     http_stream/flv/avc、http_stream/flv/hevc、http_hls/ts/avc、http_hls/ts/hevc、
//     http_hls/fmp4/avc、http_hls/fmp4/hevc
//   选 **http_stream/flv/avc**：flv 是长连接实时流（无分片，起播最快、拖动语义天然不支持），
//   avc 走 mppvideodec（hevc 也能硬解，但 avc 兼容面更广、码率更低）。
//
// 未登录只能拿 qn=250（720p）：accept_qn 恒为 [10000,400,250]，current_qn 恒 250。
// 720p 在硬解面前毫无压力，所以不做画质档位选择——档位循环是软解时代的遗留。
import { validateStreamUrl } from './playurl.js';

export const LIVE_PLAYINFO_URL =
  'https://api.live.bilibili.com/xlive/web-room/v2/index/getRoomPlayInfo';

/* 直播输出帧率：B 站直播源固定 30fps（真机 ffprobe 实测 30 fps） */
export const LIVE_FPS = 30;

/* 取流参数。protocol=0,1 → http_stream + http_hls；format=0,1,2 → flv + ts + fmp4；
 * codec=0,1 → avc + hevc；ptype=8 → 网页端播放器形态（缺它会返回精简 playurl）。 */
export function buildPlayInfoQuery(roomid) {
  const room = Math.floor(Number(roomid) || 0);
  if (!(room > 0)) return '';
  return (
    'room_id=' + room +
    '&protocol=0,1&format=0,1,2&codec=0,1&qn=10000&platform=web&ptype=8'
  );
}

/* 服务端返回的 extra 里 & 被转义成 \u0026（JSON 里的字面 6 字符），必须反转，
 * 否则 URL 只有一个 query 参数、CDN 直接 403。 */
export function unescapeExtra(extra) {
  return String(extra == null ? '' : extra)
    .replace(/\\u0026/gi, '&')
    .replace(/&amp;/g, '&');
}

/* 单条 codec → 完整直链。host 自带 https://，base_url 以 '?' 结尾。 */
export function joinCodecUrl(codec) {
  const c = codec || {};
  const ui = (c.url_info && c.url_info[0]) || {};
  const host = String(ui.host || '').trim();
  const base = String(c.base_url || '');
  if (!host || !base) return '';
  return host + base + unescapeExtra(ui.extra);
}

/* 优先级：flv/avc > flv/hevc > hls ts/avc > 其它。返回选中项与全部候选（日志用）。 */
export function pickCodec(streams) {
  const cands = [];
  const list = Array.isArray(streams) ? streams : [];
  for (const s of list) {
    const proto = String((s && s.protocol_name) || '');
    const formats = (s && s.format) || [];
    for (const f of formats) {
      const fmt = String((f && f.format_name) || '');
      const codecs = (f && f.codec) || [];
      for (const c of codecs) {
        cands.push({
          proto: proto,
          format: fmt,
          codec: String((c && c.codec_name) || ''),
          qn: Number((c && c.current_qn) || 0),
          width: Number((c && c.media_info && c.media_info.width) || 0),
          height: Number((c && c.media_info && c.media_info.height) || 0),
          url: joinCodecUrl(c)
        });
      }
    }
  }
  const score = (x) => {
    if (x.proto === 'http_stream' && x.format === 'flv' && x.codec === 'avc') return 0;
    if (x.proto === 'http_stream' && x.format === 'flv') return 1;
    if (x.format === 'ts' && x.codec === 'avc') return 2;
    if (x.codec === 'avc') return 3;
    return 4;
  };
  const usable = cands.filter((x) => !!x.url);
  usable.sort((a, b) => score(a) - score(b));
  return { best: usable[0] || null, candidates: cands };
}

/* playinfo 响应 → {ok, url, qn, width, height, acceptQn, candidates}
 * | {ok:false, stage, code?, message} */
export function parsePlayInfo(res) {
  if (!res || res.ok !== true) {
    return {
      ok: false,
      stage: (res && res.stage) || 'api',
      code: res && res.code,
      message: (res && res.message) || '直播取流失败'
    };
  }
  const d = res.data || {};
  if (Number(d.live_status) !== 1) {
    return { ok: false, stage: 'offline', message: '主播未开播' };
  }
  const pu = d.playurl_info && d.playurl_info.playurl;
  if (!pu || !Array.isArray(pu.stream) || pu.stream.length === 0) {
    return { ok: false, stage: 'parse', message: '服务端未返回播放流' };
  }
  const picked = pickCodec(pu.stream);
  if (!picked.best) {
    return { ok: false, stage: 'parse', message: '没有可用的直播流地址' };
  }
  const best = picked.best;
  const v = validateStreamUrl(best.url);
  if (!v.ok) return { ok: false, stage: 'param', message: '直播流 URL ' + v.reason };
  return {
    ok: true,
    url: best.url,
    qn: best.qn || 0,
    width: best.width || 0,
    height: best.height || 0,
    host: v.host,
    proto: best.proto,
    format: best.format,
    codec: best.codec,
    candidates: picked.candidates
  };
}

/* 取直播直链。每次播放都要重新取：URL 带 expires（实测有效期约 1~2 小时），
 * 过期后 CDN 直接 403 —— 旧实现把地址缓存进 storage 是转码时代的做法，现已无意义。 */
export async function fetchLivePlayUrl(client, roomid, opts) {
  const q = buildPlayInfoQuery(roomid);
  if (!q) return { ok: false, stage: 'param', message: '直播间号无效' };
  const o = opts || {};
  const res = await client.request(LIVE_PLAYINFO_URL, q, { timeout: o.timeout || 12000 });
  if (o.log && res && res.ok && res.data) {
    const n = (res.data.playurl_info && res.data.playurl_info.playurl &&
      res.data.playurl_info.playurl.stream) || [];
    o.log('[bili] live playinfo streams=' + n.length);
  }
  return parsePlayInfo(res);
}
