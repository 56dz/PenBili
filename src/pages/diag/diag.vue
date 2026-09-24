<template>
  <div class="page">
    <div class="video-col">
      <hole v-if="playing" class="video-hole"></hole>
      <div v-else class="video-idle">
        <text class="video-idle-text">{{ idleText }}</text>
      </div>
    </div>
    <div class="panel">
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
      <div class="foot">
        <text class="summary">{{ summary }}</text>
        <div :class="running ? 'btn btn-dim' : 'btn'" @click="onPrimaryTap">
          <text class="btn-text">{{ running ? '停止' : '重跑' }}</text>
        </div>
        <div class="btn" @click="goBack">
          <text class="btn-text">返回</text>
        </div>
      </div>
    </div>
  </div>
</template>

<script>
import { runProbe, STEPS } from '../../services/bili/probe.js';
import { createClient, cookieFromSession, DEFAULT_UA, REFERER } from '../../services/bili/client.js';
import { netAvailable } from '../../services/net.js';
import { loadSession, loadSettings, saveSession, logWarn } from '../../services/storage.js';
import * as player from '../../services/player.js';

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
    return {
      steps: STEPS.map((s) => ({ id: s.id, label: s.label, state: 'pending', detail: '' })),
      running: false,
      playing: false,
      summary: '准备就绪',
      bvidText: '',
      idleText: '视频区 · 起播后显示 640x360'
    };
  },
  created() {
    this.gen = 0;
    this._timers = new Set();
    this.client = createClient({});
    this.target = { bvid: '' };
    this.run();
  },
  methods: {
    glyphOf(state) {
      return GLYPHS[state] || '·';
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
