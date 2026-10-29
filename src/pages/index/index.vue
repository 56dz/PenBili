<template>
  <!-- page-live 只在「视频像素真的在 fb 上」时启用透明背景（见 base.less 注释）；
       未起播/起播失败/已播完时必须用不透明底色，否则会把上一屏的 fb 残留像素透出来。 -->
  <div :class="holeVisible ? 'page page-live' : 'page'">
    <!-- 浏览态左栏：只有四个标签，不再有 PenBili 字样（用户要求删掉，把高度还给内容） -->
    <div v-if="mode === 'browse'" class="bar">
      <!-- 软件 logo（用户要求 m07040-1：首页左上角加软件标志） -->
      <image class="bar-logo" :src="ic.logo" :width="30" :height="30"></image>
      <div
        v-for="t in tabs"
        :key="t.id"
        :class="tab === t.id ? 'tab tab-on' : 'tab'"
        @click="selectTab(t.id)"
      >
        <text :class="tab === t.id ? 'tab-text tab-text-on' : 'tab-text'">{{ t.label }}</text>
      </div>
      <text class="bar-status">{{ statusText }}</text>
    </div>

    <!-- 播放/图文态左栏：纯图标工具条（参考 iPad 版 B站 播放页）
         点赞/投币/收藏在图标右侧带全站计数，激活后整体变粉（用户要求 m07040）。 -->
    <div v-else class="ibar">
      <div class="ibtn" @click="onIbarBack">
        <image class="ibtn-icon" :src="ic.back" :width="26" :height="26"></image>
      </div>
      <template v-if="mode === 'play'">
        <!-- 用户要求 m09254：点赞/投币/收藏**放大**、数量移到**图标下方**；
             左栏不再保留评论区按钮（评论区已是右栏默认态）。 -->
        <div class="ibtn ibtn-big" @click="onToggleLike">
          <image class="ibtn-icon ibtn-icon-big" :src="act.like ? ic.likeOn : ic.like" :width="32" :height="32"></image>
          <text :class="act.like ? 'ibtn-count ibtn-count-on' : 'ibtn-count'">{{ formatCount(act.likeCount) }}</text>
        </div>
        <div class="ibtn ibtn-big" @click="onAddCoin">
          <image class="ibtn-icon ibtn-icon-big" :src="act.coin ? ic.coinOn : ic.coin" :width="32" :height="32"></image>
          <text :class="act.coin ? 'ibtn-count ibtn-count-on' : 'ibtn-count'">{{ formatCount(act.coinCount) }}</text>
        </div>
        <div class="ibtn ibtn-big" @click="openFavOverlay">
          <image class="ibtn-icon ibtn-icon-big" :src="act.fav ? ic.starOn : ic.star" :width="32" :height="32"></image>
          <text :class="act.fav ? 'ibtn-count ibtn-count-on' : 'ibtn-count'">{{ formatCount(act.favCount) }}</text>
        </div>
        <div class="ibtn" @click="onTogglePlay">
          <image class="ibtn-icon" :src="play.state === 'playing' ? ic.pause : ic.play" :width="26" :height="26"></image>
        </div>
      </template>
      <template v-else>
        <div class="ibtn" @click="toggleArticleComments">
          <image class="ibtn-icon" :src="commentsOpen ? ic.commentOn : ic.comment" :width="28" :height="28"></image>
        </div>
      </template>
      <div class="info-spacer"></div>
      <text class="ibar-time">{{ ibarTimeCur }}</text>
      <text class="ibar-time-sub">{{ ibarTimeDur }}</text>
    </div>

    <!-- 浏览内容 -->
    <div v-if="mode === 'browse'" class="content">
      <!-- 推荐 / 热门：封面卡片网格 + 底部（刷新 | 加载更多） -->
      <scroller v-if="tab === 'rcmd' || tab === 'hot'" class="list">
        <div class="grid">
          <div v-for="it in currentItems" :key="it.bvid" class="vrow" @click="onPlayItem(it)">
            <image class="thumb" :src="it.cover" :width="166" :height="93"></image>
            <div class="vinfo">
              <text class="vtitle">{{ it.title }}</text>
              <text class="vmeta">{{ vmeta(it) }}</text>
            </div>
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
        <!-- 视频/图文/直播：同款封面卡片网格 -->
        <div class="grid">
        <template v-for="it in searchItems">
          <div v-if="it.kind === 'video'" :key="it.key" class="vrow" @click="onPlayItem(it)">
            <image class="thumb" :src="it.cover" :width="166" :height="93"></image>
            <div class="vinfo">
              <text class="vtitle">{{ it.title }}</text>
              <text class="vmeta">{{ vmeta(it) }}</text>
            </div>
          </div>
          <!-- 图文：同款卡片，进入图文详情（评论区与视频同族，专栏实测 type=12） -->
          <div v-else-if="it.kind === 'article'" :key="it.key" class="vrow" @click="openArticle(it)">
            <image class="thumb" :src="it.cover" :width="166" :height="93"></image>
            <div class="vinfo">
              <text class="vtitle">{{ it.title }}</text>
              <text class="vmeta">{{ it.up }}{{ it.view ? ' · 阅读 ' + formatCount(it.view) : '' }}{{ it.reply ? ' · 评论 ' + it.reply : '' }}</text>
            </div>
          </div>
          <!-- 直播：点击开播（本机 MPP 硬解，直连 B 站 FLV 源） -->
          <div v-else-if="it.kind === 'live'" :key="it.key" class="vrow" @click="onPlayItem(it)">
            <image class="thumb thumb-sq" :src="it.cover" :width="93" :height="93"></image>
            <div class="vinfo">
              <text class="vtitle">{{ it.title }}</text>
              <text class="vmeta">{{ it.up }}{{ it.online ? ' · 人气 ' + formatCount(it.online) : '' }}{{ it.cate ? ' · ' + it.cate : '' }}</text>
            </div>
          </div>
          </template>
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
            <text class="mine-name">{{ profile ? profile.uname : '未登录' }}</text>
            <text class="mine-id">{{ profile ? ('Lv.' + profile.level + ' · mid ' + profile.mid + (profile.vipType ? ' · 大会员' : '')) : '登录后个性化推荐 · 身份持久保存' }}</text>
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
        <div v-if="history.length" class="grid">
          <div v-for="h in history" :key="'h' + h.bvid" class="vrow" @click="onPlayItem(h)">
            <image class="thumb" :src="h.cover" :width="166" :height="93"></image>
            <div class="vinfo">
              <text class="vtitle">{{ h.title }}</text>
              <text class="vmeta">{{ h.up }} · {{ historyTime(h.at) }}</text>
            </div>
          </div>
        </div>
        <div v-if="!history.length" class="mine-row">
          <text class="mine-label">还没有播放记录</text>
        </div>
        <div class="mine-secrow">
          <text class="mine-sec">直播</text>
        </div>
        <div class="mine-row">
          <text class="mine-label">取流方式</text>
          <text class="mine-value">本地直连 · 芯片硬解</text>
        </div>
        <div class="mine-row">
          <text class="mine-label">清晰度</text>
          <text class="mine-value">720p（未登录上限）</text>
        </div>
        <text class="mine-notice">直播由本机 MPP 硬解，不再经云端转码；搜索「直播」栏即可开播。</text>
        <div class="mine-row">
          <text class="mine-label">PenBili</text>
          <text class="mine-value">v3.0.0 · {{ profile ? '已登录' : '匿名' }}</text>
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

    <!-- 播放态：图标栏与视频区之间的 16px 间距（用户要求 m06919-4 增大间距）。
         必须是**不透明占位块**：播放态根容器 page-live 是透明的（为了不让全页重绘盖掉
         fb 上的视频像素），这段空隙若无元素覆盖就会把上一屏的 fb 残留像素透出来
         —— 真机实测表现为左上角 x=64..79 一块彩色噪点。 -->
    <div v-if="mode === 'play'" class="ibar-gap"></div>

    <!-- 播放态：中列视频（KMS 覆盖平面，UI 画不上去）+ 下方可拖动进度条 -->
    <div v-if="mode === 'play'" class="stage">
      <div class="video-box">
        <hole v-if="holeVisible" class="video-hole"></hole>
        <div v-else class="play-idle">
          <text class="play-idle-text">{{ idleText }}</text>
        </div>
      </div>
      <div
        class="seek"
        @touchstart="onSeekTouch"
        @touchmove="onSeekTouch"
        @touchend="onSeekRelease"
      >
        <div class="seek-track">
          <div class="seek-fill" :style="{ width: seekFillW + 'px' }"></div>
          <div class="seek-knob"></div>
        </div>
      </div>
    </div>

    <!-- 播放态右栏：默认评论区（用户要求 m07040-5：不要选项栏）；
         点收藏按钮时整栏被收藏夹选择覆盖，上方有退出按钮，退出恢复评论区。 -->
    <div v-if="mode === 'play'" class="side">
      <!-- 收藏夹覆盖层 -->
      <template v-if="favOverlay">
        <div class="fav-head">
          <div class="fav-exit" @click="closeFavOverlay">
            <text class="fav-exit-text">‹ 退出</text>
          </div>
          <text class="fav-title">选择收藏夹</text>
        </div>
        <div class="side-body">
          <scroller class="side-scroll">
            <div
              v-for="f in favFolders"
              :key="'f' + f.id"
              :class="f.has ? 'favrow-sel' : 'favrow'"
              @click="onFavPick(f)"
            >
              <text :class="f.has ? 'favrow-name-sel' : 'favrow-name'">{{ f.title }}</text>
              <text :class="f.has ? 'favrow-count-sel' : 'favrow-count'">{{ f.count }}</text>
            </div>
            <text v-if="favStatus" class="side-status">{{ favStatus }}</text>
          </scroller>
        </div>
      </template>
      <!-- 评论区（默认） -->
      <template v-else>
        <div class="side-body side-body-full">
          <scroller class="side-scroll">
            <div class="sc-writeline">
              <div class="reply-write" @click="onWriteReply">
                <text class="reply-write-text">写评论</text>
              </div>
              <text v-if="!hasLogin" class="sc-login-hint">未登录 · 只能查看</text>
            </div>
            <text v-if="replyStatus" class="reply-status">{{ replyStatus }}</text>
            <div v-for="rp in replies" :key="'rp' + rp.rpid" class="reply-block reply-block-n">
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
        <!-- 操作反馈（点赞/投币/收藏结果、进度条调试），同时兼作播放提示 -->
        <text class="side-foot">{{ act.status || play.note }}</text>
      </template>
    </div>

    <!-- 图文详情：中列正文（896 宽，评论打开时被评论面板替换） -->
    <div v-if="mode === 'article' && !commentsOpen" class="astage">
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
            :width="880"
            :height="article.coverDh || 160"
            :style="{ width: '880px', height: (article.coverDh || 160) + 'px' }"
          ></image>
          <text class="art-title">{{ article.title }}</text>
          <text class="art-author">{{ article.author }}{{ article.read ? ' · 阅读 ' + formatCount(article.read) : '' }}{{ article.like ? ' · 点赞 ' + formatCount(article.like) : '' }}</text>
          <template v-for="(b, bi) in article.blocks">
            <image
              v-if="b.t === 'img'"
              :key="'ab' + bi"
              class="art-img"
              :src="b.src"
              :width="b.dw || 880"
              :height="b.dh || 240"
              :style="{ width: (b.dw || 880) + 'px', height: (b.dh || 240) + 'px' }"
            ></image>
            <text v-else :key="'ab' + bi" class="art-p">{{ b.text }}</text>
          </template>
          <text class="art-end">— 全文完 —</text>
        </template>
      </scroller>
    </div>

    <!-- 图文评论面板（896 宽，盖住正文区） -->
    <div v-if="mode === 'article' && commentsOpen" class="comments-col">
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
import { fetchViewState, fetchFavFolders, setLike, addCoin, dealFav } from '../../services/bili/action.js';
// 图标：本地 PNG 走 aiot-vue-cli 的 image 插件 → 产物 images/<md5>.png（见 rollup-plugins/image.js）。
// 不用 falcon-ui 的 icon 组件：它依赖 font-family: falcon-icons，但包内没有该字体文件（渲染为空）。
import icHome from '../../assets/icons/home.png';
import icLike from '../../assets/icons/like.png';
import icLikeOn from '../../assets/icons/like_on.png';
import icCoin from '../../assets/icons/coin.png';
import icCoinOn from '../../assets/icons/coin_on.png';
import icStar from '../../assets/icons/star.png';
import icStarOn from '../../assets/icons/star_on.png';
import icComment from '../../assets/icons/comment.png';
import icCommentOn from '../../assets/icons/comment_on.png';
import icPlay from '../../assets/icons/play.png';
import icPause from '../../assets/icons/pause.png';
import icBack from '../../assets/icons/back.png';
import icLogo from '../../assets/icons/logo.png';
import { fetchArticle } from '../../services/bili/article.js';
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
//   video/article/live 三栏本版全部可用。live 直连 B 站 FLV 源、本机 MPP 硬解
//   （见 services/bili/live.js）；未开播时播放页会给出「主播未开播」提示。
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
      commentsOpen: false,
      // 播放页右栏：默认只显示评论区；点收藏时整栏被收藏夹选择覆盖（favOverlay=true）
      favOverlay: false,
      // 互动（点赞/投币/收藏）状态与结果反馈
      act: { like: 0, coin: 0, fav: false, status: '', busy: false, likeCount: 0, coinCount: 0, favCount: 0 },
      favFolders: [],
      favStatus: '',
      // 可拖动进度条：拖动中的临时位置（null=未拖动，跟随播放）
      seekDragMs: null
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
    },
    // 图标表（模板里按状态选白/粉两版）
    ic() {
      return {
        home: icHome,
        like: icLike,
        likeOn: icLikeOn,
        coin: icCoin,
        coinOn: icCoinOn,
        star: icStar,
        starOn: icStarOn,
        comment: icComment,
        commentOn: icCommentOn,
        play: icPlay,
        pause: icPause,
        back: icBack,
        logo: icLogo
      };
    },
    // 图标栏底部小屏显：图文态显示「图文」，播放态显示进度。
    // 必须拆成两行——64px 宽 10px 字号装不下 "00:08 / 08:54" 13 个字符，会折成
    // "00:08 / 08:5" + "4"（真机实测）。
    ibarTimeCur() {
      if (this.mode === 'article') return '图文';
      const s = this.play.session;
      return formatDuration((s ? this.play.positionMs || 0 : 0) / 1000);
    },
    ibarTimeDur() {
      if (this.mode === 'article') return '';
      const s = this.play.session;
      return s ? formatDuration((s.durationMs || 0) / 1000) : '';
    },
    // 进度条填充宽（px）：轨道 432，圆点 10，二者同排 → 填充最多 422
    seekFillW() {
      const s = this.play.session;
      const dur = s ? s.durationMs || 0 : 0;
      if (!(dur > 0)) return 0;
      const ms = this.seekDragMs == null ? this.play.positionMs : this.seekDragMs;
      const r = Math.max(0, Math.min(1, ms / dur));
      return Math.round(406 * r);
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
      const sess = await loadSession();
      this.session = sess;
      if (sess.buvid3) {
        // 仅供诊断页/日志使用；「我的」页不展示这种短码，未登录时给用户看得懂的引导文案
        this.buvidShort = sess.buvid3;
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
      // 换片即重置右栏与互动态（避免上一条的点赞数/收藏夹残留）
      this.favOverlay = false;
      this.act = { like: 0, coin: 0, fav: false, status: '', busy: false, likeCount: 0, coinCount: 0, favCount: 0 };
      this.favFolders = [];
      this.favStatus = '';
      this.seekDragMs = null;
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
      this.refreshAct();  // 点赞/投币/收藏当前态 + 计数徽标
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
    /* 播放态右栏只有评论区；点收藏按钮时用收藏夹覆盖层顶掉它（m07040-5）。
       覆盖层不遮挡视频（视频在 KMS 平面），切换时不暂停 render。 */
    openComments() {
      if (this.mode === 'play') {
        this.favOverlay = false;
        this.ensureReply();
        return;
      }
      if (this.commentsOpen) return;
      const ctx = this.replyCtx || { oid: 0, type: 1 };
      const ready = ctx.oid > 0;
      // 面板先开、再补数据：以前这里是「无 oid 直接 return」，而 oid 只在 openVideo **成功**路径
      // 才会被写入 → 播放器起不来（如 X6 Pro 的 PLAYER_NATIVE_MISSING）时评论区成了死路，
      // 表现是点按钮毫无反应。现在改为：面板打开 + 若无 oid 用 bvid 补一次 view 拿 aid。
      this.commentsOpen = true;
      if (ready) return;
      this.resolveReplyCtx();
    },
    // 评论 oid 兜底解析：feed 层不带 aid，正常靠 openVideo 成功时回传的 session.aid；
    // 播放失败 / 直播 / 图文异常时这里按 bvid 现取一次（x/web-interface/view 已在播放链用过，有缓存）。
    async resolveReplyCtx() {
      const bvid = (this.play.session && this.play.session.bvid) || '';
      if (!/^BV[0-9A-Za-z]{10}$/.test(bvid)) {
        this.replyLoading = false;
        this.replyStatus = '评论暂不可用（拿不到稿件号）';
        return;
      }
      const myGen = this.gen;
      this.replyLoading = true;
      this.replyStatus = '评论加载中…';
      let v = { ok: false };
      try {
        v = await this.client.fetchView(bvid);
      } catch (e) {
        logWarn('[bili] replyCtx view fail: ' + (e && e.message));
      }
      if (this.gen !== myGen) return;
      const aid = (v && v.ok && v.aid) || 0;
      if (!(aid > 0)) {
        this.replyLoading = false;
        this.replyStatus = '评论加载失败（拿不到稿件号）';
        return;
      }
      this.replyCtx = { oid: aid, type: 1 };
      logWarn('[bili] replyCtx resolved aid=' + aid + ' bvid=' + bvid);
      await this.loadReplies(aid, 1); // loadReplies 内部会自己置 loading 状态
    },
    closeComments() {
      if (!this.commentsOpen) return;
      this.commentsOpen = false;
    },

    /* ---------- 左图标栏 ---------- */
    // 返回：播放态走 onBack（停会话回列表），图文态走 onBackArticle（回搜索结果）
    onIbarBack() {
      if (this.mode === 'article') this.onBackArticle();
      else this.onBack();
    },
    // 图文态图标栏的评论开关
    toggleArticleComments() {
      if (this.commentsOpen) this.closeComments();
      else this.openComments();
    },

    /* ---------- 播放页右栏：评论区 + 收藏夹覆盖层 ---------- */
    // 打开收藏夹覆盖层（点左栏收藏按钮）
    openFavOverlay() {
      this.favOverlay = true;
      this.loadFavFolders();
    },
    // 退出收藏夹覆盖层 → 回到评论区
    closeFavOverlay() {
      this.favOverlay = false;
      this.ensureReply();
    },
    // 当前稿件 aid：优先播放会话回传的，其次评论上下文（起播失败时靠 bvid 补解析过）
    playAid() {
      const s = this.play.session || {};
      const a = Math.floor(Number(s.aid) || 0);
      if (a > 0) return a;
      return Math.floor(Number(this.replyCtx && this.replyCtx.oid) || 0);
    },
    // 评论区就位：已有 oid 直接拉，没有就走 bvid → view 兜底解析
    async ensureReply() {
      const ctx = this.replyCtx || { oid: 0, type: 1 };
      if (ctx.oid > 0) {
        if (!this.replies.length && !this.replyLoading) await this.loadReplies(ctx.oid, ctx.type);
        return;
      }
      await this.resolveReplyCtx();
    },
    // csrf（bili_jct）：点赞/投币/收藏都要带
    async csrfToken() {
      try {
        const sess = await loadSession();
        return (sess && sess.login && sess.login.bili_jct) || '';
      } catch (e) {
        return '';
      }
    },
    // 互动状态回读（点赞/投币/收藏夹选中态）
    async refreshAct() {
      const aid = this.playAid();
      if (!(aid > 0)) return;
      const r = await fetchViewState(this.client, aid);
      if (!r.ok) return;
      this.act.like = r.like;
      this.act.coin = r.coin;
      this.act.fav = !!r.favorite;
      // 左栏计数徽标（全站总数，不是本人状态）
      this.act.likeCount = r.likeCount || 0;
      this.act.coinCount = r.coinCount || 0;
      this.act.favCount = r.favCount || 0;
    },
    async onToggleLike() {
      if (this.act.busy) return;
      const aid = this.playAid();
      if (!(aid > 0)) {
        this.act.status = '拿不到稿件号';
        return;
      }
      const csrf = await this.csrfToken();
      if (!csrf) {
        this.act.status = '需要登录后才能点赞';
        return;
      }
      const on = !(this.act.like > 0);
      this.act.busy = true;
      const r = await setLike(this.client, aid, on, csrf);
      this.act.busy = false;
      if (!r.ok) {
        this.act.status = '点赞失败：' + r.message;
        logWarn('[bili] like fail stage=' + r.stage + ' code=' + r.code);
        return;
      }
      this.act.like = on ? 1 : 0;
      this.act.likeCount = Math.max(0, (this.act.likeCount || 0) + (on ? 1 : -1));
      this.act.status = on ? '已点赞' : '已取消赞';
      logWarn('[bili] like → ' + this.act.like);
    },
    async onAddCoin() {
      if (this.act.busy) return;
      const aid = this.playAid();
      if (!(aid > 0)) {
        this.act.status = '拿不到稿件号';
        return;
      }
      if (this.act.coin >= 2) {
        this.act.status = '已投满 2 枚';
        return;
      }
      const csrf = await this.csrfToken();
      if (!csrf) {
        this.act.status = '需要登录后才能投币';
        return;
      }
      this.act.busy = true;
      const r = await addCoin(this.client, aid, 1, false, csrf);
      this.act.busy = false;
      if (!r.ok) {
        this.act.status = '投币失败：' + r.message;
        logWarn('[bili] coin fail stage=' + r.stage + ' code=' + r.code);
        return;
      }
      this.act.coin = Math.min(2, this.act.coin + 1);
      this.act.coinCount = (this.act.coinCount || 0) + 1;
      this.act.status = '已投 ' + this.act.coin + ' 枚';
      logWarn('[bili] coin → ' + this.act.coin);
    },
    async loadFavFolders() {
      const aid = this.playAid();
      if (!(aid > 0)) {
        this.favStatus = '拿不到稿件号';
        return;
      }
      const mid = (this.profile && this.profile.mid) || 0;
      if (!(mid > 0)) {
        this.favStatus = '需要登录后才能收藏';
        return;
      }
      this.favStatus = '加载中…';
      const myGen = this.gen;
      // 收藏夹列表的 fav_state 是「该夹是否含此稿件」——list-all 不带 rid 时恒为 0，
      // 所以选中态判断不了具体夹，这里按整稿收藏态（act.fav）兜底标注。
      const r = await fetchFavFolders(this.client, mid);
      if (this.gen !== myGen) return;
      if (!r.ok) {
        this.favStatus = '收藏夹加载失败：' + r.message;
        return;
      }
      this.favFolders = r.items;
      if (r.needLogin) this.favStatus = '需要登录后才能收藏';
      else if (!r.items.length) this.favStatus = '还没有收藏夹 · 请先在手机端创建';
      else this.favStatus = this.act.fav ? '已收藏 · 点一行可移出' : '点一行加入该收藏夹';
    },
    async onFavPick(f) {
      if (!f || this.act.busy) return;
      const aid = this.playAid();
      if (!(aid > 0)) return;
      const csrf = await this.csrfToken();
      if (!csrf) {
        this.favStatus = '需要登录后才能收藏';
        return;
      }
      this.act.busy = true;
      const r = await dealFav(this.client, aid, f.has ? [] : [f.id], f.has ? [f.id] : [], csrf);
      this.act.busy = false;
      if (!r.ok) {
        this.favStatus = '操作失败：' + r.message;
        logWarn('[bili] fav deal fail stage=' + r.stage + ' code=' + r.code);
        return;
      }
      // 用户要求 m09254：点击后该收藏夹行**变粉**（favrow-sel）+ **数量 +1**，作为添加成功的反馈。
      // 真机踩坑：falcon 的 QuickJS 运行时里 `this.$set(obj, k, v)` **不会触发重渲染**
      // （实测点了 4 次，日志 n 每次都 +1，但屏幕上的数字和背景色一直不动；
      //  只有退出再进覆盖层、重新 fetch 整个列表后数字才变）。
      // 所以这里改成**替换整个数组**（不可变更新）——引用变了，v-for 必然重渲染。
      const on = !f.has;
      this.favFolders = this.favFolders.map((x) => {
        if (x.id !== f.id) return x;
        return {
          id: x.id,
          title: x.title,
          count: Math.max(0, (Number(x.count) || 0) + (on ? 1 : -1)),
          isDefault: x.isDefault,
          has: on
        };
      });
      this.act.fav = this.favFolders.some((x) => x.has);
      this.act.favCount = Math.max(0, (this.act.favCount || 0) + (on ? 1 : -1));
      this.favStatus = (on ? '已加入「' : '已移出「') + f.title + '」';
      logWarn('[bili] fav ' + (on ? 'add' : 'del') + ' id=' + f.id);
    },

    /* ---------- 可拖动进度条 ---------- */
    // falcon 运行时只有 changedTouches[0].pageX / pageY（offsetX/clientX/touches 全为 0），
    // 没有 panstart/panmove，所以自己按逻辑坐标算比例。
    // 轨道几何固定：.seek 逻辑 x=80 起（图标栏 64 + 间隙 16）、左右 padding 8 → 轨道左端 88、宽 416。
    // 轨道几何：pageX 是「相对被触摸元素」的坐标，不是整页坐标——实测在 .seek
    // 内点逻辑 x=100 得到 pageX=37（偏移恒为 -63，即 -(.seek 逻辑 x)），点 420 得 357。
    // 所以减的是 .seek 的 8px 左 padding，不是 88（图标栏 64 + 间隙 16 + 8）。
    seekRatioAt(x) {
      if (!isFinite(x)) return null;
      return Math.max(0, Math.min(1, (x - 8) / 416));
    },
    onSeekTouch(e) {
      const t = (e && e.changedTouches && e.changedTouches[0]) || null;
      if (!t) return;
      const s = this.play.session;
      const dur = s ? s.durationMs || 0 : 0;
      if (!(dur > 0)) return;
      const ratio = this.seekRatioAt(Number(t.pageX));
      if (ratio == null) return;
      this.seekDragMs = Math.round(dur * ratio);
    },
    // touchend 必须再取一次触点：falcon 的 touchmove 在快速滑动时会被丢弃，
    // 实测从 x=100 拖到 x=420（应 80.6%）只收到第 16 步（66%），落点会偏。
    async onSeekRelease(e) {
      const t = (e && e.changedTouches && e.changedTouches[0]) || null;
      const s = this.play.session;
      const dur = s ? s.durationMs || 0 : 0;
      if (t && dur > 0) {
        const ratio = this.seekRatioAt(Number(t.pageX));
        if (ratio != null) this.seekDragMs = Math.round(dur * ratio);
      }
      const target = this.seekDragMs;
      this.seekDragMs = null;
      if (target == null) return;
      await this.seekTo(target);
    },
    // 绝对定位（拖动结束/点击轨道时用）：复用 seekBy 的相对位移
    async seekTo(targetMs) {
      const s = this.play.session;
      const st = this.play.state;
      if (!s || !(s.durationMs > 0)) {
        this.play.note = '时长未知，无法定位';
        return;
      }
      if (st !== 'playing' && st !== 'paused') {
        this.play.note = '尚未起播';
        return;
      }
      const dur = s.durationMs;
      const goal = Math.max(0, Math.min(dur - 1000, Math.floor(Number(targetMs) || 0)));
      const myGen = this.gen;
      this.play.note = '定位中…';
      const r = await seekBy(this.makeCtx(myGen), this.play.positionMs, dur, goal - this.play.positionMs);
      if (this.gen !== myGen) return;
      if (!r.ok) {
        this.play.note = r.message;
        logWarn('[bili] seekTo fail: ' + r.message);
        return;
      }
      this.play.positionMs = r.positionMs;
      this.play.note = '';
      logWarn('[bili] seekTo ' + goal + 'ms → pos=' + r.positionMs + 'ms');
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
        // cell=4px 时 49x49(+quiet 4×2)=57 格 = 228px，正好等于 qr-box 的边长（X6 Pro 画布 266 高）
        // 更大版本需另调 cell
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
      this.teardown('');
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
    // 只跑直播链路 —— 直播栏搜索 → 取 FLV 直链 → 播放 → 首帧断言 → 返回。
    // 真机证据链：设备日志出现 `live playinfo streams=2` 与 `stream LIVE`（证明直连取流成功）。
    async runLiveAutotest(room, holdMs) {
      const ensure = (cond, msg) => {
        if (!cond) throw new Error(msg);
      };
      try {
        logWarn('[bili] LIVE AUTOTEST start' + (room ? ' room=' + room : ' (搜首个直播间)'));
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

        // 右栏回归 + 截图取证窗口（每态停留 3s，便于外部 capture 取图）
        // v3.0.0：播放态右栏只剩「评论区」，收藏夹改为覆盖层（favOverlay）。
        this.closeFavOverlay();
        await this.sleep(3000);
        ensure(!this.favOverlay, '收藏夹覆盖层未关闭');
        ensure(this.replies.length > 0 || this.replyLoading || !!this.replyStatus, '评论区无数据也无提示');
        logWarn('[bili] AUTOTEST side comments n=' + this.replies.length);
        this.openFavOverlay();
        await this.sleep(3000);
        ensure(this.favOverlay, '收藏夹覆盖层未打开');
        ensure(this.favFolders.length > 0 || !!this.favStatus, '收藏夹覆盖层无数据也无提示');
        logWarn('[bili] AUTOTEST fav overlay n=' + this.favFolders.length + ' st=' + this.favStatus);
        this.closeFavOverlay();
        await this.sleep(500);

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

        // 图文详情：切到「图文」分栏 → 打开首条 → 断言正文块渲染 → 返回搜索结果。
        // 这条路径没有别的程序化入口（输入框需人工），只能靠自检覆盖。
        this.setSearchType('article');
        const artDeadline = Date.now() + 15000;
        while (!this.searchResults.article.length && Date.now() < artDeadline) await this.sleep(300);
        ensure(this.searchResults.article.length > 0, '图文搜索结果为空: ' + this.statusText);
        const artItem = this.searchResults.article[0];
        await this.openArticle(artItem);
        ensure(this.mode === 'article', '图文详情未打开');
        const artBody = Date.now() + 20000;
        while (!(this.article.blocks && this.article.blocks.length) && this.article.state !== 'error' && Date.now() < artBody) {
          await this.sleep(300);
        }
        ensure(this.article.blocks && this.article.blocks.length > 0, '图文正文为空: ' + this.article.note);
        logWarn('[bili] AUTOTEST article aid=' + this.article.aid + ' blocks=' + this.article.blocks.length);
        // 图文页取证窗口：外部 capture 取图用（3s 足够一次 adb pull）
        await this.sleep(3000);
        this.onBackArticle();
        ensure(this.mode === 'browse' && this.tab === 'search', '图文返回失败');
        this.setSearchType('video');
        await this.sleep(300);

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
