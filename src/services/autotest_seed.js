// 自检种子的运行时守卫（bili_autotest）。
//
// 为什么单独抽一个模块：种子解析写在 index.vue 里，逻辑是纯函数但无法单测
// （页面组件不在 Node 单测范围内），而这条守卫的正确性直接决定
// 「普通用户装上会不会一进软件就自动播放视频」—— 必须可测。
//
// 背景（用户真机反馈）：测试期靠 `bili_autotest={"playProbe":150}` 让 App 启动即
// 自动播指定稿件。但这份种子会残留在 preferences.json 里，正式包一旦读到就自动
// 起播，用户一脸懵。所以从「有种子就跑」收紧成「双开关都显式为 true 才跑」：
//   - enabled === true    —— 总闸
//   - autotest === true   —— 本次确实要跑自检
// 只有 playProbe / soak / soakBv 等参数、缺 autotest:true 的种子，一律当残留忽略。
//
// 为什么连布尔 `true` 也不认：历史上种子的形态演进过（布尔 true / 字符串 'true' /
// 对象），任何一种「宽松识别」都会给残留数据开后门。这里只接受严格的对象双开关，
// 让语义没有歧义 —— 要跑自检就写 {"enabled":true,"autotest":true}。
//
// 注意：`enabled` 必须是布尔 true，字符串 'true' 不认（不猜类型）。同理
// `autotest` 必须是布尔 true。宁可自检不跑，也不能让普通用户被自动播放打扰。

/**
 * 判断自检种子是否显式开启（双开关）。
 * @param {*} at `getJson(KEYS.autotest, null)` 的返回值
 * @returns {boolean} true = 允许进入自动探针/自检流程
 */
export function isAutotestSeedEnabled(at) {
  if (!at || typeof at !== 'object' || Array.isArray(at)) return false;
  return at.enabled === true && at.autotest === true;
}

/**
 * 从已开启的种子里读出各探针参数（只在 isAutotestSeedEnabled(at) 为真时调用）。
 * 抽出来是为了让「非法值不产生副作用」这件事也能被单测钉住：
 * 任何字段缺失/类型不对都退化成「该探针不跑」，绝不因为一个脏值触发意外行为。
 * @param {*} at 种子对象
 * @returns {{soak:number, soakBv:string, liveOnly:boolean, liveRoom:number,
 *            wbiStress:number, liveHold:number, pollEvery:number, playProbe:number,
 *            noKeep:boolean, probeSeek:number, probePause:number, probeRect:boolean,
 *            probePauseRect:boolean, probeLiveEnd:boolean}}
 */
export function readAutotestSeed(at) {
  const o = at && typeof at === 'object' ? at : {};
  const intOf = (v, min) => {
    const n = Math.floor(Number(v) || 0);
    return n >= min ? n : min;
  };
  // soakBv 必须严格是 BV 号形态，否则视为没指定（避免脏值被拼进 URL）
  const bv = typeof o.soakBv === 'string' && /^BV[0-9A-Za-z]{10}$/.test(o.soakBv) ? o.soakBv : '';
  return {
    soak: intOf(o.soak, 0),
    soakBv: bv,
    liveOnly: o.liveOnly === true,
    liveRoom: intOf(o.liveRoom, 0),
    wbiStress: intOf(o.wbiStress, 0),
    liveHold: intOf(o.liveHold, 0),
    // 巡检间隔默认 5s；显式给正数（最小 1s）才生效，0/负数/非数一律退回默认 5s
    pollEvery: (() => {
      const n = Math.floor(Number(o.pollEvery) || 0);
      return n >= 1 ? n : 5;
    })(),
    playProbe: intOf(o.playProbe, 0),
    noKeep: o.noKeep === true,
    probeSeek: intOf(o.probeSeek, 0),
    probePause: intOf(o.probePause, 0),
    probeRect: o.probeRect === true,
    // probePauseRect：暂停态键盘避让（SET_RECT）探针。
    // 必须与 probeRect 分开：render-rectangle 是 write-only 属性，kmssink 只在
    // 重新 configure plane 时才读它，PAUSED 下不 reconfigure —— 播放态能隐藏、
    // 暂停态完全无效（真机实测）。用户报的正是暂停态，所以单列一个开关。
    probePauseRect: o.probePauseRect === true,
    // probeLiveEnd：直播「下播」判据的引擎级自证（纯计算，不取流不播放不亮屏）。
    // 为什么单独一个开关：真实下播没法随时复现，而判据跑在真机 QuickJS 上，
    // 需要一条能在真机上一秒跑完、结论可 grep 的自证路径。
    probeLiveEnd: o.probeLiveEnd === true,
    /* probeUpdate：检查更新全链路自证（检测 → 下载 → 安装）。
     * 为什么需要：http.downloadFile 与 pm.installPackage 的参数形态没有文档，
     * 只能靠真机日志确认；这条探针把三次调用的结论逐行打出来，一条 grep 就能判定。 */
    probeUpdate: o.probeUpdate === true
  };
}
