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
// 画质：**主动请求最低档 qn=150（480p）**，并按分辨率从低到高选流。
// 未登录时服务端只给 480p/720p 两档（qn 参数不影响结果）；**已登录时 qn=10000 会给
// 1920x1080@60 原画**，而 mppvideodec 的 rotation 让 1080p60 解码慢 2.89x，直接
// 卡成「一秒一卡」。笔端视频区只有 432x240，480p 足够且余量充足。
import { validateStreamUrl } from './playurl.js';

export const LIVE_PLAYINFO_URL =
  'https://api.live.bilibili.com/xlive/web-room/v2/index/getRoomPlayInfo';

/* 直播输出帧率：B 站直播源固定 30fps（真机 ffprobe 实测 30 fps） */
export const LIVE_FPS = 30;

/* 取流参数。protocol=0,1 → http_stream + http_hls；format=0,1,2 → flv + ts + fmp4；
 * codec=0,1 → avc + hevc；ptype=8 → 网页端播放器形态（缺它会返回精简 playurl）。
 *
 * qn 由「我的 → 直播 → 分辨率」决定（见 services/video_settings.js），默认 150（480p）。
 * 为什么默认不是原画：笔端视频区只有 469x240，原画 1080p60 是纯浪费；更关键的是
 * mppvideodec 的 rotation 让 1080p60 解码慢 2.89x（14.7MB 从 35s 变 101s），
 * 直接卡成「一秒一卡」。480p 带旋转仍有约 13x 实时余量。
 * 注意 qn 对**未登录**基本无效：实测 150/250/400/10000 四个值，服务端一律回 480p。 */
export function buildPlayInfoQuery(roomid, qn) {
  const room = Math.floor(Number(roomid) || 0);
  if (!(room > 0)) return '';
  const q = Math.floor(Number(qn) || 0);
  const want = q > 0 ? q : 150;
  return (
    'room_id=' + room +
    '&protocol=0,1&format=0,1,2&codec=0,1&qn=' + want + '&platform=web&ptype=8'
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
  /* 选流策略（v3.0.3 重写，用户要求「为什么不用 b 站最低画质的流」）：
   *   1) 协议/容器/编码优先：http_stream/flv/avc 最好（长连接实时流、起播最快、
   *      avc 走 mppvideodec 兼容面最广）。
   *   2) **同档内按分辨率从低到高**：笔端屏幕只有 480x960、视频区 432x240，
   *      1080p60 是纯浪费。真机实测（2026-10-30）：1920x1080@60 用
   *      `mppvideodec rotation=270` 解码 14.7MB 要 101s，而不带 rotation 只要 35s
   *      —— 旋转在 MPP 里做会慢 2.9x，直接导致「一秒一卡」。480p 只需约 1/5 像素，
   *      即使加上旋转也远快于实时。
   *   3) 分辨率相同时按码率/未登录可得的 qn 兜底。
   * 注：未登录上限 qn=250，房间若只提供 720p 就只能在 720p 里选最低的。 */
  const score = (x) => {
    if (x.proto === 'http_stream' && x.format === 'flv' && x.codec === 'avc') return 0;
    if (x.proto === 'http_stream' && x.format === 'flv') return 1;
    if (x.format === 'ts' && x.codec === 'avc') return 2;
    if (x.codec === 'avc') return 3;
    return 4;
  };
  const pixels = (x) => (x.width > 0 && x.height > 0 ? x.width * x.height : Number.MAX_SAFE_INTEGER);
  const usable = cands.filter((x) => !!x.url);
  /* **分辨率优先于协议/编码**（v3.0.4，真机踩坑后修正）：
   * 一开始写成「先按 score 排、同档内再按分辨率」，结果设备带登录态时服务端把
   * 1920x1080@60 也标成 http_stream/flv/avc（score=0），和 480p 同档 → 仍选中 1080p，
   * 卡顿照旧。现在改成先比像素数：笔端 432x240 的视频区，任何高于 720p 的流都是
   * 纯粹的 CPU 浪费，而 mppvideodec 的 rotation 又让解码慢 2.89x。
   * 只在分辨率完全相同时才用 score（flv/avc 优先）与 qn 兜底。 */
  usable.sort((a, b) => {
    const dp = pixels(a) - pixels(b);
    if (dp !== 0) return dp;
    const d = score(a) - score(b);
    if (d !== 0) return d;
    return a.qn - b.qn;
  });
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
  const o = opts || {};
  const q = buildPlayInfoQuery(roomid, o.qn);
  if (!q) return { ok: false, stage: 'param', message: '直播间号无效' };
  const res = await client.request(LIVE_PLAYINFO_URL, q, { timeout: o.timeout || 12000 });
  if (o.log && res && res.ok && res.data) {
    const n = (res.data.playurl_info && res.data.playurl_info.playurl &&
      res.data.playurl_info.playurl.stream) || [];
    o.log('[bili] live playinfo streams=' + n.length);
  }
  const parsed = parsePlayInfo(res);
  /* 落一条选流日志：真机「一秒一卡」排查时必须能一眼看出选中的是哪条流。
   * 1080p60 + mppvideodec rotation 会慢 2.9x（实测 14.7MB 从 35s 变 101s），
   * 所以这里把分辨率打出来，方便确认 pickCodec 的低分辨率优先是否真的生效。 */
  if (o.log && parsed.ok) {
    o.log('[bili] live pick ' + parsed.proto + '/' + parsed.format + '/' + parsed.codec +
      ' qn=' + parsed.qn + ' ' + parsed.width + 'x' + parsed.height);
  } else if (o.log && !parsed.ok) {
    o.log('[bili] live parse fail stage=' + parsed.stage + ' msg=' + parsed.message);
  }
  return parsed;
}
