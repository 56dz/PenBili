// 稿件结构解析（分 P / 合集）—— 来自 `x/web-interface/view`，纯函数、可单测。
//
// ======================== 上游字段（bilibili-API-collect） ========================
// 分 P：
//   data.pages[] = { cid, page, from, part, duration, dimension:{width,height,rotate} }
//   data.videos  = 分 P 数（老字段，和 pages.length 一致）
//   data.cid     = 默认第一 P 的 cid（有 pages 时等于 pages[0].cid）
//
// 合集（UP 主「合集」，不是番剧 season）：
//   data.ugc_season = {
//     id, title, cover, mid, ep_count,
//     sections: [ { id, title, type, episodes: [ {
//        id, aid, bvid, cid, title,
//        arc: { aid, pic, title, duration, pubdate, desc, ... },
//        page: { cid, page, part, duration }        // 部分响应才有
//     } ] } ]
//   }
//
// ============================== 三个必须处理的坑 ==============================
// 1. **bvid 可能只在 arc 里**：早期响应的 episode 顶层没有 bvid，只有 arc.bvid；
//    反过来也有只有顶层 bvid、arc 缺字段的。两边都取，取不到就跳过这一集 ——
//    宁可少一集，也不能拿一个空 bvid 去请求（会 404 并弹错误）。
// 2. **标题可能为空**：episode.title 在某些稿件是 "1"、"2" 这种序号。
//    这时回退 arc.title；两者都空才用 "第 N 集"。
// 3. **sections 可能为空但 episodes 在顶层**（极少数老合集）。
//    所以除了 sections，还要兜一层「如果没有任何 section 有内容，但顶层有
//    episodes 数组，就当成单节」。
// ==============================================================================

const BV_RE = /^BV[0-9A-Za-z]{10}$/;

function str(v) {
  return typeof v === 'string' ? v.trim() : '';
}

function positive(v) {
  const n = Number(v);
  return isFinite(n) && n > 0 ? Math.floor(n) : 0;
}

/* 分 P 列表。返回 [] 表示单 P 视频（或字段缺失）。
 * 每项：{ cid, page, title, durationSec }
 *   title 取 part；part 为空时用 "P" + 序号（真机上至少能认出是第几段）。 */
export function parsePages(data) {
  const d = data || {};
  const arr = Array.isArray(d.pages) ? d.pages : [];
  const out = [];
  for (let i = 0; i < arr.length; i++) {
    const p = arr[i] || {};
    const cid = positive(p.cid);
    if (!cid) continue;
    const page = positive(p.page) || i + 1;
    const part = str(p.part);
    out.push({
      cid: cid,
      page: page,
      title: part || ('P' + page),
      durationSec: positive(p.duration)
    });
  }
  return out;
}

/* 合集。返回 null 表示这份稿件不属于任何合集。
 * → { id, title, count, sections: [ { title, items: [ { bvid, cid, title, durationSec, cover, page } ] } ] }
 *   items 是扁平顺序，页面按 section 分组渲染；合集可能有多节（正片 / 花絮 / 预告）。 */
export function parseSeason(data) {
  const d = data || {};
  const s = d.ugc_season;
  if (!s || typeof s !== 'object' || Array.isArray(s)) return null;

  const sections = [];
  let count = 0;

  const pushEpisodes = (list, secTitle) => {
    const src = Array.isArray(list) ? list : [];
    const items = [];
    for (let i = 0; i < src.length; i++) {
      const ep = src[i] || {};
      const arc = ep.arc || {};
      const page = ep.page || {};
      let bvid = str(ep.bvid) || str(arc.bvid);
      if (!BV_RE.test(bvid)) continue;
      const cid = positive(ep.cid) || positive(page.cid);
      const rawTitle = str(ep.title);
      /* 顶层的 title 有时只是 "1"/"2" 这种序号 —— 那种情况用 arc.title 更好读 */
      const useArc = !rawTitle || /^\d+$/.test(rawTitle);
      const title = (useArc ? str(arc.title) : rawTitle) || str(arc.title) || ('第 ' + (count + 1) + ' 集');
      items.push({
        bvid: bvid,
        cid: cid,
        title: title,
        durationSec: positive(arc.duration) || positive(page.duration),
        cover: str(arc.pic),
        page: positive(page.page) || 1
      });
      count++;
    }
    if (items.length) {
      sections.push({
        title: str(secTitle) || ('第 ' + (sections.length + 1) + ' 节'),
        items: items
      });
    }
  };

  const secs = Array.isArray(s.sections) ? s.sections : [];
  for (let i = 0; i < secs.length; i++) {
    const sec = secs[i] || {};
    pushEpisodes(sec.episodes, sec.title);
  }
  /* 兜底：没有 sections 但有顶层 episodes 的老响应 */
  if (!count && Array.isArray(s.episodes)) pushEpisodes(s.episodes, s.title);

  if (!count) return null;
  return {
    id: positive(s.id),
    title: str(s.title) || '合集',
    count: count,
    sections: sections
  };
}

/* 当前 cid 在分 P 列表里的下标（用于高亮）；找不到返回 0 —— 
 * 互动视频/合集片段的 cid 不在 pages 里时，不应该让"P1"被点亮。 */
export function pageIndexOf(pages, cid) {
  const arr = Array.isArray(pages) ? pages : [];
  const want = positive(cid);
  if (!want) return -1;
  for (let i = 0; i < arr.length; i++) {
    if (arr[i] && arr[i].cid === want) return i;
  }
  return -1;
}

/* 当前 bvid 在合集里的位置（section 下标 + item 下标）；找不到返回 { si:-1, ii:-1 }。 */
export function episodeIndexOf(season, bvid) {
  const empty = { si: -1, ii: -1 };
  if (!season || !Array.isArray(season.sections)) return empty;
  const want = str(bvid);
  if (!want) return empty;
  for (let si = 0; si < season.sections.length; si++) {
    const items = (season.sections[si] && season.sections[si].items) || [];
    for (let ii = 0; ii < items.length; ii++) {
      if (items[ii] && items[ii].bvid === want) return { si: si, ii: ii };
    }
  }
  return empty;
}

/* "mm:ss" / "h:mm:ss"（分P/合集行尾的时长）。0 → 空串（不显示 "--:--"，
 * 列表里那一位空着比一排占位符干净）。 */
export function fmtDur(sec) {
  const n = Math.max(0, Math.floor(Number(sec) || 0));
  if (!n) return '';
  const h = Math.floor(n / 3600);
  const m = Math.floor((n % 3600) / 60);
  const s = n % 60;
  const p2 = (x) => (x < 10 ? '0' + x : String(x));
  return h > 0 ? h + ':' + p2(m) + ':' + p2(s) : m + ':' + p2(s);
}
