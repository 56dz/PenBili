<template>
  <div :class="mode === 'play' ? 'page page-live' : 'page'">
    <!-- 左侧栏：浏览态=选项栏，播放态=播放控制，图文态=详情返回 -->
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
      <template v-else-if="mode === 'article'">
        <div class="ctrl" @click="onBackArticle">
          <text class="ctrl-text">返回图文列表</text>
        </div>
        <text class="bar-time">图文详情</text>
        <text class="bar-state">评论区可在右栏进入</text>
      </template>
      <template v-else>
        <div class="ctrl" @click="onBack">
          <text class="ctrl-text">返回列表</text>
        </div>
        <div class="ctrl ctrl-main" @click="onTogglePlay">
          <text class="ctrl-text">{{ btnText }}</text>
        </div>
        <!-- 直播不支持定位（时长未知、HLS 只有滑动窗口）→ 隐藏 ±20s -->
        <div v-if="!play.session.live" class="ctrl-row">
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

      <!-- 搜索：关键词行 + 三分栏（视频/图文/直播）+ 结果 -->
      <scroller v-else-if="tab === 'search'" class="list">
        <div class="searchbar" @click="onOpenInput">
          <text class="search-kw">{{ keyword || '点此输入关键词搜索 B 站' }}</text>
          <div class="search-btn">
            <text class="search-btn-text">搜索</text>
          </div>
        </div>
        <!-- 类型三分栏：切换零请求（各类型独立缓存），有关键词且该类型无结果时自动补拉 -->
        <div class="stype-row">
          <div
            v-for="st in searchTypes"
            :key="'st' + st.id"
            :class="searchType === st.id ? 'stype stype-on' : 'stype'"
            @click="setSearchType(st.id)"
          >
            <text :class="searchType === st.id ? 'stype-text stype-text-on' : 'stype-text'">{{ st.label }}</text>
          </div>
        </div>
        <!-- 视频：与推荐/热门同款卡片 -->
        <template v-for="it in searchItems">
          <div v-if="it.kind === 'video'" :key="it.key" class="vrow" @click="onPlayItem(it)">
            <image class="thumb" :src="it.cover" :width="112" :height="63"></image>
            <div class="vinfo">
              <text class="vtitle">{{ it.title }}</text>
              <text class="vmeta">{{ vmeta(it) }}</text>
            </div>
          </div>
          <!-- 图文：同款卡片，进入图文详情（评论区与视频同族，专栏实测 type=12） -->
          <div v-else-if="it.kind === 'article'" :key="it.key" class="vrow" @click="openArticle(it)">
            <image class="thumb" :src="it.cover" :width="112" :height="63"></image>
            <div class="vinfo">
              <text class="vtitle">{{ it.title }}</text>
              <text class="vmeta">{{ it.up }}{{ it.view ? ' · 阅读 ' + formatCount(it.view) : '' }}{{ it.reply ? ' · 评论 ' + it.reply : '' }}</text>
            </div>
          </div>
          <!-- 直播：点击开播（经「我的 → 直播设置」里的转码服务器；未填地址 → 播放页提示） -->
          <div v-else-if="it.kind === 'live'" :key="it.key" class="vrow" @click="onPlayItem(it)">
            <image class="thumb" :src="it.cover" :width="112" :height="63"></image>
            <div class="vinfo">
              <text class="vtitle">{{ it.title }}</text>
              <text class="vmeta">{{ it.up }}{{ it.online ? ' · 人气 ' + formatCount(it.online) : '' }}{{ it.cate ? ' · ' + it.cate : '' }}</text>
            </div>
          </div>
          </template>
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
        <div class="mine-secrow">
          <text class="mine-sec">直播设置（转码服务器）</text>
        </div>
        <div class="mine-row" @click="onLiveAddrEdit">
          <text class="mine-label">服务器地址</text>
          <text class="mine-value">{{ live.addr || '未填写 · 点此输入' }}</text>
        </div>
        <div class="mine-row" @click="onLiveBufferCycle">
          <text class="mine-label">服务端分片窗口</text>
          <text class="mine-value">{{ (live.bufMs / 1000).toFixed(0) }} s</text>
        </div>
        <div class="mine-row" @click="onLiveQualityCycle">
          <text class="mine-label">直播画质</text>
          <text class="mine-value">{{ live.res === 254 ? '匹配屏幕' : live.res + 'p' }} · {{ live.bv }}{{ live.trans ? '' : ' · 直通' }}</text>
        </div>
        <div class="mine-row" @click="onLiveProbe">
          <text class="mine-label">测连服务器</text>
          <text class="mine-value">{{ liveTestText || '点此测试' }}</text>
        </div>
        <div class="mine-row" @click="onLiveCacheClear">
          <text class="mine-label">解析缓存</text>
          <text class="mine-value">{{ live.resolvedAddr || '空' }}</text>
        </div>
        <text v-if="liveNote" class="mine-notice">{{ liveNote }}</text>
        <div class="mine-row">
          <text class="mine-label">PenBili</text>
          <text class="mine-value">v2.9.4 · {{ profile ? '已登录' : '匿名' }}</text>
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

    <!-- 评论覆盖面板：不透明覆盖播放/图文内容区（左栏控制保留；视频被盖 → 面板内滑动无视频可闪） -->
    <div v-if="(mode === 'play' || mode === 'article') && commentsOpen" class="comments-col">
      <div class="comments-head">
        <text class="comments-title">评论{{ replyCount ? ' · ' + replyCount : '' }}</text>
        <div class="ctrl comments-close" @click="closeComments">
          <text class="ctrl-text">{{ commentsBackLabel }}</text>
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
      <!-- 右栏按钮：评论区入口（容器 flex 居中，数字变长也不偏）。弹幕功能已于 v2.7.0 移除
           （drawtext×4 逐帧渲染占解码预算 ~20%，弱稿件软解跌破实时 → 卡顿，用户决策移除） -->
      <div v-if="!play.session.live" class="info-ctrl" @click="openComments">
        <text class="info-ctrl-text">评论区{{ replyCount ? ' · ' + replyCount : '' }}</text>
      </div>
      <!-- 直播：无评论区（reply 无 oid）→ 显示播放态，不提供入口 -->
      <text v-else class="info-live-tag">{{ play.state === 'playing' ? '直播中' : stateLabel }}</text>
      <div class="info-spacer"></div>
      <text class="info-state">{{ play.note }}</text>
      <text class="info-hint">{{ play.session.live ? '直播不支持定位/回看' : '左侧：暂停/开始 · ±' + seekStepSec + 's' }}</text>
    </div>

    <!-- 图文详情：中列正文滚动 + 右列信息（评论面板打开时两列收起；评论区与视频同族，专栏实测 type=12） -->
    <div v-if="mode === 'article' && !commentsOpen" class="play-col">
      <scroller class="art-scroll">
        <div v-if="article.state === 'loading'" class="art-loading">
          <text class="play-idle-text">图文加载中…</text>
        </div>
        <div v-else-if="article.state === 'error'" class="art-loading">
          <text class="play-idle-text">{{ article.note || '图文加载失败 · 返回列表' }}</text>
        </div>
        <template v-else>
          <image
            v-if="article.cover"
            class="art-cover"
            :src="article.cover"
            :width="436"
            :height="article.coverDh || 140"
            :style="{ width: '436px', height: (article.coverDh || 140) + 'px' }"
          ></image>
          <text class="art-title">{{ article.title }}</text>
          <text class="art-author">{{ article.author }}{{ article.read ? ' · 阅读 ' + formatCount(article.read) : '' }}{{ article.like ? ' · 点赞 ' + formatCount(article.like) : '' }}</text>
          <template v-for="(b, bi) in article.blocks">
            <image
              v-if="b.t === 'img'"
              :key="'ab' + bi"
              class="art-img"
              :src="b.src"
              :width="b.dw || 436"
              :height="b.dh || 240"
              :style="{ width: (b.dw || 436) + 'px', height: (b.dh || 240) + 'px' }"
            ></image>
            <text v-else :key="'ab' + bi" class="art-p">{{ b.text }}</text>
          </template>
          <text class="art-end">— 全文完 —</text>
        </template>
      </scroller>
    </div>
    <div v-if="mode === 'article' && !commentsOpen" class="info-col">
      <text class="info-title">{{ article.title }}</text>
      <text class="info-up">{{ article.author }}{{ article.words ? ' · ' + article.words + ' 字' : '' }}</text>
      <div class="info-ctrl" @click="openComments">
        <text class="info-ctrl-text">评论区{{ replyCount ? ' · ' + replyCount : '' }}</text>
      </div>
      <div class="info-spacer"></div>
      <text class="info-state">{{ article.state === 'ok' ? '图文详情' : article.note }}</text>
      <text class="info-hint">左侧：返回图文列表</text>
    </div>
  </div>
</template>

<script>
import {
  fetchRecommended,
  fetchPopular,
  searchVideos,
  searchBili,
  appendDeduped,
  describeListError,
  formatDuration,
  formatCount
} from '../../services/feed.js';
import { fetchArticle } from '../../services/bili/article.js';
import { createClient, cookieFromSession, DEFAULT_UA, REFERER } from '../../services/bili/client.js';
import { KEYS, getJson, setJson, loadSession, logWarn, loadLive, saveLive, normalizeLive, normalizeLiveAddr, LIVE_LIMITS } from '../../services/storage.js';
import { ensureLiveAddr } from '../../services/live.js';
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
import { httpGet, httpGetTextNative, cancelNativePosts } from '../../services/net.js';
import { fetchReplies, addReply, fetchSubReplies } from '../../services/bili/reply.js';
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
  ensureMixinResult,
  SEEK_STEP_MS
} from '../../services/play_session.js';
import * as player from '../../services/player.js';

const TABS = [
  { id: 'rcmd', label: '推荐' },
  { id: 'hot', label: '热门' },
  { id: 'search', label: '搜索' },
  { id: 'mine', label: '我的' }
];

// 搜索三分栏（与 feed.js searchBili 的 search_type 对应）：
//   video/article/live 三栏本版全部可用。live 走服务端转码（HLS 分片回传，见 server/live_proxy.py v5），
//   地址取「我的 → 直播设置」；未填地址时点播会提示"未填写转码服务器地址，无法播放"。
const SEARCH_TYPES_UI = [
  { id: 'video', label: '视频' },
  { id: 'article', label: '图文' },
  { id: 'live', label: '直播' }
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
      searchTypes: SEARCH_TYPES_UI,
      tab: 'rcmd',
      mode: 'browse', // browse=列表 | play=视频 | article=图文详情
      rcmdItems: [],
      hotItems: [],
      // 搜索三分栏（视频/图文/直播）：按类型独立缓存结果与分页；searchItems 为 computed 活跃视图
      searchType: 'video',
      searchResults: { video: [], article: [], live: [] },
      searchPages: { video: 1, article: 1, live: 1 },
      searchNoMoreMap: { video: false, article: false, live: false },
      hotPn: 1,
      hotNoMore: false,
      hotMorePn: 0, // 推荐耗尽后用热门续底的页码（与 hot tab 独立）
      moreNoMore: false,
      keyword: '',
      loading: false,
      statusText: '',
      // 评论上下文：视频=type 1（oid=aid）、图文=type 12（oid=专栏 aid）——回复/加载共用
      replyCtx: { oid: 0, type: 1 },
      // 图文详情（mode=article）：blocks = 文本/图片混排块（{t:'text',text} | {t:'img',src,dw,dh}）
      article: { aid: 0, title: '', author: '', cover: '', coverDh: 140, blocks: [], read: 0, like: 0, state: 'idle', note: '' },
      history: [],
      buvidShort: '',
      seekStepSec: SEEK_STEP_MS / 1000,
      play: idlePlay(),
      // 直播设置（转码服务器地址 / 起播缓冲 / 画质）+ 重定向解析缓存（服务端外网端口会变）
      live: normalizeLive(null),
      liveActiveAddr: '', // 本次会话实际使用的服务器地址（缓存探测或重解析的结果）
      liveNote: '',       // 直播设置区状态说明
      liveTestText: '',   // 测连结果
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
      // 评论面板（弹幕功能已于 v2.7.0 移除：drawtext 烧帧拖垮弱稿软解）
      commentsOpen: false
    };
  },
  computed: {
    currentItems() {
      return this.tab === 'hot' ? this.hotItems : this.rcmdItems;
    },
    // 搜索活跃类型的条目视图（三分栏：视频/图文/直播各自缓存，切换零请求）
    searchItems() {
      return this.searchResults[this.searchType] || [];
    },
    // 评论面板返回按钮文案（跟随所在模式）
    commentsBackLabel() {
      return this.mode === 'article' ? '返回图文' : '返回播放';
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
      if (this.tab === 'search') return this.searchNoMoreMap[this.searchType] ? '没有更多了' : '加载更多';
      return this.hotNoMore ? '没有更多了' : '加载更多';
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
        cancelled: () => this.gen !== myGen,
        // 直播配置：地址优先用"本次会话已解析出来的"，其次缓存，最后入口地址。
        // ck = 笔端登录 cookie（服务端用它拉 720p 源，并按 cookie 隔离多用户会话）。
        live: {
          addr: this.liveActiveAddr || this.live.resolvedAddr || this.live.addr,
          bufMs: this.live.bufMs,
          res: this.live.res,
          bv: this.live.bv,
          trans: this.live.trans,
          ck: cookieFromSession(this.session)
        }
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
    // 模板不能直接用模块导入 → 方法转发（图文/直播卡片计数展示）
    formatCount(n) {
      return formatCount(n);
    },
    historyTime(at) {
      return formatHistoryTime(at, Date.now());
    },
    async loadLocal() {
      const h = await loadHistory();
      this.history = h.items;
      this.live = await loadLive();
      // 回写一次：normalizeLive 可能做了 schema 迁移（如 v1→v2 把画质 480/360 纠正为匹配屏幕的 254），
      // 落盘后存储即与内存一致，避免"设置里显示旧值"的错觉。
      saveLive(this.live);
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
      // 直播服务器地址：启动时**后台**探测缓存的解析结果（2s 内无响应 → 重新解析并回写缓存）。
      // 不阻塞首屏；结果只影响直播播放，失败不打扰用户（进「我的」手动测连即可）。
      if (this.live.addr || this.live.resolvedAddr) {
        this.checkLiveAddr(false).catch(() => {});
      }
      const at = await getJson(KEYS.autotest, null);
      if (at && (at === true || at.enabled === true)) {
        const soakSec = at && typeof at === 'object' ? Number(at.soak) || 0 : 0;
        // soakBv：把 soak 固定到指定稿件（跨轮可复现同一稿件 → 才能做换网/换版本的 A/B 对比）
        const soakBv = at && typeof at === 'object' && /^BV[0-9A-Za-z]{10}$/.test(at.soakBv || '') ? at.soakBv : '';
        // liveOnly：只跑直播专项（跳过视频全流程），用于直播链路真机验证
        const liveOnly = at && typeof at === 'object' && at.liveOnly === true;
        const liveRoom = at && typeof at === 'object' ? Math.floor(Number(at.liveRoom) || 0) : 0;
        // wbiStress：连打 N 次 nav 取 WBI 密钥，统计失败率与走的传输（诊断 nav 间歇失败用）
        const wbiStress = at && typeof at === 'object' ? Math.floor(Number(at.wbiStress) || 0) : 0;
        // liveHold：直播自检的取证窗口（ms，默认 15000）——诊断时拉长便于外部干预
        const liveHold = at && typeof at === 'object' ? Math.floor(Number(at.liveHold) || 0) : 0;
        // pollEvery：巡检打点间隔秒数（默认 5）——诊断时设 1 便于看时序
        this._pollEvery = at && typeof at === 'object' ? Math.max(1, Math.floor(Number(at.pollEvery) || 5)) : 5;
        logWarn('[bili] autotest seeded → ' + (liveOnly
          ? 'liveOnly room=' + (liveRoom || '-')
          : wbiStress > 0 ? 'wbiStress n=' + wbiStress
          : 'run' + (soakSec > 0 ? ' soak=' + soakSec + 's' : '') + (soakBv ? ' bv=' + soakBv : '')));
        if (liveOnly) this.runLiveAutotest(liveRoom, liveHold);
        else if (wbiStress > 0) this.runWbiStress(wbiStress);
        else this.runAutotest(soakSec, soakBv); // 自检内部自带首屏加载（避免与这里并发双 load 竞态）
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
          const mx = await ensureMixinResult(this.makeCtx(this.gen));
          if (!mx.ok) {
            logWarn('[bili] wbi fail stage=' + mx.stage + ' code=' + (mx.code != null ? mx.code : '-') + ' ' + mx.message);
            return { ok: false, stage: mx.stage, code: mx.code, message: mx.message, wbiRetried: mx.wbiRetried };
          }
          const r = await searchBili(this.client, mx.key, this.keyword, 1, this.searchType);
          if (r.ok) {
            this.searchPages[this.searchType] = 1;
            this.searchNoMoreMap[this.searchType] = r.noMore;
          }
          return r;
        }
        // 注意：此处不能无条件 `return await fetchRecommended()` —— 会把下面 hot 分支变成死代码，
        // 导致热门首屏写入推荐流数据（而翻页 loadMore 又用 fetchPopular，首屏与翻页内容不一致）。
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
      // wbiRetried：nav 取密钥失败时 client 内部已完成多趟退避重试 → 这里不再整体重跑
      if (r && r.ok === false && this.gen === myGen && r.stage !== 'param' && !r.wbiRetried) {
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
      else if (id === 'search') this.searchResults[this.searchType] = r.items;
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
        // 新关键词：三分栏全部重置（切到 search tab 由 loadTab 拉当前类型首屏）
        this.searchResults = { video: [], article: [], live: [] };
        this.searchPages = { video: 1, article: 1, live: 1 };
        this.searchNoMoreMap = { video: false, article: false, live: false };
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
        // 搜索刷新：保留关键词，当前类型重置分页重拉第一页（其它类型缓存保留）
        this.searchResults[this.searchType] = [];
        this.searchPages[this.searchType] = 1;
        this.searchNoMoreMap[this.searchType] = false;
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
    // 搜索翻页（search/type 支持 page；三分栏各自独立分页）
    async loadMoreSearch() {
      const st = this.searchType;
      if (this.searchNoMoreMap[st] || !this.keyword) return;
      const myGen = ++this.gen;
      this.loading = true;
      this.statusText = '加载更多…';
      const mx = await ensureMixinResult(this.makeCtx(this.gen));
      if (this.gen !== myGen) {
        this.loading = false;
        return;
      }
      if (!mx.ok) {
        this.loading = false;
        this.statusText = describeListError({ ok: false, stage: mx.stage, code: mx.code, message: mx.message });
        return;
      }
      const r = await searchBili(this.client, mx.key, this.keyword, this.searchPages[st] + 1, st);
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
      this.searchPages[st] = r.page;
      this.searchNoMoreMap[st] = r.noMore;
      const merged = appendDeduped(this.searchResults[st], r.items);
      this.searchResults[st] = merged.items;
      this.statusText = merged.added === 0 ? '没有更多了' : '';
      logWarn('[bili] search(' + st + ') page ' + r.page + ' +' + merged.added + ' total=' + merged.items.length);
    },

    /* ---------- 搜索三分栏切换 / 图文 / 直播 ---------- */
    // 切换类型：有缓存零请求；有关键词且该类型为空 → 自动补拉首屏
    setSearchType(id) {
      if (this.searchType === id) return;
      this.searchType = id;
      this.statusText = '';
      if (this.tab !== 'search') return;
      if (this.keyword && !this.searchResults[id].length && !this.loading) {
        const myGen = ++this.gen;
        this.loadTab('search', myGen);
      }
    },
    // 图文详情：正文 + 评论区（type=12，与视频评论同族）
    async openArticle(it) {
      if (!it || !(it.id > 0)) return;
      const myGen = ++this.gen;
      this.mode = 'article';
      this.article = {
        aid: it.id,
        title: it.title,
        author: it.up || '',
        cover: it.cover || '',
        coverDh: 140,
        blocks: [],
        read: it.view || 0,
        like: 0,
        state: 'loading',
        note: ''
      };
      // 换稿件即清空上一条的评论上下文（专栏评论实测 type=12，17 为旧文档值已 -404）
      this.replies = [];
      this.replyStatus = '';
      this.replyCount = 0;
      this.commentsOpen = false;
      this.replyCtx = { oid: it.id, type: 12 };
      // article/view 有限流/风控（-509/-352 实测）→ 退避重试两次（1.5s / 6s，同 loadTab 策略的加强版）
      const mx = await ensureMixinResult(this.makeCtx(myGen));
      if (this.gen !== myGen) return;
      if (!mx.ok) {
        this.article.state = 'error';
        this.article.note = (mx.message || 'WBI 密钥失败') + (mx.code != null ? '（code ' + mx.code + '）' : '');
        return;
      }
      const mixin = mx.key;
      const backoffs = [0, 1500, 6000];
      let r = { ok: false, stage: 'init' };
      for (let i = 0; i < backoffs.length; i++) {
        if (backoffs[i] > 0) {
          this.article.note = '限流/风控，' + Math.round(backoffs[i] / 1000) + 's 后重试…';
          await this.sleep(backoffs[i]);
        }
        if (this.gen !== myGen) return;
        r = await fetchArticle(this.client, mixin, it.id);
        if (this.gen !== myGen) return;
        if (r.ok) break;
        if (r.stage === 'param' || r.stage === 'wbi') break; // 非瞬态错误不重试
      }
      if (!r.ok) {
        this.article.state = 'error';
        this.article.note = describeListError(r);
        logWarn('[bili] article fail stage=' + r.stage + ': ' + r.message);
        return;
      }
      this.article = {
        aid: r.article.aid,
        title: r.article.title,
        author: r.article.author,
        cover: r.article.cover || '',
        coverDh: r.article.coverDh || 140,
        blocks: r.article.blocks,
        read: r.article.read,
        like: r.article.like,
        state: 'ok',
        note: ''
      };
      this.loadReplies(r.article.aid, 12); // 评论首屏（专栏 type=12；不阻塞正文展示）
      logWarn('[bili] article open aid=' + r.article.aid + ' blocks=' + r.article.blocks.length);
    },
    // 返回图文列表（无播放会话，仅取消图文/评论在途请求）
    onBackArticle() {
      const myGen = ++this.gen;
      this.commentsOpen = false;
      this.mode = 'browse';
      this.tab = 'search';
    },

    /* ---------- 直播设置 + 重定向解析缓存 ---------- */
    // 解析/校验服务器地址：ensureLiveAddr 先探缓存地址（2s），失效则重走入口地址解析并回写缓存。
    // withUi=true 时把结果写进「测连」文案（用户手动触发）；启动时传 false（后台静默）。
    async checkLiveAddr(withUi) {
      if (!this.live.addr && !this.live.resolvedAddr) {
        this.liveNote = '未填写服务器地址 → 直播无法播放';
        this.liveTestText = withUi ? '未填写地址' : this.liveTestText;
        return { ok: false, message: '未配置服务器地址' };
      }
      // 外网入口 live.example.com 是 CF 301 → tunnel.example.net:<动态端口>；
      // jsapi.http 不跟随 301（/health 只拿到 301 的 HTML → 解析降级回入口域名 → 播放时
      // ffmpeg 跟 301 又丢 query → 400）。必须走 native libcurl（CURLOPT_FOLLOWLOCATION=1）
      // 才能拿到终点 host 回写 resolvedAddr。
      const r = await ensureLiveAddr((u, o) => httpGetTextNative(u, o), this.live, {});
      if (!r.ok) {
        this.liveActiveAddr = '';
        this.liveNote = '地址不可用：' + (r.message || '解析失败');
        if (withUi) this.liveTestText = '连接失败';
        return r;
      }
      this.liveActiveAddr = r.addr;
      // 只在「解析出真实终点」时回写缓存；degraded（解析失败、用入口域名兜底）不回写——
      // 否则会把入口域名（如 live.example.com）当成有效缓存，下次探缓存"命中"入口域名，
      // 播放时 ffmpeg 跟 301 又丢 query → 400（2026-10-07 外网真机踩到）。
      if (r.changed && !r.degraded) {
        // 解析结果变了（外网端口重新打洞）→ 回写缓存
        this.live = normalizeLive(
          Object.assign({}, this.live, { resolvedAddr: r.addr, resolvedAt: Date.now() })
        );
        saveLive(this.live);
      }
      const h = r.health || {};
      const load = h.load || {};
      const srcText = r.source === 'cache' ? '缓存命中' : r.source === 'resolved' ? '重新解析' : '入口兜底';
      this.liveNote =
        r.addr + '（' + srcText + '）' +
        (h.version ? ' · 服务端 v' + h.version : '') +
        (load.sessions != null ? ' · 会话 ' + load.sessions : '') +
        (load.high ? ' · 负载高' : '');
      if (withUi) this.liveTestText = r.degraded ? '可用（兜底）' : '连接正常';
      logWarn('[live] addr=' + r.addr + ' source=' + r.source + ' changed=' + r.changed);
      return r;
    },
    // 服务器地址（系统输入法输入；改地址即作废旧解析缓存）
    async onLiveAddrEdit() {
      const myGen = this.gen;
      this.imBusy = true;
      try {
        const r = await input.open({ value: this.live.addr, placeholder: '如 http://192.168.1.100:2050', maxlength: 80 });
        if (this.gen !== myGen) return;
        if (r && r.error) {
          this.liveTestText = '输入法不可用';
          return;
        }
        if (!(r && r.confirmed)) return;
        const addr = normalizeLiveAddr((r.text || '').trim());
        this.live = normalizeLive(
          Object.assign({}, this.live, { addr: addr, resolvedAddr: '', resolvedAt: 0 })
        );
        saveLive(this.live);
        this.liveActiveAddr = '';
        this.liveTestText = '';
        this.liveNote = addr ? '已保存：' + addr : '已清空服务器地址';
        logWarn('[live] addr saved len=' + addr.length);
        if (addr) this.checkLiveAddr(true).catch(() => {});
      } finally {
        this.imBusy = false;
      }
    },
    onLiveBufferCycle() {
      const list = [2000, 3000, 4000, 6000, 8000, 12000];
      const i = list.indexOf(this.live.bufMs);
      const next = list[(i + 1) % list.length];
      this.live = normalizeLive(Object.assign({}, this.live, { bufMs: next }));
      saveLive(this.live);
      this.liveNote =
        '服务端保留 ' + Math.round(next / 1000) + 's 分片：只影响笔端落后时还能取到多旧的分片（抗抖动余量）；' +
        '不影响起播时间（起播只由「服务端出首个分片 + 笔端探流」决定）。';
      logWarn('[bili] live window -> ' + next + 'ms');
    },
    // 画质档循环：匹配屏幕(254) → 标清(360) → 高清(480) → 循环
    // 顺序即推荐度：254 是唯一"清晰 + 流畅"档（服务端输出 == 视口 452x254，笔端缩放被跳过）；
    // 360/480 输出会被笔端再缩回 452x254，白烧 CPU，真机实测 1.03x / 0.83x → 必卡，仅留调试。
    onLiveQualityCycle() {
      const seq = [
        { res: 254, trans: 1 },
        { res: 360, trans: 1 },
        { res: 480, trans: 1 }
      ];
      let i = 0;
      for (let k = 0; k < seq.length; k++) {
        if (seq[k].res === this.live.res && seq[k].trans === this.live.trans) {
          i = k;
          break;
        }
      }
      const n = seq[(i + 1) % seq.length];
      this.live = normalizeLive(Object.assign({}, this.live, n));
      saveLive(this.live);
      this.liveNote =
        n.res === 254
          ? '匹配屏幕 452×254：服务端输出尺寸 == 视口，笔端零缩放 → 清晰且流畅（推荐）'
          : n.res + 'p：会被笔端缩回 452×254，白烧解码预算（实测 360→1.03x / 480→0.83x，会卡）';
    },
    async onLiveProbe() {
      this.liveTestText = '测试中…';
      await this.checkLiveAddr(true);
    },
    onLiveCacheClear() {
      this.live = normalizeLive(Object.assign({}, this.live, { resolvedAddr: '', resolvedAt: 0 }));
      saveLive(this.live);
      this.liveActiveAddr = '';
      this.liveNote = '已清空解析缓存；下次播放或测连会重新解析';
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
      this.replyCtx = { oid: 0, type: 1 };
      this.replyOpen = { root: 0, items: [], shown: 0, page: 1, noMore: false, loading: false };
      this.play = {
        state: 'loading',
        session: {
          kind: item.kind === 'live' ? 'live' : 'video',
          live: item.kind === 'live',
          roomid: item.roomid || 0,
          bvid: item.kind === 'live' ? 'live:' + (item.roomid || 0) : item.bvid,
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
      this.replyCtx = { oid: r.session.aid || 0, type: 1 };
      this.loadReplies(r.session.aid || 0, 1); // 评论首屏（不阻塞播放；无 aid 内部直接返回）
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
      this._audioDeadWarned = false; // 每次新播放重置音频中断提示状态。
      this._resyncNoteOn = false;
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
        // v1.8.0 追加 skip(滞后帧)/drift(可闻音频−画面,正=声音超前)：修复后 drift 应稳在 ±100ms
        // v1.9.0 追加 buf(可闻锚修正量=管道+ALSA缓冲,恒定即正常)
        // v1.9.3 追加 rst(视频断流重启次数)
        // v2.1.0 追加 rs(强制重同步次数)/RESYNC(正在强制重同步)
        // 巡检仍每秒执行；常规详细状态每 5 秒输出一次，异常事件（resync/audioDead/gate）即时单独打点。
        this._pollN = (this._pollN || 0) + 1;
        if (this._pollN % (this._pollEvery || 5) === 0) {
          logWarn(
            '[bili] poll tick frames=' + st.frames + ' pos=' + st.positionMs +
            ' ab=' + st.audioBytes + ' ad=' + st.audioDropped +
            ' u=' + st.audioUnderruns + ' w=' + st.audioWrErrors + ' rd=' + st.audioRingDrops +
            ' stall=' + (st.videoStallMs || 0) + (st.gateActive ? 'G' : '') +
            ' skip=' + (st.videoSkips || 0) + ' drift=' + (st.resyncing ? '?' : (st.avDriftMs || 0)) + 'ms' +
            ' buf=' + (st.audioBufMs || 0) + 'ms' +
            ' vpk=' + (st.videoPipeKb || 0) + ' pal=' + (st.paceAlignMs || 0) +
            ' rst=' + (st.videoRestarts || 0) + ' rs=' + (st.resyncCount || 0) +
            ' lead=' + (st.resyncLeadMs || 0) +
            (st.resyncing ? ' RESYNC(' + (st.resyncingMs || 0) + 'ms)' : '') +
            (st.audioDead ? ' DEAD' : '')
          );
        }
        // 音画同步分层提示（v2.1.0）：轻微滞后由 native 平滑快进自愈（不打扰用户）；
        // 过大滞后由 native 强制重同步（视频流重启到音频位置，声音不断）→ 显示"加载中…"，
        // 首帧落屏后 native 清 resyncing，提示自动消失。
        // v2.2.5：只有"恢复确实慢"才弹「加载中」。视频流重启通常 1~2s（缓冲还顶着），这种快速自愈
        // 不打扰用户（只表现为一次极短画面停顿）；真正慢（>2.5s）才给提示，避免提示刷屏。
        if (st.state === 'playing' && st.resyncing && (st.resyncingMs || 0) > 2500) {
          if (!this._resyncNoteOn) {
            this._resyncNoteOn = true;
            this.play.note = '加载中…';
            // 注意：重同步窗口内 avDriftMs 是伪值（video_base 已前移到 apos+lead、frames=0
            // → 算出来恒等于 -lead），所以这里打 native 的触发漂移 avDriftNowMs。
            // 注意：重启也可能来自 native 的"首帧超时重试"（resyncCount 不增），故文案不写"强制重同步"
            logWarn(
              '[bili] 视频流重启中（强制重同步累计=' + (st.resyncCount || 0) + ' 触发漂移=' +
              (st.avDriftNowMs || 0) + 'ms）已耗时=' + (st.resyncingMs || 0) + 'ms → 加载中…'
            );
          }
        } else if (this._resyncNoteOn) {
          this._resyncNoteOn = false;
          if (this.play.note === '加载中…') this.play.note = '';
        }
        // 音频链已断时只在播放中提示：视频播完后音频进程正常结束也会置 audioDead，
        // 不能在"已播完"后再追加无声提示。
        if (st.audioDead && st.state === 'playing' && !this._audioDeadWarned) {
          this._audioDeadWarned = true;
          this.play.note = '无声：音频输出被其他应用占用';
          logWarn('[bili] audio chain dead（播放中）→ 音频输出可能被其他应用占用');
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
          // v2.2.5：重同步期间让位，但**有限让位**——超过 15s 仍未恢复就交回 JS 兜底，
          // 否则一旦 native 侧重启卡死，两个看门狗都被压住 → 画面永久冻结（真机实测过 66s+）。
          (!st.resyncing || (st.resyncingMs || 0) > 15000) &&
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
        // 僵尸涓流看门狗（v2.6.1）：视频停帧 ≥25s 且音频字节也不再增长 = 双链路假死
        // （TCP 涓流让 rw_timeout 永不触发，实测可冻结 60s+）。主动 seek(pos+1) 双重启：
        // 网络恢复即瞬回，仍断网则按冷却重试（优于无限冻结）。25s 冷却防弱网风暴。
        if (
          st.state === 'playing' &&
          !st.gateActive &&
          (!st.resyncing || (st.resyncingMs || 0) > 15000) &&
          st.videoStallMs > 25000 &&
          st.audioBytes === (this._lastAvAudioBytes || 0) &&
          !this.commentsOpen
        ) {
          const now2 = Date.now();
          if (!this._hardStallAt || now2 - this._hardStallAt > 25000) {
            this._hardStallAt = now2;
            logWarn('[bili] hard stall: video ' + st.videoStallMs + 'ms 无帧且音频冻结 → 双重启');
            this.play.note = '网络不稳定，正在重连…';
            this.onSeek(0.001).catch(function (e) {
              logWarn('[bili] hard stall restart: ' + (e && e.message));
            });
          }
        }
        this._lastAvAudioBytes = st.audioBytes;
        // v2.9.4：**只要还没有任何视频帧就提示「加载中」**。原因（真机诊断实测）：起播门 15s 会
        // 先放行音频（"极弱网先出声"），此时音频已响但画面仍是冻的 → 用户感受正是"画面卡住但声音
        // 正常"。原判据（首帧+位置0+无音频字节）在音频放行后就不成立了，于是画面冻着却没有任何提示。
        // 注意区分：resyncing（视频流重启）走上面的 2.5s 门槛保持静默自愈，避免提示刷屏。
        if (st.state === 'playing' && st.frames === 0 && !st.resyncing) {
          if (this.play.note !== '加载中…') {
            this.play.note = '加载中…';
            logWarn('[bili] 无画面帧（起播/卡死）→ 加载中…');
          }
        } else if (this.play.note === '加载中…' && st.frames > 0) {
          this.play.note = '';
        }
        this.play.positionMs = st.positionMs;
        this.play.frames = st.frames;
        if (st.state === 'ended') {
          this.play.state = 'ended';
          this.play.note = ''; // 播完时清掉残留提示。
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
    },
    /* 评论面板显示权交接：打开=暂停 fb 输出（解码/音频照常）→ UI 独占无闪；关闭恢复。
       图文模式无播放会话，不碰 player。 */
    openComments() {
      if (!(this.replyCtx && this.replyCtx.oid > 0)) {
        this.statusText = '评论加载中，请稍候';
        return;
      }
      this.commentsOpen = true;
      if (this.mode === 'play') {
        player.pauseRender().catch(function (e) {
          logWarn('[bili] pauseRender: ' + (e && e.message));
        });
        logWarn('[bili] comments open → render paused');
      }
    },
    closeComments() {
      if (!this.commentsOpen) return;
      this.commentsOpen = false;
      if (this.mode === 'play') {
        player.resumeRender().catch(function (e) {
          logWarn('[bili] resumeRender: ' + (e && e.message));
        });
        logWarn('[bili] comments close → render resumed');
      }
    },

    /* ---------- 评论（覆盖面板 scroller 滑动 + 行内子楼） ---------- */
    replyMeta(rp) {
      const parts = [];
      if (rp.likes > 0) parts.push('▲ ' + rp.likes);
      if (rp.rcount > 0) parts.push(rp.rcount + ' 条回复');
      return parts.join(' · ');
    },
    async loadReplies(oid, type) {
      if (!(oid > 0)) return;
      const myGen = this.gen;
      this.replyLoading = true;
      this.replyStatus = '';
      this.replies = [];
      this.replyPage = 1;
      this.replyNoMore = false;
      this.replyOpen = { root: 0, items: [], shown: 0, page: 1, noMore: false, loading: false };
      const r = await fetchReplies(this.client, oid, 1, type);
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
      const ctx = this.replyCtx || { oid: 0, type: 1 };
      if (!(ctx.oid > 0)) return;
      const myGen = this.gen;
      this.replyLoading = true;
      const r = await fetchReplies(this.client, ctx.oid, this.replyPage + 1, ctx.type);
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
      const ctx = this.replyCtx || { oid: 0, type: 1 };
      if (!(ctx.oid > 0)) {
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
        const r = await addReply(this.client, ctx.oid, text, csrf, ctx.type);
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
      const ctx = this.replyCtx || { oid: 0, type: 1 };
      if (!(ctx.oid > 0)) return;
      this.replyOpen = { root: rp.rpid, items: [], shown: 0, page: 1, noMore: false, loading: true };
      const myGen = this.gen;
      const myRoot = rp.rpid;
      const r = await fetchSubReplies(this.client, ctx.oid, myRoot, 1, ctx.type);
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
      const ctx = this.replyCtx || { oid: 0, type: 1 };
      if (!(ctx.oid > 0)) return;
      const myGen = this.gen;
      const myRoot = this.replyOpen.root;
      this.replyOpen.loading = true;
      const r = await fetchSubReplies(this.client, ctx.oid, myRoot, this.replyOpen.page + 1, ctx.type);
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
    // WBI 压测（诊断；种子 bili_autotest = {enabled:true, wbiStress:<N>}）：
    // 连打 N 次 nav 取密钥，统计失败率与成功时走的传输（jsapi / native）。
    async runWbiStress(n) {
      let okJsapi = 0;
      let okNative = 0;
      let bad = 0;
      try {
        for (let i = 0; i < n; i++) {
          const r = await this.client.fetchWbiKeys();
          if (r.ok) {
            if (r.via === 'native') okNative++;
            else okJsapi++;
          } else {
            bad++;
          }
          logWarn(
            '[bili] WBI stress #' + (i + 1) + '/' + n + ' ok=' + r.ok +
              ' via=' + (r.via || '-') + ' stage=' + (r.stage || '-') +
              ' code=' + (r.code != null ? r.code : '-') + ' ms=' + (r.ms != null ? r.ms : '-') +
              (r.ok ? '' : ' msg=' + (r.message || ''))
          );
          await this.sleep(300);
        }
      } catch (e) {
        logWarn('[bili] WBI stress 异常: ' + ((e && e.message) || e));
      }
      logWarn('[bili] WBI stress done n=' + n + ' jsapi=' + okJsapi + ' native=' + okNative + ' fail=' + bad);
    },
    // 直播专项自检（种子 bili_autotest = {enabled:true, liveOnly:true, liveRoom:<id 可选>}）：
    // 只跑直播链路 —— 解析服务器地址 → 直播栏搜索 → 播放 → 首帧断言 → 返回。
    // 真机证据链：服务端 server.log 会出现 /live 请求与分片拉取（证明笔端 ffmpeg 在消费 HLS）。
    async runLiveAutotest(room, holdMs) {
      const ensure = (cond, msg) => {
        if (!cond) throw new Error(msg);
      };
      try {
        logWarn('[bili] LIVE AUTOTEST start' + (room ? ' room=' + room : ' (搜首个直播间)'));
        await this.checkLiveAddr(true);
        const addr = this.liveActiveAddr || this.live.resolvedAddr || this.live.addr;
        ensure(!!addr, '直播服务器地址未配置');
        logWarn('[bili] LIVE addr=' + addr);
        this.keyword = '游戏';
        this.searchType = 'live';
        await this.selectTab('search');
        ensure(this.searchItems.length > 0, '直播搜索结果为空: ' + this.statusText);
        const item = room ? { kind: 'live', roomid: room, title: 'LIVE ' + room } : this.searchItems[0];
        logWarn('[bili] LIVE open room=' + item.roomid + ' title=' + (item.title || ''));
        await this.onPlayItem(item);
        ensure(this.play.state === 'playing', '直播起播失败: ' + this.play.note);
        ensure(this.play.session.live === true, '会话未标记为 live');
        const w = await this.waitForFrames(45000);
        ensure(w.st.ok && w.st.frames > 0, '45s 内无帧: ' + (w.st && (w.st.message || w.st.state)));
        logWarn('[bili] LIVE AUTOTEST PASS first frame ' + w.ms + 'ms frames=' + w.st.frames + ' pos=' + w.st.positionMs);
        // 取证窗口：停在播放态 15s，便于外部 adb dump /dev/fb0 抓画面（自检本身不改画面）
        await this.sleep(holdMs > 0 ? holdMs : 15000);
        logWarn('[bili] LIVE AUTOTEST end, back to list');
        await this.onBack();
      } catch (e) {
        logWarn('[bili] LIVE AUTOTEST FAIL: ' + ((e && e.message) || e));
      }
    },
    async runAutotest(soakSec, soakBv) {
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
        const w1 = await this.waitForFrames(30000);
        ensure(w1.st.ok && w1.st.frames > 0, '30s 内无帧输出: ' + (w1.st.message || w1.st.state));
        logWarn('[bili] AUTOTEST first frame ' + w1.ms + 'ms frames=' + w1.st.frames + ' pos=' + w1.st.positionMs);

        await this.onTogglePlay();
        ensure(this.play.state === 'paused', '暂停失败: ' + this.play.note);
        await this.sleep(800);
        await this.onTogglePlay();
        ensure(this.play.state === 'playing', '继续失败: ' + this.play.note);

        // 评论面板开关回归 + 截图取证窗口（停留 7s，便于外部 captureFB 取图）
        this.openComments();
        await this.sleep(7000);
        ensure(this.commentsOpen === true, '评论面板未打开');
        ensure(this.replies.length > 0 || this.replyLoading, '评论面板打开后无评论数据');
        this.closeComments();
        await this.sleep(500);
        ensure(this.commentsOpen === false, '评论面板未关闭');

        /* 位置断言用"相对量"而非绝对值：onSeek 内部要等帧恢复（seek=重启解码+探流，弱网/重稿件
         * 下实测可达 15s+），这期间画面仍在前进 → 绝对位置天然会偏大（旧断言 <6000ms 会把
         * "恢复慢"误判成"定位错"）。容差 6s 覆盖恢复期前进量。 */
        const posBeforePlus = this.play.positionMs;
        await this.onSeek(this.seekStepSec);
        await this.sleep(1200);
        ensure(
          this.play.positionMs >= posBeforePlus + 14000,
          'seek+' + this.seekStepSec + 's 位置异常: ' + posBeforePlus + '→' + this.play.positionMs
        );
        const posBeforeMinus = this.play.positionMs;
        await this.onSeek(-this.seekStepSec);
        // seek 返回即目标位置（native 直给）→ 立即断位置；再等帧恢复（seek=重启解码）
        ensure(
          this.play.positionMs <= Math.max(6000, posBeforeMinus - 14000),
          'seek-' + this.seekStepSec + 's 位置异常: ' + posBeforeMinus + '→' + this.play.positionMs
        );
        // v2.2.5：seek 恢复窗口 12s → 25s。seek=重启解码+重新探流，弱网/CDN 劣化时实测可超 12s
        // （探流阶段等 probesize 就够 60s+）→ 12s 会把"网络慢"误判成"定位失败"（真机连续踩过）。
        const w2 = await this.waitForFrames(25000);
        ensure(w2.st.ok && w2.st.frames > 0, 'seek 后 25s 帧未恢复: frames=' + (w2.st && w2.st.frames));
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

        /* —— v2.4.0 音画对齐长播验证（seed soak 秒，缺省 0=跳过）：复播推荐第一个视频持续播放。
         * poll tick 每秒带 skip=/drift=/rs= 落设备日志；这里每 5s 采样，每 30s 打一条检查点，
         * 结束输出漂移上界与强制重同步次数。通过线 |drift|≤1500ms——v2.2.7 起 native AV_RESYNC_MS
         * 为 1000ms（音频最多超前画面 1s 就强制重同步；提示频率已由"快速重启不弹提示"解决），
         * 故采样点合法上界 = 1000 + 锚偏移 ~42ms + 采样裕量。
         * 另：重同步窗口本身与其后 10s 追赶瞬态不计入判据（那是设计内的"跳到同步 + 快进吸收"）。 */
        if (soakSec > 0) {
          await this.onPlayItem(soakBv ? { bvid: soakBv, title: 'SOAK ' + soakBv, up: '', cover: '', durationSec: 0 } : this.rcmdItems[0]);
          ensure(this.play.state === 'playing', 'soak 起播失败: ' + this.play.note);
          const wsf = await this.waitForFrames(30000);
          ensure(wsf.st.ok && wsf.st.frames > 0, 'soak 30s 内无帧: ' + (wsf.st.message || wsf.st.state));
          logWarn('[bili] AUTOTEST SOAK start ' + soakSec + 's bv=' + (this.play.bvid || ''));
          const t0 = Date.now();
          let maxDrift = 0;
          let minDrift = 0;
          let skipBase = -1;
          let skipEnd = 0;
          let n = 0;
          let resyncEnd = 0;
          let seenResync = 0;
          let settleUntil = 0;
          while (Date.now() - t0 < soakSec * 1000 && this.mode === 'play' && this.play.state === 'playing') {
            await this.sleep(5000);
            if (this.mode !== 'play' || this.play.state !== 'playing') break;
            const st = await readStatus(this.makeCtx(this.gen));
            if (!st.ok || st.state !== 'playing') continue;
            // 重同步窗口内 avDriftMs 是伪值（video_base 已前移到 apos+lead、frames=0 → 恒为 -lead），
            // 计入 min/max 会把判据打成假 FAIL；只统计稳态漂移。
            if (st.resyncing) continue;
            // 重同步后的追赶瞬态同样不算：强制重同步是"跳到同步点 + 平滑快进吸收残余"，
            // 首帧落屏到残余排空之间 drift 会短暂偏高（实测可达 ~1.8s），这是设计内的，
            // 稳态判据只看排空之后。每次 resyncCount 增加即开 10s 稳定窗口。
            if ((st.resyncCount || 0) !== seenResync) {
              seenResync = st.resyncCount || 0;
              settleUntil = Date.now() + 10000;
            }
            if (Date.now() < settleUntil) continue;
            const d = st.avDriftMs || 0;
            if (d > maxDrift) maxDrift = d;
            if (d < minDrift) minDrift = d;
            if (skipBase < 0) skipBase = st.videoSkips || 0;
            skipEnd = st.videoSkips || 0;
            resyncEnd = st.resyncCount || 0;
            n++;
            const el = Math.round((Date.now() - t0) / 1000);
            if (el % 30 < 5) {
              logWarn(
                '[bili] SOAK t=' + el + 's pos=' + st.positionMs + ' drift=' + d + 'ms skips=' + st.videoSkips +
                ' rs=' + (st.resyncCount || 0)
              );
            }
          }
          const over = Math.max(Math.abs(maxDrift), Math.abs(minDrift)) > 1500;
          logWarn(
            '[bili] AUTOTEST SOAK ' + (over ? 'FAIL' : 'PASS') +
            ' n=' + n + ' drift=[' + minDrift + ',' + maxDrift + ']ms' +
            ' skips=+' + (skipEnd - (skipBase < 0 ? skipEnd : skipBase)) +
            ' resyncs=' + resyncEnd +
            (over ? '（|drift| 超 ±1500ms）' : '')
          );
          if (this.mode === 'play') await this.onBack();
        }
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
