<template>
  <div :class="mode === 'play' ? 'page page-live' : 'page'">
    <!-- 左侧栏：浏览态=选项栏，播放态=播放控制 -->
    <div class="bar">
      <text class="brand">PenBili</text>
      <template v-if="mode === 'browse'">
        <div
          v-for="t in tabs"
          :key="t.id"
          :class="tab === t.id ? 'tab tab-on' : 'tab'"
          @click="selectTab(t.id)"
        >
          <text :class="tab === t.id ? 'tab-text tab-text-on' : 'tab-text'">{{ t.label }}</text>
        </div>
        <text class="bar-status">{{ statusText }}</text>
      </template>
      <template v-else>
        <div class="ctrl" @click="onBack">
          <text class="ctrl-text">返回列表</text>
        </div>
        <div class="ctrl ctrl-main" @click="onTogglePlay">
          <text class="ctrl-text">{{ btnText }}</text>
        </div>
        <div class="ctrl-row">
          <div class="ctrl ctrl-half" @click="onSeek(-seekStepSec)">
            <text class="ctrl-text">{{ '-' + seekStepSec + 's' }}</text>
          </div>
          <div class="ctrl ctrl-half" @click="onSeek(seekStepSec)">
            <text class="ctrl-text">{{ '+' + seekStepSec + 's' }}</text>
          </div>
        </div>
        <!-- 播放进度移至左栏 ±20 下方（用户指定布局） -->
        <text class="bar-time">{{ timeText }}</text>
        <text class="bar-state">{{ stateLabel }}{{ play.note ? ' · ' + play.note : '' }}</text>
      </template>
    </div>

    <!-- 浏览内容 -->
    <div v-if="mode === 'browse'" class="content">
      <!-- 推荐 / 热门：视频行 + 底部（刷新 | 加载更多） -->
      <scroller v-if="tab === 'rcmd' || tab === 'hot'" class="list">
        <div v-for="it in currentItems" :key="it.bvid" class="vrow" @click="onPlayItem(it)">
          <image class="thumb" :src="it.cover" :width="112" :height="63"></image>
          <div class="vinfo">
            <text class="vtitle">{{ it.title }}</text>
            <text class="vmeta">{{ vmeta(it) }}</text>
          </div>
        </div>
        <div v-if="currentItems.length" class="more-row">
          <div class="more-half" @click="onRefreshTab">
            <text class="more-text">刷新</text>
          </div>
          <div class="more-half" @click="tab === 'rcmd' ? loadMoreFromHot() : loadMoreHot()">
            <text class="more-text">{{ moreBtnText }}</text>
          </div>
        </div>
        <div v-if="!currentItems.length && !loading" class="empty">
          <text class="empty-text">{{ statusText || '暂无内容 · 点左侧标签可重试' }}</text>
        </div>
      </scroller>

      <!-- 视频搜索：关键词行 + 结果（直播 tab 的替代，2026-09-23 用户决策） -->
      <scroller v-else-if="tab === 'search'" class="list">
        <div class="searchbar" @click="onOpenInput">
          <text class="search-kw">{{ keyword || '点此输入关键词搜索 B 站视频' }}</text>
          <div class="search-btn">
            <text class="search-btn-text">搜索</text>
          </div>
        </div>
        <div v-for="it in searchItems" :key="'s' + it.bvid" class="vrow" @click="onPlayItem(it)">
          <image class="thumb" :src="it.cover" :width="112" :height="63"></image>
          <div class="vinfo">
            <text class="vtitle">{{ it.title }}</text>
            <text class="vmeta">{{ vmeta(it) }}</text>
          </div>
        </div>
        <!-- 与推荐/热门同款（刷新 | 加载更多）双按钮 -->
        <div v-if="searchItems.length" class="more-row">
          <div class="more-half" @click="onRefreshTab">
            <text class="more-text">刷新</text>
          </div>
          <div class="more-half" @click="loadMoreSearch">
            <text class="more-text">{{ moreBtnText }}</text>
          </div>
        </div>
        <div v-if="!searchItems.length && !loading" class="empty">
          <text class="empty-text">{{ statusText || '输入关键词开始搜索' }}</text>
        </div>
      </scroller>

      <!-- 我的：登录态 + 播放历史 + 自检入口；mineView='qr' 时切换为扫码视图 -->
      <scroller v-else-if="mineView === 'info'" class="list">
        <div class="mine-head">
          <div class="mine-idcol">
            <text class="mine-name">{{ profile ? profile.uname : (buvidShort || '未登录 · 点击扫码登录') }}</text>
            <text class="mine-id">
              {{ profile ? ('Lv.' + profile.level + ' · mid ' + profile.mid + (profile.vipType ? ' · 大会员' : '')) : '登录后个性化推荐 · 身份持久保存' }}
            </text>
          </div>
        </div>
        <div v-if="!profile" class="mine-row" @click="onGoLogin">
          <text class="mine-label">扫码登录 B 站账号</text>
          <text class="mine-value">›</text>
        </div>
        <!-- 未登录说明（收官文案）：视频显示风险 + 行动引导 -->
        <text v-if="!profile" class="mine-notice">未登录时视频可能无法正常显示，登录后体验更完整</text>
        <div v-else class="mine-row" @click="onLogout">
          <text class="mine-label">退出登录</text>
          <text class="mine-value">{{ profile.uname }}</text>
        </div>
        <div class="mine-row" @click="onRefreshProfile">
          <text class="mine-label">刷新登录态</text>
          <text class="mine-value">{{ profileStatus }}</text>
        </div>
        <div class="mine-secrow">
          <text class="mine-sec">播放历史（{{ history.length }}）</text>
        </div>
        <div v-for="h in history" :key="'h' + h.bvid" class="vrow" @click="onPlayItem(h)">
          <image class="thumb" :src="h.cover" :width="112" :height="63"></image>
          <div class="vinfo">
            <text class="vtitle">{{ h.title }}</text>
            <text class="vmeta">{{ h.up }} · {{ historyTime(h.at) }}</text>
          </div>
        </div>
        <div v-if="!history.length" class="mine-row">
          <text class="mine-label">还没有播放记录</text>
        </div>
        <div class="mine-row">
          <text class="mine-label">PenBili</text>
          <text class="mine-value">v2.3.0 · {{ profile ? '已登录' : '匿名' }}</text>
        </div>
      </scroller>

      <!-- 扫码视图：二维码真正水平居中（左右等宽弹力列夹持），状态/按钮在左列 -->
      <div v-else class="qr-wrap">
        <div class="qr-left">
          <text class="qr-state">{{ qr.message }}</text>
          <text class="qr-hint">打开手机B站 App 扫码</text>
          <text class="qr-hint">{{ qr.state === 'waiting' ? '剩余 ' + qr.seconds + ' 秒' : '' }}</text>
          <div class="info-spacer"></div>
          <!-- 单一主按钮：等待中=取消（回列表）；过期/出错=重新生成（真语义，此前两个按钮行为重复） -->
          <div class="ctrl qr-btn" @click="onQrPrimary">
            <text class="ctrl-text">{{ qrPrimaryLabel }}</text>
          </div>
        </div>
        <div class="qr-box">
          <div v-for="(row, ri) in qr.rows" :key="'r' + ri" class="qr-row">
            <div
              v-for="(seg, si) in row"
              :key="'s' + si"
              :class="seg.dark ? 'qr-seg qr-dark ' + seg.cls : 'qr-seg ' + seg.cls"
            ></div>
          </div>
        </div>
        <div class="qr-right"></div>
      </div>
    </div>

    <!-- 评论覆盖面板：不透明覆盖播放内容区（左栏控制保留；视频被盖 → 面板内滑动无视频可闪） -->
    <div v-if="mode === 'play' && commentsOpen" class="comments-col">
      <div class="comments-head">
        <text class="comments-title">评论{{ replyCount ? ' · ' + replyCount : '' }}</text>
        <div class="ctrl comments-close" @click="closeComments">
          <text class="ctrl-text">返回播放</text>
        </div>
      </div>
      <scroller class="comments-scroll">
        <div class="reply-writeline">
          <div class="reply-write" @click="onWriteReply">
            <text class="reply-write-text">写评论</text>
          </div>
          <!-- 未登录说明（收官文案）：显示不全 + 无法发表，引导去「我的」登录 -->
          <text v-if="!hasLogin" class="reply-login-hint">未登录 · 评论可能显示不全，登录后可发表评论</text>
        </div>
        <text v-if="replyStatus" class="reply-status">{{ replyStatus }}</text>
        <div v-for="rp in replies" :key="'rp' + rp.rpid" class="reply-block">
          <div class="reply-item" @click="toggleReply(rp)">
            <text class="reply-name">{{ rp.name }}</text>
            <text class="reply-msg">{{ rp.message }}</text>
            <text class="reply-meta">{{ replyMeta(rp) }}{{ replyOpen.root === rp.rpid ? ' · 收起' : (rp.rcount > 0 ? ' › 查看' + rp.rcount + '回复' : '') }}</text>
          </div>
          <div v-if="replyOpen.root === rp.rpid" class="reply-children">
            <text v-if="replyOpen.loading && !replyOpen.items.length" class="reply-sub-status">回复加载中…</text>
            <div v-for="ch in replyOpen.items.slice(0, replyOpen.shown)" :key="'ch' + ch.rpid" class="reply-child">
              <text class="reply-name">{{ ch.name }}</text>
              <text class="reply-msg">{{ ch.message }}</text>
            </div>
            <div v-if="!replyOpen.loading && (replyOpen.shown < replyOpen.items.length || !replyOpen.noMore)" class="reply-more" @click="moreChildren">
              <text class="reply-more-text">更多回复</text>
            </div>
            <text v-if="!replyOpen.items.length && !replyOpen.loading" class="reply-sub-status">暂无回复</text>
          </div>
        </div>
        <div v-if="replies.length && !replyNoMore" class="reply-more" @click="loadMoreReplies">
          <text class="reply-more-text">{{ replyLoading ? '加载中…' : '加载更多评论' }}</text>
        </div>
        <text v-if="!replies.length && !replyLoading && !replyStatus" class="reply-status">暂无评论</text>
        <text v-if="!replies.length && replyLoading" class="reply-status">评论加载中…</text>
      </scroller>
    </div>

    <!-- 播放内容：中列视频洞 + 右列信息（评论面板打开时两列收起） -->
    <div v-if="mode === 'play' && !commentsOpen" class="play-col">
      <hole v-if="holeVisible" class="video-hole"></hole>
      <div v-else class="play-idle">
        <text class="play-idle-text">{{ idleText }}</text>
      </div>
    </div>
    <div v-if="mode === 'play' && !commentsOpen" class="info-col">
      <text class="info-title">{{ infoTitle }}</text>
      <text class="info-up">{{ infoUp }}</text>
      <!-- 右栏按钮：弹幕开关（默认开）+ 评论区入口（容器 flex 居中，数字变长也不偏） -->
      <div class="info-ctrl" @click="toggleDanmaku">
        <text class="info-ctrl-text">弹幕：{{ danmakuLabel }}</text>
      </div>
      <div class="info-ctrl" @click="openComments">
        <text class="info-ctrl-text">评论区{{ replyCount ? ' · ' + replyCount : '' }}</text>
      </div>
      <div class="info-spacer"></div>
      <text class="info-state">{{ play.note }}</text>
      <text class="info-hint">左侧：暂停/开始 · ±{{ seekStepSec }}s</text>
    </div>

    <!-- 弹幕已迁至 native drawtext（v1.7.0 烧进视频帧：与 blit 同源，与 UI 永不互踩） -->
  </div>
</template>

<script>
import {
  fetchRecommended,
  fetchPopular,
  searchVideos,
  appendDeduped,
  describeListError,
  formatDuration,
  formatCount
} from '../../services/feed.js';
import { createClient, cookieFromSession, DEFAULT_UA, REFERER } from '../../services/bili/client.js';
import { KEYS, getJson, setJson, loadSession, logWarn } from '../../services/storage.js';
import { loadHistory, saveHistory, pushHistory, formatHistoryTime } from '../../services/history.js';
import {
  parseGenerate,
  parsePoll,
  parseNavProfile,
  QR_STATE,
  POLL_INTERVAL_MS,
  POLL_MAX_ROUNDS,
  POLL_ACCEPT_CODES,
  TV_GEN_URL,
  TV_POLL_URL,
  tvForm
} from '../../services/bili/qrlogin.js';
import { cancelNativePosts } from '../../services/net.js';
import { fetchReplies, addReply, fetchSubReplies } from '../../services/bili/reply.js';
import { segUrl, segCount, toBytes, isDanmakuBytes, parseDanmakuSeg } from '../../services/bili/danmaku.js';
import { encodeQr, rowRuns } from '../../services/bili/qrcode.js';
import { saveSession } from '../../services/storage.js';
import { input } from '../../services/input.js';
import {
  openVideo,
  togglePlay,
  seekBy,
  readStatus,
  closeSession,
  describePlayError,
  ensureMixin,
  SEEK_STEP_MS
} from '../../services/play_session.js';
import * as player from '../../services/player.js';

const TABS = [
  { id: 'rcmd', label: '推荐' },
  { id: 'hot', label: '热门' },
  { id: 'search', label: '搜索' },
  { id: 'mine', label: '我的' }
];

// 二维码内容 = TV 变体 url（cookie 在 poll 响应 body 的 cookie_info；取证注释见 qrlogin.js）
const QR_CELL = 4;
const QR_QUIET = 4;

// grid → 交替段行（白/暗首尾 quiet 补齐；宽度全部映射 qr-l-N 静态 class，零 inline style，
// 相邻同类段自动合并；每行总宽恒 = size + 2*quiet）
function gridToRows(grid, size) {
  const quiet = QR_QUIET;
  const rows = [];
  const blankLen = size + quiet * 2;
  const blank = () => [{ dark: false, cls: 'qr-l-' + blankLen }];
  const push = (segs, dark, len) => {
    const prev = segs[segs.length - 1];
    if (prev && prev.dark === dark) {
      segs[segs.length - 1] = { dark: dark, cls: 'qr-l-' + (lenOf(prev.cls) + len) };
    } else {
      segs.push({ dark: dark, cls: 'qr-l-' + len });
    }
  };
  for (let i = 0; i < quiet; i++) rows.push(blank());
  for (let y = 0; y < size; y++) {
    const segs = [];
    push(segs, false, quiet); // leading quiet
    let x = 0;
    while (x < size) {
      const dark = !!grid[y][x];
      let len = 1;
      while (x + len < size && !!grid[y][x + len] === dark) len++;
      push(segs, dark, len);
      x += len;
    }
    push(segs, false, quiet); // trailing quiet
    rows.push(segs);
  }
  for (let i = 0; i < quiet; i++) rows.push(blank());
  return rows;
}

function lenOf(cls) {
  return parseInt(cls.slice('qr-l-'.length), 10) || 0;
}

function idlePlay() {
  return { state: 'idle', session: null, positionMs: 0, frames: 0, note: '' };
}

const STATE_LABELS = {
  idle: '',
  loading: '取流中…',
  playing: '播放中',
  paused: '已暂停',
  ended: '已播完',
  error: '出错'
};

export default {
  name: 'PageBili',
  data() {
    return {
      tabs: TABS,
      tab: 'rcmd',
      mode: 'browse',
      rcmdItems: [],
      hotItems: [],
      searchItems: [],
      hotPn: 1,
      hotNoMore: false,
      hotMorePn: 0, // 推荐耗尽后用热门续底的页码（与 hot tab 独立）
      moreNoMore: false,
      keyword: '',
      searchPage: 1,
      searchNoMore: false,
      loading: false,
      statusText: '',
      history: [],
      buvidShort: '',
      seekStepSec: SEEK_STEP_MS / 1000,
      play: idlePlay(),
      // 登录 / 扫码
      mineView: 'info',
      profile: null,
      profileStatus: '',
      qr: { state: 'idle', rows: [], size: 0, seconds: 180, message: '准备中…', key: '' },
      // 评论（评论覆盖面板：scroller 滑动 + 行内子楼）
      replies: [],
      replyPage: 1,
      replyNoMore: false,
      replyLoading: false,
      replyStatus: '',
      replyCount: 0,
      // 展开中的子楼（单实例：root=0 收起）
      replyOpen: { root: 0, items: [], shown: 0, page: 1, noMore: false, loading: false },
      // 弹幕（默认开）+ 评论面板
      /* 弹幕档位：0=关 1=1/4屏 2=1/2屏(默认) 3=全屏 —— 持久化 */
      danmakuMode: 2,
      dmList: [], /* 真实弹幕时间轴（seg.so；接口失败=空，无种子兜底） */
      dmError: '', /* 非空=接口错误，弹幕按钮显示"弹幕：接口错误"（切档时自动重试） */
      commentsOpen: false
    };
  },
  computed: {
    currentItems() {
      return this.tab === 'hot' ? this.hotItems : this.rcmdItems;
    },
    stateLabel() {
      return STATE_LABELS[this.play.state] || '';
    },
    btnText() {
      if (this.play.state === 'loading') return '…';
      return this.play.state === 'paused' ? '开始' : '暂停';
    },
    holeVisible() {
      return this.mode === 'play' && (this.play.state === 'playing' || this.play.state === 'paused');
    },
    idleText() {
      if (this.play.state === 'loading') return '取流中…';
      if (this.play.state === 'error') return '起播失败 · 返回列表';
      if (this.play.state === 'ended') return '已播完';
      return '';
    },
    infoTitle() {
      return this.play.session ? this.play.session.title : '';
    },
    infoUp() {
      return this.play.session ? this.play.session.up : '';
    },
    timeText() {
      const s = this.play.session;
      if (!s) return '';
      const cur = formatDuration(Math.floor(this.play.positionMs / 1000));
      const dur = formatDuration(Math.floor((s.durationMs || 0) / 1000));
      return dur ? cur + ' / ' + dur : cur;
    },
    // 头像已按用户决定移除（笔端 ImageLoader 对 face 路径不显示、dev机200 无法复现差异；
    // 昵称/等级仍由 profile 展示）
    moreBtnText() {
      if (this.tab === 'rcmd') return this.moreNoMore ? '没有更多了' : '加载更多';
      if (this.tab === 'search') return this.searchNoMore ? '没有更多了' : '加载更多';
      return this.hotNoMore ? '没有更多了' : '加载更多';
    },
    danmakuLabel() {
      if (this.dmError) return '接口错误';
      const labels = ['关', '1/4屏', '1/2屏', '全屏'];
      return labels[this.danmakuMode] || '关';
    },
    // 已登录判定（以 nav 验证过的档案为准：过期 cookie 等同未登录，提示文案一致）
    hasLogin() {
      return !!this.profile;
    },
    qrPrimaryLabel() {
      if (this.qr.state === 'expired' || this.qr.state === 'error') return '重新生成';
      if (this.qr.state === 'generate') return '生成中…';
      return '取消';
    }
  },
  async created() {
    this.gen = 0;
    this._timers = new Set();
    this._intervals = new Set();
    this._poll = 0;
    this.mixinMemo = {}; // WBI 密钥会话级缓存（播放与搜索共享）
    this.client = createClient({});
    // 关键顺序：先恢复登录 Cookie（loadLocal），其末尾才发起首屏加载 ——
    // 否则首屏 rcmd 会以匿名身份发出，拿到的是默认推荐而非账号个性化推荐
    await this.loadLocal();
  },
  methods: {
    /* ---------- 基础设施 ---------- */
    sleep(ms) {
      return new Promise((resolve) => {
        const t = setTimeout(() => {
          this._timers.delete(t);
          resolve();
        }, ms);
        this._timers.add(t);
      });
    },
    stopPoll() {
      if (this._poll) {
        clearInterval(this._poll);
        this._intervals.delete(this._poll);
        this._poll = 0;
      }
    },
    // 每次操作独立 ctx：generation 变化即视为过期（过期回调不得更新页面）
    makeCtx(myGen) {
      return {
        client: this.client,
        mixinMemo: this.mixinMemo,
        player: player,
        mediaUa: DEFAULT_UA,
        mediaReferer: REFERER,
        nowSec: () => Math.floor(Date.now() / 1000),
        log: logWarn,
        cancelled: () => this.gen !== myGen
      };
    },
    vmeta(it) {
      const parts = [];
      if (it.up) parts.push(it.up);
      const c = formatCount(it.view);
      if (c) parts.push(c + '播放');
      const d = formatDuration(it.durationSec);
      if (d) parts.push(d);
      return parts.join(' · ');
    },
    historyTime(at) {
      return formatHistoryTime(at, Date.now());
    },
    async loadLocal() {
      const h = await loadHistory();
      this.history = h.items;
      // 弹幕档位偏好（默认 1/2 屏；四档切换持久化，新视频/新会话沿用上次设置）
      try {
        const pref = await getJson(KEYS.settings, null);
        const m = pref && typeof pref.danmakuMode === 'number' && pref.danmakuMode >= 0 && pref.danmakuMode <= 3 ? pref.danmakuMode : 2;
        this.danmakuMode = m;
      } catch (e) {
        this.danmakuMode = 2;
      }
      const sess = await loadSession();
      this.session = sess;
      if (sess.buvid3) {
        this.buvidShort = '触点 ' + sess.buvid3.slice(0, 8) + '…' + sess.buvid3.slice(-4);
      }
      if (sess.login && sess.login.SESSDATA) {
        // 登录态恢复：Cookie 立即生效（首屏请求依赖它）；nav 验证放后台不阻塞首屏
        this.client.setCookie(cookieFromSession(sess));
        this.profileStatus = '验证中…';
        (async () => {
          const prof = parseNavProfile(await this.client.request('/x/web-interface/nav'));
          if (prof.ok) {
            this.profile = prof;
            this.profileStatus = '已验证';
            logWarn('[bili] session restored mid=' + prof.mid + ' uname=' + prof.uname);
          } else {
            this.profileStatus = prof.stage === 'unauthorized' ? '已过期，请重新扫码' : ('验证失败：' + prof.message);
            logWarn('[bili] session restore verify fail: ' + prof.stage + ' ' + prof.message);
          }
        })();
      }
      const at = await getJson(KEYS.autotest, null);
      if (at && (at === true || at.enabled === true)) {
        logWarn('[bili] autotest seeded → run');
        this.runAutotest(); // 自检内部自带首屏加载（避免与这里并发双 load 竞态）
      } else {
        await this.selectTab('rcmd'); // 首屏加载：此刻登录 Cookie 已就位 → 个性化推荐
      }
    },

    /* ---------- 列表 ---------- */
    async selectTab(id) {
      const myGen = ++this.gen;
      this.stopPoll();
      this.stopQr();
      this.tab = id;
      this.statusText = '';
      if (id === 'mine') return;
      if (id === 'search' && !this.keyword) return; // 空态显示"输入关键词"，不发请求
      const list =
        id === 'rcmd' ? this.rcmdItems : id === 'hot' ? this.hotItems : id === 'search' ? this.searchItems : [];
      if (!list.length) await this.loadTab(id, myGen);
    },
    // 终止二维码轮询并回到 info 态（tab 切换/取消/返回共用）
    stopQr() {
      this.mineView = 'info';
      if (this.qr.state !== 'ok') {
        this.qr.state = 'idle';
      }
    },
    // 单次列表拉取（供 loadTab 重试调用）
    async loadTabOnce(id) {
      try {
        if (id === 'search') {
          if (!this.keyword) return { ok: false, stage: 'param', message: '未输入关键词' };
          const mixin = await ensureMixin(this.makeCtx(this.gen));
          if (!mixin) return { ok: false, stage: 'wbi', message: 'WBI 密钥获取失败' };
          const r = await searchVideos(this.client, mixin, this.keyword, 1);
          if (r.ok) {
            this.searchPage = 1;
            this.searchNoMore = r.noMore;
          }
          return r;
        }
        return await fetchRecommended(this.client);
        if (id === 'hot') {
          const r = await fetchPopular(this.client, 1);
          if (r.ok) {
            this.hotPn = 1;
            this.hotNoMore = r.noMore;
          }
          return r;
        }
        if (id === 'rcmd') return await fetchRecommended(this.client);
        return { ok: false, stage: 'param', message: '未知列表 id: ' + id };
      } catch (e) {
        return { ok: false, stage: 'transport', message: (e && e.message) || String(e) };
      }
    },
    async loadTab(id, myGen) {
      this.loading = true;
      if (this.gen === myGen) this.statusText = '加载中…';
      let r = await this.loadTabOnce(id);
      // 网络/解析类失败自动重试一次（真机实测直播接口偶发返回非 JSON）
      if (r && r.ok === false && this.gen === myGen && r.stage !== 'param') {
        logWarn('[bili] load ' + id + ' 首次失败(' + r.stage + ')，1s 后重试: ' + r.message);
        await this.sleep(1000);
        if (this.gen !== myGen) {
          this.loading = false;
          return;
        }
        this.statusText = '重试中…';
        r = await this.loadTabOnce(id);
      }
      if (this.gen !== myGen) {
        this.loading = false;
        return; // 过期：不更新页面
      }
      this.loading = false;
      if (!r.ok) {
        this.statusText = describeListError(r);
        logWarn('[bili] load ' + id + ' fail: ' + this.statusText);
        return;
      }
      if (id === 'rcmd') this.rcmdItems = r.items;
      else if (id === 'hot') this.hotItems = r.items;
      else if (id === 'search') this.searchItems = r.items;
      this.statusText = '';
      logWarn('[bili] load ' + id + ' ok n=' + r.items.length);
    },
    async loadMoreHot() {
      const myGen = ++this.gen;
      this.loading = true;
      this.statusText = '加载更多…';
      const r = await fetchPopular(this.client, this.hotPn + 1);
      if (this.gen !== myGen) {
        this.loading = false;
        return;
      }
      this.loading = false;
      if (!r.ok) {
        this.statusText = describeListError(r);
        logWarn('[bili] load more fail: ' + this.statusText);
        return;
      }
      const merged = appendDeduped(this.hotItems, r.items);
      this.hotItems = merged.items;
      this.hotPn = this.hotPn + 1;
      this.hotNoMore = r.noMore || merged.added === 0;
      this.statusText = merged.added === 0 ? '没有更多了' : '';
      logWarn('[bili] hot page ' + this.hotPn + ' +' + merged.added + ' total=' + merged.items.length);
    },

    /* ---------- 搜索 / 刷新 / 更多 ---------- */
    async onOpenInput() {
      const myGen = this.gen;
      // 系统输入法是独立系统 miniapp，拉起会触发宿主 onHide —— 用会话守卫跳过清理
      //（skill 指引：输入法触发的 onHide 不得执行 teardown，否则 gen++ 会让确认结果被丢弃）
      this.imBusy = true;
      try {
        const r = await input.open({ value: this.keyword, placeholder: '输入视频关键词', maxlength: 30 });
        if (this.gen !== myGen) return;
        if (r && r.error) {
          this.statusText = '输入法不可用(' + r.error + ')';
          logWarn('[bili] input open fail: ' + r.error);
          return;
        }
        const kw = (r && r.confirmed && r.text ? r.text : '').trim();
        // 日志卫生：只记长度不记内容
        logWarn('[bili] input confirmed=' + !!(r && r.confirmed) + ' kwLen=' + kw.length);
        if (!kw) {
          this.statusText = r && r.confirmed ? '输入为空' : '未输入关键词';
          return;
        }
        this.keyword = kw;
        this.searchItems = [];
        this.statusText = '搜索「' + kw + '」…';
        await this.selectTab('search');
      } finally {
        this.imBusy = false;
      }
    },
    // 刷新：清缓存重拉（登录态下推荐可能轮换；匿名同参固定——见 feed.js 实测）
    async onRefreshTab() {
      const id = this.tab;
      if (id !== 'rcmd' && id !== 'hot' && id !== 'search') return;
      const myGen = ++this.gen;
      this.stopPoll();
      if (id === 'rcmd') this.rcmdItems = [];
      else if (id === 'hot') this.hotItems = [];
      else {
        // 搜索刷新：保留关键词，重置分页重拉第一页
        this.searchItems = [];
        this.searchPage = 1;
        this.searchNoMore = false;
      }
      logWarn('[bili] refresh ' + id);
      await this.loadTab(id, myGen);
    },
    // 推荐列表耗尽（rcmd 匿名不可翻页，实测）→ 用热门后续底
    async loadMoreFromHot() {
      if (this.moreNoMore) return;
      const myGen = ++this.gen;
      this.loading = true;
      this.statusText = '加载更多…';
      const r = await fetchPopular(this.client, this.hotMorePn + 1);
      if (this.gen !== myGen) {
        this.loading = false;
        return;
      }
      this.loading = false;
      if (!r.ok) {
        this.statusText = describeListError(r);
        logWarn('[bili] more(hot) fail: ' + this.statusText);
        return;
      }
      this.hotMorePn = this.hotMorePn + 1;
      const merged = appendDeduped(this.rcmdItems, r.items);
      this.rcmdItems = merged.items;
      this.moreNoMore = r.noMore || merged.added === 0;
      this.statusText = merged.added === 0 ? '没有更多了' : '';
      logWarn('[bili] rcmd+hot 续底 +' + merged.added + ' total=' + merged.items.length + ' pn=' + this.hotMorePn);
    },
    // 搜索翻页（search/type 支持 page）
    async loadMoreSearch() {
      if (this.searchNoMore || !this.keyword) return;
      const myGen = ++this.gen;
      this.loading = true;
      this.statusText = '加载更多…';
      const mixin = await ensureMixin(this.makeCtx(this.gen));
      if (this.gen !== myGen) {
        this.loading = false;
        return;
      }
      if (!mixin) {
        this.loading = false;
        this.statusText = 'WBI 密钥失败';
        return;
      }
      const r = await searchVideos(this.client, mixin, this.keyword, this.searchPage + 1);
      if (this.gen !== myGen) {
        this.loading = false;
        return;
      }
      this.loading = false;
      if (!r.ok) {
        this.statusText = describeListError(r);
        logWarn('[bili] search more fail: ' + this.statusText);
        return;
      }
      this.searchPage = r.page;
      this.searchNoMore = r.noMore;
      const merged = appendDeduped(this.searchItems, r.items);
      this.searchItems = merged.items;
      this.statusText = merged.added === 0 ? '没有更多了' : '';
      logWarn('[bili] search page ' + r.page + ' +' + merged.added + ' total=' + merged.items.length);
    },

    /* ---------- 播放 ---------- */
    async onPlayItem(item) {
      if (!item) return;
      const myGen = ++this.gen;
      this.stopPoll();
      this.mode = 'play';
      // 换视频即清空上一条的评论上下文
      this.replies = [];
      this.replyStatus = '';
      this.replyLoading = false;
      this.replyNoMore = false;
      this.replyPage = 1;
      this.replyCount = 0;
      this.commentsOpen = false;
      this.replyOpen = { root: 0, items: [], shown: 0, page: 1, noMore: false, loading: false };
      this.dmError = '';
      this.dmList = [];
      this.play = {
        state: 'loading',
        session: {
          bvid: item.bvid,
          cid: item.cid || 0,
          title: item.title,
          up: item.up || '',
          cover: item.cover || '',
          durationMs: (item.durationSec || 0) * 1000,
          qn: 0
        },
        positionMs: 0,
        frames: 0,
        note: ''
      };
      const ctx = this.makeCtx(myGen);
      await closeSession(ctx); // 幂等：防止 PLAYER_ALREADY_RUNNING
      if (this.gen !== myGen) return;
      const r = await openVideo(ctx, item);
      if (this.gen !== myGen) {
        // 被更新的操作顶掉：回收已拉起的会话，不留孤儿进程
        if (r && r.ok) await closeSession(this.makeCtx(this.gen));
        return;
      }
      if (!r.ok) {
        if (r.stage === 'cancel') return;
        this.play.state = 'error';
        this.play.note = describePlayError(r);
        logWarn('[bili] open fail stage=' + r.stage + ': ' + r.message);
        return;
      }
      this.play.session = r.session;
      this.play.state = 'playing';
      this.play.note = '';
      this.history = pushHistory(
        this.history,
        {
          bvid: r.session.bvid,
          cid: r.session.cid,
          title: r.session.title,
          up: r.session.up,
          cover: r.session.cover,
          durationSec: Math.floor((r.session.durationMs || 0) / 1000),
          at: Date.now()
        },
        Date.now()
      ).items;
      saveHistory({ version: 1, items: this.history });
      this.loadReplies(r.session.aid || 0); // 评论首屏（不阻塞播放；无 aid 内部直接返回）
      // 弹幕：先用种子秒开，真实弹幕（protobuf 分包）异步到达即接管（失败保留种子）
      this.startDanmaku(r.session.durationMs || 0);
      this.loadDanmaku(r.session.aid || 0, r.session.cid || 0, r.session.durationMs || 0, myGen);
      this.startPoll(myGen);
      logWarn('[bili] play start ' + r.session.bvid + ' qn=' + r.session.qn + ' dur=' + r.session.durationMs + 'ms');
    },
    // 等待帧恢复：起播/seek 后解码重启延迟实测 1.5~2.5s+（网络敏感）→ 轮询至多 maxMs
    async waitForFrames(maxMs) {
      const t0 = Date.now();
      let st = await readStatus(this.makeCtx(this.gen));
      while (!(st.ok && st.frames > 0) && Date.now() - t0 < maxMs && this.play.state === 'playing') {
        await this.sleep(500);
        if (this.play.state !== 'playing') break;
        st = await readStatus(this.makeCtx(this.gen));
      }
      return { st: st, ms: Date.now() - t0 };
    },
    startPoll(myGen) {
      this.stopPoll();
      this._pollLogged = false;
      const tick = async () => {
        if (this.gen !== myGen) {
          this.stopPoll();
          return;
        }
        const st = await readStatus(this.makeCtx(myGen));
        if (this.gen !== myGen) {
          this.stopPoll();
          return;
        }
        if (!st.ok) {
          this.play.state = 'error';
          this.play.note = st.message;
          this.stopPoll();
          logWarn('[bili] poll error: ' + st.message);
          return;
        }
        // 每秒采样（v1.3.0 音频重写证据链）：ab/ad/u(欠载)w(写失败)rd(环满丢弃)
        logWarn(
          '[bili] poll tick frames=' + st.frames + ' pos=' + st.positionMs +
          ' ab=' + st.audioBytes + ' ad=' + st.audioDropped +
          ' u=' + st.audioUnderruns + ' w=' + st.audioWrErrors + ' rd=' + st.audioRingDrops +
          (st.audioDead ? ' DEAD' : '')
        );
        // 音频链已断（实测主因：蓝牙被其他应用独占——网易云 SoundPlayer 正在用 bluealsa）
        if (st.audioDead && !this._audioDeadWarned) {
          this._audioDeadWarned = true;
          this.play.note = '无声：蓝牙耳机可能被其他应用占用（请关闭音乐播放器后重试）';
          logWarn('[bili] audio chain dead → 蓝牙设备可能被其他应用占用');
        }
        // 起播门诊断（v1.7.1）：音频等待视频首帧的时长（正常=首帧耗时；15000=超时放行）
        if (st.gateWaitMs > 400 && st.gateWaitMs !== this._lastGateMs) {
          this._lastGateMs = st.gateWaitMs;
          logWarn('[bili] A/V gate wait=' + st.gateWaitMs + 'ms（音频等视频首帧后同步开播）');
        }
        // A/V 一致性巡检（v1.7.1）：视频 8s 无新帧而音频仍在写 → seek(pos+1) 重启对齐。
        // 重开必经起播门 → 音画成对重启；门期(音频未开写, audioBytes 不变)天然不触发；
        // 评论面板遮挡(commentsOpen)是用户主动暂停画面，跳过；45s 冷却防弱网打转。
        if (
          st.state === 'playing' &&
          !st.gateActive &&
          st.videoStallMs > 8000 &&
          st.audioBytes > 0 &&
          st.audioBytes !== (this._lastAvAudioBytes || 0) &&
          !this.commentsOpen
        ) {
          const now = Date.now();
          if (!this._avRecoverAt || now - this._avRecoverAt > 45000) {
            this._avRecoverAt = now;
            logWarn('[bili] A/V stall: video ' + st.videoStallMs + 'ms 无新帧但音频在播 → seek 恢复 pos=' + st.positionMs);
            this.play.note = '音画不同步，正在恢复…';
            this.onSeek(0.001) /* = seek(pos+1)：重启双进程并重新过起播门 */
              .catch(function (e) {
                logWarn('[bili] A/V recover seek: ' + (e && e.message));
              });
          }
        }
        this._lastAvAudioBytes = st.audioBytes;
        // 起播门期提示（首帧/音频都未到 → 缓冲中；不覆盖恢复/无声等后续提示）
        if (st.state === 'playing' && st.frames === 0 && st.positionMs === 0 && !st.audioBytes) {
          this.play.note = '缓冲中…';
        }
        this.play.positionMs = st.positionMs;
        this.play.frames = st.frames;
        if (st.state === 'ended') {
          this.play.state = 'ended';
          this.stopPoll();
          logWarn('[bili] video ended frames=' + st.frames);
        } else if (st.state === 'paused') {
          this.play.state = 'paused';
        } else if (st.state === 'playing') {
          this.play.state = 'playing';
        }
      };
      this._poll = setInterval(() => {
        tick();
      }, 1000);
      this._intervals.add(this._poll);
    },
    async onTogglePlay() {
      const st = this.play.state;
      if (st !== 'playing' && st !== 'paused') {
        this.play.note = '当前不可控制（' + (STATE_LABELS[st] || st) + '）';
        return;
      }
      const myGen = this.gen;
      const wasPlaying = st === 'playing';
      const r = await togglePlay(this.makeCtx(myGen), wasPlaying);
      if (this.gen !== myGen) return;
      if (!r.ok) {
        this.play.note = r.message;
        logWarn('[bili] toggle fail: ' + r.message);
        return;
      }
      if (r.state === 'playing' || r.state === 'paused') this.play.state = r.state;
      else this.play.state = wasPlaying ? 'paused' : 'playing';
      this.play.note = '';
      logWarn('[bili] toggle → ' + this.play.state);
    },
    async onSeek(deltaSec) {
      const s = this.play.session;
      const st = this.play.state;
      if (st !== 'playing' && st !== 'paused') {
        this.play.note = '尚未起播';
        return;
      }
      if (!s || !(s.durationMs > 0)) {
        this.play.note = '时长未知，无法定位';
        return;
      }
      const myGen = this.gen;
      this.play.note = '定位中…';
      const r = await seekBy(this.makeCtx(myGen), this.play.positionMs, s.durationMs, deltaSec * 1000);
      if (this.gen !== myGen) return;
      if (!r.ok) {
        this.play.note = r.message;
        logWarn('[bili] seek fail: ' + r.message);
        return;
      }
      this.play.positionMs = r.positionMs;
      this.play.note = '';
      logWarn('[bili] seek ' + (deltaSec > 0 ? '+' : '') + deltaSec + 's → pos=' + r.positionMs + 'ms');
      /* seek 后重启弹幕引擎（泳道基线对齐新位置） */
      this.startDanmaku(this.play.session ? this.play.session.durationMs : 0);
    },
    /* ---------- 真实弹幕（protobuf seg.so；collect 契约：**oid=cid**、6min/包、半匿名） ----------
     * 逐包异步拉取+解码（大包让帧不冻结 UI）；失败/二进制被破坏 → 保留种子弹幕不空屏。
     * 成功 → 替换 dmList 并重启引擎（泳道基线按真实时间轴重排）。 */
    async loadDanmaku(aid, cid, durationMs, myGen) {
      if (!(aid > 0) || !(cid > 0)) return;
      const total = segCount(durationMs);
      const cap = Math.min(total, 20); /* 最多20包=2h 稿件 */
      const all = [];
      for (let i = 1; i <= cap; i++) {
        if (this.gen !== myGen) return;
        const res = await this.client.getBinary(segUrl(cid, aid, i));
        if (this.gen !== myGen) return;
        if (!res.ok) {
          if (i === 1) logWarn('[bili] danmaku fetch fail: ' + res.stage + ' ' + res.message);
          this.dmError = '接口错误';
          break;
        }
        const bytes = toBytes(res.body);
        if (!isDanmakuBytes(bytes)) {
          logWarn('[bili] danmaku not-protobuf: len=' + (bytes ? bytes.length : 0) + ' seg=' + i);
          this.dmError = '接口错误';
          break; /* 非 proto → 按钮显示接口错误（取证史已归档 profile） */
        }
        const parsed = parseDanmakuSeg(bytes);
        if (!parsed.ok) {
          logWarn('[bili] danmaku decode fail: ' + parsed.reason + ' (keep prev ' + all.length + ')');
          this.dmError = '接口错误';
          break;
        }
        if (!parsed.items.length) logWarn('[bili] danmaku seg ' + i + ' empty → next'); /* 空段自动续下一段 */
        all.push.apply(all, parsed.items);
        if (all.length > 30000) break; /* 内存护栏：3 万条足够全片采样 */
        await new Promise((resolve) => setTimeout(resolve, 0));
      }
      if (this.gen !== myGen) return;
      if (all.length) {
        all.sort((x, y) => x.p - y.p);
        /* 服务端分包边界可能重叠 → 按 p+text 去重（同毫秒同文本=同一条） */
        const seen = {};
        const dedup = [];
        for (const it of all) {
          const key = it.p + '\u0001' + it.text;
          if (seen[key]) continue;
          seen[key] = 1;
          dedup.push(it);
        }
        this.dmList = dedup;
        this.dmError = '';
        logWarn('[bili] danmaku real n=' + dedup.length + '/' + all.length + ' pkgs=' + cap + ' span=' + (dedup[0] ? dedup[0].p : 0) + '..' + (dedup[dedup.length - 1] ? dedup[dedup.length - 1].p : 0));
        this.startDanmaku(durationMs); /* 重启引擎：游标按真实时间轴重排 */
      } else {
        if (!this.dmError) this.dmError = '接口错误'; /* 全部段为空 → 按钮显示错误 */
        logWarn('[bili] danmaku empty → dmError shown on button');
      }
    },

    /* ---------- 弹幕引擎（native drawtext 4泳道：writeDm 换文本，reload=1 即时生效） ----------
     * UI 叠层方案已判死：视频矩形归 blit 专属，任何覆盖层都与其交替抢帧（一帧弹幕一帧画面）。
     * 烧进视频帧 = 与 blit 同源 → 永不互踩。x 表达式固定周期滚动，JS 按 positionMs 把
     * 弹幕分派进 4 条泳道（展示期 4.5s 后该泳道可接下一条）。 */
    /* 泳道分派基线：统一游标定位到当前播放位（不回退已播；seek 后对齐新位置） */
    resetDmBaseline() {
      this._dmCursor = this.findDmIndex(this.dmList, Math.max(0, this.play.positionMs || 0));
      this._dmFree = [0, 0, 0, 0];
      this._dmExpire = [0, 0, 0, 0];
      this._dmTextAt = {}; /* 文案冷却表（按播放位时间戳） */
    },
    findDmIndex(list, p) {
      let i = 0;
      while (i < list.length && list[i].p < p) i++;
      return i;
    },
    stopDanmaku() {
      if (this._dmTimer) {
        clearInterval(this._dmTimer);
        this._intervals.delete(this._dmTimer);
        this._dmTimer = 0;
      }
      /* 关/换台清空 4 条泳道（空文本=drawtext 不再绘制） */
      for (let i = 0; i < 4; i++) {
        player.writeDm(i, '').catch(function () {});
      }
    },
    startDanmaku(durationMs) {
      this.stopDanmaku(); /* 清 4 泳道旧文本（换档/换台都先清干净） */
      if (this.danmakuMode === 0) {
        logWarn('[bili] danmaku off');
        return;
      }
      if (!this.dmList.length) {
        logWarn('[bili] danmaku engine idle (no data' + (this.dmError ? ', error on button' : '') + ')');
        return; /* 接口失败/无弹幕：不启 timer；错误态由弹幕按钮展示 */
      }
      this.resetDmBaseline();
      /* 四泳道滚动速度（px/s）——必须与 native vf 的 mod(t*S,…) 一一对应，改这里须同步 player.c */
      const SPD = [110, 142, 90, 166];
      const t = setInterval(() => {
        if (this.mode !== 'play') {
          this.stopDanmaku();
          return;
        }
        const pos = this.play.positionMs;
        const now = Date.now();
        /* 档位→活跃泳道（y：lane0=8 顶 / lane1=72 / lane2=136 / lane3=200 底）
         * 区域从顶部算（用户指定）：1/4屏=lane0 顶部单条 · 1/2屏=lane0+1 上半双条 · 全屏=四条 */
        const active = this.danmakuMode === 3 ? [0, 1, 2, 3] : this.danmakuMode === 2 ? [0, 1] : [0];
        for (let k = 0; k < active.length; k++) {
          const l = active[k];
          /* 单程滚完 → 清空泳道：native x=w-mod(t*S,…) 是**周期循环**（短文本一轮仅 3~5s），
           * 不清理则同一条滚过去又被 mod 从右边放回第二遍（"还是有点重复"的真凶） */
          if (this._dmExpire[l] && now >= this._dmExpire[l]) {
            player.writeDm(l, '').catch(function () {});
            this._dmExpire[l] = 0;
          }
          if (now < this._dmFree[l]) continue; /* 该泳道仍在展示期 */
          /* 统一分派游标：全泳道共享同一消费序列，消费即前进 → 同一条永不再进第二个泳道 */
          let cursor = this._dmCursor || 0;
          while (cursor < this.dmList.length && this.dmList[cursor].p < pos - 800) cursor++; /* 过期跳过 */
          this._dmCursor = cursor;
          if (cursor >= this.dmList.length) break;
          const dm = this.dmList[cursor];
          if (dm.p > pos + 300) break; /* 未到点：后续条目更晚（250ms 轮询粒度） */
          /* 同文案 15s 冷却：真实弹幕同文本短时密集（"哈哈哈"等）观感即重复 → 跳过 */
          const lastAt = this._dmTextAt[dm.text];
          if (lastAt !== undefined && pos - lastAt < 15000) {
            this._dmCursor = cursor + 1;
            continue;
          }
          player.writeDm(l, dm.text).catch(function (e) {
            logWarn('[bili] writeDm: ' + (e && e.message));
          });
          if (Object.keys(this._dmTextAt).length > 400) this._dmTextAt = {}; /* 冷却表内存护栏 */
          this._dmTextAt[dm.text] = pos;
          this._dmCursor = cursor + 1;
          /* 展示期 = 单程滚完（17px/字符估宽；偏宽多留无害——出屏后不可见） */
          const periodMs = Math.ceil(((452 + 17 * dm.text.length) / SPD[l]) * 1000);
          this._dmFree[l] = now + periodMs;
          this._dmExpire[l] = now + periodMs;
        }
      }, 250);
      this._dmTimer = t;
      this._intervals.add(t);
      logWarn('[bili] danmaku engine start mode=' + this.danmakuMode + ' lanes=' + (this.dmList ? this.dmList.length : 0) + ' dur=' + durationMs);
    },
    toggleDanmaku() {
      this.danmakuMode = (this.danmakuMode + 1) % 4; /* 关→1/4→1/2→全屏→关 */
      this.saveDanmakuPref();
      logWarn('[bili] danmaku mode -> ' + this.danmakuLabel);
      if (this.mode === 'play') {
        /* 错误态：切档同时自动重试接口（成功后 loadDanmaku 内部重启引擎接管） */
        if (this.dmError && this.play.session && this.play.session.aid > 0) {
          this.dmError = '';
          const s = this.play.session;
          this.loadDanmaku(s.aid || 0, s.cid || 0, s.durationMs || 0, this.gen);
        }
        this.startDanmaku(this.play.session ? this.play.session.durationMs : 0);
      }
    },
    async saveDanmakuPref() {
      try {
        const s = (await getJson(KEYS.settings, null)) || {};
        s.version = s.version || 1;
        s.danmakuMode = this.danmakuMode;
        await setJson(KEYS.settings, s);
      } catch (e) {
        logWarn('[bili] save danmaku pref: ' + (e && e.message));
      }
    },
    /* 评论面板显示权交接：打开=暂停 fb 输出（解码/音频照常）→ UI 独占无闪；关闭恢复 */
    openComments() {
      this.commentsOpen = true;
      player.pauseRender().catch(function (e) {
        logWarn('[bili] pauseRender: ' + (e && e.message));
      });
      logWarn('[bili] comments open → render paused');
    },
    closeComments() {
      if (!this.commentsOpen) return;
      this.commentsOpen = false;
      player.resumeRender().catch(function (e) {
        logWarn('[bili] resumeRender: ' + (e && e.message));
      });
      logWarn('[bili] comments close → render resumed');
    },

    /* ---------- 评论（覆盖面板 scroller 滑动 + 行内子楼） ---------- */
    replyMeta(rp) {
      const parts = [];
      if (rp.likes > 0) parts.push('▲ ' + rp.likes);
      if (rp.rcount > 0) parts.push(rp.rcount + ' 条回复');
      return parts.join(' · ');
    },
    async loadReplies(aid) {
      if (!(aid > 0)) return;
      const myGen = this.gen;
      this.replyLoading = true;
      this.replyStatus = '';
      this.replies = [];
      this.replyPage = 1;
      this.replyNoMore = false;
      this.replyOpen = { root: 0, items: [], shown: 0, page: 1, noMore: false, loading: false };
      const r = await fetchReplies(this.client, aid, 1);
      if (this.gen !== myGen) return;
      this.replyLoading = false;
      if (!r.ok) {
        this.replyStatus = '评论加载失败（' + (r.message || r.stage) + '）';
        logWarn('[bili] replies fail: ' + r.stage + ' ' + r.message);
        return;
      }
      this.replies = r.items;
      this.replyNoMore = r.noMore;
      this.replyCount = r.count;
      logWarn('[bili] replies load n=' + r.items.length + ' count=' + r.count);
    },
    async loadMoreReplies() {
      if (this.replyLoading || this.replyNoMore) return;
      const aid = this.play.session && this.play.session.aid;
      if (!(aid > 0)) return;
      const myGen = this.gen;
      this.replyLoading = true;
      const r = await fetchReplies(this.client, aid, this.replyPage + 1);
      if (this.gen !== myGen) return;
      this.replyLoading = false;
      if (!r.ok) {
        this.replyStatus = '加载失败（' + (r.message || '') + '）';
        return;
      }
      this.replyPage = r.pn;
      this.replyNoMore = r.noMore;
      const seen = {};
      this.replies.forEach((x) => (seen[x.rpid] = 1));
      const add = r.items.filter((x) => !seen[x.rpid]);
      this.replies = this.replies.concat(add);
      if (add.length === 0) this.replyNoMore = true;
      logWarn('[bili] replies more +' + add.length + ' total=' + this.replies.length);
    },
    async onWriteReply() {
      const sess = await loadSession();
      const csrf = (sess.login && sess.login.bili_jct) || '';
      if (!csrf) {
        this.replyStatus = '请先在「我的」扫码登录后再评论';
        return;
      }
      const aid = this.play.session && this.play.session.aid;
      if (!(aid > 0)) {
        this.replyStatus = '当前稿件无评论上下文';
        return;
      }
      const myGen = this.gen;
      // 系统输入法会触发宿主 onHide —— 同 onOpenInput 用 imBusy 守卫，防 teardown 作废本次输入
      this.imBusy = true;
      try {
        this.replyStatus = '输入评论…';
        const inp = await input.open({ value: '', placeholder: '友善评论，说点什么…', maxlength: 1000 });
        if (this.gen !== myGen) return;
        if (inp && inp.error) {
          this.replyStatus = '输入法不可用(' + inp.error + ')';
          return;
        }
        const text = (inp && inp.confirmed && inp.text ? inp.text : '').trim();
        if (!text) {
          this.replyStatus = '';
          return;
        }
        this.replyStatus = '发送中…';
        const r = await addReply(this.client, aid, text, csrf);
        if (this.gen !== myGen) return;
        if (!r.ok) {
          this.replyStatus = '发送失败：' + (r.message || '');
          logWarn('[bili] reply add fail stage=' + r.stage + ' code=' + r.code + ' ' + r.message);
          return;
        }
        this.replyStatus = '已发布';
        // 头插本地（服务端审核有延迟，先让用户看到自己那条）
        this.replies.unshift({
          rpid: r.rpid,
          name: (sess.login && sess.login.uname) || '我',
          message: text,
          likes: 0,
          ctime: Math.floor(Date.now() / 1000),
          rcount: 0
        });
        logWarn('[bili] reply add ok rpid=' + r.rpid + ' len=' + text.length);
      } finally {
        this.imBusy = false;
      }
    },

    /* ---------- 子楼（行内展开：点评论卡在其下方显示回复，scroller 滑动浏览） ---------- */
    async toggleReply(rp) {
      if (this.replyOpen.root === rp.rpid) {
        // 再点同一条 = 收起
        this.replyOpen = { root: 0, items: [], shown: 0, page: 1, noMore: false, loading: false };
        return;
      }
      const aid = this.play.session && this.play.session.aid;
      if (!(aid > 0)) return;
      this.replyOpen = { root: rp.rpid, items: [], shown: 0, page: 1, noMore: false, loading: true };
      const myGen = this.gen;
      const myRoot = rp.rpid;
      const r = await fetchSubReplies(this.client, aid, myRoot, 1);
      if (this.gen !== myGen) return;
      if (this.replyOpen.root !== myRoot) return; // 展开目标已切换，丢弃过期结果
      this.replyOpen.loading = false;
      if (!r.ok) {
        this.replyOpen.items = [];
        this.replyOpen.noMore = true;
        logWarn('[bili] sub replies fail: ' + r.stage + ' ' + r.message);
        return;
      }
      this.replyOpen.items = r.items;
      this.replyOpen.shown = Math.min(4, r.items.length); /* 首屏只显示 4 条，其余点"更多回复"每次 +4 */
      this.replyOpen.noMore = r.noMore;
      logWarn('[bili] sub replies root=' + myRoot + ' n=' + r.items.length + '/' + r.count);
    },
    async loadMoreChildren() {
      if (!this.replyOpen.root || this.replyOpen.loading || this.replyOpen.noMore) return;
      const aid = this.play.session && this.play.session.aid;
      if (!(aid > 0)) return;
      const myGen = this.gen;
      const myRoot = this.replyOpen.root;
      this.replyOpen.loading = true;
      const r = await fetchSubReplies(this.client, aid, myRoot, this.replyOpen.page + 1);
      if (this.gen !== myGen || this.replyOpen.root !== myRoot) return;
      this.replyOpen.loading = false;
      if (!r.ok) return;
      this.replyOpen.page = r.pn;
      this.replyOpen.noMore = r.noMore;
      const seen = {};
      this.replyOpen.items.forEach((x) => (seen[x.rpid] = 1));
      const add = r.items.filter((x) => !seen[x.rpid]);
      this.replyOpen.items = this.replyOpen.items.concat(add);
      if (!add.length) this.replyOpen.noMore = true;
      this.replyOpen.shown = Math.min(this.replyOpen.shown + 4, this.replyOpen.items.length);
      logWarn('[bili] sub replies more +' + add.length + ' total=' + this.replyOpen.items.length);
    },
    /* 子回复"更多"：本地已拉数据内每次 +4（纯前端无网络）；本地耗尽才走网络分页（分页后同样 +4） */
    moreChildren() {
      if (!this.replyOpen.root || this.replyOpen.loading) return;
      if (this.replyOpen.shown < this.replyOpen.items.length) {
        this.replyOpen.shown = Math.min(this.replyOpen.shown + 4, this.replyOpen.items.length);
        return;
      }
      this.loadMoreChildren();
    },

    async onBack() {
      const myGen = ++this.gen;
      this.stopPoll();
      this.stopDanmaku();
      this.closeComments();
      await closeSession(this.makeCtx(myGen));
      this.mode = 'browse';
      this.play = idlePlay();
      logWarn('[bili] back to browse');
    },
    /* ---------- 扫码登录 ---------- */
    async onGoLogin() {
      const myGen = ++this.gen;
      this.stopPoll();
      this.mineView = 'qr';
      this.qr = { state: 'generate', rows: [], size: 0, seconds: 180, message: '正在生成二维码…', key: '' };
      await this.startQrLogin(myGen);
    },
    async startQrLogin(myGen) {
      const g = parseGenerate(await this.client.postForm(TV_GEN_URL, tvForm(), { acceptCodes: [0] }));
      if (myGen !== this.gen) return;
      if (!g.ok) {
        this.qr.state = 'error';
        this.qr.message = '生成失败：' + g.message;
        logWarn('[bili] qr generate fail: ' + g.message);
        return;
      }
      const enc = encodeQr(g.url, {});
      if (!enc.ok || enc.size > 49) {
        // cell=4px 时 49x49(+quiet)=228px 正好放满 254 高；更大版本需另调 cell
        this.qr.state = 'error';
        this.qr.message = '二维码超尺寸 size=' + (enc.size || 0);
        logWarn('[bili] qr encode bad size=' + (enc.size || 0) + ' reason=' + (enc.reason || ''));
        return;
      }
      this.qr.key = g.authCode; // parseGenerate 返回字段是 authCode（g.key 为 undefined 会让 poll 提交无效码 → 86038）
      this.qr.rows = gridToRows(enc.grid, enc.size);
      this.qr.size = enc.size;
      this.qr.state = 'waiting';
      this.qr.message = '请用手机B站扫码';
      // 日志卫生：只记尺寸，不记 url/auth_code（二维码内容）
      logWarn('[bili] qr generate ok size=' + enc.size);
      await this.pollLoop(g.authCode, myGen);
    },
    async pollLoop(key, myGen) {
      for (let i = 0; i < POLL_MAX_ROUNDS; i++) {
        if (myGen !== this.gen) return;
        const res = await this.client.postForm(TV_POLL_URL, tvForm({ auth_code: key }), {
          acceptCodes: POLL_ACCEPT_CODES
        });
        if (myGen !== this.gen) return;
        const r = parsePoll(res);
        if (r.state === QR_STATE.WAITING) {
          this.qr.state = 'waiting';
          this.qr.seconds = Math.max(0, 180 - Math.round(((i + 1) * POLL_INTERVAL_MS) / 1000));
          this.qr.message = '请用手机B站扫码';
        } else if (r.state === QR_STATE.SCANNED) {
          this.qr.state = 'scanned';
          this.qr.message = '已扫码，请在手机上确认';
          logWarn('[bili] qr scanned → 等待确认');
        } else if (r.state === QR_STATE.EXPIRED) {
          this.qr.state = 'expired';
          this.qr.message = '二维码已过期，请重新生成';
          logWarn('[bili] qr expired');
          return;
        } else if (r.state === QR_STATE.OK) {
          await this.onLoginSuccess(r, myGen);
          return;
        } else {
          this.qr.state = 'error';
          this.qr.message = r.message || '轮询失败';
          logWarn('[bili] qr poll error: ' + r.message);
          return;
        }
        await this.sleep(POLL_INTERVAL_MS);
      }
      this.qr.state = 'expired';
      this.qr.message = '超时，请重新生成';
      logWarn('[bili] qr poll timeout');
    },
    // 登录成功：cookie 持久化（session v2）→ 带新 cookie 验证 nav → 展示档案
    async onLoginSuccess(r, myGen) {
      this.qr.state = 'ok';
      this.qr.message = '登录成功！';
      const sess = await loadSession();
      if (myGen !== this.gen) return;
      const merged = Object.assign({}, sess, {
        version: 2,
        login: Object.assign({}, r.cookies, {
          loggedAt: Date.now()
        })
      });
      await saveSession(merged); // 归一化落盘（用户要求：保存登录信息）
      this.client.setCookie(cookieFromSession(await loadSession()));
      // Cookie 已变更 → 立即作废登录前的匿名推荐缓存（放在 nav 验证之前：
      // 后续 gen 变化跳过日志/档案时，缓存失效也不受影响）
      this.rcmdItems = [];
      logWarn('[bili] rcmd cache invalidated (login state changed)');
      const prof = parseNavProfile(await this.client.request('/x/web-interface/nav'));
      if (myGen !== this.gen) return;
      if (prof.ok) {
        const withProfile = await loadSession();
        withProfile.login = Object.assign({}, withProfile.login, {
          mid: prof.mid,
          uname: prof.uname,
          level: prof.level,
          face: prof.face
        });
        await saveSession(withProfile);
        this.profile = prof;
        this.profileStatus = '已验证';
        // 日志只记公开档案（mid/uname），绝不记 cookie
        logWarn('[bili] login ok mid=' + prof.mid + ' uname=' + prof.uname + ' lv=' + prof.level);
      } else {
        this.profile = null;
        this.profileStatus = 'cookie 已保存，验证失败：' + prof.message;
        logWarn('[bili] login saved but verify fail: ' + prof.stage + ' ' + prof.message);
      }
      await this.sleep(1200);
      if (myGen !== this.gen) return;
      this.mineView = 'info';
    },
    onCancelQr() {
      this.gen++; // 轮询循环 guard 退出
      cancelNativePosts(); // 丢弃在途 poll（幂等）
      this.mineView = 'info';
      this.qr.state = 'idle';
      logWarn('[bili] qr cancelled');
    },
    // 扫码页单一主按钮分发：等待中=取消回列表；过期/出错=真·重新生成（原"重新生成"按钮行为是退出，名不副实）
    onQrPrimary() {
      if (this.qr.state === 'expired' || this.qr.state === 'error') {
        logWarn('[bili] qr regenerate');
        this.onGoLogin();
        return;
      }
      this.onCancelQr();
    },
    async onLogout() {
      const sess = await loadSession();
      sess.login = null;
      await saveSession(sess);
      this.client.setCookie(cookieFromSession(sess));
      this.profile = null;
      this.profileStatus = '';
      // 退出后推荐应回到匿名形态 → 同样作废缓存
      this.rcmdItems = [];
      logWarn('[bili] logout; rcmd cache invalidated');
    },
    async onRefreshProfile() {
      const prof = parseNavProfile(await this.client.request('/x/web-interface/nav'));
      if (prof.ok) {
        this.profile = prof;
        this.profileStatus = '已验证';
        logWarn('[bili] profile refresh ok lv=' + prof.level);
      } else {
        this.profileStatus =
          prof.stage === 'unauthorized' ? '已过期，请重新扫码' : ('失败：' + prof.message);
        if (prof.stage === 'unauthorized') this.profile = this.profile; // 保留展示，由用户决定退出
        logWarn('[bili] profile refresh fail: ' + prof.stage);
      }
    },

    /* ---------- 生命周期：后台/卸载与手动返回共用同一停止路径 ---------- */
    teardown(note) {
      this.gen++;
      this.stopPoll();
      this._timers.forEach((t) => clearTimeout(t));
      this._timers.clear();
      cancelNativePosts(); // 丢弃在途 form POST（幂等）
      input.dispose(); // 退订系统输入法回调（幂等）
      if (this.mode === 'play') {
        closeSession(this.makeCtx(this.gen)).then(
          () => {},
          () => {}
        );
        this.mode = 'browse';
        this.play = idlePlay();
      }
      if (note) this.statusText = note;
    },
    onShow() {
      // 回前台不自动联网（列表/播放状态原样保留）
    },
    onHide() {
      // 系统输入法拉起/关闭会触发 onHide —— 输入会话中不做任何清理
      //（否则 gen++ 会让输入确认结果被丢弃：表现为"返回后显示已停止且无内容"）
      if (this.imBusy) {
        logWarn('[bili] onHide during IM session → skip teardown');
        return;
      }
      this.teardown('已停止（切后台）');
    },
    onUnload() {
      this.teardown('');
    },

    /* ---------- 真机自检（storage 预置 bili_autotest 触发；
         全部复用按钮同一代码路径 —— 设备无触摸注入的替代取证） ---------- */
    async runAutotest() {
      const ensure = (cond, msg) => {
        if (!cond) throw new Error(msg);
      };
      logWarn('[bili] AUTOTEST start');
      try {
        await this.selectTab('rcmd');
        ensure(this.rcmdItems.length > 0, '推荐列表为空');
        await this.onPlayItem(this.rcmdItems[0]);
        ensure(this.play.state === 'playing', '起播失败: ' + this.play.note);
        // 首帧等待：DASH 双输入并发探测在窄带宽下更慢 → 窗口 12s（网歴1.5~8s）
        const w1 = await this.waitForFrames(12000);
        ensure(w1.st.ok && w1.st.frames > 0, '12s 内无帧输出: ' + (w1.st.message || w1.st.state));
        logWarn('[bili] AUTOTEST first frame ' + w1.ms + 'ms frames=' + w1.st.frames + ' pos=' + w1.st.positionMs);

        await this.onTogglePlay();
        ensure(this.play.state === 'paused', '暂停失败: ' + this.play.note);
        await this.sleep(800);
        await this.onTogglePlay();
        ensure(this.play.state === 'playing', '继续失败: ' + this.play.note);

        await this.onSeek(this.seekStepSec);
        await this.sleep(1200);
        ensure(this.play.positionMs >= 15000, 'seek+' + this.seekStepSec + 's 位置异常: ' + this.play.positionMs);
        await this.onSeek(-this.seekStepSec);
        // seek 返回即目标位置（native 直给）→ 立即断位置；再等帧恢复（seek=重启解码）
        ensure(this.play.positionMs < 6000, 'seek-' + this.seekStepSec + 's 位置异常: ' + this.play.positionMs);
        const w2 = await this.waitForFrames(12000);
        ensure(w2.st.ok && w2.st.frames > 0, 'seek 后 12s 帧未恢复: frames=' + (w2.st && w2.st.frames));
        const frames2 = w2.st.frames;
        logWarn('[bili] AUTOTEST seek recover ' + w2.ms + 'ms frames=' + frames2);

        await this.onBack();
        ensure(this.mode === 'browse', '返回列表失败');

        await this.selectTab('hot');
        ensure(this.hotItems.length > 0, '热门列表为空');
        await this.loadMoreHot();
        ensure(this.hotItems.length >= 13, '热门翻页无效: ' + this.hotItems.length);

        // 推荐"加载更多"= 热门续底（rcmd 匿名不可翻页）
        await this.selectTab('rcmd');
        const beforeMore = this.rcmdItems.length;
        await this.loadMoreFromHot();
        ensure(this.rcmdItems.length > beforeMore, '推荐续底无效: ' + beforeMore + '→' + this.rcmdItems.length);

        // 搜索：自检直连服务（输入框需人工；同一 searchVideos 代码路径）
        this.keyword = '测试';
        await this.selectTab('search');
        ensure(this.searchItems.length > 0, '搜索结果为空: ' + this.statusText);
        logWarn('[bili] AUTOTEST search n=' + this.searchItems.length);

        await this.selectTab('mine');
        ensure(this.history.length >= 1, '播放历史未记录');

        // 扫码入口自检：generate 成功 + 首轮 poll=waiting（人工扫码不在自动化范围）
        // 注意：onGoLogin 内的 pollLoop 是**持续循环**，不能 await（会阻塞到登录完成）；
        // 这里只等首轮状态落地（generate → waiting/error），随后取消。
        this.onGoLogin();
        const qrDeadline = Date.now() + 10000;
        while (this.qr.state === 'generate' && Date.now() < qrDeadline) await this.sleep(200);
        ensure(this.qr.rows.length > 0, '二维码未渲染: ' + this.qr.message);
        ensure(this.qr.size >= 21 && this.qr.size <= 49, '二维码尺寸异常: ' + this.qr.size);
        ensure(
          this.qr.state === 'waiting' || this.qr.state === 'scanned',
          '首轮轮询异常 state=' + this.qr.state + ' ' + this.qr.message
        );
        logWarn('[bili] AUTOTEST qr size=' + this.qr.size + ' state=' + this.qr.state);
        this.onCancelQr();
        ensure(this.mineView === 'info', '取消二维码未返回 info');

        // 收尾回到推荐页（截图取证的终态）
        await this.selectTab('rcmd');
        ensure(this.rcmdItems.length > 0, '收尾回推荐失败');

        logWarn(
          '[bili] AUTOTEST PASS frames=' + frames2 +
          ' hot=' + this.hotItems.length +
          ' search=' + this.searchItems.length +
          ' hist=' + this.history.length +
          ' qr=ok'
        );
      } catch (e) {
        logWarn('[bili] AUTOTEST FAIL: ' + ((e && e.message) || e));
        this.gen++; // 停掉在途操作
        this.stopPoll();
        if (this.mode === 'play') {
          // 失败不孤儿化：若已进播放页，走同一条返回路径停掉播放
          try {
            await this.onBack();
          } catch (e2) {
            logWarn('[bili] autotest 失败收尾异常: ' + (e2 && e2.message));
          }
        }
      }
    }
  }
};
</script>

<style lang="less" scoped>
/* 样式必须以 <style> 块形式存在于 .vue 内（真机证据：无 style 块 → 规则不进 bundle → 全黑）。
   形态与本工作区其它页面一致：<style lang="less" scoped> + @import。 */
@import "../../styles/base.less";
</style>
