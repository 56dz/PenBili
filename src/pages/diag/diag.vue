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
        <div class="foot">
          <text class="summary">{{ summary }}</text>
          <div class="btn btn-sm" @click="onLogTap">
            <text class="btn-text-sm">日志</text>
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
import { loadSession, loadSettings, saveSession, logWarn } from '../../services/storage.js';
import * as player from '../../services/player.js';
import { buildProfile, ensureProfile, readFalconEnv } from '../../services/device_profile.js';

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
    const PANEL_W = 352;
    const LOG_HEAD_H = 28;
    const px = (n) => Math.round(n) + 'px';
    return {
      L: {
        page: { width: px(W), height: px(H) },
        videoCol: { width: px(W - PANEL_W), height: px(H) },
        panel: { width: px(PANEL_W), height: px(H) },
        logHead: { width: px(W), height: px(LOG_HEAD_H) },
        logScroll: { width: px(W), height: px(H - LOG_HEAD_H) }
      },
      steps: STEPS.map((s) => ({ id: s.id, label: s.label, state: 'pending', detail: '' })),
      running: false,
      playing: false,
      summary: '准备就绪',
      deviceText: '设备信息读取中…',
      bvidText: '',
      idleText: '视频区 · 起播后显示 640x360',
      logOpen: false,
      logMeta: '',
      logLines: []
    };
  },
  created() {
    this.gen = 0;
    this._timers = new Set();
    this.client = createClient({});
    this.target = { bvid: '' };
    this.loadDeviceInfo();
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
          ' | plane=' + (pid < 0 ? 'auto' : pid) + ' vs' + (pvs ? 1 : 0) +
          ' · ' + win;
        logWarn('[diag] ' + this.deviceText + (info && info.rect ? ' rect=' + info.rect : ''));
      } catch (e) {
        this.deviceText = '设备探测失败: ' + ((e && e.message) || 'unknown');
        logWarn('[diag] ' + this.deviceText);
      }
    },
    glyphOf(state) {
      return GLYPHS[state] || '·';
    },
    /* 日志页：把 native 的 /tmp/pb2.log 尾部搬到屏幕上。
     * 「播放错误：」这类空消息只能靠它定位（bus error 原文在里面）。 */
    async onLogTap() {
      if (this.logOpen) {
        this.logOpen = false;
        return;
      }
      this.logOpen = true;
      this.logMeta = '读取中…';
      this.logLines = [];
      try {
        const r = await player.logTail(6000);
        if (r && r.ok && typeof r.text === 'string' && r.text.length) {
          const lines = r.text.split('\n').filter((l) => l.length > 0);
          /* 倒序显示（最新在上）：失败现场一定在最后几行，正序的话要滚到底
           * 才能看到错误，截图时反而容易漏掉关键行。 */
          this.logLines = lines.slice(-160).reverse();
          this.logMeta = 'pb2.log ' + (r.bytes || 0) + 'B · ' + this.logLines.length +
            ' 行 · 新→旧 · on=' + (r.on ? 1 : 0);
        } else if (r && r.code) {
          this.logMeta = 'logTail 不可用';
          this.logLines = ['native 返回 ' + r.code + '（.so 版本过旧？）'];
        } else {
          this.logMeta = '日志为空';
          this.logLines = ['/tmp/pb2.log 无内容：模块未加载，或本进程还没写过日志'];
        }
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
