<template>
  <div class="page" :style="L.page">
    <!-- 【3.6.2】日志全屏页：真机没有 adb 时，靠它把 native 现场（bus error /
         平面巡检 / 音频自愈 / seek 耗时）带出来，一张截图就能定位。 -->
    <div v-if="logOpen" class="logpage" :style="L.page">
      <div class="log-head" :style="L.logHead">
        <text class="log-title">{{ logMeta }}</text>
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
      <div class="video-col" :style="L.videoCol">
        <hole v-if="playing" class="video-hole" :style="L.videoCol"></hole>
        <div v-else class="video-idle" :style="L.videoCol">
          <text class="video-idle-text">{{ idleText }}</text>
        </div>
      </div>
      <div class="panel" :style="L.panel">
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
        <div class="foot">
          <text class="summary">{{ summary }}</text>
          <div class="btn btn-sm" @click="onRatioTap">
            <text class="btn-text-sm">{{ ratioLabel }}</text>
          </div>
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
          <div class="btn btn-sm" @click="goBack">
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
import { getString, loadSession, loadSettings, saveSession, logWarn } from '../../services/storage.js';
import * as player from '../../services/player.js';
import {
  buildProfile, ensureProfile, readFalconEnv, playVideoRectsFor,
  cycleRatioMode, currentRatioMode, RATIO_MODES, PROFILE_KEY
} from '../../services/device_profile.js';

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
    const pf = buildProfile({ env: readFalconEnv() });
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
    const VIDEO_W = Math.min(W - 320, Math.round((H * 16) / 9));
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
      /* 洞的逻辑矩形：probe 用它算 render-rectangle，保证两边同源 */
      videoBox: { x: 0, y: 0, width: VIDEO_W, height: H },
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
      ratioLabel: 'A'
    };
  },
  created() {
    this.gen = 0;
    this._timers = new Set();
    this.client = createClient({});
    this.target = { bvid: '' };
    this.loadDeviceInfo();
    /* 档位是持久化在 storage 里的，进页面要显示真值，不能每次都从 A0 起。 */
    getString(PROFILE_KEY, '').then((raw) => {
      let ov = {};
      try { ov = JSON.parse(raw || '{}') || {}; } catch (e) { /* 忽略坏数据 */ }
      this.ratioLabel = RATIO_MODES[currentRatioMode(ov)].label;
    });
    this.run();
  },
  methods: {
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
        this.deviceText =
          p.name + ' ' + p.cfg.width + 'x' + p.cfg.height +
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
          ' x' + (bs / 100).toFixed(1) + ' 档' + this.ratioLabel +
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
    /* 【3.5.6 第四轮】四档是「几何档」：每档都真正改变送到平面的像素尺寸。
     * 旧四档只换缩放方式/列偏移，四张真机截图的 out=253x450 一模一样。
     * 切档后必须刷新设备行 —— 以前只在 created() 读一次，四张截图里
     * deviceText 全是第一档的值，等于没有取证信息。 */
    async onRatioTap() {
      if (this.running) return;
      const m = await cycleRatioMode();
      this.ratioLabel = m.label;
      this.summary = '档 ' + m.label + ' ' + (m.desc || '') + ' · 重跑中…';
      this.run();
      this.loadDeviceInfo();
    },
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
        const noise = /amixer|ECHILD|A2DP sink|avskewMs|sigchld|SIGCHLD/;
        const key = /OPEN|rect:|video:|pad-added|plane|gate|bus error|set_state|first frame|KMS|Esmart|dst/i;
        const noisy = lines.filter((l) => noise.test(l));
        const rest = lines.filter((l) => !noise.test(l));
        const keys = rest.filter((l) => key.test(l));
        const others = rest.filter((l) => !key.test(l));
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
        this.logMeta = 'pb2.log ' + (r && r.bytes ? r.bytes : 0) + 'B · 显示' +
          this.logLines.length + ' 行(KMS' + kmsLines.length + '/关键' +
          Math.min(keys.length, 40) + ') · on=' + (r && r.on ? 1 : 0);
      } catch (e) {
        this.logMeta = '读取失败';
        this.logLines = [String((e && e.message) || e)];
      }
    },
    goBack() {
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
      // 显式钩子保留；回前台不自动重跑（避免意外网络请求，由“重跑”按钮触发）
    },
    onHide() {
      // 切后台 = 与离开页面同一条停止路径：作废 generation + 停播放
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
