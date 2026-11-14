<template>
  <div class="page" :style="L.page">
    <!-- 【3.6.2】日志全屏页：真机没有 adb 时，靠它把 native 现场（bus error /
         平面巡检 / 音频自愈 / seek 耗时）带出来，一张截图就能定位。 -->
    <div v-if="logOpen" class="logpage" :style="L.page">
      <div class="log-head" :style="L.logHead">
        <text class="log-title">{{ logMeta }}</text>
        <div class="btn btn-sm" @click="onKeyToggle">
          <text class="btn-text-sm">{{ logKeyOnly ? '全部' : '关键' }}</text>
        </div>
        <div class="btn" @click="onLogTap">
          <text class="btn-text">关闭</text>
        </div>
      </div>
      <scroller class="log-scroll" :style="L.logScroll">
        <div v-for="(ln, i) in logLines" :key="'lg' + i" class="log-row">
          <text class="log-line">{{ ln }}</text>
        </div>
      </scroller>
    </div>
    <template v-else>
      <div class="video-col" :style="vdColStyle">
        <hole v-if="playing" class="video-hole" :style="vdBoxStyle"></hole>
        <div v-else class="video-idle" :style="vdBoxStyle">
          <text class="video-idle-text">{{ idleText }}</text>
        </div>
        <!-- 【第十五轮】自检页也要有真正的进度条和全屏键：用户在真机上
             没有 adb，只能靠这一页判断「全屏到底有没有把矩形换过去」。
             条上的时间是 native status() 的实时位置，拖动直接下发 seek。 -->
        <div class="vd-bar" :style="vdBarStyle">
          <div class="vd-play" @click="onVdPlay">
            <text class="vd-play-t">{{ diagState === 'paused' ? '播放' : '暂停' }}</text>
          </div>
          <text class="vd-time">{{ vdCurText }}</text>
          <div
            class="vd-hit"
            :style="{ width: vdTrackW + 'px' }"
            @touchstart="onVdTouch"
            @touchmove="onVdTouch"
            @touchend="onVdRelease"
          >
            <div class="vd-line">
              <div class="vd-fill" :style="{ width: vdFillW + 'px' }"></div>
              <div class="vd-knob"></div>
            </div>
          </div>
          <text class="vd-time">{{ vdDurText }}</text>
          <div class="vd-fs" @click="onVdFull">
            <text class="vd-fs-t">{{ full ? '退出' : '全屏' }}</text>
          </div>
        </div>
      </div>
      <!-- 【3.5.6 第十六轮：右栏「上滚 + 下固定」】
           真机反馈「自检页的侧边无法滑动，导致 X7 无法点击到下面的四个按钮」。
           右栏内容（8 行链路步骤 + 设备/几何/播放现场 + 3 行日志）约 280~300px，
           视口只有 266~280px，末行被裁在屏幕外；原来整栏是个普通 div，手指划不动。
           现在拆成两段：
             · 上半段 <scroller>：步骤/信息/日志，可上下滑；
             · 下半段 .panel-foot：五个按钮，**固定在栏底**，永远点得到。
           不把按钮放进滚动区是刻意的 —— 用户要的是"能点到"，不是"划到底才能点"。
           万一某个型号的 scroller 不认内层 auto 高度，按钮也不会跟着消失。 -->
      <div v-if="!full" class="panel" :style="L.panel">
      <scroller class="panel-scroll" :style="panelScrollStyle">
      <div class="panel-body" :style="panelBodyStyle">
        <div class="head">
          <text class="title">链路自检</text>
          <text class="head-meta">{{ bvidText }}</text>
        </div>
        <div class="steps">
          <div v-for="s in steps" :key="s.id" class="row">
            <text :class="'glyph g-' + s.state">{{ glyphOf(s.state) }}</text>
            <text :class="'label l-' + s.state">{{ s.label }}</text>
            <text :class="'detail d-' + s.state">{{ s.detail }}</text>
          </div>
        </div>
        <!-- 【X7 Pro 适配】黑屏排障：生效的 profile、native 平面号、KMS 窗口名 -->
        <div class="row">
          <text class="detail dev">{{ deviceText }}</text>
        </div>
        <!-- 【3.5.6 第四轮】这一行是「占满」判案的关键：挖洞区 / 下发矩形 /
             实际送进平面的 buffer 尺寸 / 当前档位，四档切换时数字必须跟着变。 -->
        <div class="row">
          <text class="detail dev">{{ geomText }}</text>
        </div>
        <!-- 【3.5.6 第十轮】自检页自己的进度条 + 播放器现场。
             用户反馈「diag 界面没有进度条，无法查找 bug」：以前这里只能看到
             一次性的链路结果，看不到播放是否在推进、位置卡在哪。现在每秒读一次
             native status()，state / 位置 / 时长 / 帧数都在屏幕上，进度条跟着走；
             配合日志页的「关键」过滤，不用 adb 就能定位「进度条卡住 / 画面跑偏」。 -->
        <div class="row">
          <text class="detail dev">{{ playStateText }}</text>
        </div>
        <!-- 【第十五轮】日志同屏：以前要点「日志」进全屏页，看完就看不到画面了。
             现在常驻 3 行关键现场（SET_RECT / rect / videoscale / [diag]），
             截图一张就能同时看到「画面 + 矩形 + 日志」。 -->
        <div class="lv-head">
          <text class="lv-title">关键现场 · {{ liveLogMeta }}</text>
          <div class="btn btn-xs" @click="onLiveToggle">
            <text class="btn-text-xs">{{ liveLogOn ? '停' : '跟' }}</text>
          </div>
        </div>
        <div class="lv-body">
          <text v-for="(ln, i) in liveLog" :key="'lv' + i" class="lv-line">{{ ln }}</text>
        </div>
      </div>
      </scroller>
      <!-- 固定在栏底的按钮行：不随上面的 scroller 滚动 -->
      <div class="panel-foot" :style="panelFootStyle">
        <text class="summary">{{ summary }}</text>
        <div class="btn btn-sm" @click="onLogTap">
          <text class="btn-text-sm">日志</text>
        </div>
        <!-- 【3.5.6 第五轮】画面正常但没声音时，点一下解除全局静音并选通扬声器。 -->
        <div class="btn btn-sm" @click="onSoundTap">
          <text class="btn-text-sm">开声</text>
        </div>
        <div :class="running ? 'btn btn-sm btn-dim' : 'btn btn-sm'" @click="onPrimaryTap">
          <text class="btn-text-sm">{{ running ? '停止' : '重跑' }}</text>
        </div>
        <!-- 自检包里没有主界面（app.json 只登记了 diag 一页），返回按钮会
             调用一个不存在的路由；构建期用 DIAG_ONLY 直接把它藏掉。 -->
        <div v-if="!diagOnly" class="btn btn-sm" @click="goBack">
          <text class="btn-text-sm">返回</text>
        </div>
      </div>
      </div>
    </template>
  </div>
</template>

<script>
import { runProbe, STEPS } from '../../services/bili/probe.js';
import { createClient, cookieFromSession, DEFAULT_UA, REFERER } from '../../services/bili/client.js';
import { netAvailable } from '../../services/net.js';
import { loadSession, loadSettings, saveSession, logWarn } from '../../services/storage.js';
import * as player from '../../services/player.js';
import {
  buildProfile, ensureProfile, playVideoRectsFor
} from '../../services/device_profile.js';
import { formatClock } from '../../services/play_chrome.js';
import { DIAG_ONLY } from '../../build_flags.js';

const GLYPHS = {
  pending: '·',
  running: '>',
  ok: '成',
  fail: '败',
  skip: '跳'
};

export default {
  name: 'PageDiag',
  data() {
    /* 【3.6.2】页面尺寸也不能再写死 960x266：X7 Pro 视口是 936x280，
     * 960 宽会把右侧 24px 的按钮和最后一行文字裁在屏幕外（真机截图里
     * profile= 那行就是被裁掉的）。这里按 profile 算，X6 上仍等于 960x266。 */
    const pf = buildProfile({});
    const W = pf.cfg.width;
    const H = pf.cfg.height;
    /*
     * 【3.5.6 第四轮】视频洞改成严格的 16:9。
     *
     * 旧值是「屏幕宽 - 352 面板」= X7 的 584x280（比例 2.09），而 probe
     * 却按首页中列 457x253 算矩形 → 视频天生比洞小一圈、还偏在一角，
     * 四档怎么换缩放方式都不可能「占满」。现在洞按 H 反推成 16:9
     * （X7 = 498x280），并把这块洞原样传给 probe 当矩形基准：
     * 640x360 恰好铺满、零黑边，画面边界与挖洞边界严格重合。
     * 右侧面板随之变宽，文字与按钮不再挤在 352px 里。
     */
    /* 【第十五轮】视频列要给控制条让出 28px：洞按 (H-28) 做 16:9，
     * 于是 640x360 起播时画面边界与洞边界仍然严格重合。 */
    const VDBAR_H = 28;
    const VIDEO_W = Math.min(W - 300, Math.round(((H - VDBAR_H) * 16) / 9));
    const PANEL_W = W - VIDEO_W;
    const LOG_HEAD_H = 28;
    const px = (n) => Math.round(n) + 'px';
    return {
      L: {
        page: { width: px(W), height: px(H) },
        videoCol: { width: px(VIDEO_W), height: px(H) },
        panel: { width: px(PANEL_W), height: px(H) },
        logHead: { width: px(W), height: px(LOG_HEAD_H) },
        logScroll: { width: px(W), height: px(H - LOG_HEAD_H) }
      },
      /* 洞的逻辑矩形：probe 用它算 render-rectangle，保证两边同源。
       * 高度要扣掉自检页自己的控制条（28），否则画面会被条子盖住一截。 */
      videoBox: { x: 0, y: 0, width: VIDEO_W, height: H - VDBAR_H },
      W: W,
      H: H,
      VDBAR_H: VDBAR_H,
      /* 右栏宽度：滚动容器的内层内容按它减去左右 padding 排版 */
      panelW: PANEL_W,
      /* 底部按钮行固定占 30px（26 内容 + 4 间距），滚动区高度 = H - 30 */
      FOOT_H: 30,
      steps: STEPS.map((s) => ({ id: s.id, label: s.label, state: 'pending', detail: '' })),
      running: false,
      playing: false,
      summary: '准备就绪',
      deviceText: '设备信息读取中…',
      geomText: '几何信息读取中…',
      bvidText: '',
      idleText: '视频区 · 起播后显示 640x360',
      logOpen: false,
      logMeta: '',
      logLines: [],
      logKeyOnly: false,
      diagOnly: DIAG_ONLY,
      /* 播放器现场（每秒轮询 native status()）：自检页也要能看见进度在走 */
      diagState: 'idle',
      diagPos: 0,
      diagDur: 0,
      diagFrames: 0,
      /* 3.5.6 第十四轮：真机「起播就变形」时，这一格是唯一能直接判案的证据。
       * 它是 native 里真正生效的 render-rectangle（物理坐标 <x,y,w,h>），
       * 与页面算出来的 rects.physical 一比就知道问题在页面还是 native。 */
      diagRect: '',
      diagSw: 0,
      /* 【第十五轮】视频列自己的控制条：进度条 / 全屏 / 同屏日志 */
      full: false,
      dragMs: null,
      liveLog: ['（等日志…）'],
      liveLogMeta: 'off',
      liveLogOn: true,
      diagTrackW: Math.max(80, PANEL_W - 20)
    };
  },
  computed: {
    /* 右栏滚动内容宽 = 面板宽 − 左右 padding（10 + 8）。
     * 不写 100%：falcon 对百分比宽度的支持没有保证，内联像素最稳。 */
    panelBodyStyle() {
      return { width: Math.max(120, this.panelW - 18) + 'px' };
    },
    /* 滚动区高度：面板高 − 底部按钮行。必须是确定值 ——
     * falcon 的 scroller 只有拿到明确高度才会滚。 */
    panelScrollStyle() {
      return {
        width: this.panelW + 'px',
        height: Math.max(80, this.H - this.FOOT_H) + 'px'
      };
    },
    panelFootStyle() {
      return { width: this.panelW + 'px', height: this.FOOT_H + 'px' };
    },
    playStateText() {
      const dur = this.diagDur > 0 ? formatClock(this.diagDur) : '--:--';
      const base = '播放 ' + this.diagState + ' · ' + formatClock(this.diagPos) + ' / ' + dur +
        ' · 帧 ' + this.diagFrames + (this.diagDur > 0 ? ' · 每秒刷新' : ' · 时长未知');
      /* 生效矩形 + 缩放档：画面变形/偏移时看这一行就知道 native 拿到的是哪个盒子。
       * sw=1 是 lanczos 档，sw=3 是 bilinear 回退档（storage 可切）。 */
      if (!this.diagRect) return base;
      return base + ' · rect ' + this.diagRect + ' sw=' + this.diagSw;
    },
    playFillW() {
      if (!(this.diagDur > 0)) return 0;
      const r = Math.min(1, Math.max(0, this.diagPos / this.diagDur));
      return Math.round(this.diagTrackW * r);
    },
    /* ---------------- 视频列控制条（第十五轮） ----------------
     * 普通态：宽 = 洞宽（448），全屏态：宽 = 整个视口（936）。
     * 条高 28 从洞高里扣 —— 洞和矩形必须逐像素同源，否则点击与画面对不上。 */
    vdColW() {
      return this.full ? this.W : this.videoBox.width;
    },
    vdColStyle() {
      return { width: this.vdColW + 'px', height: this.H + 'px' };
    },
    vdBoxStyle() {
      return { width: this.vdColW + 'px', height: (this.H - this.VDBAR_H) + 'px' };
    },
    vdBarStyle() {
      return { width: this.vdColW + 'px', height: this.VDBAR_H + 'px' };
    },
    /* 槽位：播放 32 / 当前时间 38 / 轨道 / 总时间 38 / 全屏 40。
     * 五个槽位之和必须 === 列宽（148 是四段固定宽度），否则轨道会溢出被裁。 */
    vdTrackW() {
      return Math.max(60, this.vdColW - 148);
    },
    vdFillW() {
      if (!(this.diagDur > 0)) return 0;
      const ms = this.dragMs == null ? this.diagPos : this.dragMs;
      const r = Math.min(1, Math.max(0, ms / this.diagDur));
      /* 扣掉滑块直径（12，见 diag.less .vd-knob），否则圆点顶出右缘 */
      return Math.max(0, Math.round((this.vdTrackW - 12) * r));
    },
    vdCurText() {
      return formatClock(this.dragMs == null ? this.diagPos : this.dragMs);
    },
    vdDurText() {
      return this.diagDur > 0 ? formatClock(this.diagDur) : '--:--';
    }
  },
  created() {
    this.gen = 0;
    this._timers = new Set();
    this._statusPoll = 0;
    this._rawLines = [];
    this._kmsLines = [];
    this.client = createClient({});
    this.target = { bvid: '' };
    this.loadDeviceInfo();
    this.run();
    this.startStatusPoll();
    this.startLogPoll();
  },
  methods: {
    /* 【第十轮】自检页进度条：每秒读一次 native status()，只读不干预播放。
     * 用户没有 adb，进度条卡住/画面跑偏这类问题只能靠页面自己把现场摆出来。 */
    startStatusPoll() {
      this.stopStatusPoll();
      const tick = async () => {
        try {
          const st = await player.status();
          if (!st || st.ok === false) {
            this.diagState = 'unavailable';
            return;
          }
          this.diagState = st.state || 'idle';
          this.diagPos = Math.max(0, Number(st.positionMs) || 0);
          this.diagDur = Math.max(0, Number(st.durationMs) || 0);
          this.diagFrames = Number(st.frames) || 0;
          this.diagRect = String(st.rect || '');
          this.diagSw = Number(st.swScale) || 0;
        } catch (e) {
          this.diagState = 'status error';
        }
      };
      this._statusPoll = setInterval(tick, 1000);
      tick();
    },
    stopStatusPoll() {
      if (this._statusPoll) {
        clearInterval(this._statusPoll);
        this._statusPoll = 0;
      }
    },
    /* 【第十五轮】同屏日志：每 2s 抓一次 logTail 的关键行，只保留最新 3 行。
     * 用户没有 adb，"全屏点不动"到底是 JS 没下发还是 native 没生效，
     * 只能靠这一格和上面的 rect 字段对照着看。 */
    startLogPoll() {
      this.stopLogPoll();
      this._logPoll = setInterval(() => { this.refreshLiveLog(); }, 2000);
      this.refreshLiveLog();
    },
    stopLogPoll() {
      if (this._logPoll) {
        clearInterval(this._logPoll);
        this._logPoll = 0;
      }
    },
    async refreshLiveLog() {
      if (!this.liveLogOn) return;
      try {
        const r = await player.logTail(8000);
        if (!r || !r.ok || typeof r.text !== 'string') {
          this.liveLogMeta = '不可用';
          return;
        }
        const key = /SET_RECT|OPEN rect|videoscale|overlay api|\[diag\]|first frame|rect aligned|deferred/i;
        const all = r.text.split('\n').filter((l) => l.length > 0 && key.test(l));
        const cut = (l) => (l.length > 76 ? l.slice(0, 76) + '…' : l);
        const tail = all.slice(-3).reverse().map(cut);
        this.liveLog = tail.length ? tail : ['（暂无关键行：先点「重跑」跑一次链路）'];
        this.liveLogMeta = 'on · ' + all.length + ' 行';
      } catch (e) {
        this.liveLogMeta = '读取失败';
      }
    },
    onLiveToggle() {
      this.liveLogOn = !this.liveLogOn;
      if (this.liveLogOn) {
        this.startLogPoll();
      } else {
        this.liveLogMeta = '停';
      }
    },
    /* ---------------- 视频列控制条 ---------------- */
    async onVdPlay() {
      try {
        if (this.diagState === 'paused') await player.resume();
        else await player.pause();
      } catch (e) {
        this.summary = '暂停/继续失败: ' + ((e && e.message) || e);
      }
    },
    vdRatioAt(x) {
      const w = this.vdTrackW;
      if (!(w > 0)) return 0;
      const v = Number(x) / w;
      return v < 0 ? 0 : v > 1 ? 1 : v;
    },
    onVdTouch(e) {
      const t = (e && e.changedTouches && e.changedTouches[0]) || null;
      if (!t || !(this.diagDur > 0)) return;
      this.dragMs = Math.round(this.diagDur * this.vdRatioAt(t.pageX));
    },
    async onVdRelease(e) {
      const t = (e && e.changedTouches && e.changedTouches[0]) || null;
      if (t && this.diagDur > 0) this.dragMs = Math.round(this.diagDur * this.vdRatioAt(t.pageX));
      if (this.dragMs == null) return;
      const target = this.dragMs;
      this.dragMs = null;
      try {
        const r = await player.seek(target);
        logWarn('[diag] seek ' + target + 'ms ok=' + (r && r.ok !== false ? 1 : 0));
      } catch (err) {
        this.summary = 'seek 失败: ' + ((err && err.message) || err);
      }
    },
    /* 全屏：面板让位、视频列铺满整个视口宽。洞高不变（H-28），
     * 所以这是一次**只移动不改尺寸**的 SET_RECT —— 正好用来验证
     * native 那条"overlay 拿不到就重建"的新路径。 */
    async onVdFull() {
      const next = !this.full;
      this.full = next;
      const box = { x: 0, y: 0, width: this.vdColW, height: this.videoBox.height };
      try {
        const pf = await ensureProfile();
        const r = playVideoRectsFor(640, 360, pf, false, box).physical;
        const res = await player.setVideoRect(r.x, r.y, r.width, r.height);
        this.summary = '全屏=' + (next ? 1 : 0) + ' rect=' + r.x + ',' + r.y + ',' +
          r.width + ',' + r.height + ' ok=' + (res && res.ok !== false ? 1 : 0);
        logWarn('[diag] full=' + (next ? 1 : 0) + ' box=' + box.width + 'x' + box.height +
          ' rect=' + r.x + ',' + r.y + ',' + r.width + ',' + r.height);
      } catch (e) {
        this.summary = '全屏失败: ' + ((e && e.message) || e);
        logWarn('[diag] full 失败: ' + ((e && e.message) || e));
      }
      this.loadDeviceInfo();
    },
    /* 【X7 Pro 适配】把「这次播放到底用了哪套设备参数」摆到自检页上。
     * 黑屏时先读这一行：profile 判错机型 / plane=auto 却找不到 ACTIVE 平面 /
     * native 没装载（info 不可用）——三种原因一眼分开。全程只读，不改设备。 */
    async loadDeviceInfo() {
      try {
        const p = await ensureProfile();
        const info = await player.info();
        const kms = await player.kmsProbe();
        let win = 'KMS 摘要不可用';
        if (kms && kms.ok && typeof kms.summary === 'string' && kms.summary.length) {
          const m = /((?:Esmart|Cluster)\d+-win\d+): ACTIVE/.exec(kms.summary);
          win = m ? m[1] + ' ACTIVE' : 'overlay 无 ACTIVE';
        }
        /* native 生效值优先：JS 算出来的 planeId 只是「请求」，兜底重试之后
         * native 实际用的可能是 auto / 开了 videoscale —— 排障要看真值。
         * （3.6.2 真机上就是只显示了 JS 的 plane=76，掩盖了实际失败原因。） */
        const pid = info && info.planeId !== undefined ? info.planeId : p.planeId;
        const pvs = info && info.swScale !== undefined ? info.swScale : p.swScale;
        /* 这一行必须**塞得进 352px 面板**：长字段（rect / canvas）留给「日志」页，
         * 这里只留判断机型/平面是否正确的关键量。真机上文字被裁掉等于没有信息。 */
        const g = (p.layout && p.layout.grid) || {};
        const cfgTxt = p.cfg.width + 'x' + p.cfg.height;
        this.deviceText =
          p.name + ' ' + cfgTxt +
          ' ' + (g.cols || '?') + 'x' + (g.card || '?') +
          ' off' + p.panelXOffset + ' row' + (p.rowOffset || 0) +
          ' | plane=' + (pid < 0 ? 'auto' : pid) + ' vs' + pvs +
          ' buf' + (p.bufScale || 100) + ' · ' + win;
        /*
         * 【3.5.6 第四轮】把「这一档到底把多少像素送进平面」摆到屏幕上。
         * 旧四档的真机截图里这行不存在，四张的 out=253x450 又一模一样，
         * 完全看不出档位有没有生效。现在洞 / 矩形 / 缓冲 / 倍率 / 档位
         * 全部可见，一张截图就能判案。
         */
        const r = playVideoRectsFor(640, 360, p, false, this.videoBox).physical;
        const even = (n) => Math.max(2, Math.round(Number(n) || 0) & ~1);
        const bs = p.bufScale || 100;
        let bw = even(r.height);
        let bh = even(r.width);
        if (p.swScale === 2) { bw = even(r.width); bh = even(r.height); }
        bw = even((bw * bs + 50) / 100);
        bh = even((bh * bs + 50) / 100);
        /* 【3.5.6 第五轮】静音开关状态也摆出来：画面正常却没声音时，
         * 这一眼就能区分「全局静音」与「音频分支故障」。 */
        const muteSrc = String((info && info.mutedBy) || '').split('/').pop() || '?';
        const muteText = (info && info.muted !== undefined)
          ? ' 音=' + (info.muted ? '静音(' + muteSrc + ')' : '开') : '';
        this.geomText =
          '洞' + this.videoBox.width + 'x' + this.videoBox.height +
          ' 矩形' + even(r.width) + 'x' + even(r.height) +
          ' 缓冲' + (p.swScale ? bw + 'x' + bh : '硬件缩放') +
          (info && info.rect ? ' 生效' + info.rect : '') + muteText;
        logWarn('[diag] ' + this.deviceText);
        logWarn('[diag] ' + this.geomText);
      } catch (e) {
        this.deviceText = '设备探测失败: ' + ((e && e.message) || 'unknown');
        logWarn('[diag] ' + this.deviceText);
      }
    },
    glyphOf(state) {
      return GLYPHS[state] || '·';
    },
    /* 【3.5.6 第二十四轮】「档位」按钮已删：自适应分辨率/缩放档位整段移除，
     * 机型参数写死在 device_profile.js 的 X7P / X6P 两张表里。 */
    /* 【3.5.6 第五轮】画面正常但没有声音时使用。
     * 全局静音开关（/tmp/pb2_mute 或 PB2_MUTE=1）会在每次 OPEN 前把
     * Playback Path 设成 OFF：音频数据照常走、自检页仍显示「音=xxxxx/有」，
     * 但扬声器不响。开发机无 adb，用户删不了标志文件，只能由页面下发。
     * 点一次会删掉静音标志并建 /tmp/pb2_sound，之后首页播放也一直有声。 */
    async onSoundTap() {
      this.summary = '正在开声…';
      const r = await player.setSound(true);
      if (!r || r.ok === false) {
        this.summary = '开声失败：' + ((r && r.code) || 'native 不支持');
        return;
      }
      this.summary = '已开声 · 重跑自检…';
      this.run();
    },
    /* 【3.5.6 第五轮】日志页必须能一次看到「几何现场」。
     * 真机拍回来的日志页整整一屏都是 amixer / ECHILD / A2DP 噪音：每次
     * 音量探测打 6 行，几十轮就把 `OPEN rect` / `video: videoscale` 全挤出
     * logTail(6000) 的窗口，等于没有取证。现在：
     *   ① 读取 24KB（原来 6KB）；② 噪音只留最新 3 行；
     *   ③ 几何/错误关键行置顶；④ 把 KMS 摘要里的窗口名与 dst 几何插到最前面。
     * 这样一张截图就能同时看到：UI/视频平面的真实 dst、下发的矩形、buffer 尺寸。 */
    async onLogTap() {
      if (this.logOpen) {
        this.logOpen = false;
        return;
      }
      this.logOpen = true;
      this.logMeta = '读取中…';
      this.logLines = [];
      try {
        const r = await player.logTail(24000);
        let lines = [];
        if (r && r.ok && typeof r.text === 'string' && r.text.length) {
          lines = r.text.split('\n').filter((l) => l.length > 0);
        } else if (r && r.code) {
          this.logMeta = 'logTail 不可用';
          this.logLines = ['native 返回 ' + r.code + '（.so 版本过旧？）'];
          return;
        }
        let kmsLines = [];
        try {
          const k = await player.kmsProbe();
          if (k && k.ok && typeof k.summary === 'string' && k.summary.length) {
            kmsLines = k.summary.split('\n')
              .filter((l) => /Video Port|win\d|ACTIVE|dst|format|buf\[0\]|Connector/.test(l))
              .slice(0, 14)
              .map((l) => '[KMS] ' + l.trim());
          }
        } catch (e2) { /* KMS 读不到不影响日志主体 */ }
        /* 原文缓存下来：「关键 / 全部」切换时不再重新读盘 */
        this._rawLines = lines;
        this._kmsLines = kmsLines;
        this._logBytes = (r && r.bytes) || 0;
        this._logOn = r && r.on ? 1 : 0;
        this.renderLogs();
      } catch (e) {
        this.logMeta = '读取失败';
        this.logLines = [String((e && e.message) || e)];
      }
    },
    /* 【第十轮】「关键」视图：真机日志里 amixer / 音量探测能刷上百行，
     * 把 SET_RECT / 矩形 / videoscale / chrome 这些真正能判案的几行挑出来，
     * 一张截图就能看清「控件显隐到底有没有改矩形」。 */
    renderLogs() {
      const lines = this._rawLines || [];
      const kmsLines = this._kmsLines || [];
      if (this.logKeyOnly) {
        const bug = /SET_RECT|OPEN rect|overlay api|videoscale|\[chrome\]|\[cc\]|poll tick|video ended|deferred|rect aligned|\[diag\]|software scale/i;
        this.logLines = kmsLines.concat(lines.filter((l) => bug.test(l)).slice(-70).reverse());
        if (!this.logLines.length) {
          this.logLines = ['没有关键行：这台设备可能还没进过播放页，或日志被轮转了'];
        }
        this.logMeta = '关键现场 · ' + this.logLines.length + ' 行 · 点「全部」看完整日志';
        return;
      }
      const noise = /amixer|ECHILD|A2DP sink|avskewMs|sigchld|SIGCHLD/;
      /* 关键现场：只留能判案的行。amixer/A2DP 那些噪音会把真正的现场刷出窗口。
       * 3.5.6 第十一轮补上 [cc]（字幕轨道数/登录态/首句时轴）——「字幕无法显示」
       * 的答案就在那一行里，不再需要连 adb 猜。 */
      const key = /SET_RECT|OPEN|rect:|video:|pad-added|plane|gate|bus error|set_state|first frame|KMS|Esmart|dst|overlay api|videoscale|\[chrome\]|\[cc\]|\[extras\]|poll tick|deferred|video ended/i;
      const noisy = lines.filter((l) => noise.test(l));
      const rest = lines.filter((l) => !noise.test(l));
      const keys = rest.filter((l) => key.test(l));
      const others = rest.filter((l) => !key.test(l));
      /* 顺序：KMS 平面几何 → 关键行（最新在上）→ 噪音样本 → 其余（最新在上） */
      this.logLines = kmsLines
        .concat(keys.slice(-40).reverse())
        .concat(noisy.slice(-3).reverse())
        .concat(others.slice(-100).reverse());
      if (!this.logLines.length) {
        this.logMeta = '日志为空';
        this.logLines = ['/tmp/pb2.log 无内容：模块未加载，或本进程还没写过日志'];
        return;
      }
      this.logMeta = 'pb2.log ' + this._logBytes + 'B · 显示' +
        this.logLines.length + ' 行(KMS' + kmsLines.length + '/关键' +
        Math.min(keys.length, 40) + ') · on=' + this._logOn;
    },
    onKeyToggle() {
      this.logKeyOnly = !this.logKeyOnly;
      if (!this._rawLines.length) {
        this.logLines = ['日志还没读取，先退出再点「日志」'];
        return;
      }
      this.renderLogs();
    },
    goBack() {
      /* 自检包是单页应用：没有 index 可回。 */
      if (DIAG_ONLY) return;
      try {
        $falcon.navTo('index');
      } catch (e) {
        logWarn('[diag] 返回失败: ' + (e && e.message));
      }
    },
    sleep(ms) {
      return new Promise((resolve) => {
        const t = setTimeout(() => {
          this._timers.delete(t);
          resolve();
        }, ms);
        this._timers.add(t);
      });
    },
    applyStep(u) {
      const arr = this.steps;
      for (let i = 0; i < arr.length; i++) {
        if (arr[i].id === u.id) {
          arr[i].state = u.state;
          arr[i].detail = u.detail || '';
          if (u.id === 'play') this.playing = u.state === 'ok';
          return;
        }
      }
    },
    markAllSkip(reason) {
      const arr = this.steps;
      for (let i = 0; i < arr.length; i++) {
        if (arr[i].state === 'pending' || arr[i].state === 'running') {
          arr[i].state = 'skip';
          arr[i].detail = reason;
        }
      }
    },
    // 幂等停止当前播放会话（重跑前 / 切后台 / 离页都走这里）
    cancelMedia() {
      const stopChain = player.stop().then(() => player.release());
      stopChain.then(
        () => {},
        (e) => logWarn('[diag] 停止播放异常: ' + (e && e.message))
      );
      return stopChain;
    },
    async run() {
      const myGen = ++this.gen;
      this.cancelMedia();
      this.playing = false;
      this.running = true;
      this.summary = '验证中…';
      this.steps = STEPS.map((s) => ({ id: s.id, label: s.label, state: 'pending', detail: '' }));

      const settings = await loadSettings();
      if (myGen !== this.gen) return;
      this.target = { bvid: settings.bvid };
      this.bvidText = settings.bvid;

      const session = await loadSession();
      if (myGen !== this.gen) return;
      if (session.buvid3) this.client.setCookie(cookieFromSession(session));

      const cancelled = () => myGen !== this.gen;
      const result = await runProbe({
        client: this.client,
        player: player,
        ensureProfile: ensureProfile,
        hasNet: netAvailable,
        target: this.target,
        /* 自检页挖的 16:9 洞：probe 用它算矩形，画面边界与洞严格重合 */
        videoBox: this.videoBox,
        mediaUa: DEFAULT_UA,
        mediaReferer: REFERER,
        nowSec: () => Math.floor(Date.now() / 1000),
        sleep: (ms) => this.sleep(ms),
        log: logWarn,
        cancelled: cancelled,
        onStep: (u) => {
          if (myGen !== this.gen) return;
          this.applyStep(u);
          // 每步落盘（detail 已保证不含签名 URL，可安全进日志）
          logWarn('[bili] step ' + u.id + ' ' + u.state + (u.detail ? ' | ' + u.detail : ''));
        },
        onSession: (s) => {
          if (myGen !== this.gen) return;
          this.client.setCookie(cookieFromSession(s));
          saveSession(s);
        }
      });

      if (myGen !== this.gen) return;
      this.running = false;
      /* 每次自检跑完都刷新 native 生效值（rect / planeId / bufScale），
       * 切档后的截图才不会停留在上一档的信息。 */
      this.loadDeviceInfo();

      if (result && result.cancelled) {
        this.markAllSkip('已取消');
        this.summary = '已取消';
        return;
      }
      if (result && result.ok) {
        this.summary =
          '通过：qn' + result.qn + ' · 5s ' + result.frames + '帧 · 位 ' +
          (result.positionMs / 1000).toFixed(1) + 's · 继续播放';
        logWarn('[bili] PASS qn=' + result.qn + ' frames=' + result.frames + ' host=' + result.host);
        return;
      }
      const failedId = (result && result.failedId) || 'unknown';
      let label = failedId;
      for (let i = 0; i < STEPS.length; i++) {
        if (STEPS[i].id === failedId) label = STEPS[i].label;
      }
      this.playing = false;
      this.cancelMedia();
      this.summary = '失败于「' + label + '」（详见该行）';
      logWarn('[bili] probe failed at ' + failedId);
    },
    onPrimaryTap() {
      if (this.running) {
        // 手动停止：先作废 generation，再停资源（幂等）
        this.gen++;
        this.running = false;
        this.playing = false;
        this.cancelMedia();
        this.markAllSkip('已停止');
        this.summary = '已手动停止';
      } else {
        this.run();
      }
    },
    onShow() {
      // 显式钩子保留；回前台不自动重跑（避免意外网络请求，由"重跑"按钮触发）
      this.startStatusPoll();
      this.startLogPoll();
    },
    onHide() {
      // 切后台 = 与离开页面同一条停止路径：作废 generation + 停播放
      this.stopStatusPoll();
      this.stopLogPoll();
      if (this.running || this.playing) {
        this.gen++;
        this.running = false;
        this.playing = false;
        this.cancelMedia();
        this.markAllSkip('已切后台');
        this.summary = '已停止（切后台）';
      }
    },
    onUnload() {
      this.gen++;
      this.stopLogPoll();
      this.stopStatusPoll();
      this._timers.forEach((t) => clearTimeout(t));
      this._timers.clear();
      this.running = false;
      this.playing = false;
      this.cancelMedia();
    }
  }
};
</script>

<style lang="less" scoped>
/* 样式必须以 <style> 块形式存在于 .vue 内（真机证据：无 style 块时
   规则完全不进 bundle → 页面全黑）。自检页样式独立在 diag.less。 */
@import "../../styles/diag.less";
</style>
