// 弹性布局（纯函数，Node 可单测）—— 把 3.5.5 的「960x266 死画布」换成
// 「跟着设备视口走」的画布。
//
// 背景（2026-11-07 真机截屏 bugs/主界面左侧，下侧都留有黑边，进行ui的适配.png）：
//   X7 Pro 的视口是 **936x280**，但 app.js 一直 setViewPort(960)，于是
//   ① 整页按 936/960 = 0.975 等比缩小，画布高 266*0.975 = 259.35 < 280
//      → 屏幕底部剩 20.65px 黑边；
//   ② .grid 固定 870 宽，5 列卡 166+8；0.975 缩放后 848.25 恰好等于内容区
//      可用宽度 861.9-7=854.9 的临界值，第 5 列被 flex-wrap 挤到下一行，
//      首页只剩 4 列、右侧空出 184px（用户看到的「右侧黑边」）。
//
// 现在改为：**画布 = 设备视口**（app.js 用 profile.cfg.width 做 setViewPort），
// 1 个设计像素 = 1 个物理像素，不再有 0.975 的缩放与取整误差；下面这套公式
// 在 960x266 上算出来的每个值都与 3.5.5 的硬编码**逐像素相同**（单测守护），
// 在 936x280 上则把多出来的 24 列/14 行按比例分给各栏与卡片，黑边消失。

export const BASE = { width: 960, height: 266 };

// 导航栏宽度（960 设计稿定稿值；两机型都不缩放，保证文字排版不变）
export const NAV_WIDTH = 76;
// 播放/图文态左图标栏宽度
export const ICON_BAR_WIDTH = 64;
// 卡片网格：列间距、左侧缩进、基准卡宽与封面比例（166x93 = 16:8.96，沿用原值）
export const GRID_GAP = 8;
export const GRID_PAD_LEFT = 7;
export const CARD_BASE_W = 166;
export const CARD_BASE_H = 93;

function round(v) {
  return Math.round(Number(v) || 0);
}

function clamp(v, lo, hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

/*
 * 网格列数：按「960 上一行 5 列」反推目标列宽 174（166+8），再按实际内容宽取整。
 *   960-76 = 884 → (884-7+8)/174 = 5.086 → 5 列（与旧值一致）
 *   936-76 = 860 → (860-7+8)/174 = 4.948 → 5 列（卡宽缩到 162，刚好填满）
 */
export function gridColumns(contentWidth) {
  const raw = (Number(contentWidth) - GRID_PAD_LEFT + GRID_GAP) / (CARD_BASE_W + GRID_GAP);
  return clamp(Math.round(raw), 3, 6);
}

/*
 * 卡片宽度：优先用基准 166，放不下就按列数均分（留出左缩进与每列右间距）。
 *   884 内容区：floor((884-7-40-6)/5) = 166 → min(166,166) = 166（与旧值一致）
 *   860 内容区：floor((860-7-40-6)/5) = 161 → 161
 *
 * 末尾那个 6px 是**防换行余量**，不是审美留白。真机证据（bugs 截图）：
 * X7 Pro 上 falcon 的 scroller 会按自己测到的视口宽（视口−导航栏）来决定
 * 内容宽，而不是照抄 CSS 的 884px；一旦 5 张卡的总宽刚好等于容器宽，
 * 第 5 张就会整列换行 —— 表现正是「首页只剩 4 列、右侧空出 180px」。
 * 留 6px 余量后，X6 仍是 166（容器 884，用掉 877，余 7），
 * X7 是 161（容器 860，用掉 852，余 8）：两边都不会踩到临界值。
 */
export function cardWidth(contentWidth, cols) {
  const n = clamp(round(cols) || 1, 1, 12);
  const safe = 6;
  const fit = Math.floor((Number(contentWidth) - GRID_PAD_LEFT - GRID_GAP * n - safe) / n);
  return Math.max(96, Math.min(CARD_BASE_W, fit));
}

/*
 * 主布局。参数是设备视口（cfg.json 的 screen.width/height）。
 * 返回值里所有数字都是「物理像素」，页面用 :style 直接拼 px。
 */
export function buildLayout(width, height) {
  const W = Math.max(320, round(width) || BASE.width);
  const H = Math.max(120, round(height) || BASE.height);

  // ---------- 浏览态 ----------
  const contentW = W - NAV_WIDTH;
  const cols = gridColumns(contentW);
  const card = cardWidth(contentW, cols);
  const thumbH = Math.max(40, round((card * CARD_BASE_H) / CARD_BASE_W));
  const rowH = thumbH + 40; // 封面 + 标题两行 + 元信息（与 3.5.5 的卡片高度一致）

  // ---------- 播放态三列（213 : 469 : 278 的定稿比例）----------
  const left = round((213 * W) / BASE.width);
  const mid = round((469 * W) / BASE.width);
  const right = W - left - mid;
  // 高度：底部进度条 26 / 左列上下 192+74 / 右列 240+26，全部按 H 等比
  const seekH = Math.max(18, round((26 * H) / BASE.height));
  const videoH = Math.max(80, H - seekH);
  const infoH = round((192 * H) / BASE.height);
  const btnsH = Math.max(40, H - infoH);
  const rightBodyH = round((240 * H) / BASE.height);
  const rightInputH = Math.max(18, H - rightBodyH);

  // ---------- 图文态 ----------
  const bodyW = W - ICON_BAR_WIDTH;

  return {
    width: W,
    height: H,
    nav: NAV_WIDTH,
    iconBar: ICON_BAR_WIDTH,
    content: { width: contentW, height: H },
    grid: {
      cols: cols,
      card: card,
      gap: GRID_GAP,
      padLeft: GRID_PAD_LEFT,
      width: cols * (card + GRID_GAP),
      thumbW: card,
      thumbH: thumbH,
      rowH: rowH
    },
    play: {
      left: left,
      mid: mid,
      right: right,
      videoW: mid,
      videoH: videoH,
      seekH: seekH,
      seekTrackW: Math.max(40, mid - 16),
      infoH: infoH,
      btnsH: btnsH,
      rightBodyH: rightBodyH,
      rightInputH: rightInputH
    },
    article: {
      bodyW: bodyW,
      // 正文左右 padding 各 8 → 图片/封面可用宽
      coverW: Math.max(120, bodyW - 16)
    }
  };
}

// 缓存：页面每次渲染都调 layoutStyles()，别每次重算对象
let cachedKey = '';
let cachedLayout = null;

export function layoutFor(width, height) {
  const key = (round(width) || 0) + 'x' + (round(height) || 0);
  if (key !== cachedKey) {
    cachedKey = key;
    cachedLayout = buildLayout(width, height);
  }
  return cachedLayout;
}

// 把布局翻译成 falcon 能吃的内联样式（td: 'style'，值是 'Npx' 字符串）
export function layoutStyles(L) {
  const px = (n) => round(n) + 'px';
  const page = { width: px(L.width), height: px(L.height) };
  return {
    // 数值版本（<image :width> 这类属性要数字，不能给 '166px'）
    num: {
      width: L.width,
      height: L.height,
      card: L.grid.card,
      thumbH: L.grid.thumbH,
      cols: L.grid.cols,
      videoW: L.play.videoW,
      videoH: L.play.videoH,
      mid: L.play.mid,
      left: L.play.left,
      right: L.play.right,
      seekTrackW: L.play.seekTrackW,
      cover: L.article.coverW
    },
    page: page,
    body: page,
    // 封面：**必须用内联样式**。.thumb 在 base.less 里写的是 166x93，
    // 而 <image> 的 width/height 属性不一定能压过类样式；X7 Pro 卡宽 161 时
    // 若被 166 覆盖，封面会横向溢出 5px 压到下一列。
    thumb: { width: px(L.grid.card), height: px(L.grid.thumbH) },
    vinfo: { width: px(L.grid.card) },
    nav: { width: px(L.nav), height: px(L.height) },
    iconBar: { width: px(L.iconBar), height: px(L.height) },
    content: { width: px(L.content.width), height: px(L.content.height) },
    /* 内容区里的「整行块」（我的页每一行、搜索栏、空态、更多行…）。
     * 它们原来在 less 里写死 864（内容区 884 减左右各 10）；X7 Pro 内容区只有
     * 860，继续用 864 会溢出 4px，scroller 就可能出现横向可拖动。 */
    wide: { width: px(Math.max(200, L.content.width - 20)) },
    grid: { width: px(L.grid.width) },
    card: { width: px(L.grid.card) },
    playLeft: { width: px(L.play.left), height: px(L.height) },
    playInfo: { width: px(L.play.left), height: px(L.play.infoH) },
    playBtns: { width: px(L.play.left), height: px(L.play.btnsH) },
    playMid: { width: px(L.play.mid), height: px(L.height) },
    video: { width: px(L.play.videoW), height: px(L.play.videoH) },
    videoLive: { width: px(L.play.videoW), height: px(L.height) },
    seek: { width: px(L.play.videoW), height: px(L.play.seekH) },
    seekTrack: { width: px(L.play.seekTrackW) },
    playRight: { width: px(L.play.right), height: px(L.height) },
    rightBody: { width: px(L.play.right), height: px(L.play.rightBodyH) },
    rightInput: { width: px(L.play.right), height: px(L.play.rightInputH) },
    articleBody: { width: px(L.article.bodyW), height: px(L.height) },
    cover: { width: px(L.article.coverW) }
  };
}
