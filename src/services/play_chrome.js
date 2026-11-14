// 播放页「悬浮控件」的分段几何与交互度量（纯函数，Node 可单测）
//
// ==================== 3.5.6 第二十四轮：控制条改成「浮在画面上」 ====================
// 前二十三轮一直按「KMS overlay 平面 zpos 高于 Falcon UI，UI 像素压不到画面上」
// 来设计：控制条必须从视频矩形里**让出** 32px。用户反复指出参考系统播放器
// 不是这么做的 —— 它的 bottom-bar 带 `linear-gradient(to top,
// rgba(0,0,0,0.8), transparent)` 背景，明显是准备透出背后画面的；而它的
// `calculateNativeVideoRect` 给 kmssink 的矩形也是**到屏幕底部**的。
//
// 这一轮按参考系统播放器还原，几何上只改一件事：
//
//     视频矩形 = 中列全高 − 顶部条 − 字幕带        （不再扣控制条）
//     DOM     = 视频洞(全高−32−18) + 字幕带(18) + 控制条(32)   （一个像素都不动）
//
// 也就是说 **DOM 完全不重叠**，控制条仍然占真实的 32px；只是 native 的视频
// 平面多画了 32 行到控制条底下。控制条背景是半透明的，那 32 行画面会透出来
// —— 这正是参考系统播放器"进度条盖住视频、背景虚化"的观感。
//
// 这么做的前提是 UI 层能被合成到视频平面**之上**（否则控制条会被画面盖死）。
// X7 Pro 的合成层是 Cluster/Esmart0 组，与 X6 Pro 的排布不同；真机若出现
// "视频正常但控制条整条不见了"，把 device_profile.js 里 X7P.barOverlay 改成
// false 重新打包即可回到旧的分段扣减（没有 storage 开关，避免残留覆盖值）。
// ==================================================================================
//
// 交互语义（用户要求）：
//   · 播放后显示顶部标题（悬浮文字）+ 底部控制条；
//   · 单击画面空白处收起整套控件 —— top 与 bar 归零，视频洞吃掉这两段，
//     字幕带留在最底部（正好是控制条原来的位置）；
//   · 再点一次（或重新播放）控件出现 —— 字幕带上移，让位给控制条；
//   · 暂停时强制显示控件，顶部条变成暂停标记（中间大图标压不到画面上，
//     见上面的设备约束，只能落在画面正上方这条 UI 带里）。
//
// ==================== 3.5.6 第十轮：页面不再使用"动态四段" ====================
// 上面这套「cc 带随字幕出现/消失、top/bar 随控件显隐归零」在 X7 Pro 上是**错的**：
// sw_scale=1 时管线里插了 `videoscale ! video/x-raw,width=<rect.w>,height=<rect.h>`，
// 输出 caps 在 OPEN 那一刻按 g_w.rect 定死。播放中只改 render-rectangle，
// kmssink 拿到的是旧尺寸 buffer 却要往新矩形里贴 —— 真机第十轮照片里
// 「点一下隐藏进度条，画面变宽却被推到下半屏、底部被裁」就是这么来的。
//
// 页面现在**恒定**按 `chrome: true / cc: false` 调 planBands：
//   · 顶栏与控制条那两段像素永远让出来，控件隐藏只是把内容换成纯黑；
//   · 字幕改由顶栏那 18px 单行承载，不再有独立 cc 带；
//   · 全屏态 barH 传 0 让画面段变大，且**只有进/出全屏**才下发 setVideoRect，
//     native 会走一次管线重建让 caps 重算（见 session.c 的 PB2_CMD_SET_RECT）。
// planBands 本身仍保留完整四段能力与压缩逻辑（单测守护、其它机型可用），
// 只是主页不再把 ui.chrome / ccActive 的动态值喂给它。
//
// ================================ 全屏（3.5.6 第七轮） ================================
// 参考系统播放器：独立播放页与详情页内嵌播放器都提供「全屏」动作
// （动作行里是 `循环 / 夜间 / 旋转 / 全屏 / 选择分P`）。
// 它那边的"全屏"是**把周围的 UI 让出去**，不是换管线 —— 和本项目的约束正好同构：
//
//   普通态：left(208) + mid(457) + right(271) = 936
//   全屏态：视频洞独占 936，左右列让位，顶栏/控制条自动收起
//
// 但这里有一笔必须写清楚的账：视口 936x280 是 **3.34:1**，16:9 内容在这么宽的屏上
// **永远是高度受限**（fit 后 498x280 就已经贴满上下沿）。所以：
//
//   · 普通态（控件收起）中列 457x252 → 画面 448x252
//   · 全屏态（无字幕带）    936x280 → 画面 498x280   ← 面积 +23%
//   · 全屏态（保留 28px 底部字幕带）936x252 → 画面 448x252  ← 和普通态**一模一样**
//
// 也就是说：全屏态若照搬"字幕带占底部 28px"，用户在 16:9 稿件上会看到"画面没变大"。
// 因此全屏态的字幕**改由顶栏那条 18px 承载**（`ccInTop`）：控件收起时那条带子就是
// 字幕行，控件展开时它变回 标题/按钮。代价是字幕跑到画面上方，收益是 498x280。
// 这条权衡写在 docs/player-chrome.md §5，改之前先读。
// ==============================================================================

/* 顶部悬浮条高度：一行 10px 标题 + 上下留白。18 是「能看清又不心疼」的折中：
 * 它从视频洞里扣，扣多了画面明显变小（16:9 在中列里本来就是高度受限的）。 */
export const CHROME_TOP_H = 18;
/* 字幕带高度：单行 12px 字号 + 上下各 3 = 18。
 *
 * 【3.5.6 第十四轮】从 28（两行）收成 18（一行），因为**顶栏被删掉了**：
 * 用户在 3.34:1 的小屏上要求「视频占满、进度条别占地方」，那 18px 的标题栏
 * 就是最该省的一段（标题左列已经有一份）。字幕于是回到独立的一条带子，
 * 但只给一行的高度 —— 两行会把画面再压掉 10px。
 * 字幕带只在「字幕开着且真有句子」时存在（ccActive），默认（未登录无字幕）
 * 视频洞就是 250 高，16:9 能到 444x250，比原来 409x230 大 21%。 */
export const CC_BAND_H = 18;
/* 视频洞的最小高度：再怎么挤也要留 80（16:9 的 142x80 仍然能看）。
 * 只有视口极小的机型才会触发下面的"逐段压缩"，正常 266/280 视口碰不到。 */
export const CHROME_MIN_VIDEO = 80;
/* 加载动画节拍（ms）：QuickJS 里跑 setInterval，120ms 一帧既够顺也不心疼 CPU。 */
export const CHROME_LOAD_MS = 120;
/* 全屏态控件自动收起的静默时长（ms）。参考系统播放器的 `scheduleHideControls` + `hideTimer`
 * 是同一条规则。
 * 只在全屏态生效：普通态收起控件会让中列画面变大、左右列不动，观感突变且不是用户
 * 主动点的，容易当成 bug —— 普通态仍然只认"点画面"这一种显隐方式。 */
export const AUTO_HIDE_MS = 3500;

/* 底部控制条各槽位宽度（逻辑像素）。轨道宽 = 中列宽 - 其它所有槽位，
 * 由 barMetrics() 算出来**同一份数字**喂给 DOM 和命中测试 —— 以前 seek 比例靠
 * .seek 的 453 写死，布局一改就静默错位（defect-inventory P1-14）。
 *
 * 【3.5.6 第九轮：为什么 pad 归零】
 * 真机反馈「没有全屏按钮」。根因是**宽度被算了两次**：.chrome-bar 在 CSS 里
 * 有 padding-left/right: 8px，barMetrics 又扣了 BAR_PAD_X*2 = 16。如果 falcon 的
 * width 含 padding（border-box），160+16 就白扣一遍，控制条里第 6 个槽位（全屏键）
 * 正好被挤出可视区 —— 它还在 DOM 里、能点，但看不见。
 * falcon 的盒模型没有官方保证，与其赌，不如让「子元素之和 === 容器宽」在两种
 * 盒模型下都成立：CSS 取消 padding，这里 pad 也归零。留白改由控件自身的内边距
 * 提供（.cb-play 是 30 的圆，图标 20 居中，观感上已经有边距）。
 * 单测 `pad*2 + play + time*2 + ... + track === midW` 继续守着这条守恒。 */
export const BAR_PAD_X = 0;
export const BAR_PLAY_W = 30;
/* 【3.5.6 第十六轮】时间格 40 → 42：参考系统播放器的 `time-current` 是 42 宽
 * （控件常量：bottom-bar 55px / play-btn / 42px / time-current / 68px /
 * time-total / video-seekbar）。它的 time-total 是 68，
 * 但那是因为它写 `02:29 / 1:32:10` 带毫秒位；我们只画 mm:ss，42 够用。
 * 两格各 +2px，轨道少 4px —— 换来 "00:00" 不再贴边。 */
export const BAR_TIME_W = 42;
export const BAR_CC_W = 30;
export const BAR_FS_W = 30;
/* 【3.5.6 第十六轮】进度条滑块直径。参考系统播放器的 seekbar 参数：
 * activeColor=#FF6699、trackSize、handleSize、handleColor=#fff，
 * 轨道色是 `rgba(255,255,255,0.3)`。
 * 实测参考系统播放器截图：轨道 ≈4px、滑块 ≈12px。我们原来是 10px 粗槽 + 16px 大圆点，
 * 在黑底上像"一条挖出来的槽"；改成 4/12 之后才像参考系统播放器那样"浮在黑上的一条细线"。
 * 填充宽必须扣掉这个直径，否则滑块顶出轨道右缘。 */
export const SEEK_KNOB_W = 12;
/* 返回键宽度：**只被全屏侧栏面板的 .cb-back 使用**，不再参与底部条槽位计算。
 * 【3.5.6 第十六轮】真机反馈「全屏模式下有两个退出按钮，左一个右一个」：
 * 底部条的 cb-back（左）与 cb-fs（右，全屏态文字也是「退出」）撞了名，
 * 而侧栏面板里本来就有自己的 cb-back。现在底部条不再有返回键 ——
 * 全屏退出只剩右侧那一个入口（面板的 cb-back，或侧栏不可用时的 cb-fs）。 */
export const BAR_BACK_W = 30;
/* 快退/快进 10 秒：原来在顶栏那 18px 里（-1m/-10s/+10s/+1m 四个）。顶栏删掉后
 * 只保留最有用的两个 ±10s，24 宽够笔尖点（30 高由控制条给）。 */
export const BAR_STEP_W = 24;
export const BAR_GAP = 3;

function num(v, dflt) {
  const n = typeof v === 'number' ? v : parseFloat(v);
  return isFinite(n) ? n : dflt;
}

function clamp(v, lo, hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

/*
 * 分段方案。opts:
 *   totalH  中列总高（= 视口高）
 *   chrome  控件是否展开（直播恒 false，直播没有这些控件）
 *   cc      是否有字幕句（ccActive）
 *   live    直播态（无控件、无字幕带）
 *   full    全屏态（左右列让位，视频洞占满视口宽；字幕改由顶栏承载）
 *   topH / ccH / barH  覆盖默认高度（barH 传 L.seekH）
 * → { total, top, video, cc, bar, chrome, live, full, ccInTop }
 *
 * 恒等式：top + video + cc + bar === total（单测守护）。
 * 空间不够时按「字幕带 → 顶栏 → 控制条」的顺序压 —— 控制条是唯一能拖动/暂停的
 * 入口，最后才让；字幕带压到 0 只是少一行字，不影响操作。
 *
 * full 态的两处不同（见文件头 §全屏）：
 *   1. 不再有独立的底部字幕带（cc 恒 0），否则 16:9 内容在全屏里一点都不会变大；
 *   2. 需要字幕时把顶栏那 18px 征用为字幕行（ccInTop），控件收起也不归零。
 *      顶栏被挤成 0 时 ccInTop 跟着失效 —— 宁可没字幕，也不能让四段加起来超视口。
 */
export function planBands(opts) {
  const o = opts || {};
  const total = Math.max(CHROME_MIN_VIDEO, Math.round(num(o.totalH, 266)));
  const live = o.live === true;
  const full = o.full === true && !live;
  const chrome = o.chrome !== false && !live;
  const wantCc = o.cc === true && !live;
  /* 【3.5.6 第十四轮】顶栏删掉后，"字幕借顶栏"（ccInTop）已经没有载体：
   * 页面现在传 ccFull:true，全屏态也有自己的一条 18px 字幕带 —— 这样全屏与
   * 普通态在「有字幕 / 无字幕」两种情况下视频段高度完全相同，进出全屏连尺寸
   * 都不用改（caps 不用重算，也就不会压扁）。
   * ccFull 不传时保留老行为（ccInTop 借顶栏），给别的页面/单测用。 */
  const ccFull = o.ccFull === true;
  const ccOwn = wantCc && (!full || ccFull);
  let ccInTop = wantCc && full && !ccFull;
  let top = chrome ? Math.max(0, Math.round(num(o.topH, CHROME_TOP_H))) : 0;
  let bar = chrome ? Math.max(0, Math.round(num(o.barH, 0))) : 0;
  let cc = ccOwn ? Math.max(0, Math.round(num(o.ccH, CC_BAND_H))) : 0;

  /* 老路径（ccInTop）：全屏态控件收起时，顶栏那 18px 要靠字幕"挣"出来。 */
  if (ccInTop && top === 0) top = Math.max(0, Math.round(num(o.topH, CHROME_TOP_H)));

  let over = top + bar + cc + CHROME_MIN_VIDEO - total;
  if (over > 0) {
    const a = Math.min(cc, over);
    cc -= a;
    over -= a;
  }
  if (over > 0) {
    const b = Math.min(top, over);
    top -= b;
    over -= b;
  }
  if (over > 0) {
    bar = Math.max(0, bar - over);
    over = 0;
  }
  if (top === 0) ccInTop = false;
  const video = total - top - cc - bar;
  return {
    total: total, top: top, video: video, cc: cc, bar: bar,
    chrome: chrome, live: live, full: full, ccInTop: ccInTop
  };
}

/*
 * 分段 → native 的信箱盒子。area 是**中列**矩形（height 必须是中列总高，
 * 不是 videoArea.height —— 后者已经把进度条那 26px 扣掉了，混用会少 26px）。
 * 视频洞从 area.y + top 开始，高度扣掉 top / 字幕带。
 *
 * 【3.5.6 第二十四轮：控制条不再从矩形里扣】
 * 参考系统播放器的 bottom-bar 是 `linear-gradient(to top,
 * rgba(0,0,0,0.8), transparent)` **浮在画面上**的 —— 视频本身的显示区域没有
 * 因为控制条而缩短。这一轮照此还原：
 *
 *     render-rectangle 高度 = 中列全高 − 顶部条 − 字幕带
 *
 * 控制条那 32px 仍然占 DOM 的像素（它必须能点、能看清），但它盖住的是
 * **视频的最后 32 行**，而不是把画面顶上去。字幕带同理（18px 纯黑，字在上面）。
 *
 * 画面比例由 native 的 `videoscale add-borders=true` 保证（见 session.c）：
 * 矩形是什么比例都不影响内容比例，多出来的部分补黑边。所以这条路径对
 * 「矩形偶尔算错」是免疫的 —— 以前那种"盒子比例一变画面就扁"不会再发生。
 */
export function bandBox(area, bands) {
  const a = area || {};
  const b = bands || {};
  const h = Math.max(0, Math.round(num(a.height, 0)));
  const top = Math.max(0, Math.round(num(b.top, 0)));
  /* bar 不再参与扣减；保留字段是为了调用方不用改签名（也便于回退到旧布局）。 */
  const bottom = Math.max(0, Math.round(num(b.cc, 0)));
  return {
    x: Math.round(num(a.x, 0)),
    y: Math.round(num(a.y, 0)) + top,
    width: Math.max(0, Math.round(num(a.width, 0))),
    height: Math.max(CHROME_MIN_VIDEO, h - top - bottom)
  };
}

/*
 * 控制条槽位。midW = 中列宽（X7 普通态 457 / 全屏态 936），
 * opts.cc = 是否要放"字幕"按钮，opts.fs = 是否要放"全屏"按钮。
 * → { pad, play, time, cc, fs, track }
 * track 是**唯一**的轨道宽度来源：DOM 的 width 与 seekRatioAt() 的命中测试都用它。
 */
export function barMetrics(midW, opts) {
  const o = opts || {};
  const mid = Math.max(120, Math.round(num(midW, 469)));
  const ccOn = o.cc !== false;   // 默认给字幕按钮留位
  const fsOn = o.fs === true;    // 全屏键
  const backOn = o.back === true; // 返回键（仅全屏态）
  const steps = Math.max(0, Math.round(num(o.step, 0))); // ±10s 的个数（0 或 2）
  /* 子元素：(+返回) / 播放键 / 当前时间 / 轨道 / 总时长 / (-10s) / (+10s) /
   * (+字幕) / (+全屏)，间距 = 子元素数 - 1。 */
  const gaps = 3 + (ccOn ? 1 : 0) + (fsOn ? 1 : 0) +
    (backOn ? 1 : 0) + (steps > 0 ? steps : 0);
  const fixed = BAR_PAD_X * 2 + BAR_PLAY_W + BAR_TIME_W * 2 + BAR_GAP * gaps +
    (ccOn ? BAR_CC_W : 0) + (fsOn ? BAR_FS_W : 0) +
    (backOn ? BAR_BACK_W : 0) + steps * BAR_STEP_W;
  return {
    pad: BAR_PAD_X,
    play: BAR_PLAY_W,
    time: BAR_TIME_W,
    cc: ccOn ? BAR_CC_W : 0,
    fs: fsOn ? BAR_FS_W : 0,
    back: backOn ? BAR_BACK_W : 0,
    step: steps > 0 ? BAR_STEP_W : 0,
    steps: steps,
    track: Math.max(40, mid - fixed)
  };
}

/*
 * 轨道上的触点 → [0,1]。x 是**相对轨道元素**的 pageX（falcon 实测：
 * 在处理函数所在的元素内点 x=37 拿到的就是 37，不用减容器绝对坐标），
 * 所以这里不做任何 padding 补偿；越界钳到 0/1（快速拖动时触点会跑出轨道）。
 * 非数字返回 null —— 页面据此忽略这次触摸。
 */
export function trackRatioFromX(x, trackW) {
  const n = Number(x);
  if (!isFinite(n)) return null;
  const w = Number(trackW) > 0 ? Number(trackW) : 1;
  const r = n / w;
  if (!isFinite(r)) return null;
  return clamp(r, 0, 1);
}

/*
 * ±10s 的目标位置（绝对毫秒）。dur<=0（时长未知/直播）返回 0，调用方直接忽略。
 * 右端留 1s 余量：停在 dur 正好会触发 native 的"播完"，-1s 能稳定重播最后一帧。
 */
export function seekGoal(posMs, durMs, deltaMs) {
  const dur = Math.max(0, Math.round(num(durMs, 0)));
  if (!(dur > 0)) return 0;
  const pos = Math.max(0, Math.round(num(posMs, 0)));
  const delta = Math.round(num(deltaMs, 0));
  return clamp(pos + delta, 0, Math.max(0, dur - 1000));
}

/*
 * 播放器时间码（毫秒 → "mm:ss" / "h:mm:ss"）。
 *
 * 为什么不复用 feed.formatDuration：那个函数对 0 返回**空串**（列表卡片上
 * "时长为 0 就别显示"是对的），但控制条里当前时间那一格必须一直有字。
 * 真机事故：视频刚起播 / 暂停在开头时 positionMs = 0 → 那格渲染成空白，
 * 用户看到的是「进度条卡在开头，时间也没了」，像是播放器没在走。
 * 负数（seek 后短暂出现的黑帧位置）按 0 处理。
 */
export function formatClock(ms) {
  const n = Math.max(0, Math.floor(num(ms, 0) / 1000));
  const h = Math.floor(n / 3600);
  const m = Math.floor((n % 3600) / 60);
  const s = n % 60;
  const p2 = (x) => (x < 10 ? '0' + x : String(x));
  return h > 0 ? h + ':' + p2(m) + ':' + p2(s) : p2(m) + ':' + p2(s);
}

/*
 * 加载动画帧（B 站那套「小电视 + 三个跳动的点」）。
 * tick 是 120ms 递增的帧号，返回值直接喂给模板：
 *   dot    高亮的点（0..2），每 2 帧换一个 = 240ms
 *   dx     电视屏幕里那道扫描条的横向偏移（-12..12，乒乓）
 */
const BAR_STEPS = [-12, -7, -2, 3, 8, 12, 8, 3, -2, -7];
export function loadingFrame(tick) {
  const t = Math.max(0, Math.floor(num(tick, 0)));
  return { dot: Math.floor(t / 2) % 3, dx: BAR_STEPS[t % BAR_STEPS.length] };
}

/* 控件该不该在「这个播放状态」出现。暂停/播完必须显示（暂停标记就在里面），
 * 播放中由用户点按决定，错误态显示（让用户能看到返回/再次点击）。 */
export function chromeForState(state) {
  if (state === 'paused' || state === 'ended' || state === 'error') return true;
  return null; // null = 保持用户当前选择
}

/*
 * 全屏态要不要开始"静默 3.5s 自动收起"的计时。四个条件缺一不可：
 *   full   只在全屏态做（普通态收起控件会让中列画面跳变，见 AUTO_HIDE_MS 注释）；
 *   chrome 控件本来就是收起的，没什么可收；
 *   busy   正在拖进度条 / 互动题面挂着 —— 用户正要操作，这时候收起来是打断；
 *   state 必须真的在播；暂停/播完/出错/取流中一律不自动收。
 */
export function canAutoHide(state, opts) {
  const o = opts || {};
  if (o.full !== true) return false;
  if (o.chrome !== true) return false;
  if (o.busy === true) return false;
  return state === 'playing';
}
