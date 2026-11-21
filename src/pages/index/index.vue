<template>
  <!-- page-live 只在「视频像素真的在 fb 上」时启用透明背景（见 base.less 注释）；
       未起播/起播失败/已播完时必须用不透明底色，否则会把上一屏的 fb 残留像素透出来。 -->
  <div :class="holeVisible && !upd.visible ? 'page page-live' : 'page'">
    <!-- 检查更新（v3.5.5）：弹窗不是浮层而是**整页替换**。
         falcon 运行时 position:absolute 不可靠、全仓没有一个 z-index 能用（base.less
         文件头铁律），所以靠 upd.visible 把正常视图整体不渲染 —— 零定位依赖，
         也就不会出现「弹窗和列表叠在一起」。 -->
    <div v-if="upd.visible" class="upd">
      <div class="upd-card">
      <div class="upd-left">
        <div class="upd-head">
          <text class="upd-title">{{ updHead }}</text>
          <!-- 版本徽标与包大小只在「真的有一个待装的包」时才有意义：
               已是最新 / 检查失败这两种状态下标题里已经有版本号，再挂一枚
               同样的 v3.5.5 徽标就是自己跟自己重复，右上也挤。 -->
          <text v-if="updShowMeta && upd.release" class="upd-ver">{{ upd.release.tag }}</text>
          <text v-if="updShowMeta && updSizeText" class="upd-size">{{ updSizeText }}</text>
        </div>
        <text class="upd-sub">{{ upd.message }}</text>
        <!-- 更新说明可上下滑动：发布说明经常上千字，266px 屏只放得下十来行。
             scroller 内每行必须是独立 <text> —— falcon 的 <text> 内容里出现换行就
             整块不渲染，所以 bodyLines() 先把 Markdown 切成行数组。 -->
        <scroller class="upd-scroll">
          <template v-for="(ln, i) in upd.lines">
            <text v-if="ln" :key="'ul' + i" class="upd-line">{{ ln }}</text>
            <div v-else :key="'ug' + i" class="upd-gap"></div>
          </template>
          <text v-if="!upd.lines.length" class="upd-line">（本版本没有填写更新说明）</text>
        </scroller>
      </div>
      <div class="upd-right">
        <!-- 进入软件时自动检测：4 个按钮 -->
        <template v-if="upd.mode === 'auto' && upd.stage === 'ready'">
          <div class="upd-btn upd-btn-main" @click="onUpdInstall(false)">
            <text class="upd-btn-t-main">立即更新（直连 GitHub）</text>
          </div>
          <div class="upd-btn upd-btn-main" @click="onUpdInstall(true)">
            <text class="upd-btn-t-main">立即更新（使用镜像）</text>
          </div>
          <div class="upd-btn" @click="onUpdSkip">
            <text class="upd-btn-t">跳过此版本</text>
          </div>
          <div class="upd-btn" @click="onUpdQuiet">
            <text class="upd-btn-t">今天内不提示</text>
          </div>
        </template>
        <!-- 「我的」页手动检测：2 个按钮（下载源由 onUpdInstall 自动回退，不给用户选） -->
        <template v-else-if="upd.mode === 'manual' && upd.stage === 'ready'">
          <div class="upd-btn upd-btn-main" @click="onUpdInstall(false)">
            <text class="upd-btn-t-main">立即更新</text>
          </div>
          <div class="upd-btn" @click="onUpdClose">
            <text class="upd-btn-t">暂不更新</text>
          </div>
        </template>
        <!-- 其它阶段（检查中 / 已最新 / 下载中 / 安装中 / 已完成 / 失败）只有一个关闭 -->
        <template v-else>
          <div class="upd-btn" @click="onUpdClose">
            <text class="upd-btn-t">{{ updBusy ? '请稍候…' : '关闭' }}</text>
          </div>
          <div v-if="upd.release && upd.release.htmlUrl" class="upd-btn" @click="onUpdShowUrl">
            <text class="upd-btn-t">查看发布页</text>
          </div>
        </template>
        <text class="upd-tip">{{ updTip }}</text>
      </div>
      </div>
    </div>
    <!-- 正常视图整体包一层：让「整页替换」只改这一处 v-else，
         不必给下面 10 个顶层子节点各加一个 !upd.visible（漏一个就会和弹窗叠加）。 -->
    <div v-else class="page-body">
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

    <!-- 播放态左列（用户手绘稿 m11950）：上=视频标题/简介(192)，下=点赞/投币/收藏(74) -->
    <div v-if="mode === 'detail'" class="pleft">
      <div class="pleft-info">
        <div class="pback" @click="onIbarBack">
          <image class="pback-icon" :src="ic.back" :width="22" :height="22"></image>
        </div>
        <!-- 详情页显示完整标题（lines:2 省略），跑马灯只在全屏播放页用：
             详情页左列是信息卡，滚动窗口只会让人看到标题中段，读不出是什么稿。 -->
        <text class="ptitle">{{ infoTitle }}</text>
        <!-- UP 名（蓝色）：点进 UP 主页（用户要求「点详情页蓝色UP名跳转」）。
             用 div 承接点击 —— falcon 的 <text> 不保证派发 click，外层容器最稳。 -->
        <div v-if="infoUp || infoMid > 0" class="pup" @click="onOpenUp">
          <text class="pup-text">{{ infoUp || 'UP主页' }}{{ infoMid > 0 ? ' ›' : '' }}</text>
        </div>
        <!-- 简介必须可滚动（用户反馈：「播放界面出现长简介时，简介栏无法上下滑动，查看全部的」）。
             以前是 <text class="pdesc" lines:5>，超过 5 行直接省略号截断，用户没有任何办法看到全文。
             改成 scroller 后高度仍是 72px（192px 的左列预算是固定的，见 base.less .pleft-info
             注释），但内容可以上下滑动。内层 text 不能带 lines —— 带 lines 会在 scroller 里
             先被截断再滚，等于白滚。 -->
        <scroller class="pdesc">
          <text class="pdesc-text">{{ playDesc }}</text>
        </scroller>
        <!-- 播放提示（「定位中…」「加载中…」「音画不同步，正在恢复…」等）。
             为什么要单独一行：这些 note 以前只赋值给 this.play.note，**模板里从来没有渲染过**，
             用户完全看不到（P0-2 要求 2 的真实缺口）。而 .pvideo 里是 <hole>（透明挖孔，让 KMS
             视频平面透出来），不能把文字塞进 hole 里 —— hole 的职责就是"这里什么都没有"。
             所以提示落在左列信息卡最下面一行：播放态（holeVisible）同样看得见。
             这一行**恒定渲染**（无提示时是空串），不改几何尺寸，避免提示出现/消失时布局抖动；
             代价是简介从 7 行缩到 5 行（192px 高度是固定预算，见 base.less .pleft-info）。 -->
        <text class="pnote">{{ play.note }}</text>
      </div>
      <!-- 三个按钮（点赞/投币/收藏）直播态隐藏（用户反馈 m07570-3）。
           直播没有「收藏夹」语义，点赞/投币对房间也不适用，留着只会占掉 74px
           且点了没反馈。留空 div 保持 pleft 的 192+74 高度预算不变。 -->
      <div v-if="!isLivePlay" class="pleft-btns">
        <div class="pbtn" @click="onToggleLike">
          <image class="pbtn-icon" :src="act.like ? ic.likeOn : ic.like" :width="26" :height="26"></image>
          <text :class="act.like ? 'pbtn-count-on' : 'pbtn-count'">{{ formatCount(act.likeCount) }}</text>
        </div>
        <div class="pbtn" @click="onAddCoin">
          <image class="pbtn-icon" :src="act.coin ? ic.coinOn : ic.coin" :width="26" :height="26"></image>
          <text :class="act.coin ? 'pbtn-count-on' : 'pbtn-count'">{{ formatCount(act.coinCount) }}</text>
        </div>
        <div class="pbtn" @click="openFavOverlay">
          <image class="pbtn-icon" :src="act.fav ? ic.starOn : ic.star" :width="26" :height="26"></image>
          <text :class="act.fav ? 'pbtn-count-on' : 'pbtn-count'">{{ formatCount(act.favCount) }}</text>
        </div>
      </div>
    </div>

    <!-- 图文态左栏：纯图标工具条（评论开关）。
         注意这里必须写 v-else-if="mode === 'article'"，**不能写 v-else**：
         上面的兄弟节点条件是 v-if="mode === 'play'"，v-else 会接住所有非 play 的
         情况（包括 mode === 'browse'），于是首页也会渲染出返回箭头和评论图标。 -->
    <div v-else-if="mode === 'article'" class="ibar">
      <div class="ibtn" @click="onIbarBack">
        <image class="ibtn-icon" :src="ic.back" :width="26" :height="26"></image>
      </div>
      <div class="ibtn" @click="toggleArticleComments">
        <image class="ibtn-icon" :src="commentsOpen ? ic.commentOn : ic.comment" :width="28" :height="28"></image>
      </div>
      <div class="info-spacer"></div>
      <text class="ibar-time">{{ ibarTimeCur }}</text>
      <text class="ibar-time-sub">{{ ibarTimeDur }}</text>
    </div>

    <!-- 浏览内容 -->
    <div v-if="mode === 'browse'" class="content">
      <!-- 推荐 / 热门：封面卡片网格 + 底部（刷新 | 加载更多） -->
      <scroller v-if="tab === 'rcmd' || tab === 'hot'" class="list" @scroll="onListScroll(tab, $event)">
        <div class="grid">
          <div
            v-for="(it, idx) in currentItems"
            :key="it.bvid"
            :ref="'scr-' + tab + '-' + idx"
            class="vrow"
            @click="onOpenDetail(it)"
          >
            <image class="thumb" :src="it.cover" :width="427" :height="242"></image>
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
      <scroller v-else-if="tab === 'search'" class="list" @scroll="onListScroll('search', $event)">
        <div class="searchbar" ref="scr-search-top" @click="onOpenInput">
          <text class="search-kw">{{ keyword || '点此输入关键词搜索 B 站' }}</text>
          <div class="search-btn">
            <text class="search-btn-text">搜索</text>
          </div>
        </div>
        <!-- 搜索记录（用户要求）：只在没有关键词的空态显示，点条目直接复用关键词搜索 -->
        <div v-if="!keyword && searchHistory.length" class="sh-wrap">
          <div class="sh-head">
            <text class="sh-title">搜索记录</text>
            <div class="sh-clear" @click="onClearSearchHistory">
              <text class="sh-clear-text">清空</text>
            </div>
          </div>
          <div class="sh-list">
            <div v-for="h in searchHistory" :key="'sh' + h.kw" class="sh-item" @click="onHistorySearch(h.kw)">
              <text class="sh-text">{{ h.kw }}</text>
            </div>
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
        <template v-for="(it, idx) in searchItems">
          <div v-if="it.kind === 'video'" :key="it.key" :ref="'scr-search-' + idx" class="vrow" @click="onOpenDetail(it)">
            <image class="thumb" :src="it.cover" :width="427" :height="242"></image>
            <div class="vinfo">
              <text class="vtitle">{{ it.title }}</text>
              <text class="vmeta">{{ vmeta(it) }}</text>
            </div>
          </div>
          <!-- 图文：同款卡片，进入图文详情（评论区与视频同族，专栏实测 type=12） -->
          <div v-else-if="it.kind === 'article'" :key="it.key" :ref="'scr-search-' + idx" class="vrow" @click="openArticle(it)">
            <image class="thumb" :src="it.cover" :width="427" :height="242"></image>
            <div class="vinfo">
              <text class="vtitle">{{ it.title }}</text>
              <text class="vmeta">{{ it.up }}{{ it.view ? ' · 阅读 ' + formatCount(it.view) : '' }}{{ it.reply ? ' · 评论 ' + it.reply : '' }}</text>
            </div>
          </div>
          <!-- 直播：点击开播（本机 MPP 硬解，直连 B 站 FLV 源） -->
          <div v-else-if="it.kind === 'live'" :key="it.key" :ref="'scr-search-' + idx" class="vrow" @click="onOpenDetail(it)">
            <!-- 封面必须是 16:9 长方形（用户反馈：「搜索直播时，每个直播的封面图片是正方形，
                 而不是长方形」）。此前这里用 .thumb-sq 强制 93x93，与视频/图文/历史三栏的
                 166x93 不一致，视觉上突然出现一个方块。统一走 .thumb。 -->
            <image class="thumb" :src="it.cover" :width="427" :height="242"></image>
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

      <!-- 下载：离线缓存列表（文件在 /userdisk/PenBili/，元数据在 storage） -->
      <scroller v-else-if="tab === 'downloads'" class="list" @scroll="onListScroll('downloads', $event)">
        <div class="dl-head">
          <text class="dl-head-text">共 {{ downloads.length }} 个 · {{ dlTotalText }}</text>
          <text class="dl-head-dir">{{ dlDir }}</text>
        </div>
        <div v-if="downloads.length" class="grid">
          <div
            v-for="(it, idx) in downloads"
            :key="'dl' + it.bvid + it.cid"
            :ref="'scr-downloads-' + idx"
            class="vrow"
            @click="onPlayDownload(it)"
          >
            <image class="thumb" :src="it.cover" :width="427" :height="242"></image>
            <div class="vinfo">
              <text class="vtitle">{{ it.title }}</text>
              <text class="vmeta">{{ dlMeta(it) }}</text>
            </div>
          </div>
        </div>
        <div v-if="!downloads.length" class="empty">
          <text class="empty-text">暂无下载 · 播放页点「•••」→「缓存」即可离线保存</text>
        </div>
      </scroller>

      <!-- 历史：独立一页（用户要求，放在「下载」下面），最多 20 条 -->
      <scroller v-else-if="tab === 'history'" class="list" @scroll="onListScroll('history', $event)">
        <div class="dl-head">
          <text class="dl-head-text">共 {{ history.length }} 条 · 最多 20 条</text>
          <text class="dl-head-dir">点卡片重新打开</text>
        </div>
        <div v-if="history.length" class="grid">
          <div
            v-for="(h, idx) in history"
            :key="'h' + h.bvid"
            :ref="'scr-history-' + idx"
            class="vrow"
            @click="onOpenDetail(h)"
          >
            <image class="thumb" :src="h.cover" :width="427" :height="242"></image>
            <div class="vinfo">
              <text class="vtitle">{{ h.title }}</text>
              <text class="vmeta">{{ h.up }} · {{ historyTime(h.at) }}</text>
            </div>
          </div>
        </div>
        <div v-if="!history.length" class="empty">
          <text class="empty-text">暂无播放记录 · 看过的视频会出现在这里</text>
        </div>
      </scroller>

      <!-- 我的：登录态 + 设置入口；mineView='qr' 时切换为扫码视图 -->
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
        <!-- 视频调节：点整行轮转选项（falcon 无原生下拉，见 onCycleVideoQuality 注释） -->
        <div class="mine-secrow">
          <text class="mine-sec">视频调节</text>
        </div>
        <div class="mine-row" @click="onCycleVideoQuality">
          <text class="mine-label">画质</text>
          <text class="mine-value">{{ vqText() }} ›</text>
        </div>
        <div class="mine-row" @click="onCcToggle">
          <text class="mine-label">CC 字幕</text>
          <text class="mine-value">{{ cc.on ? '开' : '关' }} ›</text>
        </div>
        <div class="mine-row" @click="onToggleVideoZoom">
          <text class="mine-label">视频缩放</text>
          <text class="mine-value">{{ videoSettings.videoZoom ? '开 · 双指捏合' : '关' }} ›</text>
        </div>
        <div v-if="cc.on" class="mine-row" @click="onCcLanTap">
          <text class="mine-label">字幕语言</text>
          <text class="mine-value">{{ ccLanText }} ›</text>
        </div>
        <text class="mine-notice">{{ ccSettingHint }}</text>
        <div class="mine-secrow">
          <text class="mine-sec">直播</text>
        </div>
        <div class="mine-row" @click="onCycleLiveQuality">
          <text class="mine-label">画质</text>
          <text class="mine-value">{{ lqText() }} ›</text>
        </div>
        <div class="mine-row">
          <text class="mine-label">取流方式</text>
          <text class="mine-value">本地直连 · 芯片硬解</text>
        </div>
        <text class="mine-notice">点画质行可逐项切换（分辨率与帧率合一，不会出现服务端不存在的组合）。直播由本机 MPP 硬解，不经云端转码；未登录时服务端最高只给 480p。</text>
        <div class="mine-secrow">
          <text class="mine-sec">播放</text>
        </div>
        <!-- 历史已独立成左栏「历史」页（用户要求）；这里保留一个入口行 -->
        <div class="mine-row" @click="selectTab('history')">
          <text class="mine-label">播放历史</text>
          <text class="mine-value">{{ history.length ? history.length + ' 条 ›' : '暂无 ›' }}</text>
        </div>
        <!-- 我的下载（用户要求「下载页面放我的下面」）：与左栏「下载」标签同一页 -->
        <div class="mine-row" @click="selectTab('downloads')">
          <text class="mine-label">我的下载</text>
          <text class="mine-value">{{ downloads.length ? downloads.length + ' 个 · ' + dlTotalText : '暂无' }} ›</text>
        </div>
        <!-- 检查更新（用户需求 m14218-2）：「我的」页手动检测，点开弹窗只有 2 个按钮 -->
        <div class="mine-row" @click="onCheckUpdateTap">
          <text class="mine-label">检查更新</text>
          <text class="mine-value">v{{ appVersion }} ›</text>
        </div>
        <div class="mine-row">
          <text class="mine-label">PenBili</text>
          <text class="mine-value">v{{ appVersion }} · {{ profile ? '已登录' : '匿名' }}</text>
        </div>

        <!-- 关于：项目信息（GitHub / 开源协议 / 免责）。
             词典笔上没有能唤起浏览器的 JSAPI，所以 GitHub 地址同时给二维码，
             手机扫一下就能打开仓库——比让用户在 266px 高的屏上抄 URL 现实。 -->
        <div class="mine-secrow">
          <text class="mine-sec">关于</text>
        </div>
        <div class="mine-about">
          <div class="mine-about-text">
            <text class="mine-about-title">PenBili · 有道词典笔 B 站客户端</text>
            <text class="mine-about-line">S6 Pro 分支 · 作者 56dz · GPL-3.0</text>
            <text class="mine-about-line">github.com/noguimc/PenBili</text>
            <text class="mine-about-line">基于 Falcon 小程序运行时，视频由本机软解</text>
            <text class="mine-about-note">第三方客户端，与哔哩哔哩无隶属关系；所有内容版权归原作者及平台所有</text>
          </div>
          <div class="mine-about-qr">
            <div v-for="(row, ri) in aboutQrRows" :key="'aq' + ri" class="aq-row">
              <div
                v-for="(seg, si) in row"
                :key="'aqs' + si"
                :class="seg.dark ? 'aq-seg qr-dark ' + seg.cls : 'aq-seg ' + seg.cls"
              ></div>
            </div>
          </div>
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

    <!-- UP 主主页（用户要求「增加UP主页解析」）：左栏只有返回，内容区复用首页双列卡片 -->
    <div v-if="mode === 'up'" class="bar">
      <div class="tab tab-back" @click="onBackUp">
        <image class="tab-back-icon" :src="ic.back" :width="30" :height="30"></image>
      </div>
      <text class="bar-status">{{ upBarText }}</text>
    </div>
    <scroller v-if="mode === 'up'" class="list" @scroll="onListScroll('up', $event)">
      <div class="up-head">
        <text class="up-title">{{ upName }}</text>
        <text class="up-stat">{{ upStatText }}</text>
      </div>
      <text v-if="upSign" class="up-sign">{{ upSign }}</text>
      <div v-if="up.items.length" class="grid">
        <div
          v-for="(it, idx) in up.items"
          :key="'upv' + it.bvid"
          :ref="'scr-up-' + idx"
          class="vrow"
          @click="onOpenDetail(it)"
        >
          <image class="thumb" :src="it.cover" :width="427" :height="242"></image>
          <div class="vinfo">
            <text class="vtitle">{{ it.title }}</text>
            <text class="vmeta">{{ upVmeta(it) }}</text>
          </div>
        </div>
      </div>
      <div v-if="up.loading && !up.items.length" class="empty">
        <text class="empty-text">正在解析 UP 主页…</text>
      </div>
      <div v-else-if="!up.items.length" class="empty" @click="onRefreshUp">
        <text class="empty-text">{{ up.status ? up.status + ' · 点此重试' : '该 UP 暂无公开投稿' }}</text>
      </div>
      <div v-if="up.items.length" class="more-row">
        <div class="more-half" @click="onRefreshUp">
          <text class="more-text">刷新</text>
        </div>
        <div class="more-half" @click="loadMoreUp">
          <text class="more-text">{{ up.loading ? '加载中…' : (up.noMore ? '没有更多了' : '加载更多') }}</text>
        </div>
      </div>
    </scroller>

    <!-- 详情态中列（用户手绘稿）：黑底 + 16:9 大封面；点击封面才启动播放页 -->
    <div v-if="mode === 'detail'" class="pmid">
      <div class="detail-cover-wrap" @click="onStartPlayFromDetail">
        <image class="detail-cover" :src="detail.item && detail.item.cover" :width="469" :height="264"></image>
        <text class="detail-play-hint">点击封面播放</text>
      </div>
    </div>

    <!-- 全屏播放页（移植文件管理 shell 播放器 UI）：
         顶栏 96（返回/标题/菜单）+ 视频带 216 + 底栏 78（播放/时间/进度条）。
         native 只写视频带（FULL_VIDEO_AREA），所以顶/底栏不会被视频覆盖。
         点视频区 = 隐藏/显示这两条栏（切布局 + native setRect 换矩形，不重启解码）；
         隐藏时视频带撑满 390，画面按 16:9 信箱适配全屏。
         打开设置菜单时调 pauseRender 停写 fb，否则菜单区会被下一帧视频盖掉。 -->
    <div
      v-if="mode === 'play' && !interact.visible"
      :class="chromeHidden ? 'fp-root fp-root-full' : 'fp-root'"
      :style="{ backgroundColor: chromeTick % 2 === 0 ? '#000000' : '#010101' }"
    >
      <div v-if="!chromeHidden" class="fp-nav">
        <div class="fp-back" @click="onPlayBack">
          <text class="fp-back-text">‹ 返回</text>
        </div>
        <text class="fp-title">{{ titleDisplay }}</text>
        <text v-if="play.note" class="fp-note">{{ play.note }}</text>
        <div class="fp-menu-btn" @click="togglePlayerMenu">
          <text class="fp-menu-dots">•••</text>
        </div>
      </div>

      <div
        :class="chromeHidden ? 'fp-video fp-video-full' : 'fp-video'"
        :style="{ height: fpVideoHeight + 'px' }"
        @click="onVideoTap"
      >
        <!-- 不用 <hole>：真机实测 hole 在大面积重绘（标题滚动 400ms 一次）时会把
             整条视频带清成白色，视频两侧信箱区尤其明显。改用 .fp-video 自身黑底
             作信箱区，native 每帧只覆写中间真实视频矩形，重绘后 33ms 内即补回。
             空闲（未出画）时叠一行提示文字。 -->
        <text v-if="!holeVisible" class="play-idle-text">{{ play.note || idleText }}</text>
      </div>

      <!-- CC 字幕带（3.5.9 同步）：native 视频帧覆盖不了 UI，字幕必须占真实像素。
           开字幕时视频带让出 30px（见 screen.js playAreaFor），这条带画在让出的位置。 -->
      <div v-if="ccBandVisible" class="fp-cc" :style="{ height: ccBandH + 'px' }">
        <text class="fp-cc-text">{{ subtitleText }}</text>
      </div>

      <div v-if="!chromeHidden" class="fp-bottom">
        <div class="fp-play" @click="onTogglePlay">
          <image class="fp-play-icon" :src="play.state === 'playing' ? ic.pause : ic.play" :width="46" :height="36"></image>
        </div>
        <text class="fp-time">{{ fpCurTime }}</text>
        <div
          v-if="!isLivePlay"
          class="fp-seek"
          @touchstart="onSeekTouch"
          @touchmove="onSeekTouch"
          @touchend="onSeekRelease"
        >
          <div class="fp-track">
            <div class="fp-fill" :style="{ width: fpSeekFillW + 'px' }"></div>
            <div class="fp-knob"></div>
          </div>
        </div>
        <text v-else class="fp-time">直播</text>
        <text class="fp-time">{{ fpDurTime }}</text>
      </div>

      <!-- 右侧设置菜单（移植文件管理播放器的 menu-overlay 布局）：
           布局 384 宽，画质/直播画质两行 + 关闭。点视频空白处也会关闭。 -->
      <div v-if="playerMenuOpen" class="fp-menu-overlay">
        <div class="fp-menu-head">
          <text class="fp-menu-title">播放设置</text>
          <div class="fp-menu-close" @click="togglePlayerMenu">
            <text class="fp-menu-close-text">关闭</text>
          </div>
        </div>
        <div class="fp-menu-row" @click="onCycleVideoQuality">
          <text class="fp-menu-label">画质</text>
          <text class="fp-menu-value">{{ vqMenuText() }}</text>
        </div>
        <div class="fp-menu-row" @click="onCycleLiveQuality">
          <text class="fp-menu-label">直播画质</text>
          <text class="fp-menu-value">{{ lqMenuText() }}</text>
        </div>
        <!-- 视频放大：开 = 双指捏合 1..4x（native 中心裁切，不重启解码）。 -->
        <div class="fp-menu-row" @click="onZoomCycle">
          <text class="fp-menu-label">缩放</text>
          <text class="fp-menu-value">{{ zoomText }}</text>
        </div>
        <!-- 样式：流畅=按视频带尺寸解码（现状）；清晰=按无栏（全屏）分辨率解码后等比显示，
             画面不裁切、不变形，切全屏时 1:1 落屏。切换会按当前位置重开一次解码。 -->
        <div class="fp-menu-row" @click="onCyclePlayStyle">
          <text class="fp-menu-label">样式</text>
          <text class="fp-menu-value">{{ playStyleText }}</text>
        </div>
        <!-- 字幕（3.5.9 同步）：点整行轮转「关 → 开·自动 → 开·其它语言 → …」。
             笔端没有下拉控件，轮转是唯一不越屏的交互。 -->
        <div class="fp-menu-row" @click="onCcMenuTap">
          <text class="fp-menu-label">字幕</text>
          <text class="fp-menu-value">{{ ccMenuText }}</text>
        </div>
        <!-- 离线缓存：在线→下载；已缓存→删除；直播→不可下载（见 onDownloadCurrent） -->
        <div class="fp-menu-row" @click="onDownloadCurrent">
          <text class="fp-menu-label">缓存</text>
          <text class="fp-menu-value">{{ dlMenuText }}</text>
        </div>
      </div>
    </div>

    <!-- 互动视频选项页（3.5.9 同步）：native 视频帧盖不住 UI，题面改为整页替换。
         出现时调 pauseRender 冻结画面，选择/收起后恢复。 -->
    <div v-if="mode === 'play' && interact.visible" class="ip-page">
      <div class="ip-head">
        <text class="ip-title">{{ interactTitle }}</text>
        <text class="ip-count">{{ interact.leaf ? '结局' : (interact.countdown > 0 ? interact.countdown + 's' : '选择') }}</text>
      </div>
      <scroller class="ip-list">
        <div
          v-for="(ch, ci) in interactChoices"
          :key="'ic' + ch.id + '_' + ci"
          :class="ch.isDefault ? 'ip-choice ip-choice-def' : 'ip-choice'"
          @click="onChoiceTap(ch)"
        >
          <text class="ip-choice-idx">{{ ci + 1 }}</text>
          <text class="ip-choice-text">{{ ch.option }}</text>
          <text v-if="ch.isDefault" class="ip-choice-tag">默认</text>
        </div>
        <text v-if="!interactChoices.length && !interact.leaf" class="ip-status">{{ interactHint }}</text>
        <text v-if="interact.status" class="ip-status">{{ interact.status }}</text>
      </scroller>
      <div class="ip-foot">
        <div v-if="interact.history.length && !interact.noBacktracking" class="ip-btn" @click="onInteractBack">
          <text class="ip-btn-t">‹ 回退</text>
        </div>
        <div class="ip-btn" @click="onInteractReplay">
          <text class="ip-btn-t">重播本段</text>
        </div>
        <div class="ip-btn" @click="onInteractClose">
          <text class="ip-btn-t">收起</text>
        </div>
      </div>
    </div>

    <!-- 播放态右列（用户手绘稿 m11950）：上=评论区(240)，下=输入框+发送按钮(26)。
         点收藏按钮时整栏被收藏夹选择覆盖，上方有退出按钮，退出恢复评论区。
         直播态整列隐藏（用户反馈 m07570-3「直播界面不应该出现评论区」）：
         直播的评论流与点播不同族（弹幕式、实时滚动），复用点播评论区语义不对；
         而且三个按钮（含收藏）在直播态已隐藏，favOverlay 永远不会被打开，
         整列一起隐藏是安全的。隐藏后中列视频区靠 .pvideo-live 撑满 266px。 -->
    <div v-if="mode === 'detail' && !isLivePlay" class="pright">
      <!-- 分P / 合集页签（3.5.9 同步）：只在稿件真的有多段时出现。
           笔端没有下拉，三个胶囊页签是唯一不越屏的入口。 -->
      <div v-if="!favOverlay && (epHasPages || epHasSeason)" class="ptabs">
        <div :class="epTabNow === 'reply' ? 'ptab ptab-on' : 'ptab'" @click="onEpTab('reply')">
          <text :class="epTabNow === 'reply' ? 'ptab-t ptab-t-on' : 'ptab-t'">评论</text>
        </div>
        <div v-if="epHasPages" :class="epTabNow === 'pages' ? 'ptab ptab-on' : 'ptab'" @click="onEpTab('pages')">
          <text :class="epTabNow === 'pages' ? 'ptab-t ptab-t-on' : 'ptab-t'">{{ epPageLabel }}</text>
        </div>
        <div v-if="epHasSeason" :class="epTabNow === 'season' ? 'ptab ptab-on' : 'ptab'" @click="onEpTab('season')">
          <text :class="epTabNow === 'season' ? 'ptab-t ptab-t-on' : 'ptab-t'">{{ epSeasonLabel }}</text>
        </div>
      </div>
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
      <!-- 分P 列表：点任意一段切换（同一 bvid，不同 cid） -->
      <template v-if="!favOverlay && epTabNow === 'pages'">
        <div class="side-body">
          <scroller class="side-scroll">
            <div
              v-for="(p, pi) in ep.pages"
              :key="'ep' + p.cid"
              :class="pi === ep.pageIndex ? 'ep-row ep-row-on' : 'ep-row'"
              @click="switchEpFromDetail('page', p)"
            >
              <text :class="pi === ep.pageIndex ? 'ep-row-t ep-row-t-on' : 'ep-row-t'">{{ p.title }}</text>
              <text class="ep-row-d">{{ epDur(p.durationSec) }}</text>
            </div>
            <text class="ep-note">{{ ep.note || ('共 ' + ep.pages.length + ' P · 点任意一段播放') }}</text>
          </scroller>
        </div>
      </template>
      <!-- 合集列表：按 sections 分组 -->
      <template v-if="!favOverlay && epTabNow === 'season' && ep.season">
        <div class="side-body">
          <scroller class="side-scroll">
            <text class="ep-head">{{ ep.season.title }} · 共 {{ ep.season.count }} 集</text>
            <template v-for="(sec, si) in ep.season.sections">
              <text v-if="ep.season.sections.length > 1" class="ep-sec">{{ sec.title }}</text>
              <div
                v-for="(it, ii) in sec.items"
                :key="'se' + si + '_' + ii"
                :class="it.bvid === epCurBvid ? 'ep-row ep-row-on' : 'ep-row'"
                @click="switchEpFromDetail('season', it)"
              >
                <text :class="it.bvid === epCurBvid ? 'ep-row-t ep-row-t-on' : 'ep-row-t'">{{ it.title }}</text>
                <text class="ep-row-d">{{ epDur(it.durationSec) }}</text>
              </div>
            </template>
            <text class="ep-note">{{ ep.note || ('第 ' + (epSeasonPos.flat + 1) + ' / ' + ep.season.count + ' 集') }}</text>
          </scroller>
        </div>
      </template>
      <!-- 评论区（默认） -->
      <template v-if="!favOverlay && epTabNow === 'reply'">
        <div :class="epTabsVisible ? 'pright-body pright-body-tabs' : 'pright-body'">
          <scroller class="side-scroll">
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
        <!-- 输入框 + 发送（用户要求 m11950：右下是输入框，点输入框输入文字，点右侧按钮发送）。
             笔端没有内嵌软键盘，所以点输入框唤起系统输入法，文本回填到框里，再点发送才真正提交。 -->
        <div class="pinput">
          <div class="pinput-box" @click="onComposeReply">
            <text :class="cinText ? 'pinput-text' : 'pinput-ph'">{{ cinText || (hasLogin ? '发个友善的评论…' : '未登录 · 不能评论') }}</text>
          </div>
          <div :class="cinText ? 'pinput-send-on' : 'pinput-send'" @click="onSendReply">
            <text class="pinput-send-text">发送</text>
          </div>
        </div>
      </template>
    </div>

    <!-- 直播态右列占位（用户反馈 m07570-3 隐藏评论区后的副作用，必须补）。
         为什么不能只删：holeVisible 为真时根节点套 .page-live（背景透明，让 KMS 视频
         平面透出来 —— 见 base.less 顶部关于 2026-09-24 闪烁的注释）。.pleft 和 .pvideo
         都有自己的不透明底色，但右列 278px 原本靠 .pright 的 @nav-background 遮住。
         一旦 .pright 不渲染，这 278px 就没有任何不透明元素兜底，会把上一屏的 fb 残留
         像素透出来（浏览态列表的封面、图文正文等），看起来像"右侧半屏花了"。
         所以直播态放一个同宽同高、同底色的空占位，几何与 .pright 完全一致。 -->
    <div v-if="isLivePlay" class="pright-fill"></div>

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
/* 离线缓存（「下载」页）：文件落 /userdisk/PenBili/，元数据进 storage。
 * 下载走 native httpjson.startDownload（libcurl，可带 UA/Referer —— B 站 CDN 无头 403）。 */
import {
  DL_DIR,
  loadDownloads,
  saveDownloads,
  upsertDownload,
  removeDownloadEntry,
  ensureDlDir,
  downloadToFile,
  dlFilePath,
  fileSize,
  removeFile,
  formatBytes
} from '../../services/downloads.js';
// 自检种子守卫：判断是否真该自动播（双开关），纯函数可单测 —— 详见该文件头注释
import { isAutotestSeedEnabled, readAutotestSeed } from '../../services/autotest_seed.js';
/* 直播「下播」判据的引擎级自证（probeLiveEnd）：纯计算，不取流不播放不亮屏。
 * 单独成模块是为了让「假 status → 判据结论」这张用例表能被 Node 单测直接跑。 */
import { runLiveEndCases } from '../../services/live_end_probe.js';
/* 检查更新（GitHub Release）：版本比较 / 资产挑选 / 镜像 URL / 提示决策 / 正文切行
 * 全部是纯函数（可 Node 单测），运行时只有 fetch/download/install 三个薄封装。
 * 为什么不用 $falcon.jsapi.updater：它是有道自家服务端升级通道（service_upgrade_push），
 * 需要自家服务器下发 payload，与 GitHub Release 无关 —— strings libfalcon.so 已确认。 */
import {
  APP_VERSION,
  REPO_URL,
  MIRROR_PREFIX,
  SAVE_DIR,
  downloadUrl,
  fetchLatestRelease,
  downloadFileToDisk,
  installAmr,
  normalizeUpdateState,
  isNewer,
  shouldAutoPrompt,
  bodyLines,
  dayKey,
  progressText
} from '../../services/updater.js';
// 主页网格列数 + 尾部裁剪（用户要求 m11950-3：加载更多后条目数必须能被列数整除）
import {
  GRID_COLS,
  trimToGridRows,
  KEEP_INTERVAL_MS,
  keepScreenOn,
  keepScreenOff,
  keepScreenBackend,
  keepScreenTicks,
  isKeepingScreen,
  getScreenState,
  screenStateName,
  getBrightness,
  seekRatioFromX,
  seekFillWidth,
  FP_SEEK_TRACK_WIDTH,
  FP_SEEK_FILL_MAX,
  playVideoRects,
  FULL_VIDEO_AREA,
  FULLSCREEN_VIDEO_AREA,
  FP_NAV_H,
  FP_CC_H,
  playAreaFor
} from '../../services/screen.js';
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
import { loadSearchHistory, saveSearchHistory, pushSearchHistory } from '../../services/search_history.js';
import { fetchUpCard, fetchUpVideos } from '../../services/bili/space.js';
/* 【3.5.9 同步】分P / 合集（结构解析）、CC 字幕、互动视频、播放器信息。
 * 弹幕（3.5.14–3.5.20）按 S6 Pro 分支要求**整体移除**，不引入 danmaku.js。 */
import { pageIndexOf, episodeIndexOf, fmtDur as fmtEpDur } from '../../services/bili/video_meta.js';
import {
  SUBTITLE_SETTINGS_KEY, normalizeSubtitleSettings, pickSubtitleTrack,
  rankSubtitleTracks, subtitleFit, SUBTITLE_FIT_GOOD, fetchSubtitleCues,
  cueAt
} from '../../services/bili/subtitle.js';
import { fetchPlayerV2Tracks } from '../../services/bili/player_v2.js';
import {
  CHOICE_LEAD_MS, AUTO_CHAIN_DELAY_MS,
  defaultChoice, mergeHiddenVars, visibleChoices, applyNativeAction,
  buildSegmentItem, segmentEndPlan, shouldShowChoices, fetchEdgeInfo
} from '../../services/bili/interactive.js';
import { canAutoHide, AUTO_HIDE_MS } from '../../services/play_chrome.js';
/* 原生多指：Falcon 页面层拿不到双指距离，读取 /dev/input/event4 的 ABS_MT_*。
 * 与文件管理 3.1.9 图片页共用同一个 libjsapi_picpinch.so。 */
import { start as pinchStart, stop as pinchStop, read as pinchRead } from 'picpinch';
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
  resolveDetail,
  resolveDownloadStream,
  togglePlay,
  seekBy,
  // 与页面方法 seekTo 同名，取别名避免词法歧义（页面方法是入口，这个是服务层实现）
  seekTo as seekToAbsolute,
  recoverSession,
  readStatus,
  waitLanded,
  waitFirstFrame,
  closeSession,
  describePlayError,
  ensureMixinResult,
  isStreamEnded,
  isLiveEnded,
  isLiveReconnecting,
  num,
  SEEK_STEP_MS,
  SEEK_FRAME_RETRY_DEADLINE_MS,
  SEEK_LAND_NEAR_MS,
  seekNoteDone
} from '../../services/play_session.js';
import * as player from '../../services/player.js';
import {
  DEFAULT_VIDEO_SETTINGS,
  VIDEO_QUALITY_OPTIONS,
  LIVE_QUALITY_OPTIONS,
  loadVideoSettings,
  saveVideoSettings,
  nextOptionId,
  videoQualityLabel,
  liveQualityLabel,
  playStyleLabel,
  nextPlayStyle
} from '../../services/video_settings.js';

const TABS = [
  { id: 'rcmd', label: '推荐' },
  { id: 'hot', label: '热门' },
  { id: 'search', label: '搜索' },
  { id: 'mine', label: '我的' },
  /* 「下载」放在「我的」下面（用户要求）：左栏是纵向标签栏，这就是字面意义的下面。
   * 同时「我的 → 我的下载」也有一个入口行，两条路都能进。 */
  { id: 'downloads', label: '下载' },
  /* 历史单独一页（用户要求）：放在「下载」下面，最多 20 条（history.js MAX_HISTORY） */
  { id: 'history', label: '历史' }
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
// 单元 6 设计 px = 4 物理 px（960 设计宽 → 640 面板，k=2/3）：
// 57 格（v8 49 + 2×4 quiet）× 6 = 342 设计 = 228 物理，模块宽高都是整数像素，
// 不再出现旧 4px 单元（2.67 物理 px）的粗细不匀/扫描失败。单元宽度由 base.less 的
// qr-l-N（N×6px）驱动，行高/段高同样是 6px。
const QR_QUIET = 4;

// grid → 交替段行（白/暗首尾 quiet 补齐；宽度全部映射 qr-l-N 静态 class，零 inline style，
// 相邻同类段自动合并；每行总宽恒 = size + 2*quiet）
// prefix 允许复用同一套逻辑渲染第二种尺寸（关于区的小二维码用 aq-l-N，单元 2px）。
function gridToRows(grid, size, prefix) {
  const p = prefix || 'qr-l-';
  const quiet = QR_QUIET;
  const rows = [];
  const blankLen = size + quiet * 2;
  const blank = () => [{ dark: false, cls: p + blankLen }];
  const push = (segs, dark, len) => {
    const prev = segs[segs.length - 1];
    if (prev && prev.dark === dark) {
      segs[segs.length - 1] = { dark: dark, cls: p + (lenOf(prev.cls, p) + len) };
    } else {
      segs.push({ dark: dark, cls: p + len });
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

function lenOf(cls, prefix) {
  const p = prefix || 'qr-l-';
  return parseInt(cls.slice(p.length), 10) || 0;
}

// 关于区的 GitHub 二维码：URL 是常量，模块加载时算一次即可（不必放进 computed
// 每次重算——编码要走完整的 Reed-Solomon，在 A35 上是实打实的开销）。
const ABOUT_URL = 'https://github.com/noguimc/PenBili';
const ABOUT_QR_ROWS = (() => {
  const enc = encodeQr(ABOUT_URL, {});
  if (!enc.ok) return [];
  return gridToRows(enc.grid, enc.size, 'aq-l-');
})();

function idlePlay() {
  return { state: 'idle', session: null, positionMs: 0, frames: 0, note: '' };
}

// 下载任务状态（同一时刻只允许一个；done=true 表示当前稿件的缓存已完成）
function idleDl() {
  return { busy: false, bvid: '', cid: 0, bytes: 0, total: 0, done: false, note: '' };
}

const STATE_LABELS = {
  idle: '',
  loading: '取流中…',
  playing: '播放中',
  paused: '已暂停',
  ended: '已播完',
  error: '出错'
};

/* 自愈看门狗参数（任务 D）。
 *
 * WATCHDOG_COOLDOWN_MS=60000：旧值 45s/25s。两次自愈之间至少隔 1 分钟 —— 弱网下
 *   视频停帧是常态，短冷却会让看门狗连环触发，每次都往管线里塞一次 seek，反而
 *   把它拖死。1 分钟也足够长，真正的假死用户早就察觉到了。
 * WATCHDOG_MAX_RECOVER=3：每个播放会话最多自愈 3 次。旧实现无上限（靠冷却限流），
 *   长时间弱网下能无限重启。用尽后停止动手、给一行提示让用户自己返回重进 ——
 *   比"永远在重启中"的体验好得多。 */
const WATCHDOG_COOLDOWN_MS = 60000;
const WATCHDOG_MAX_RECOVER = 3;

/* seek 交互参数（缺陷 B 的 JS 侧体验）。
 *
 * SEEK_DEBOUNCE_MS=250：touchend 后等 250ms 再下发。真机上手指离开屏幕到系统把
 *   touchend 送进 QuickJS 有几十毫秒抖动，用户"拖一下就松"时也可能收到两次 touchend
 *   （falcon 的 touch 事件在快速滑动下会重发）。250ms 内再来的 release 直接顶掉前一次，
 *   保证一次拖动最多一次 seek —— 每次 seek 都要重建管线，代价 500ms+，绝不能连发。
 * SEEK_NOTE_TIMEOUT_MS=4000：seek 受理后「定位中…」要撑到首帧落屏（实测 500~600ms）。
 *   这里给 4s 上限：弱网下 seek 后首帧可能很久，超时就收起提示，不把用户困在提示里。 */
const SEEK_DEBOUNCE_MS = 250;
const SEEK_NOTE_TIMEOUT_MS = 4000;

/* 「无首帧」判失败阈值（review2 审计 P0-1 的 JS 侧防线）。
 *
 * 背景：native 旧实现在"从未出过一帧"时把 videoStallMs 返回成哨兵 -1，
 * 页面看门狗判的是 `videoStallMs > 8000` → 恒 false → **永不触发**，界面就一直
 * 停在「加载中…」，用户只能杀进程。B 站 m4s 直链带 deadline（约 2 小时过期），
 * 这是现实中最常见的一条失败路径（缓存里的老 URL 直接 403/超时，一帧都出不来）。
 * play_session.readStatus() 已把 -1 归一化成 0（防线一），但 0 同样不满足 > 8000，
 * 所以这里必须有独立判据（防线二）：只要**还没有任何视频帧且已等超过 15s**，
 * 就直接判失败，不再等"帧间隔"这类只对"出过帧之后"才有意义的指标。
 *
 * 15s 的取值依据：真机弱网起播门最坏情况实测 8~12s 才出首帧（audio 先放行），
 * 15s 既留了余量，又不会让用户盯着"加载中"超过十五秒还不知道发生了什么。 */
/* CV1826 上 360p 起播实测 6~16s（CDN/探流/起播门 + native 15s 门重试），
 * 15s 阈值会在视频刚出帧时误判失败（真机 17:23 日志：rst=1 后 15.0s 判死，
 * 下一帧其实已经落屏）。放宽到 30s，让 native 的「首帧超时重试」跑完。 */
const NO_FRAME_FAIL_MS = 30000;

/* 直播重连提示文案（P1-4 配套）：抽成常量，因为 tick 里有两处要跟它比对
 * （「该挂了吗」「该撤了吗」），写两遍字符串迟早拼错。 */
const LIVE_RECONNECT_NOTE = '直播重连中…';

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
      // 搜索记录（storage: bili_search_history；空态展示，点条目复用关键词）
      searchHistory: [],
      loading: false,
      statusText: '',
      // 评论上下文：视频=type 1（oid=aid）、图文=type 12（oid=专栏 aid）——回复/加载共用
      replyCtx: { oid: 0, type: 1 },
      // 图文详情（mode=article）：blocks = 文本/图片混排块（{t:'text',text} | {t:'img',src,dw,dh}）
      article: { aid: 0, title: '', author: '', cover: '', coverDh: 140, blocks: [], read: 0, like: 0, state: 'idle', note: '' },
      history: [],
      /* 离线缓存（「下载」页）：元数据来自 storage，文件在 /userdisk/PenBili/。
       * dl = 当前下载任务状态（busy/bytes/total），在播放页菜单里显示进度。 */
      downloads: [],
      dl: idleDl(),
      buvidShort: '',
      seekStepSec: SEEK_STEP_MS / 1000,
      play: idlePlay(),
      // 详情态（用户手绘稿）：item=当前稿件，rs=已解析的流（点封面后直接复用，避免二次取流）
      detail: { item: null, rs: null },
      /* 【3.5.9 同步】CC 字幕：开关与语言持久化，轨道/句子按稿件加载。 */
      cc: {
        on: false, lan: '', keepOnSegment: true,
        tracks: [], cues: [], lanDoc: '', status: '', loading: false, posMs: 0
      },
      /* 【3.5.9 同步】分P / 合集：数据来自 view（play_session 透传），不额外请求。 */
      ep: { tab: 'reply', pages: [], season: null, pageIndex: -1, note: '', switching: false },
      /* 【3.5.9 同步】互动视频：片段选项、隐藏变量与连播状态。 */
      interact: {
        active: false, visible: false, busy: false,
        bvid: '', graphVersion: 0, edgeId: 0, cid: 0,
        node: null, vars: {}, choices: [], history: [],
        step: 0, status: '', countdown: 0, leaf: false,
        noBacktracking: false, baseItem: null, segment: 0,
        pending: false, autoJump: false, pauseVideo: false,
        leadMs: 0, questionMs: 0, waiting: false, dismissed: false
      },
      /* 导航来源（用户反馈：从搜索 → UP 主页 → 详情 → 播放，返回时错回搜索页）：
       *   detailFrom = 进详情前所在的页面（'up' | 'browse'）
       *   playFrom   = 进播放前所在的模式（'detail' | 'browse' | 'up'）
       * 播放页返回时只有「detailFrom === 'up'」才回 UP 主页，其余回列表。 */
      detailFrom: '',
      playFrom: '',
      /* 列表滚动位置缓存：key → {frac, y, h}（scroll 事件里的 contentOffset/contentSize）。
       * 返回列表时按 frac 近似定位到对应卡片并 scrollToElement。 */
      scrollMemo: {},
      /* UP 主主页（mode='up'）：card=名片，items=投稿列表（分页拉取）。
       * 入口只有详情页的蓝色 UP 名；返回回到详情页（detail 还在内存里）。 */
      up: { mid: 0, name: '', card: null, items: [], page: 1, noMore: false, loading: false, status: '' },
      titleShift: 0, // 标题滚动展示的字符偏移（见 titleDisplay）
      // 全屏播放页的右侧设置菜单（移植文件管理播放器 menu-overlay）
      playerMenuOpen: false,
      /* 播放页「隐藏状态栏」：true = 顶栏/底栏不渲染、视频带撑满 390。
       * 实现方式是切换（v-if + native setRect），不做动画 —— falcon 没有
       * transition，且 native 视频矩形只能在帧与帧之间整体替换。 */
      chromeHidden: false,
      /* 每次切栏 +1，绑到 .fp-root 的背景色（#000000 / #010101 交替）。
       * 为什么需要：栏被 v-if 移除后，Falcon 只重绘「变化元素」的区域，
       * 全屏后左右信箱条（视频矩形之外）会残留上一帧的顶/底栏像素
       * （真机实测：左侧还挂着「返回」胶囊、右侧挂着时间）。根节点铺满整页，
       * 改它的背景色 = 整页 dirty → 强制全量重绘一次，残影被清成黑色。
       * 两个颜色肉眼看不出差别（都是黑），只是为了让 dirty 判定成立。 */
      chromeTick: 0,
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
      /* 版本号只有一个来源：services/updater.js 的 APP_VERSION（单测断言它等于
       * package.json 的 version）。模板里不再写死版本号 —— 以前写死过一次，
       * 发版时漏改就会在「我的」页显示旧版本号。
       *
       * 必须是 data 而不是 methods：falcon 的模板遇到 {{ appVersion }} 不会自动调用
       * 方法，只会把函数本身渲染成 "function bound appVersion(){...}"（真机实测），
       * 想吃方法就得写 {{ appVersion() }}。版本号是常量，放 data 最省事也最不容易错。 */
      appVersion: APP_VERSION,
      seekDragMs: null,
      // 字幕带高度（逻辑 px，来自 screen.js；模板不能直接引用模块常量）
      ccBandH: FP_CC_H,
      /* 视频放大：开关存在 videoSettings.videoZoom；zoomValue 是当前倍率。
       * pinch 状态只用于一次捏合会话（baseD/baseZoom）。 */
      zoomValue: 1,
      pinch: { active: false, baseD: 0, baseZoom: 1, miss: 0 },
      // 播放页右下输入框里的待发送评论（用户手绘稿 m11950：
      // 点输入框唤起系统输入法回填文本，点右侧「发送」按钮才真正提交）
      cinText: '',
      /* 「我的 → 视频调节 / 直播」设置（services/video_settings.js）。
       * 注意 falcon 没有原生 <select>，用「点击轮转下一个选项」交互（nextOptionId）。
       * v3.3.0 起分辨率与帧率合并为单一画质档（如 '1080p 60fps'），
       * 因为 B 站的 qn 编码本身就把两者绑在一起，360p/480p 根本没有 60fps 档。 */
      videoSettings: DEFAULT_VIDEO_SETTINGS,
      /* 检查更新弹窗状态（v3.5.5）。stage 是单向推进的状态机：
       *   idle → checking → (ready | latest | error)
       *   ready → downloading → installing → done | error
       * mode 只影响按钮组：auto（进入软件时自动检测，4 个按钮）
       *                   manual（「我的」页手动检测，2 个按钮）。
       * 弹窗本身是「整页替换」（v-if="upd.visible"），不是浮层 —— 见模板注释。 */
      upd: {
        visible: false,
        mode: 'auto',
        stage: 'idle',
        message: '',
        release: null,
        lines: [],
        tip: '',
        busy: false
      }
    };
  },
  computed: {
    // 关于区的小二维码（模块加载时已编码好，这里只做取值，避免每次重算 RS 编码）
    aboutQrRows() {
      return ABOUT_QR_ROWS;
    },
    currentItems() {
      return this.tab === 'hot' ? this.hotItems : this.rcmdItems;
    },
    // ---- 检查更新弹窗的展示文案（全部由 upd.stage 推导，模板里不写分支逻辑）----
    updHead() {
      if (this.upd.stage === 'checking') return '正在检查更新…';
      if (this.upd.stage === 'latest') return '已是最新版本 · v' + APP_VERSION;
      if (this.upd.stage === 'downloading') return '正在下载新版本…';
      if (this.upd.stage === 'installing') return '正在安装…';
      if (this.upd.stage === 'done') return '安装完成';
      if (this.upd.stage === 'error') return '检查更新失败';
      return this.upd.release ? '发现新版本 ' + this.upd.release.tag : '检查更新';
    },
    // 包大小（下载前告诉用户要下多少，266px 屏没有进度条，这是唯一的量级提示）
    // 版本徽标 / 包大小只在「有包可装」的阶段显示（见模板注释）
    updShowMeta() {
      const st = this.upd.stage;
      return st === 'ready' || st === 'downloading' || st === 'installing' || st === 'done';
    },
    updSizeText() {
      const r = this.upd.release;
      if (!r || !(r.assetSize > 0)) return '';
      return (r.assetSize / 1024).toFixed(0) + ' KB';
    },
    updBusy() {
      return this.upd.stage === 'checking' || this.upd.stage === 'downloading' ||
             this.upd.stage === 'installing';
    },
    // 右下角小字：把「为什么不能点」讲清楚（镜像 403、资产缺失、来源是直连还是镜像）
    updTip() {
      if (this.upd.tip) return this.upd.tip;
      const r = this.upd.release;
      if (this.upd.stage === 'ready' && r && !r.assetUrl) return '该版本没有可用安装包';
      if (this.upd.stage === 'ready') return '当前 v' + APP_VERSION + ' · 检测走直连，镜像仅用于下载加速';
      if (this.upd.stage === 'done') return '重新打开软件即可使用新版本';
      return '';
    },
    // 搜索活跃类型的条目视图（三分栏：视频/图文/直播各自缓存，切换零请求）
    searchItems() {
      return this.searchResults[this.searchType] || [];
    },
    // ---- 离线缓存（下载页 / 播放页菜单）----
    dlDir() {
      return DL_DIR;
    },
    dlTotalText() {
      let n = 0;
      for (let i = 0; i < this.downloads.length; i++) n += Number(this.downloads[i].sizeBytes) || 0;
      return formatBytes(n) || '0B';
    },
    dlPercent() {
      const d = this.dl || {};
      if (!d.busy) return 0;
      const total = Number(d.total) || 0;
      if (!total) return 0;
      return Math.min(99, Math.floor(((Number(d.bytes) || 0) * 100) / total));
    },
    /* 播放页菜单「缓存」行的值（行为在 onDownloadCurrent 里分发） */
    dlMenuText() {
      const s = this.play.session;
      if (!s) return '';
      if (s.live) return '直播不可缓存';
      const d = this.dl || {};
      if (d.busy && d.bvid === s.bvid) return '缓存中 ' + this.dlPercent + '%';
      if (s.local) return '删除缓存';
      if (this.findDownload(s.bvid, s.cid)) return '已缓存';
      return '点击缓存';
    },
    /* 播放页菜单「样式」行：流畅（信箱）/ 清晰（铺满裁切） */
    playStyleText() {
      return playStyleLabel(this.videoSettings);
    },
    /* 「缩放」行：开时显示当前倍率（双指捏合实时更新）。 */
    zoomText() {
      if (this.videoSettings.videoZoom !== true) return '关';
      return '开 · ' + (Math.round(this.zoomValue * 10) / 10).toFixed(1) + 'x';
    },
    /* ---------- 【3.5.9 同步】CC 字幕 / 分P / 互动 ---------- */
    /* 字幕带是否占像素：开字幕 + 真有句子 + 点播。几何由 ccBandVisible 决定，
     * native 矩形与 DOM 同源（screen.js 的 playAreaFor）。 */
    ccBandVisible() {
      return this.mode === 'play' && !this.isLivePlay && !this.interact.visible &&
        this.cc.on && this.cc.cues.length > 0;
    },
    subtitleText() {
      return this.ccBandVisible ? cueAt(this.cc.cues, this.cc.posMs) : '';
    },
    /* 视频带高度：全屏 390、带栏 216；开字幕时各让出 FP_CC_H。 */
    fpVideoHeight() {
      if (this.chromeHidden) return this.ccBandVisible ? 390 - FP_CC_H : 390;
      return this.ccBandVisible ? 216 - FP_CC_H : 216;
    },
    ccMenuText() {
      if (!this.cc.on) return '关';
      if (this.cc.loading) return '加载中…';
      if (!this.cc.cues.length) return this.cc.status || '无轨道';
      return '开 · ' + (this.cc.lanDoc || '自动') + ' ' + this.cc.cues.length + ' 句';
    },
    ccLanText() {
      if (!this.cc.tracks.length) return this.cc.status || '自动（暂无轨道）';
      const t = pickSubtitleTrack(this.cc.tracks, this.cc.lan);
      return t ? t.lanDoc : '自动';
    },
    /* 「我的」页说明：把"为什么没字幕"讲清楚（匿名接口不返回字幕地址）。 */
    ccSettingHint() {
      if (!this.hasLogin) return '未登录：B 站匿名接口不返回字幕地址，登录后才能显示字幕。';
      if (this.cc.loading) return '字幕加载中…';
      if (this.cc.cues.length > 0) return '已加载 ' + this.cc.cues.length + ' 句字幕' + (this.cc.lanDoc ? '（' + this.cc.lanDoc + '）' : '') + '。';
      if (this.cc.tracks.length > 0) return '找到 ' + this.cc.tracks.length + ' 条字幕轨，但内容没取到：' + (this.cc.status || '播放时会再试一次');
      if (this.cc.status) return this.cc.status;
      return '播放视频后，这里会显示该稿件的字幕状态。';
    },
    /* 分P / 合集：数据都在 play.session（view 一次响应）。 */
    epHasPages() {
      return this.mode === 'detail' && !this.isLivePlay && this.ep.pages.length > 1;
    },
    epHasSeason() {
      return this.mode === 'detail' && !this.isLivePlay && !!(this.ep.season && this.ep.season.count > 0);
    },
    epTabsVisible() {
      return this.epHasPages || this.epHasSeason;
    },
    epTabNow() {
      if (this.ep.tab === 'pages' && this.epHasPages) return 'pages';
      if (this.ep.tab === 'season' && this.epHasSeason) return 'season';
      return 'reply';
    },
    epPageLabel() {
      const n = this.ep.pages.length;
      const cur = this.ep.pageIndex >= 0 ? this.ep.pageIndex + 1 : 1;
      return '分P ' + cur + '/' + n;
    },
    epSeasonLabel() {
      const s = this.ep.season;
      return s ? '合集 ' + s.count : '合集';
    },
    epCurBvid() {
      return this.play.session ? this.play.session.bvid : '';
    },
    epSeasonPos() {
      const s = this.ep.season;
      const p = s ? episodeIndexOf(s, this.epCurBvid) : { si: -1, ii: -1 };
      let flat = 0;
      if (s && p.si >= 0) {
        for (let i = 0; i < p.si; i++) flat += s.sections[i].items.length;
        flat += p.ii;
      }
      return { si: p.si, ii: p.ii, flat: flat };
    },
    interactChoices() {
      const list = this.interact.choices || [];
      if (this.interact.autoJump) return [];
      return list.filter((c) => !c.auto);
    },
    interactTitle() {
      const n = this.interact.node;
      const t = n && n.title ? n.title : '互动片段';
      return '第 ' + (this.interact.segment || 1) + ' 段 · ' + t;
    },
    interactHint() {
      if (this.interact.autoJump) return '自动连播片段，本段结束自动进入下一段';
      return '本段没有可点选项，等待自动连播…';
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
      /* 直播下播与点播播完是两种语义：点播说「已播完」，直播说「直播已结束」
       * （用户反馈里「主播下播」≠「我看完了」）。P1-4。 */
      if (this.play.state === 'ended') return this.isLivePlay ? '直播已结束' : '已播完';
      return '';
    },
    infoTitle() {
      const s = this.play.session;
      if (!s) return '';
      const t = s.title || '';
      /* 多 P 稿件：标题后附录当前段名（与 B 站「稿件名 · P2 第二段」一致）。 */
      const pages = s.pages || [];
      if (s.part && pages.length > 1) {
        const idx = pageIndexOf(pages, s.cid);
        return t + ' · P' + (idx >= 0 ? pages[idx].page : 1) + ' ' + s.part;
      }
      return t;
    },
    infoUp() {
      return this.play.session ? this.play.session.up : '';
    },
    // UP mid：优先会话（view 兜底补过），其次详情条目（列表卡里的 owner.mid）
    infoMid() {
      const s = this.play.session || {};
      if (Number(s.mid) > 0) return Number(s.mid);
      return Number(this.detail.item && this.detail.item.mid) || 0;
    },
    // UP 主页展示名：名片优先，其次详情页带过来的 UP 名
    upName() {
      const c = this.up.card;
      return (c && c.name) || this.up.name || (this.up.mid ? 'UP ' + this.up.mid : 'UP 主页');
    },
    upSign() {
      return this.up.card && this.up.card.sign ? this.up.card.sign : '';
    },
    upStatText() {
      const c = this.up.card;
      if (!c) return this.up.loading ? '解析中…' : '';
      const parts = [];
      if (c.fans > 0) parts.push(formatCount(c.fans) + ' 粉丝');
      if (c.archiveCount > 0) parts.push(c.archiveCount + ' 投稿');
      if (c.level > 0) parts.push('Lv.' + c.level);
      return parts.join(' · ');
    },
    upBarText() {
      const n = this.up.name || (this.up.card && this.up.card.name) || '';
      return n ? 'UP · ' + n : 'UP 主页';
    },
    /* 当前播放会话是不是直播（用户反馈 m07570-3：「直播界面不应该出现评论区、
       三个按钮、和进度条」）。
     *
     * 判据只能看 session.live：play_session.js 的 openVideo 会把 resolveLiveUrl()
     * 的 live:true 原样带进 session（:404 `live: !!rs.live`），而 session 是
     * 播放页唯一的真相源（idlePlay() 的 session 是 null）。不能看 durationMs===0，
     * 因为 durationMs 未知的点播（如 durl 回退）也是 0，会误判。 */
    isLivePlay() {
      return this.mode === 'play' && !!(this.play.session && this.play.session.live === true);
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
    /* 进度条填充宽（px）。几何来自 screen.js（P1-14：以前这里写死 443/453，
     * 布局一改就静默错位）。拖动中用 seekDragMs，松手前不下发 seek。 */
    seekFillW() {
      const s = this.play.session;
      const dur = s ? s.durationMs || 0 : 0;
      if (!(dur > 0)) return 0;
      const ms = this.seekDragMs == null ? this.play.positionMs : this.seekDragMs;
      return seekFillWidth(ms / dur);
    },
    /* 全屏播放页底栏（移植文件管理 UI）：当前/总时长 + 600px 轨道填充。 */
    fpCurTime() {
      const d = (this.play.positionMs || 0) / 1000;
      return d > 0 ? formatDuration(d) : '00:00';
    },
    fpDurTime() {
      const s = this.play.session;
      const d = (s && s.durationMs ? s.durationMs : 0) / 1000;
      return d > 0 ? formatDuration(d) : '00:00';
    },
    fpSeekFillW() {
      const s = this.play.session;
      const dur = s ? s.durationMs || 0 : 0;
      if (!(dur > 0)) return 0;
      const ms = this.seekDragMs == null ? this.play.positionMs : this.seekDragMs;
      return seekFillWidth(ms / dur, FP_SEEK_FILL_MAX);
    },
    // 左列简介（用户手绘稿 m11950：左列上方显示视频标题和简介）
    playDesc() {
      const s = this.play.session;
      if (!s) return '';
      const d = s.desc || '';
      return d || '（暂无简介）';
    },
    /* 标题滚动展示（用户手绘稿要求「尝试复刻哔哩哔哩终端的滚动展示」）：
     * falcon 没有 CSS animation/transform，这里用窗口文本按 400ms 左移一格模拟跑马灯。
     * 短标题（≤16 字）原样显示，长标题才启动（startTitleScroll）。 */
    titleDisplay() {
      const s = this.infoTitle || '';
      if (s.length <= 16) return s;
      const n = 16;
      const off = this.titleShift % (s.length + 4);
      return (s + '    ' + s).slice(off, off + n);
    }
  },
  async created() {
    this.gen = 0;
    this._timers = new Set();
    this._intervals = new Set();
    this._poll = 0;
    this.mixinMemo = {}; // WBI 密钥会话级缓存（播放与搜索共享）
    this.client = createClient({});
    /* 播放器实现选择（v1=旧 player.c，v2=新 player2 模块）在这里一次性读盘：
     * 之后每次 open/status 都走内存值，热路径不碰 storage（storage 是同步 FS 调用，
     * 每秒一次轮询里读盘会拖慢 QuickJS 主线程）。 */
    this._playerImpl = await player.loadPlayerImpl();
    /* 屏幕保活状态（任务 A）：_keepOn=当前是否在保活；_bgPaused=这次暂停是退后台
     * 触发的（回前台要自动恢复），用户手动暂停的不动。 */
    this._keepOn = false;
    this._bgPaused = false;
    this._noKeep = false; // 诊断种子可置 true 关闭保活（保持息屏前提）
    /* seek 交互状态（缺陷 B 的 JS 侧体验）：
     *   _seekDebounceT = 排队中的 seek 定时器句柄（0=没有排队）
     *   _seekNoteAt    = 最近一次下发 seek 的时刻（0=没有在等首帧）→ 「定位中…」靠它撑着
     *   _seekFrames0   = 下发 seek 前的累计帧数（用来判断首帧有没有到）
     *   _seekNoteTarget= 这次 seek 的落点（毫秒），用于「位置已靠近落点」的补充判据
     *   _seekSawPending= 是否亲眼见过 native 的 seekPending=true（判完成必须先见到它） */
    this._seekDebounceT = 0;
    this._seekNoteAt = 0;
    this._seekFrames0 = 0;
    this._seekNoteTarget = 0;
    this._seekSawPending = false;
    /* _noFrameAt = 「连续无视频帧」这个状态是从什么时候开始的（0=当前有帧）。
     * 用途：看门狗的「无首帧」判据（NO_FRAME_FAIL_MS）—— frames 恒为 0 时没有
     * "上次出帧时刻"可比，只能靠"零帧已经持续多久"来判断起播是不是真死了。
     * 用滚动计时而不是"起播时刻"：这样 seek 后、recover 后重新零帧也能被兜住。 */
    this._noFrameAt = 0;
    /* 直播会话状态（P1-4）：
     *   _livePlayed          = 本会话是否**曾经**出现过 playing（判「下播」时用来
     *                          排除「刚进页面还没起播」—— 那时 state 同样是 idle）
     *   _liveReconnectLogged = 「直播重连中…」是否已打过日志（避免每秒刷屏） */
    this._livePlayed = false;
    this._liveReconnectLogged = false;
    /* 【3.5.9 同步】CC 字幕钟 / 互动连播 / 控件自动收起 的定时器与锚点。 */
    this._ccClock = 0;
    this._ccAnchor = null;
    this._tickPos = -1;
    this._tickFrames = -1;
    this._autoChainT = 0;
    this._choiceT = 0;
    this._hideT = 0;
    this._pinchTimer = 0;
    this._zoomBusy = false;
    this._zoomPending = null;
    this._suppressTapUntil = 0;
    this._ccSaved = null;
    await this.loadCcSettings();
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
      /* 字幕插值钟与轮询同生共死：页面离开/换片后不再推进 cue。 */
      if (this._ccClock) {
        clearInterval(this._ccClock);
        this._intervals.delete(this._ccClock);
        this._ccClock = 0;
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
        /* 「我的 → 视频调节 / 直播」的选择，play_session 用它算 qn 阶梯、像素预算、帧率上限 */
        videoSettings: this.videoSettings,
        /* 播放页「样式」：clear = 按无栏（全屏）分辨率解码，显示始终等比信箱；
         * smooth = 按当前显示矩形解码（旧行为）。 */
        playStyle: this.videoSettings.playStyle,
        /* 起播时视频落点：隐藏状态栏时直接用全屏矩形，避免「小窗→拉大」的闪跳；
         * 字幕带已经占像素时用带字幕的矩形（同一套几何，见 screen.js playAreaFor）。 */
        videoArea: playAreaFor(this.chromeHidden, this.ccBandVisible),
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
      const sh = await loadSearchHistory();
      this.searchHistory = sh.items;
      this.downloads = (await loadDownloads()).items;
      logWarn('[bili] dl load n=' + this.downloads.length);
      this.videoSettings = await loadVideoSettings();
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
      /* 【防误触】自动探针/自检只认「双开关」：enabled 且 autotest 显式为 true。
       * 背景：测试期靠 bili_autotest={playProbe:N} 让 App 启动即自动播放，
       * 但正式包若用户存储里残留这份种子，普通用户一进软件就自动播视频（用户实测反馈）。
       * 现在改为：只有 enabled===true 且 autotest===true 才进自动流程；
       * 只有 playProbe 数值、没有 autotest:true 的种子一律视为残留，走正常首屏。
       * 判断本身抽到 services/autotest_seed.js —— 它是纯函数，单测能钉住
       * 「布尔 true / 字符串 'true' / 缺 autotest 的种子都不许自动播」。
       * 这里不再接受旧的布尔 `bili_autotest=true` 写法（历史遗留，等于给残留开后门）。 */
      const atOn = isAutotestSeedEnabled(at);
      if (atOn) {
        const seed = readAutotestSeed(at);
        const soakSec = seed.soak;
        // soakBv：把 soak 固定到指定稿件（跨轮可复现同一稿件 → 才能做换网/换版本的 A/B 对比）
        const soakBv = seed.soakBv;
        // liveOnly：只跑直播专项（跳过视频全流程），用于直播链路真机验证
        const liveOnly = seed.liveOnly;
        const liveRoom = seed.liveRoom;
        // wbiStress：连打 N 次 nav 取 WBI 密钥，统计失败率与走的传输（诊断 nav 间歇失败用）
        const wbiStress = seed.wbiStress;
        // liveHold：直播自检的取证窗口（ms，默认 15000）——诊断时拉长便于外部干预
        const liveHold = seed.liveHold;
        // pollEvery：巡检打点间隔秒数（默认 5）——诊断时设 1 便于看时序
        this._pollEvery = seed.pollEvery;
        /* playProbe：只做「起播 + 按秒采样」，不跑全链路自检（缺陷 C / 屏幕保活取证用）。
         * 种子 {enabled:true, playProbe:90} → 起播推荐首个稿件，持续 90s 每秒打一行
         * [bili] probe，包含 frames/positionMs/videoStallMs/保活后端/屏幕电源态。
         * 这样就能在**关屏**状态下验证：息屏期间看门狗有没有误杀管线（缺陷 C 根因）。 */
        const playProbe = seed.playProbe;
        /* noKeep：诊断用，强制关掉屏幕保活。
         * 为什么需要：验证「息屏期间看门狗有没有误杀管线」必须让屏幕**保持关闭**，
         * 而保活生效后每 20s 会把屏幕点亮（实测 hal-screen keep 能把 bp 4→0、br 5→113），
         * 既烧屏又让"息屏"这个前提不成立。只在种子显式给出时生效，正常路径不受影响。 */
        this._noKeep = seed.noKeep;
        if (this._noKeep) logWarn('[bili] noKeep=1 → 本次运行关闭屏幕保活（仅诊断用）');
        // probeSeek：探针收尾时连打 N 次绝对定位，量 JS 侧往返耗时（验证 seek 不阻塞主线程）
        this._probeSeek = seed.probeSeek;
        /* probePause：探针收尾时做 N 轮「暂停 1.2s → 恢复」，量缺陷 A（暂停后恢复卡顿）。
         * 判据不是"能不能恢复"，而是**恢复后帧率立刻回到满速、没有大空档**：
         * 旧实现恢复后 fps 掉到 10、出现 1127/540/293ms 的空档、要 10 秒才收敛。 */
        this._probePause = seed.probePause;
        /* probeRect：探针收尾时验证键盘避让（SET_RECT）全链路真的通了。
         * 判据不在 JS 侧——JS 拿到 ok:1 只证明命令入队成功。真正的证据是
         * native 日志里出现 `SET_RECT <x,y,w,h>`（= process_cmd 真的执行了
         * gst_util_set_object_arg），以及恢复时出现原矩形。 */
        this._probeRect = seed.probeRect;
        /* probePauseRect：验证「暂停态」键盘避让。
         * 为什么必须单独测：render-rectangle 是 write-only 属性，kmssink 只在
         * 重新 configure（分配 plane）时才读它，PAUSED 下没有新帧流过、不会
         * reconfigure —— 真机实测暂停态 set 1x1 后平面仍是 240x427 完全无效，
         * 而播放态同一条命令立刻生效。用户报的正是「暂停后点评论框，画面盖住
         * 键盘」，所以这个探针必须在暂停态下跑。 */
        this._probePauseRect = seed.probePauseRect;
        /* probeMenu：起播 3s 后自动打开右侧设置菜单（验证 menu-overlay 与 pauseRender）。
         * 仅诊断种子使用；正常启动 seed 为空，不会触发。 */
        this._probeMenu = seed.probeMenu;
        /* probeChrome：起播 +26s 自动隐藏状态栏（全屏）、+34s 恢复，验证 setRect 链路。 */
        this._probeChrome = seed.probeChrome;
        /* probeSearch：搜索链路取证（关键词 → 结果数/首尾标题）；probeDownload：缓存链路取证。 */
        this._probeDownload = seed.probeDownload;
        this._probeStyle = seed.probeStyle;
    this._probeZoom = seed.probeZoom;
        const probeSearch = seed.probeSearch;
        const probeLocalPlay = seed.probeLocalPlay;
        const probeTab = seed.probeTab;
        const probeQr = seed.probeQr;
        /* probeDetail：首屏加载完自动进第一条视频的详情页（纯 UI 验证，不取流播放）。 */
        const probeDetail = seed.probeDetail;
        /* probeUp：先进详情页，再点蓝色 UP 名进 UP 主页（验证名片 + 投稿列表解析）。 */
        const probeUp = seed.probeUp;
        /* probePlayBackUp：UP 主页 → 视频详情 → 播放 → 返回，验证回到 UP 而不是搜索页。 */
        const probePlayBackUp = seed.probePlayBackUp;
        /* probeLiveEnd：直播「下播」判据的引擎级自证（纯计算，不取流/不播放/不亮屏）。
         * 为什么需要：真实下播要等主播关播，或断网等 souphttpsrc 重试耗尽才能复现；
         * 而判据跑在真机 QuickJS 上（typeof/Number 行为可能与 Node 有差异）。
         * 这个开关用假 status 喂真函数，几毫秒跑完，结论可直接 grep。 */
        const probeLiveEnd = seed.probeLiveEnd;
        /* probeUpdate：检查更新全链路自证（检测 → 下载 → 安装）。
         * 与 probeLiveEnd 同类：有副作用（会真的下载 221KB 并装包），
         * 所以只在显式种子下跑，且不进 hasAutoBody 的优先级链。 */
        const probeUpdate = seed.probeUpdate;
        /* 只开了 probeLiveEnd 的种子不能顺带跑全链路自检 —— 那会起播/切 tab，
         * 把「纯计算自证」变成有副作用的播放流程。其余种子组合保持原有优先级不变。 */
        const hasAutoBody = liveOnly || wbiStress > 0 || playProbe > 0 || soakSec > 0;
        logWarn('[bili] autotest seeded → ' + (liveOnly
          ? 'liveOnly room=' + (liveRoom || '-')
          : wbiStress > 0 ? 'wbiStress n=' + wbiStress
          : playProbe > 0 ? 'playProbe ' + playProbe + 's'
          : (probeLiveEnd && !hasAutoBody) ? 'probeLiveEnd only'
          : 'run' + (soakSec > 0 ? ' soak=' + soakSec + 's' : '') + (soakBv ? ' bv=' + soakBv : '')));
        // 自证先跑（无副作用、不打乱播放），再按原优先级分发
        if (probeLiveEnd) this.runLiveEndProbe();
        // 更新探针自带副作用（下载+装包），单独一条路径，不与播放自检混跑
        if (probeUpdate) {
          this.runUpdateProbe();
          await this.selectTab('rcmd');
          return;
        }
        if (probeDetail) {
          await this.selectTab('rcmd');
          if (this.rcmdItems.length) await this.onOpenDetail(this.rcmdItems[0]);
        } else if (probeUp) {
          this.runUpProbe();
        } else if (probePlayBackUp) {
          this.runPlayBackUpProbe();
        } else if (probeSearch) this.runSearchProbe(probeSearch);
        else if (probeTab) this.runTabProbe(probeTab);
        else if (probeQr) {
          await this.selectTab('mine');
          await this.onGoLogin();
        } else if (probeLocalPlay) this.runLocalPlayProbe();
        else if (liveOnly) this.runLiveAutotest(liveRoom, liveHold);
        else if (wbiStress > 0) this.runWbiStress(wbiStress);
        else if (playProbe > 0) this.runPlayProbe(playProbe, soakBv);
        else if (probeLiveEnd && !hasAutoBody) await this.selectTab('rcmd'); // 只跑自证：停在正常首屏
        else this.runAutotest(soakSec, soakBv); // 自检内部自带首屏加载（避免与这里并发双 load 竞态）
      } else {
        await this.selectTab('rcmd'); // 首屏加载：此刻登录 Cookie 已就位 → 个性化推荐
        /* 自动检查更新放在首屏加载**之后**且不 await：
         *   - 放前面会拖慢首屏（GitHub API 实测 2.5s，弱网更久）；
         *   - await 会占住 created()，页面迟迟不渲染。
         * 失败只打一行日志，绝不弹窗打扰（autoCheckUpdate 内部全 try/catch）。 */
        this.autoCheckUpdate();
        /* 预热 WBI 密钥（nav ≈0.3~1.2s）：第一次搜索不用再等密钥请求。
         * 失败不影响任何功能 —— 真正搜索时还会再取一次。 */
        ensureMixinResult(this.makeCtx(this.gen)).catch(() => {});
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
      if (id === 'downloads') {
        // 只做本地校验（文件被外部删掉就摘掉条目），不发任何网络请求
        this.refreshDownloads();
        return;
      }
      // 历史独立页（用户要求）：每次进来从 storage 重新读，保证是最新 20 条
      if (id === 'history') {
        const h = await loadHistory();
        this.history = h.items;
        return;
      }
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
      // 首屏也裁到列数整数倍，保证第一屏就是整行（用户要求 m11950-3）
      const first = trimToGridRows(r.items, GRID_COLS);
      if (id === 'rcmd') this.rcmdItems = first.items;
      else if (id === 'hot') this.hotItems = first.items;
      else if (id === 'search') this.searchResults[this.searchType] = first.items;
      this.statusText = '';
      logWarn('[bili] load ' + id + ' ok n=' + first.items.length +
        (first.trimmed ? ' trim=' + first.trimmed : ''));
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
      // 用户要求 m11950-3：加载更多后条目数必须能被列数整除，否则最后一行填不满。
      // 只裁尾部（不重排），被裁掉的条目会在下一次「加载更多」时由新页补上。
      const cut = trimToGridRows(merged.items, GRID_COLS);
      this.hotItems = cut.items;
      this.hotPn = this.hotPn + 1;
      this.hotNoMore = r.noMore || merged.added === 0;
      this.statusText = merged.added === 0 ? '没有更多了' : '';
      logWarn('[bili] hot page ' + this.hotPn + ' +' + merged.added + ' total=' + this.hotItems.length +
        (cut.trimmed ? ' trim=' + cut.trimmed : ''));
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
        this.recordSearch(kw);
        // 新关键词：三分栏全部重置（切到 search tab 由 loadTab 拉当前类型首屏）
        this.searchResults = { video: [], article: [], live: [] };
        this.searchPages = { video: 1, article: 1, live: 1 };
        this.searchNoMoreMap = { video: false, article: false, live: false };
        this.statusText = '搜索「' + kw + '」…';
        /* 系统输入法是独立 miniapp：确认返回后宿主页要几百毫秒才真正回到前台。
         * 真机日志（2026-11-10 22:25/23:24/09:59）显示：输入法返回后**立刻**发搜索，
         * 第一发 jsapi.http 每次都挂满 15s 超时，重试才成功 —— 这就是「搜索慢」。
         * 等 600ms 让宿主恢复前台；搜索本身也已改走 native libcurl（见 feed.js）。 */
        await this.sleep(600);
        await this.selectTab('search');
        this.scrollToTop('search'); // 新关键词：列表回到顶部
      } finally {
        this.imBusy = false;
      }
    },
    /* ---------- 搜索记录（storage 持久化，最多 10 条、newest-first） ---------- */
    recordSearch(kw) {
      const list = pushSearchHistory({ version: 1, items: this.searchHistory }, kw, Date.now());
      this.searchHistory = list.items;
      saveSearchHistory(list);
    },
    // 点历史条目：复用关键词，清空三分栏缓存后按当前类型重搜
    async onHistorySearch(kw) {
      const k = String(kw || '').trim();
      if (!k) return;
      const myGen = ++this.gen;
      this.stopPoll();
      this.keyword = k;
      this.searchResults = { video: [], article: [], live: [] };
      this.searchPages = { video: 1, article: 1, live: 1 };
      this.searchNoMoreMap = { video: false, article: false, live: false };
      this.recordSearch(k);
      logWarn('[bili] history search kwLen=' + k.length);
      this.loading = true;
      this.statusText = '搜索「' + k + '」…';
      await this.loadTab('search', myGen);
      this.scrollToTop('search'); // 历史条目重搜：列表回到顶部
    },
    async onClearSearchHistory() {
      this.searchHistory = [];
      await saveSearchHistory({ version: 1, items: [] });
      logWarn('[bili] search history cleared');
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
      if (id === 'search') this.scrollToTop('search');
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
      // 同上：尾部裁到列数整数倍（用户要求 m11950-3）
      const cut = trimToGridRows(merged.items, GRID_COLS);
      this.rcmdItems = cut.items;
      this.moreNoMore = r.noMore || merged.added === 0;
      this.statusText = merged.added === 0 ? '没有更多了' : '';
      logWarn('[bili] rcmd+hot 续底 +' + merged.added + ' total=' + this.rcmdItems.length + ' pn=' + this.hotMorePn +
        (cut.trimmed ? ' trim=' + cut.trimmed : ''));
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
      // 同上：尾部裁到列数整数倍（用户要求 m11950-3）
      const cut = trimToGridRows(merged.items, GRID_COLS);
      this.searchResults[st] = cut.items;
      /* 整页都被相关性守卫裁光（B 站拿相关推荐凑数）→ 别让用户继续点「加载更多」
       * 重复请求同样不相关的结果。 */
      if (merged.added === 0) this.searchNoMoreMap[st] = true;
      this.statusText = merged.added === 0 ? '没有更多了' : '';
      logWarn('[bili] search(' + st + ') page ' + r.page + ' +' + merged.added + ' total=' + cut.items.length +
        (cut.trimmed ? ' trim=' + cut.trimmed : ''));
    },

    /* ---------- 离线缓存（下载页 / 播放页菜单） ---------- */
    findDownload(bvid, cid) {
      const c = Number(cid) || 0;
      for (let i = 0; i < this.downloads.length; i++) {
        const it = this.downloads[i];
        if (it.bvid === bvid && (c <= 0 || it.cid === c)) return it;
      }
      return null;
    },
    dlMeta(it) {
      const parts = [];
      if (it.up) parts.push(it.up);
      const sz = formatBytes(it.sizeBytes);
      if (sz) parts.push(sz);
      const d = formatDuration(it.durationSec);
      if (d) parts.push(d);
      return parts.join(' · ');
    },
    // 进「下载」页时校验文件是否还在（外部删文件/换机不残留死条目）
    async refreshDownloads() {
      if (!this.downloads.length) return;
      const keep = [];
      for (let i = 0; i < this.downloads.length; i++) {
        const it = this.downloads[i];
        const n = await fileSize(it.filePath);
        if (n > 0) {
          if (!it.sizeBytes) it.sizeBytes = n;
          keep.push(it);
        } else {
          logWarn('[bili] dl missing ' + it.bvid + '_' + it.cid + ' size=' + n + ' path=' + it.filePath);
        }
      }
      if (keep.length !== this.downloads.length) {
        this.downloads = keep;
        saveDownloads({ version: 1, items: keep });
      }
    },
    // 下载页点卡片 → 直接播本地文件（preResolved.local=true，不再取流）
    async onPlayDownload(it) {
      if (!it || !it.filePath) return;
      const n = await fileSize(it.filePath);
      if (!(n > 0)) {
        this.statusText = '缓存文件已丢失，已从列表移除';
        this.downloads = removeDownloadEntry(this.downloads, it.bvid, it.cid);
        saveDownloads({ version: 1, items: this.downloads });
        return;
      }
      const sec = Number(it.durationSec) || 0;
      const pre = {
        ok: true,
        url: it.filePath,
        audioUrl: '',
        qn: 0,
        cid: it.cid,
        aid: 0,
        durationMs: sec * 1000,
        title: it.title,
        up: it.up,
        desc: '离线缓存 · ' + DL_DIR,
        width: it.width || 640,
        height: it.height || 360,
        host: 'local',
        local: true
      };
      this.stopTitleScroll();
      await this.onPlayItem(
        { kind: 'video', bvid: it.bvid, cid: it.cid, title: it.title, up: it.up, cover: it.cover, durationSec: sec },
        pre
      );
    },
    /* 播放页菜单「缓存」行：在线→下载；已缓存/本地→删除；直播→拒绝。
     * 下载期间菜单保持打开（进度就在这里看），不阻塞轮询/播放。 */
    async onDownloadCurrent() {
      const s = this.play.session;
      if (!s) return;
      if (s.live) {
        this.play.note = '直播不支持缓存';
        return;
      }
      if (this.dl.busy) {
        this.play.note = '已有缓存任务进行中（' + this.dlPercent + '%）';
        return;
      }
      const rec = this.findDownload(s.bvid, s.cid);
      if (s.local || rec) {
        await this.deleteDownload(rec || { bvid: s.bvid, cid: s.cid, filePath: dlFilePath(s.bvid, s.cid) });
        return;
      }
      this.dl = { busy: true, bvid: s.bvid, cid: s.cid, bytes: 0, total: 0, done: false, note: '解析中' };
      this.play.note = '解析缓存地址…';
      logWarn('[bili] dl resolve ' + s.bvid);
      const dir = await ensureDlDir();
      if (!dir.ok) {
        this.dl = idleDl();
        this.play.note = '缓存失败：' + dir.message;
        logWarn('[bili] dl dir fail: ' + dir.message);
        return;
      }
      const r = await resolveDownloadStream(this.makeCtx(this.gen), { bvid: s.bvid, title: s.title, up: s.up });
      if (!r.ok) {
        this.dl = idleDl();
        this.play.note = '缓存失败：' + (r.message || '取流失败');
        logWarn('[bili] dl resolve fail stage=' + r.stage + ': ' + r.message);
        return;
      }
      const filePath = dlFilePath(s.bvid, r.cid);
      this.dl = { busy: true, bvid: s.bvid, cid: r.cid, bytes: 0, total: r.sizeBytes || 0, done: false, note: '下载中' };
      this.play.note = '缓存中…';
      const dl = await downloadToFile({
        url: r.url,
        headers: { 'User-Agent': DEFAULT_UA, Referer: REFERER },
        filePath: filePath,
        onProgress: (b) => {
          if (this.dl.bvid === s.bvid) this.dl.bytes = b;
        }
      });
      let size = Number(dl.bytes) || 0;
      if (!(size > 0)) size = await fileSize(filePath);
      if (!dl.ok || !(size > 0)) {
        this.dl = idleDl();
        this.play.note = '缓存失败：' + (dl.message || '文件为空');
        logWarn('[bili] dl fail ' + s.bvid + ': ' + (dl.message || 'empty'));
        return;
      }
      const entry = {
        bvid: s.bvid,
        cid: r.cid,
        title: s.title || r.title,
        up: s.up || r.up,
        cover: s.cover || '',
        durationSec: Math.floor((r.durationMs || 0) / 1000),
        sizeBytes: size,
        width: r.width,
        height: r.height,
        filePath: filePath,
        at: Date.now()
      };
      this.downloads = upsertDownload(this.downloads, entry);
      saveDownloads({ version: 1, items: this.downloads });
      logWarn('[bili] dl save n=' + this.downloads.length + ' path=' + filePath);
      this.dl = { busy: false, bvid: s.bvid, cid: r.cid, bytes: size, total: size, done: true, note: '' };
      this.play.note = '已缓存 ' + formatBytes(size);
      logWarn('[bili] dl done ' + s.bvid + ' cid=' + r.cid + ' ' + size + 'B');
    },
    async deleteDownload(rec) {
      const path = rec.filePath || dlFilePath(rec.bvid, rec.cid);
      await removeFile(path);
      this.downloads = removeDownloadEntry(this.downloads, rec.bvid, rec.cid);
      saveDownloads({ version: 1, items: this.downloads });
      this.dl = idleDl();
      logWarn('[bili] dl delete ' + rec.bvid + '_' + rec.cid);
      if (this.play.session && this.play.session.local) {
        await this.onBack();
        this.statusText = '已删除缓存';
      } else {
        this.play.note = '已删除缓存';
      }
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
      this.restoreListScroll('search');
    },

    /* ---------- 列表滚动位置（用户要求：返回时保留滚动到哪） ----------
     * scroll 事件对象（Falcon）：{ contentOffset:{y}, contentSize:{height} }，y 为负值。
     * 只存比例 frac，不依赖原生滚动单位；返回时按 frac 近似选中同一位置的卡片，
     * 再用 $dom.scrollToElement(ref, {offset:0, animated:false}) 滚过去。
     * 无法拿到事件时（例如 runtime 不派发 scroll）静默降级为回到顶部。 */
    onListScroll(key, e) {
      if (!key || !e) return;
      const co = e.contentOffset || {};
      const cs = e.contentSize || {};
      const y = Math.abs(Number(co.y) || 0);
      const h = Number(cs.height) || 0;
      // 每个列表只打一行：确认 runtime 真的派发 scroll + contentOffset/contentSize 形态
      this._scrollSeen = this._scrollSeen || {};
      if (!this._scrollSeen[key]) {
        this._scrollSeen[key] = 1;
        logWarn('[bili] scroll ' + key + ' y=' + Math.round(y) + ' h=' + Math.round(h));
      }
      this.scrollMemo[key] = h > 0 ? { frac: Math.min(1, y / h), y: y, h: h } : { frac: 0, y: y, h: 0 };
    },
    scrollListFor(key) {
      if (key === 'search') return this.searchItems;
      if (key === 'downloads') return this.downloads;
      if (key === 'history') return this.history;
      if (key === 'up') return this.up.items;
      return this.currentItems; // rcmd / hot
    },
    // 回到列表顶部（新搜索/换关键词时用；否则 scroller 会停在上一批结果的滚动位置）
    scrollToTop(key) {
      this.scrollMemo[key] = { frac: 0, y: 0, h: 0 };
      setTimeout(() => {
        try {
          const r = this.$refs ? this.$refs['scr-' + key + '-top'] : null;
          const el = Array.isArray(r) ? r[0] : r;
          if (!el) return;
          const page = this.$page || (typeof $falcon !== 'undefined' && $falcon.getPage ? $falcon.getPage(this) : null);
          if (page && page.$dom && page.$dom.scrollToElement) {
            page.$dom.scrollToElement(el, { offset: 0, animated: false });
          }
        } catch (e) { /* 静默 */ }
      }, 80);
    },
    restoreListScroll(key) {
      const m = this.scrollMemo[key];
      if (!m || !(m.frac > 0.01)) return;
      const list = this.scrollListFor(key);
      if (!list || !list.length) return;
      const idx = Math.min(list.length - 1, Math.max(0, Math.floor(m.frac * list.length)));
      const refName = 'scr-' + key + '-' + idx;
      // 等列表重新挂载/布局完成（Falcon 约 1~2 帧）
      setTimeout(() => {
        try {
          const r = this.$refs ? this.$refs[refName] : null;
          const el = Array.isArray(r) ? r[0] : r;
          if (!el) return;
          const page = this.$page || (typeof $falcon !== 'undefined' && $falcon.getPage ? $falcon.getPage(this) : null);
          if (page && page.$dom && page.$dom.scrollToElement) {
            page.$dom.scrollToElement(el, { offset: 0, animated: false });
            logWarn('[bili] scroll restore ' + key + ' idx=' + idx + ' frac=' + m.frac.toFixed(2));
          }
        } catch (e) {
          logWarn('[bili] scroll restore fail ' + key + ': ' + ((e && e.message) || e));
        }
      }, 150);
    },

    /* ---------- 标题滚动 ---------- */
    startTitleScroll() {
      this.stopTitleScroll();
      const s = this.infoTitle || '';
      if (s.length <= 16) return;
      /* 用全局 setInterval（Vue 组件上没有 this.setInterval 方法，
       * 直接调会报 "not a function"），句柄记进 _intervals 由 release 清理。 */
      this._titleTimer = setInterval(() => {
        this.titleShift = (this.titleShift || 0) + 1;
      }, 400);
      this._intervals.add(this._titleTimer);
    },
    stopTitleScroll() {
      if (this._titleTimer) {
        clearInterval(this._titleTimer);
        this._intervals.delete(this._titleTimer);
        this._titleTimer = 0;
      }
      this.titleShift = 0;
    },

    /* ---------- 详情 → 播放 ----------
     * 用户手绘稿：点首页卡片先到详情页（左 up主/标题/简介，中 16:9 封面，右评论），
     * 点击封面才启动播放页。resolveDetail 只取流/元信息，不拉起 native 进程；
     * rs 存进 detail，点封面时直接复用，避免二次取流（弱网下能省 1~2s）。 */
    async onOpenDetail(item) {
      if (!item) return;
      // 直播没有封面/评论语义，直接进原播放页
      if (item.kind === 'live') return this.onPlayItem(item);
      this.resetExtras();
      this.ep = { tab: 'reply', pages: [], season: null, pageIndex: -1, note: '', switching: false };
      const myGen = ++this.gen;
      this.stopPoll();
      this.stopTitleScroll();
      this.playerMenuOpen = false;
      this.chromeHidden = false;
      // 记录来源（UP 主页来的详情，播放返回时才能回 UP 而不是搜索页）
      this.detailFrom = this.mode === 'up' ? 'up' : 'browse';
      this.playFrom = '';
      this.mode = 'detail';
      this.replies = [];
      this.replyStatus = '';
      this.replyLoading = false;
      this.replyNoMore = false;
      this.replyPage = 1;
      this.replyCount = 0;
      this.commentsOpen = false;
      this.replyCtx = { oid: 0, type: 1 };
      this.replyOpen = { root: 0, items: [], shown: 0, page: 1, noMore: false, loading: false };
      this.favOverlay = false;
      this.act = { like: 0, coin: 0, fav: false, status: '', busy: false, likeCount: 0, coinCount: 0, favCount: 0 };
      this.favFolders = [];
      this.favStatus = '';
      this.seekDragMs = null;
      this.cinText = '';
      this.detail = { item: item, rs: null };
      this.play = {
        state: 'loading',
        session: {
          kind: 'video',
          live: false,
          bvid: item.bvid,
          cid: item.cid || 0,
          aid: item.aid || 0,
          title: item.title || '',
          up: item.up || '',
          mid: Number(item.mid) || 0,
          desc: item.desc || '',
          cover: item.cover || '',
          durationMs: (item.durationSec || 0) * 1000,
          qn: 0
        },
        positionMs: 0,
        frames: 0,
        note: ''
      };
      const ctx = this.makeCtx(myGen);
      await closeSession(ctx); // 清掉上一条播放会话（详情页没有 native 进程）
      if (this.gen !== myGen) return;
      const r = await resolveDetail(ctx, item);
      if (this.gen !== myGen) return;
      if (!r.ok) {
        this.play.state = 'idle';
        this.play.note = '详情加载失败：' + (r.message || '未知错误');
        this.startTitleScroll();
        return;
      }
      this.detail = { item: item, rs: r.rs };
      this.play.session = r.session;
      this.play.state = 'idle';
      this.play.note = '';
      /* 【分P / 合集】详情阶段就把结构挂上右栏页签（不额外请求）。 */
      this.ep.pages = Array.isArray(r.session.pages) ? r.session.pages : [];
      this.ep.season = r.session.season || null;
      this.ep.pageIndex = pageIndexOf(this.ep.pages, r.session.cid);
      this.replyCtx = { oid: r.session.aid || 0, type: 1 };
      this.loadReplies(r.session.aid || 0, 1);
      this.refreshAct();
      logWarn('[bili] detail open ' + r.session.bvid + ' aid=' + (r.session.aid || 0));
    },
    /* ---------- UP 主主页（详情页点蓝色 UP 名进入） ---------- */
    async onOpenUp() {
      let mid = this.infoMid;
      /* 历史旧条目没有 mid（本版之前写的），点 UP 名时信息卡可能还没解析完。
       * 这里用 bvid 走一次 view 兜底取 owner.mid，保证「历史 → 详情 → UP 主页」能进。 */
      if (!(mid > 0) && this.detail.item && this.detail.item.bvid) {
        const startGen = this.gen;
        logWarn('[bili] up mid missing → fetchView ' + this.detail.item.bvid);
        const v = await this.client.fetchView(this.detail.item.bvid);
        if (this.gen !== startGen) return;
        if (v && v.ok) {
          mid = Number(v.mid) || 0;
          if (!this.detail.item.mid) this.detail.item.mid = mid;
          if (!this.detail.item.up && v.up) this.detail.item.up = v.up;
          if (this.play.session) {
            if (!this.play.session.mid) this.play.session.mid = mid;
            if (!this.play.session.up && v.up) this.play.session.up = v.up;
          }
          logWarn('[bili] up mid resolved=' + mid);
        }
      }
      if (!(mid > 0)) {
        this.play.note = '缺少 UP 信息，无法进入主页';
        return;
      }
      const myGen = ++this.gen;
      this.stopTitleScroll();
      this.playerMenuOpen = false;
      this.mode = 'up';
      this.up = {
        mid: mid,
        name: this.infoUp || '',
        card: null,
        items: [],
        page: 1,
        noMore: false,
        loading: true,
        status: ''
      };
      logWarn('[bili] up open mid=' + mid);
      await this.loadUp(myGen, 1);
    },
    // 返回详情页：detail/play.session 都还在内存里，不需要重新解析
    onBackUp() {
      ++this.gen;
      this.mode = 'detail';
      logWarn('[bili] up back to detail');
    },
    async loadUp(myGen, page) {
      const u = this.up;
      u.loading = true;
      u.status = '';
      const mx = await ensureMixinResult(this.makeCtx(myGen));
      if (this.gen !== myGen || this.mode !== 'up') return;
      if (!mx.ok) {
        u.loading = false;
        u.status = 'UP 主页解析失败：' + (mx.message || mx.stage);
        logWarn('[bili] up wbi fail stage=' + mx.stage + ' ' + (mx.message || ''));
        return;
      }
      // 名片只在第一页拉一次；失败不阻塞投稿列表（风控下 card 仍可能成功）
      if (page === 1 && !u.card) {
        const c = await fetchUpCard(this.client, u.mid);
        if (this.gen !== myGen || this.mode !== 'up') return;
        if (c.ok) {
          u.card = c.card;
          if (c.card.name) u.name = c.card.name;
        } else {
          logWarn('[bili] up card fail stage=' + c.stage + ' ' + (c.message || ''));
        }
      }
      const name = (u.card && u.card.name) || u.name;
      const r = await fetchUpVideos(this.client, mx.key, u.mid, page, name);
      if (this.gen !== myGen || this.mode !== 'up') return;
      u.loading = false;
      if (!r.ok) {
        u.status = '投稿列表获取失败：' + (r.message || r.stage);
        logWarn('[bili] up videos fail stage=' + r.stage + ' code=' + (r.code != null ? r.code : '-') + ' ' + (r.message || ''));
        return;
      }
      const merged = page === 1 ? r.items : appendDeduped(u.items, r.items).items;
      u.items = merged;
      u.page = page;
      u.noMore = !!r.noMore || r.items.length === 0;
      if (!u.name && u.card && u.card.name) u.name = u.card.name;
      logWarn('[bili] up page ' + page + ' +' + r.items.length + ' total=' + u.items.length +
        ' via=' + (r.via || '-') + ' noMore=' + u.noMore);
    },
    async onRefreshUp() {
      if (!this.up.mid) return;
      const myGen = ++this.gen;
      this.up.items = [];
      this.up.page = 1;
      this.up.noMore = false;
      this.up.status = '';
      await this.loadUp(myGen, 1);
    },
    async loadMoreUp() {
      const u = this.up;
      if (u.loading || u.noMore) return;
      await this.loadUp(this.gen, u.page + 1);
    },
    upVmeta(it) {
      const parts = [];
      const c = formatCount(it.view);
      if (c) parts.push(c + '播放');
      const d = formatDuration(it.durationSec);
      if (d) parts.push(d);
      return parts.join(' · ');
    },
    async onStartPlayFromDetail() {
      const d = this.detail || {};
      if (!d.item) return;
      this.stopTitleScroll();
      const pre = d.rs;
      // 直接复用详情阶段解析出的流；失败时 openVideo 会自己重新 resolve
      await this.onPlayItem(d.item, pre);
    },

    /* ---------- 播放 ---------- */
    async onPlayItem(item, pre) {
      if (!item) return;
      /* 【分P / 合集】换稿件（不是切 P）时清空旧结构；切 P 传 keepEp 保留列表。 */
      if (!item.keepEp) {
        this.ep = { tab: 'reply', pages: [], season: null, pageIndex: -1, note: '', switching: false };
      }
      /* 换片即清空上一条的字幕与互动会话（cc.on/lan 是用户设置，保留）。 */
      this.resetExtras();
      const myGen = ++this.gen;
      this.stopPoll();
      this.stopTitleScroll();
      this.playerMenuOpen = false;
      this.chromeHidden = false; // 每次进播放页都先显示顶/底栏（隐藏是用户主动切的）
      this.playFrom = this.mode; // 'detail' | 'browse' | 'up'（live 卡直接起播）
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
      this.cinText = '';
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
      const r = await openVideo(ctx, item, pre);
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
      /* 起播成功 → 立刻开始屏幕保活（缺陷 C）。
       * 必须在这里而不是"第一帧之后"：起播本身就可能要几秒，且暂停态同样要保活
       * （用户按暂停思考时屏幕变黑同样会触发 600s 背光断电）。 */
      this.startKeepScreen();
      this._avRecoverCount = 0; // 每次播放会话重置自愈配额（任务 D）
      this.history = pushHistory(
        this.history,
        {
          bvid: r.session.bvid,
          cid: r.session.cid,
          title: r.session.title,
          up: r.session.up,
          mid: Number(r.session.mid) || 0,
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
      this.startTitleScroll();
      this.startPoll(myGen);
      /* 【分P / 合集】起播成功才更新结构：失败时保留旧列表，用户可以直接再点一次。 */
      this.ep.pages = Array.isArray(r.session.pages) ? r.session.pages : [];
      this.ep.season = r.session.season || null;
      this.ep.pageIndex = pageIndexOf(this.ep.pages, r.session.cid);
      this.ep.note = '';
      /* 附加功能不阻塞播放；失败只影响字幕/互动。 */
      this.initExtras(myGen);
      this.scheduleAutoHide();
      if (this._probeMenu) {
        // 14s 后才开菜单：首帧实测常在起播后 10~13s 才落屏，太早开 pauseRender
        // 会把「无帧可暂停」的状态冻住，菜单背后的视频带只能是纯黑/提示字。
        setTimeout(() => {
          logWarn('[bili] menu probe open');
          this.togglePlayerMenu();
        }, 14000);
        // 菜单打开 8s 后自动关闭：验证 resumeRender 恢复出画（pauseRender 的对称路径）。
        setTimeout(() => {
          logWarn('[bili] menu probe close');
          this.togglePlayerMenu();
        }, 22000);
      }
      if (this._probeChrome) {
        /* 隐藏状态栏探针：等首帧落屏（+26s）后切全屏，8s 后再切回。
         * 判据：日志出现 [bili] chrome hidden/shown + chrome rect … → ok，
         * 截图看视频是否铺满 640x260（16:9 信箱 462x260 居中）。 */
        setTimeout(() => {
          logWarn('[bili] chrome probe hide');
          this.toggleChrome();
        }, 26000);
        setTimeout(() => {
          logWarn('[bili] chrome probe show');
          this.toggleChrome();
        }, 34000);
      }
      if (this._probeStyle) {
        /* 样式探针：+12s 打开菜单、+14s 切「清晰（无栏解码）」、+22s 关菜单。
         * 切样式要按当前位置重开解码（约 2~6s），关菜单必须等重启完成后再做。
         * 日志判据：`playStyle -> clear` + `style restart ok ... decode=390x693`。 */
        setTimeout(() => {
          logWarn('[bili] style probe menu');
          this.togglePlayerMenu();
        }, 12000);
        setTimeout(() => {
          logWarn('[bili] style probe switch');
          this.onCyclePlayStyle();
        }, 14000);
        setTimeout(() => {
          logWarn('[bili] style probe close');
          this.togglePlayerMenu();
        }, 22000);
      }
      if (this._probeZoom) {
        /* 放大探针：+10s 先打开顶底栏，+12s 切 2x、+18s 回 1x；
         * 专门验证「开栏状态下也能放大」与 native setRect+zoom 共存。 */
        setTimeout(() => {
          logWarn('[bili] zoom probe show chrome');
          this.toggleChrome();
        }, 10000);
        setTimeout(() => {
          logWarn('[bili] zoom probe 2x');
          this.applyZoom(2);
        }, 12000);
        setTimeout(() => {
          logWarn('[bili] zoom probe 1x');
          this.applyZoom(1);
        }, 18000);
      }
      if (this._probeDownload) {
        /* 缓存探针：+15s 打开播放菜单、+17s 点「缓存」，之后菜单里能看到进度；
         * 判据：日志 [bili] dl resolve / dl task id / dl done + 文件存在。 */
        setTimeout(() => {
          logWarn('[bili] dl probe menu');
          this.togglePlayerMenu();
        }, 15000);
        setTimeout(() => {
          logWarn('[bili] dl probe start');
          this.onDownloadCurrent();
        }, 17000);
      }
      /* 起播期间用户就点了视频区（切状态栏）：open 用的是那一刻的 videoArea，
       * 这里按最新状态再对齐一次，避免「加载中切换」落在旧矩形上
       * （恢复栏的方向同样要对齐，不能只在 chromeHidden 时补）。 */
      await this.applyVideoChromeRect();
      this.startPinch();
      logWarn('[bili] play start ' + r.session.bvid + ' qn=' + r.session.qn + ' dur=' + r.session.durationMs + 'ms');
    },
    /* ==================== CC 字幕 / 分P / 合集 / 互动视频（3.5.9 同步） ====================
     *
     * 上游链路：
     *   x/web-interface/view  → pages[] / ugc_season（分P、合集，零额外请求）
     *   x/player/v2          → subtitle.subtitles[] → subtitle_url → {body:[{from,to,content}]}
     *                        → interaction.graph_version → x/stein/edgeinfo_v2
     *
     * S6 Pro 的约束：
     *   · 视频由 native 直接写 /dev/fb0，Falcon 文字**盖不到画面上面** → 字幕必须占
     *     一条真实 UI 带（30px，见 screen.js playAreaFor），视频矩形同步收缩；
     *   · 互动题面用「整页替换 + pauseRender」而不是浮层（falcon 无可靠 z-index）；
     *   · 弹幕整体移除：本分支不引入 danmaku.js，也不调用任何 dm_* native 接口。
     * ============================================================================ */
    resetExtras() {
      this.stopPinch();
      this.zoomValue = 1;
      this.pinch = { active: false, baseD: 0, baseZoom: 1, miss: 0 };
      this.clearAutoChain();
      this.clearChoiceCountdown();
      this.clearAutoHide();
      this._ccAnchor = null;
      this._tickPos = -1;
      this._tickFrames = -1;
      this.cc.tracks = [];
      this.cc.cues = [];
      this.cc.lanDoc = '';
      this.cc.status = '';
      this.cc.loading = false;
      this.cc.posMs = 0;
      this.interact = {
        active: false, visible: false, busy: false,
        bvid: '', graphVersion: 0, edgeId: 0, cid: 0,
        node: null, vars: {}, choices: [], history: [],
        step: 0, status: '', countdown: 0, leaf: false,
        noBacktracking: false, baseItem: null, segment: 0,
        pending: false, autoJump: false, pauseVideo: false,
        leadMs: 0, questionMs: 0, waiting: false, dismissed: false
      };
    },
    saveCcSettings() {
      setJson(SUBTITLE_SETTINGS_KEY, {
        version: 1, on: this.cc.on, lan: this.cc.lan,
        keepOnSegment: this.cc.keepOnSegment
      });
    },
    async loadCcSettings() {
      const raw = await getJson(SUBTITLE_SETTINGS_KEY, null);
      const s = normalizeSubtitleSettings(raw);
      this.cc.on = s.on;
      this.cc.lan = s.lan;
      this.cc.keepOnSegment = s.keepOnSegment;
      this._ccSaved = raw && typeof raw === 'object' && 'on' in raw ? (raw.on === true) : null;
    },
    /* 播放页菜单「字幕」行：关 → 开（自动/当前轨）→ 逐轨轮转 → 关。
     * 笔端没有下拉，轮转是唯一不越屏的交互。 */
    async onCcMenuTap() {
      if (this.isLivePlay) return;
      if (!this.cc.on) {
        this.cc.on = true;
      } else if (this.cc.tracks.length > 1) {
        const idx = this.cc.tracks.findIndex((t) => t.lan === this.cc.lan);
        if (idx < 0) {
          this.cc.lan = this.cc.tracks[0].lan;
        } else if (idx === this.cc.tracks.length - 1) {
          this.cc.on = false;
          this.cc.lan = '';
        } else {
          this.cc.lan = this.cc.tracks[idx + 1].lan;
        }
      } else {
        this.cc.on = false;
      }
      this._ccSaved = this.cc.on;
      this.saveCcSettings();
      await this.applyCcChange();
    },
    /* 「我的」页开关：只切 on/off，不动语言。 */
    async onCcToggle() {
      if (this.isLivePlay) return;
      this.cc.on = !this.cc.on;
      this._ccSaved = this.cc.on;
      this.saveCcSettings();
      await this.applyCcChange();
    },
    /* 「我的」页语言行：轮转轨道（不改开关）。 */
    async onCcLanTap() {
      if (!this.cc.tracks.length) return;
      const idx = this.cc.tracks.findIndex((t) => t.lan === this.cc.lan);
      this.cc.lan = this.cc.tracks[(idx + 1) % this.cc.tracks.length].lan;
      this.saveCcSettings();
      if (this.mode === 'play' && !this.isLivePlay && this.cc.on) await this.reloadCc(this.gen);
    },
    async applyCcChange() {
      if (this.mode !== 'play' || this.isLivePlay) return;
      if (this.cc.on && !this.cc.cues.length) {
        await this.reloadCc(this.gen);
        return;
      }
      // 开关变化 → 字幕带出现/消失 → 视频矩形同步收缩/恢复
      await this.applyVideoChromeRect();
    },
    /* 重新拉轨道 + 字幕文件（切换开关/语言、切段后都用它）。 */
    async reloadCc(myGen) {
      const s = this.play.session;
      if (!s || s.live) return;
      this.cc.loading = true;
      const m = await ensureMixinResult(this.makeCtx(myGen));
      if (this.gen !== myGen) return;
      const info = await fetchPlayerV2Tracks(this.client, s.bvid, s.cid, {
        tries: 3,
        wantTracks: 2,
        mixinKey: m.ok ? m.key : '',
        nowSec: () => Math.floor(Date.now() / 1000)
      });
      if (this.gen !== myGen) return;
      this.cc.loading = false;
      if (!info.ok) {
        this.cc.status = '字幕信息获取失败';
        logWarn('[cc] player/v2 fail: ' + info.message);
        await this.applyVideoChromeRect();
        return;
      }
      this.cc.tracks = info.subtitles || [];
      await this.loadCcCues(myGen, this.cc.tracks);
    },
    /* 候选池逐条试：上游对同一 cid 会挂几条互不对应的 AI 字幕，按「末条结束时间
     * / 视频时长」离 1.0 最近的那条取。并发下载，最多 3 条。 */
    async loadCcCues(myGen, tracks) {
      const cands = rankSubtitleTracks(tracks, this.cc.lan);
      if (!cands.length) {
        this.cc.cues = [];
        this.cc.status = this.hasLogin ? '该稿件暂无 CC 字幕' : '未登录 · B 站匿名接口不返回字幕地址';
        logWarn('[cc] 无轨道可用 tracks=' + ((tracks && tracks.length) || 0) + ' login=' + (this.hasLogin ? 1 : 0));
        await this.applyVideoChromeRect();
        return;
      }
      const s0 = this.play.session;
      const durMs = s0 && s0.durationMs > 0 ? s0.durationMs : 0;
      const limit = Math.min(cands.length, 3);
      this.cc.loading = true;
      const tasks = [];
      for (let i = 0; i < limit; i++) tasks.push(fetchSubtitleCues(this.client, cands[i].url));
      const rs = await Promise.all(tasks);
      if (this.gen !== myGen) {
        this.cc.loading = false;
        return;
      }
      let best = null;
      let lastErr = '';
      for (let i = 0; i < rs.length; i++) {
        const r = rs[i];
        if (!r || !r.ok) {
          lastErr = (r && r.message) || '未知错误';
          logWarn('[cc] 候选 ' + (i + 1) + '/' + limit + ' 下载失败：' + lastErr);
          continue;
        }
        const fit = subtitleFit(r.cues, durMs);
        logWarn('[cc] 候选 ' + (i + 1) + '/' + limit + ' ' + cands[i].lan + ' ' + r.count +
          ' cues 覆盖=' + fit.ratio.toFixed(2) + ' 偏差=' + fit.dev.toFixed(2) + ' ' + fit.reason);
        if (!best || fit.dev < best.fit.dev) best = { t: cands[i], r: r, fit: fit };
      }
      this.cc.loading = false;
      if (!best) {
        this.cc.cues = [];
        this.cc.status = '字幕下载失败：' + (lastErr || '未知错误');
        await this.applyVideoChromeRect();
        return;
      }
      this.cc.lanDoc = best.t.lanDoc;
      this.cc.cues = best.r.cues;
      this.cc.status = best.fit.ok ? '' : '字幕可能与本视频不完全对应';
      logWarn('[cc] loaded ' + best.r.count + ' cues lang=' + best.t.lan +
        ' fit=' + best.fit.reason + ' dev=' + best.fit.dev.toFixed(2));
      await this.applyVideoChromeRect();
    },
    /* 起播后的附加功能入口：一次 player/v2 同时拿字幕轨道与互动 graph_version。
     * 失败只影响附加功能，画面与声音不受牵连。 */
    async initExtras(myGen) {
      const s = this.play.session;
      if (!s || s.live) return;
      let mixinKey = '';
      try {
        const m = await ensureMixinResult(this.makeCtx(myGen));
        if (m.ok) mixinKey = m.key;
      } catch (e) { /* WBI 只用于风控回退 */ }
      if (this.gen !== myGen) return;
      const info = await fetchPlayerV2Tracks(this.client, s.bvid, s.cid, {
        tries: 3,
        wantTracks: 2,
        mixinKey: mixinKey,
        nowSec: () => Math.floor(Date.now() / 1000)
      });
      if (this.gen !== myGen) return;
      if (!info.ok) {
        this.cc.status = '字幕/互动信息获取失败';
        logWarn('[extras] player/v2 fail: ' + info.message);
        return;
      }
      this.cc.tracks = info.subtitles || [];
      logWarn('[cc] player/v2 tracks=' + this.cc.tracks.length +
        ' login=' + (this.hasLogin ? 1 : 0) + ' ccOn=' + (this.cc.on ? 1 : 0));
      if (this.cc.on) await this.loadCcCues(myGen, this.cc.tracks);
      if (info.interactive) {
        logWarn('[interact] graph_version=' + info.graphVersion + ' bvid=' + s.bvid);
        await this.startInteractive(myGen, info.graphVersion);
      }
    },
    /* ---------- 分P / 合集 ---------- */
    epDur(sec) {
      return fmtEpDur(sec);
    },
    onEpTab(tab) {
      if (tab === 'pages' && !this.epHasPages) return;
      if (tab === 'season' && !this.epHasSeason) return;
      if (this.ep.tab === tab) return;
      this.ep.tab = tab;
      this.ep.note = '';
    },
    /* 详情页点分P/合集：直接起播该段（换 cid 等于换一条流，必须重开解码）。 */
    async switchEpFromDetail(kind, row) {
      if (this.ep.switching || !row) return;
      const base = this.detail.item || {};
      if (kind === 'page' && row.cid === this.play.session.cid) return;
      if (kind === 'season' && row.bvid === this.play.session.bvid) return;
      this.ep.switching = true;
      this.ep.note = '切换中…';
      try {
        await this.onPlayItem({
          kind: 'video',
          bvid: row.bvid || base.bvid,
          cid: row.cid || 0,
          useCid: true,
          title: '',
          up: base.up || '',
          cover: row.cover || base.cover || '',
          durationSec: row.durationSec || 0,
          keepEp: true
        });
        this.ep.note = '';
      } catch (e) {
        this.ep.note = '切换失败：' + ((e && e.message) || e);
      } finally {
        this.ep.switching = false;
      }
    },
    /* ---------- 互动视频 ---------- */
    async startInteractive(myGen, graphVersion) {
      const s = this.play.session;
      if (!s) return;
      const it = this.interact;
      it.active = true;
      it.bvid = s.bvid;
      it.graphVersion = graphVersion;
      it.cid = s.cid;
      it.vars = {};
      it.history = [];
      it.segment = 1;
      it.step = 0;
      it.visible = false;
      it.pending = false;
      it.autoJump = false;
      it.pauseVideo = false;
      it.leadMs = 0;
      it.questionMs = 0;
      it.waiting = false;
      it.dismissed = false;
      it.countdown = 0;
      it.leaf = false;
      it.baseItem = {
        kind: 'video',
        bvid: s.bvid,
        title: s.title || '',
        up: s.up || '',
        cover: s.cover || ''
      };
      await this.gotoEdge(myGen, 0, s.cid);
    },
    async gotoEdge(myGen, edgeId, cid) {
      const it = this.interact;
      it.busy = true;
      const r = await fetchEdgeInfo(this.client, it.bvid, it.graphVersion, edgeId);
      if (this.gen !== myGen) return;
      it.busy = false;
      if (!r.ok) {
        it.status = '选项加载失败：' + r.message;
        logWarn('[interact] edgeinfo fail: ' + r.message);
        return;
      }
      const node = r.node;
      if (!(node.cid > 0)) node.cid = cid;
      it.node = node;
      it.edgeId = node.edgeId || edgeId;
      it.cid = node.cid;
      it.leaf = node.isLeaf;
      it.noBacktracking = node.noBacktracking;
      it.autoJump = !!(node.question && node.question.autoJump);
      it.pauseVideo = !!(node.question && node.question.pauseVideo);
      it.leadMs = node.question ? node.question.leadMs : 0;
      it.questionMs = node.question ? node.question.durationMs : 0;
      it.vars = mergeHiddenVars(it.vars, node.hiddenVars);
      it.choices = visibleChoices(node, it.vars);
      it.visible = false;
      it.pending = false;
      it.dismissed = false;
      it.waiting = false;
      it.countdown = 0;
      it.step += 1;
      const d = defaultChoice(it.choices);
      it.status = node.isLeaf
        ? '本片段是结局 · 可回退或重播'
        : (it.autoJump
            ? '自动连播片段 · 本段结束进入下一段'
            : (it.pauseVideo
                ? '片尾会停住，选一个选项继续'
                : (d ? '倒计时结束自动走默认选项' : '请在片段结束前选择')));
      logWarn('[interact] node edge=' + it.edgeId + ' cid=' + it.cid +
        ' choices=' + it.choices.length + ' leaf=' + (it.leaf ? 1 : 0) +
        ' pause=' + (it.pauseVideo ? 1 : 0) + ' lead=' + it.leadMs);
    },
    /* 显示/收起互动题面：题面是整页 UI，必须 pauseRender 冻结 native 写屏。 */
    async openInteractPanel() {
      if (this.interact.visible) return;
      this.interact.visible = true;
      this.interact.pending = true;
      this.clearAutoHide();
      try { await player.pauseRender(); } catch (e) { /* 老 native 没有 pauseRender */ }
    },
    async closeInteractPanel() {
      if (!this.interact.visible) return;
      this.interact.visible = false;
      try { await player.resumeRender(); } catch (e) { }
      this.chromeTick = (this.chromeTick || 0) + 1;
    },
    onInteractClose() {
      this.interact.pending = true;
      this.interact.dismissed = true;
      this.closeInteractPanel();
    },
    async onChoiceTap(choice) {
      if (!choice || this.interact.busy) return;
      await this.goToChoice(choice, false);
    },
    async goToChoice(choice, auto) {
      const it = this.interact;
      if (!it.active || !choice || it.busy) return;
      this.clearAutoChain();
      this.clearChoiceCountdown();
      it.waiting = false;
      it.dismissed = false;
      it.history.push({
        node: it.node, vars: it.vars, cid: it.cid,
        edgeId: it.edgeId, segment: it.segment
      });
      if (it.history.length > 32) it.history.shift();
      it.vars = applyNativeAction(choice.nativeAction, it.vars);
      it.segment += 1;
      const item = buildSegmentItem(it.baseItem, it.node, choice);
      item.nodeTitle = choice.option;
      logWarn('[interact] ' + (auto ? 'auto-chain' : 'choice') +
        ' -> edge=' + choice.id + ' cid=' + choice.cid + ' text=' + choice.option.slice(0, 24));
      await this.playSegment(item, choice.id, choice.cid, auto);
    },
    /* 重新起播一个片段：复用 openVideo，cid 来自选项。 */
    async playSegment(segItem, edgeId, cid, auto) {
      const myGen = ++this.gen;
      this.stopPoll();
      this.clearAutoChain();
      this.clearChoiceCountdown();
      const it = this.interact;
      it.busy = true;
      it.visible = false;
      it.pending = false;
      it.choices = [];
      it.waiting = false;
      it.countdown = 0;
      it.dismissed = false;
      try { await player.resumeRender(); } catch (e) { }
      const ctx = this.makeCtx(myGen);
      this.play.state = 'loading';
      this.play.note = auto ? '连播下一段…' : '切换片段…';
      await closeSession(ctx);
      if (this.gen !== myGen) return;
      const r = await openVideo(ctx, segItem);
      if (this.gen !== myGen) {
        if (r && r.ok) await closeSession(this.makeCtx(this.gen));
        return;
      }
      if (!r.ok) {
        it.busy = false;
        this.play.state = 'error';
        this.play.note = describePlayError(r);
        it.status = '片段起播失败：' + r.message;
        return;
      }
      this.play.session = r.session;
      this.play.state = 'playing';
      this.play.note = '';
      this.play.positionMs = 0;
      this.play.frames = 0;
      this._avRecoverCount = 0;
      it.busy = false;
      it.cid = r.session.cid;
      this.startKeepScreen();
      this.startPoll(myGen);
      if (this.cc.on && this.cc.keepOnSegment) this.reloadCc(myGen);
      await this.applyVideoChromeRect();
      this.startPinch();
      await this.gotoEdge(myGen, edgeId, r.session.cid);
      logWarn('[interact] segment ' + it.segment + ' playing cid=' + r.session.cid +
        ' dur=' + r.session.durationMs + 'ms');
    },
    clearAutoChain() {
      if (this._autoChainT) {
        clearTimeout(this._autoChainT);
        this._timers.delete(this._autoChainT);
        this._autoChainT = 0;
      }
    },
    scheduleAutoChain(choice, delayMs) {
      this.clearAutoChain();
      const wait = Math.max(120, Math.floor(Number(delayMs) || AUTO_CHAIN_DELAY_MS));
      const t = setTimeout(() => {
        this._timers.delete(t);
        this._autoChainT = 0;
        if (this.interact.active && !this.interact.busy && !this.interact.waiting) {
          this.goToChoice(choice, true);
        }
      }, wait);
      this._timers.add(t);
      this._autoChainT = t;
    },
    /* 片段末尾：按 edgeinfo_v2 的题型决定下一步。 */
    handleSegmentEnd() {
      const it = this.interact;
      if (!it.active) return false;
      it.countdown = 0;
      const plan = segmentEndPlan(it.node, it.choices);
      if (plan.kind === 'leaf') {
        it.leaf = true;
        it.status = '本片段是结局 · 可回退或重播';
        it.pending = true;
        this.openInteractPanel();
        return true;
      }
      if (plan.kind === 'auto') {
        if (!plan.choice) {
          it.status = '本段没有可点选项，已停住';
          it.pending = true;
          this.openInteractPanel();
          return true;
        }
        it.status = (it.autoJump ? '自动进入下一段：' : '即将自动连播：') + plan.choice.option;
        this.scheduleAutoChain(plan.choice, plan.delayMs || AUTO_CHAIN_DELAY_MS);
        return true;
      }
      /* wait：片尾停住，无限期等用户点。 */
      it.waiting = true;
      it.pending = true;
      this.openInteractPanel();
      if (plan.countdownMs > 0) {
        it.status = plan.choice
          ? '请选择（倒计时结束走默认项：' + plan.choice.option + '）'
          : '请选择下一步';
        this.startChoiceCountdown(plan.countdownMs, plan.choice);
      } else {
        it.status = plan.choice
          ? '请选择下一步（默认项不会自动跳）'
          : '请选择下一步';
      }
      return true;
    },
    clearChoiceCountdown() {
      if (this._choiceT) {
        clearTimeout(this._choiceT);
        this._timers.delete(this._choiceT);
        this._choiceT = 0;
      }
      this.interact.waiting = false;
    },
    startChoiceCountdown(totalMs, def) {
      this.clearChoiceCountdown();
      const it = this.interact;
      let left = Math.max(1000, Math.floor(Number(totalMs) || 0));
      it.countdown = Math.ceil(left / 1000);
      it.waiting = true;
      const step = () => {
        this._choiceT = 0;
        left -= 1000;
        if (left <= 0) {
          it.countdown = 0;
          it.waiting = false;
          if (def) this.goToChoice(def, true);
          return;
        }
        it.countdown = Math.ceil(left / 1000);
        const t = setTimeout(step, 1000);
        this._timers.add(t);
        this._choiceT = t;
      };
      const t = setTimeout(step, 1000);
      this._timers.add(t);
      this._choiceT = t;
    },
    async onInteractBack() {
      const it = this.interact;
      if (!it.active || it.busy || !it.history.length || it.noBacktracking) return;
      const snap = it.history.pop();
      it.vars = snap.vars || {};
      it.segment = snap.segment || 1;
      const item = {
        kind: 'video', bvid: it.bvid, cid: snap.cid, useCid: true,
        title: it.baseItem.title, up: it.baseItem.up, cover: it.baseItem.cover,
        nodeTitle: snap.node ? snap.node.title : ''
      };
      await this.playSegment(item, snap.edgeId, snap.cid, false);
    },
    async onInteractReplay() {
      const it = this.interact;
      if (!it.active || it.busy || !(it.cid > 0)) return;
      const item = {
        kind: 'video', bvid: it.bvid, cid: it.cid, useCid: true,
        title: it.baseItem.title, up: it.baseItem.up, cover: it.baseItem.cover,
        nodeTitle: it.node ? it.node.title : ''
      };
      await this.playSegment(item, it.edgeId, it.cid, false);
    },
    /* ---------- 控件 5 秒自动收起 ---------- */
    scheduleAutoHide() {
      this.clearAutoHide();
      if (!canAutoHide(this.play.state, {
        full: false,
        chrome: !this.chromeHidden,
        busy: this.seekDragMs != null || this.interact.visible === true
      })) return;
      const t = setTimeout(() => {
        this._timers.delete(t);
        this._hideT = 0;
        if (this.play.state === 'playing' && !this.chromeHidden) this.toggleChrome();
      }, AUTO_HIDE_MS);
      this._hideT = t;
      this._timers.add(t);
    },
    clearAutoHide() {
      if (!this._hideT) return;
      clearTimeout(this._hideT);
      this._timers.delete(this._hideT);
      this._hideT = 0;
    },

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
    /* 当前是否"真的黑了"（看门狗门禁用）——只有背光断电才算，两级优先：
     *  1) native `status().screenOff`（player2 直接读 /sys/class/backlight/*\/bl_power，
     *     true=背光断电、false=背光上电）；
     *  2) 回退 brightness JSAPI 的 getScreenState()：0=亮屏、3=调暗、1=熄屏断电、null=拿不到。
     * 注意两者语义不同（真机实测）：JSAPI 把"调暗 DIM=3"也算非亮屏，但调暗时用户**看得见**
     * 画面，看门狗不该让位 —— 那会把"画面冻死"这种真故障拖成不可恢复。native 的 bl_power
     * 判据只认断电，正好对应"用户看不见"这一真实条件，所以优先用它。
     * 都拿不到时**当作亮屏**（false）：宁可让看门狗按老逻辑工作，也不能因为探测失败
     * 就永久禁用自愈。 */
    isScreenOff(st) {
      if (st && typeof st.screenOff === 'boolean') return st.screenOff;
      const s = getScreenState();
      if (s == null) return false;
      /* JSAPI 回退路径：只有"断电熄屏(1)"算黑屏，调暗(3)仍然可见 → 不让位。
       * 早期版本把 3 也算非亮屏，真机验证后发现会导致调暗期间看门狗全程让位。 */
      const n = Number(s);
      return n === 1;
    },
    /* 原地自愈（任务 D 核心）：优先 native recover()，退化到"原地 seek 一次"。
     *
     * 两个动作都**不跳位置**：
     *   - recover()：native 侧自己重挂平面/重连，不动播放位置（最理想）；
     *   - seek(st.positionMs)：把当前位置再下发一次，等于"重新解一次这一帧"。
     *     注意旧实现是 onSeek(0.001) = seek(pos+1)，会**跳过 1ms 并重建整条管线**；
     *     这里明确用当前 positionMs，不再 +1、不再重开进程。
     * 两者都失败时只打日志，不动 play.state —— 让轮询继续，下一轮冷却后还能再试。 */
    async tryRecover(myGen, st, tag) {
      const ctx = this.makeCtx(myGen);
      const pos = Math.max(0, Number(st && st.positionMs) || 0);
      /* n9 前置判据：已经播完就别救。旧实现无条件原地 seek(pos)，而末尾时
       * pos 已越过时长（真机 pos=118776 / dur=114900），seek 会被 native clamp 到
       * 113900，重开 ffmpeg 读 1 秒又死 → 每 60s 一次的无限自愈循环。
       * 这里直接收尾：置 ended、清提示、停轮询，与 tick 里的末尾判据同一出口。 */
      const durMs = num(st && st.durationMs, this.play.session && this.play.session.durationMs);
      if (isStreamEnded(st, durMs)) {
        this.play.state = 'ended';
        this.play.note = '';
        this.play.positionMs = pos;
        /* 互动片段不能在自愈路径上丢掉连播入口。 */
        this.handleSegmentEnd();
        this.stopPoll();
        logWarn(
          '[bili] ' + tag + ' 判定为正常播完（pos=' + pos + ' dur=' + durMs +
          ' alive=' + (st && st.pipelineAlive ? 1 : 0) + '）→ 不做自愈 seek'
        );
        return;
      }
      const r = await recoverSession(ctx);
      if (this.gen !== myGen) return;
      if (r.ok) {
        logWarn('[bili] ' + tag + ' 自愈: recover() ok state=' + r.state);
        return;
      }
      if (r.code === 'PLAYER_UNSUPPORTED') {
        // v2 契约里没有 recover() → 退化到原地 seek（不重启管线，只重新解当前帧）
        const rr = await seekBy(ctx, pos, durMs, 0);
        if (this.gen !== myGen) return;
        if (rr.ok) {
          logWarn('[bili] ' + tag + ' 自愈: 原地 seek(' + pos + 'ms) ok（无 recover()）');
        } else {
          logWarn('[bili] ' + tag + ' 自愈失败（原地 seek）: ' + rr.message);
        }
        return;
      }
      logWarn('[bili] ' + tag + ' 自愈失败（recover）: ' + (r.code || '') + ' ' + (r.message || ''));
    },
    startPoll(myGen) {
      this.stopPoll();
      this._pollLogged = false;
      this._audioDeadWarned = false; // 每次新播放重置音频中断提示状态。
      this._resyncNoteOn = false;
      /* 自愈状态按会话重置（任务 D）：换一个视频就是新会话，配额重新给满。 */
      this._avRecoverAt = 0;
      this._avRecoverCount = 0;
      this._avQuotaWarned = false;
      this._hardQuotaWarned = false;
      this._screenOffStallLogged = false;
      this._seekNoteAt = 0; // 新会话没有在等的 seek
      this._noFrameAt = 0;  // 新会话的"连续零帧"计时也归零
      /* 直播会话状态按会话重置（P1-4）：换一场直播要重新判定「有没有播起来过」。 */
      this._livePlayed = false;
      this._liveReconnectLogged = false;
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
        // seek 等首帧期间（_seekNoteAt>0）不走这里：seek 本身也可能让 native 触发重同步，
        // 但此时提示必须是「定位中…」，不能被改写成「加载中…」。
        if (st.state === 'playing' && st.resyncing && (st.resyncingMs || 0) > 2500 && !(this._seekNoteAt > 0)) {
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
        /* v3.5.1 起播时序（验证定格门）：audioLateMs = 声音比画面首帧晚多久开写。
         * 定格门生效时应接近 0；若仍是几百毫秒，说明门没起作用。 */
        if (st.audioLateVisibleMs >= 0 && st.audioLateVisibleMs !== this._lastAudioLateMs) {
          this._lastAudioLateMs = st.audioLateVisibleMs;
          logWarn('[bili] A/V 音画起播差=' + st.audioLateVisibleMs + 'ms（声音比画面可见晚多少，定格门应压到 ~0；总启动 ' + st.audioLateMs + 'ms）');
        }
        /* ---------- 自愈看门狗（任务 D 重写） ----------
         *
         * 旧实现的两个致命问题（defect-inventory P1-13）：
         *  1) 动作是 `onSeek(0.001)` —— v1 的 seek 会停掉双进程、重开、重过 15s 起播门，
         *     为了"对齐一下"把整条管线杀掉重建。真机实测这一次 seek 阻塞 QuickJS 主线程
         *     4961ms，用户看到的是画面卡住好几秒再回来。
         *  2) **没有息屏门禁**。息屏后视频 vsink 不再消费帧 → native 的 videoStallMs 持续
         *     增长，而音频进程独立于平面仍在写 → "视频 8s 无新帧但音频在播"成立，看门狗
         *     判定管线卡死并重启。可息屏期间画面停滞是**完全正常**的。真机表现：熄屏一会儿
         *     再点一下，只亮屏不出画（管线正被自己杀掉重建），要再点一次才好 —— 这就是
         *     用户说的"要点两次"。所以下面每个看门狗都必须带 !screenOff。
         *
         * 新动作分级（从轻到重，绝不无条件杀管线）：
         *  1. native 提供 recover() → 调它（原地重挂平面/重连，不动进程）；
         *  2. 不支持 recover → 最多补一次 seek(currentPos)（原地重试，不跳位置）；
         *  3. 冷却 ≥60s（旧的是 45s/25s，弱网下会连环重启）；
         *  4. 每个播放会话最多 3 次（旧的无上限，弱网下能无限重启）。
         */
        const screenOff = this.isScreenOff(st);
        const now = Date.now();
        const cooldownOk = !this._avRecoverAt || now - this._avRecoverAt > WATCHDOG_COOLDOWN_MS;
        const quotaOk = (this._avRecoverCount || 0) < WATCHDOG_MAX_RECOVER;
        /* seek 进行中让位（P0-2 要求 2）：seek 会重建管线，期间 videoStallMs 必然增长、
         * frames 停涨，这在看门狗看来和"卡死"一模一样。若不放行，用户每拖一次进度条就
         * 可能被自愈逻辑再补一次 seek（雪上加霜，且耗尽 3 次配额）。 */
        const seekBusy = this._seekNoteAt > 0;
        /* 【CC 字幕】位置冻结判定 + 锚点（250ms 字幕钟按墙钟外推）。
         * 缓冲/卡顿时 state 仍是 playing 但 positionMs 不动，锚点必须标 frozen，
         * 否则字幕会按墙钟一路跑到画面前面去。 */
        const posStalled = this._tickPos === st.positionMs && this._tickFrames === st.frames;
        this._tickPos = st.positionMs;
        this._tickFrames = st.frames;
        this._ccAnchor = {
          pos: st.positionMs,
          at: Date.now(),
          frozen: st.state !== 'playing' || posStalled || seekBusy
        };
        /* 直播标志：本 tick 内反复用（终止判据 + 重连提示），取一次避免多次走 computed。 */
        const isLivePlay = this.isLivePlay;
        /* ---------- 流末尾判据（n9，必须在所有自愈看门狗之前） ----------
         * 播到结尾时源 ffmpeg 正常退出但 EOS 可能一直不到，native 的 state 会永远
         * 停在 playing、只把 pipelineAlive 置 0。若先跑看门狗，就会被判成"假死"而
         * 原地 seek 回末尾 → 重开 ffmpeg 读 1 秒又死 → 无限循环（真机 08:22 实锤）。
         * 所以这里先确认"其实已经播完了"，直接收尾并停轮询。 */
        let durMs = num(st.durationMs, this.play.session && this.play.session.durationMs);
        /* 【互动视频】以本段 playurl 的 timelength 为准：view.duration 是整张剧情图
         * 的总时长，用错会让题面等到最后一刻才出现。 */
        if (this.interact.active && this.play.session &&
            this.play.session.cid === this.interact.cid &&
            this.play.session.durationMs > 0) {
          durMs = this.play.session.durationMs;
        }
        if (this.interact.active && !this.interact.leaf && durMs > 0) {
          const remain = Math.max(0, durMs - st.positionMs);
          this.interact.countdown = Math.ceil(remain / 1000);
          const lead = this.interact.leadMs > 0 ? this.interact.leadMs : CHOICE_LEAD_MS;
          if (!this.interact.autoJump && !this.interact.dismissed &&
              shouldShowChoices(st.positionMs, durMs, lead)) {
            this.openInteractPanel();
          }
        }
        /* 只在 playing 态判末尾：暂停在最后一秒（用户拖到末尾后按了暂停）不该被判成
         * "已播完" —— 那会清提示、停轮询，用户再点播放就没轮询跟进了。 */
        if (!seekBusy && st.state === 'playing' && isStreamEnded(st, durMs)) {
          this.play.state = 'ended';
          this.play.note = ''; // 播完不保留"网络不稳定/加载中"之类残留提示
          this.play.frames = st.frames;
          this.play.positionMs = durMs > 0 ? Math.min(st.positionMs, durMs) : st.positionMs;
          /* 互动视频：末尾不是终点，而是「连播下一个片段」的触发点。 */
          this.handleSegmentEnd();
          this.stopPoll();
          logWarn(
            '[bili] video ended（末尾判据）pos=' + st.positionMs + ' dur=' + durMs +
            ' frames=' + st.frames + ' alive=' + (st.pipelineAlive ? 1 : 0) +
            (this.interact.active ? ' [interact] 等待连播' : ' → 停轮询，不再自愈 seek')
          );
          return;
        }
        /* ---------- 直播终止判据（P1-4，仅直播） ----------
         * 直播没有时长，走不了上面的 isStreamEnded（dur<=0 恒 false）。主播下播时
         * native 发 EOS → patch_state(STOPPED) → normalizeState 归一成 'idle'，
         * 而页面原有的 `if (st.state === 'ended')` 分支在 native 上永远不可达 →
         * 界面停在「播放中」、进度条（直播位置=墙钟）继续走。
         *
         * 为什么必须放在这里（自愈看门狗之前）：下播后管线还在但不再出帧，
         * videoStallMs 会一路涨，两个 stall 看门狗会把「正常下播」当成假死，
         * 反复原地 seek 重连 —— 用户看到的是界面卡住 + 无任何提示。
         *
         * 注意直播位置是墙钟（native live_position_ms()），EOS 后照样每秒 +1000，
         * 所以**不能**用「位置停止增长」判结束，只能看 state 落回 idle/ended。 */
        /* 先算重连标志：正在重连时 state 理论上仍是 playing（native2 约定不动 state），
         * 但万一日后实现改了把它置回 idle，也不能把「重连中」误报成「已结束」——
         * 这两个提示的语义完全相反（等一会儿就好 vs 别等了）。 */
        const liveReconnecting = isLivePlay && isLiveReconnecting(st, isLivePlay);
        if (!seekBusy && !liveReconnecting && isLiveEnded(st, isLivePlay, this._livePlayed)) {
          this.play.state = 'ended';
          this.play.note = ''; // 清掉「直播重连中…」之类残留
          this.play.frames = st.frames;
          this.play.positionMs = st.positionMs;
          this.stopPoll();
          logWarn(
            '[bili] live ended（直播终止判据）state=' + st.state +
            ' pos=' + st.positionMs + ' frames=' + st.frames +
            ' played=' + (this._livePlayed ? 1 : 0) +
            ' alive=' + (st.pipelineAlive ? 1 : 0) +
            ' → 停轮询，界面显示「直播已结束」'
          );
          return;
        }
        // 息屏期间画面停滞是正常现象，不判故障（但仍然每 5s 打一行日志留证据）
        if (screenOff && st.state === 'playing' && st.videoStallMs > 8000 && !this._screenOffStallLogged) {
          this._screenOffStallLogged = true;
          logWarn('[bili] 息屏期间 videoStall=' + st.videoStallMs + 'ms（正常，画面不在屏上）→ 看门狗让位');
        }
        if (!screenOff) this._screenOffStallLogged = false;

        /* ---------- 无首帧兜底看门狗（review2 审计 P0-1） ----------
         * 下面两个 stall 看门狗都建立在「videoStallMs 会增长」之上，但 videoStallMs
         * 只在"出过帧之后"才有意义：一帧都没出时 native 返回哨兵 -1（现在已被
         * readStatus 归一化成 0），0 > 8000 同样恒 false → 两个看门狗都永远不触发。
         * B 站 m4s 直链带 deadline（约 2 小时过期），缓存里的老 URL 会 403/超时，
         * 表现正是"一帧都出不来"且 native 不报错 —— 用户看到永久「加载中…」。
         *
         * 判据：还在等起播（opening/loading）或累计帧数仍为 0，且这种状态已经
         * 连续持续超过 15s → 直接判失败，不再等帧间隔指标。用**滚动计时**
         * （_noFrameAt）而不是"起播时刻"：这样 seek 后/recover 后重新零帧也能兜住，
         * 同时只要中途出过帧（frames > 0）就清零重新计。 */
        const waitingFrames =
          st.state === 'opening' ||
          st.state === 'loading' ||
          (st.state === 'playing' && st.frames === 0);
        /* 直播重连期间让位：重连必然伴随 frames 停涨（最坏 45s = souphttpsrc
         * retries=3 × timeout=15s），按 15s 判失败会把「等一会儿就好」直接判死。
         * native 侧有硬上限（45s 后自己报错 → state=error → 走页面失败路径），
         * 所以这里不需要再加时间闸。 */
        if (waitingFrames && !liveReconnecting && !screenOff && !seekBusy && !this.commentsOpen) {
          if (this._noFrameAt <= 0) this._noFrameAt = now;
          const noFrameMs = now - this._noFrameAt;
          if (noFrameMs > NO_FRAME_FAIL_MS) {
            this._noFrameAt = 0;
            logWarn(
              '[bili] 无首帧 ' + noFrameMs + 'ms（state=' + st.state + ' frames=' + st.frames +
              ' stall=' + st.videoStallMs + ' gate=' + st.gateWaitMs +
              '）/ 阈值 ' + NO_FRAME_FAIL_MS + 'ms → 判起播失败，停止轮询让用户返回重试'
            );
            this.play.state = 'error';
            this.play.note = '起播失败 · 可返回重试';
            this.stopPoll();
            return;
          }
        } else {
          // 出过帧 / 不在等起播 → 计时归零，下次零帧重新起算
          this._noFrameAt = 0;
        }

        // A/V 一致性巡检：视频 8s 无新帧而音频仍在写 → 原地恢复（不重启管线）
        if (
          !screenOff &&
          !seekBusy &&
          /* 直播重连中让位：native 自己正在重连（最坏 45s），JS 再动手只会
           * 和它抢管线；等 native 报错（state=error）或恢复，页面各自有路径。 */
          !liveReconnecting &&
          st.state === 'playing' &&
          !st.gateActive &&
          // 重同步期间让位，但**有限让位**——超过 15s 仍未恢复就交回 JS 兜底，
          // 否则一旦 native 侧重启卡死，两个看门狗都被压住 → 画面永久冻结（真机实测过 66s+）。
          (!st.resyncing || (st.resyncingMs || 0) > 15000) &&
          st.videoStallMs > 8000 &&
          st.audioBytes > 0 &&
          st.audioBytes !== (this._lastAvAudioBytes || 0) &&
          !this.commentsOpen
        ) {
          if (cooldownOk && quotaOk) {
            this._avRecoverAt = now;
            this._avRecoverCount = (this._avRecoverCount || 0) + 1;
            logWarn(
              '[bili] A/V stall: video ' + st.videoStallMs + 'ms 无新帧但音频在播 → 原地恢复' +
              '（第 ' + this._avRecoverCount + '/' + WATCHDOG_MAX_RECOVER + ' 次）pos=' + st.positionMs
            );
            this.play.note = '音画不同步，正在恢复…';
            this.tryRecover(myGen, st, 'av');
          } else if (!quotaOk && !this._avQuotaWarned) {
            this._avQuotaWarned = true;
            logWarn('[bili] A/V stall 自愈配额已用尽（' + WATCHDOG_MAX_RECOVER + ' 次）→ 不再动手，交给用户');
            this.play.note = '画面已停滞，可返回重进';
          }
        }
        // 僵尸涓流看门狗：视频停帧 ≥25s 且音频字节也不再增长 = 双链路假死
        // （TCP 涓流让 rw_timeout 永不触发，实测可冻结 60s+）。同样先原地恢复。
        if (
          !screenOff &&
          !seekBusy &&
          !liveReconnecting &&
          st.state === 'playing' &&
          !st.gateActive &&
          (!st.resyncing || (st.resyncingMs || 0) > 15000) &&
          st.videoStallMs > 25000 &&
          st.audioBytes === (this._lastAvAudioBytes || 0) &&
          !this.commentsOpen
        ) {
          if (cooldownOk && quotaOk) {
            this._avRecoverAt = now;
            this._avRecoverCount = (this._avRecoverCount || 0) + 1;
            logWarn(
              '[bili] hard stall: video ' + st.videoStallMs + 'ms 无帧且音频冻结 → 原地恢复' +
              '（第 ' + this._avRecoverCount + '/' + WATCHDOG_MAX_RECOVER + ' 次）'
            );
            this.play.note = '网络不稳定，正在重连…';
            this.tryRecover(myGen, st, 'hard');
          } else if (!quotaOk && !this._hardQuotaWarned) {
            this._hardQuotaWarned = true;
            logWarn('[bili] hard stall 自愈配额已用尽（' + WATCHDOG_MAX_RECOVER + ' 次）→ 不再动手');
            this.play.note = '网络异常，可返回重进';
          }
        }
        this._lastAvAudioBytes = st.audioBytes;
        /* ---------- seek 的「定位中…」（P0-2 要求 2） ----------
         * seek 受理后画面要到首帧才换（实测 500~600ms），这段窗口里：
         *   1) 必须显示「定位中…」——不能是「加载中…」，也不能是黑屏无提示；
         *   2) 不能被下面「无画面帧 → 加载中…」覆盖（seek 期间 frames 会短暂停涨/归零）；
         *   3) 不能触发看门狗（seek 本身会让 videoStallMs 增长，属正常）。
         * 判据见 play_session.js 的 seekNoteDone()：**必须先看到 st.seekPending 落回
         * false**（= native 的 do_seek 真的跑完），再看帧数/落点。为什么不能只看
         * frames 增长（旧实现的真 bug，真机 23:12:51 复现）：seek 下发后约 200ms 内
         * 旧管线仍在出帧（native 还在 debounce），帧数一涨就被误判成"首帧已到"，
         * 于是下一轮 tick 用 native 的**旧**位置覆盖刚写下的落点 → 进度条往左弹一下。
         * 4s 兜底收起，避免弱网下把用户困在提示里。 */
        let seekWaiting = false;
        if (this._seekNoteAt > 0) {
          const waited = Date.now() - this._seekNoteAt;
          if (st.seekPending === true) this._seekSawPending = true;
          if (
            seekNoteDone(st, this._seekFrames0, this._seekNoteTarget, SEEK_LAND_NEAR_MS, this._seekSawPending)
          ) {
            logWarn(
              '[bili] seek 首帧到达 耗时=' + waited + 'ms frames=' + st.frames +
              ' pos=' + st.positionMs + ' target=' + this._seekNoteTarget
            );
            this._seekNoteAt = 0;
            this._seekSawPending = false;
            if (this.play.note === '定位中…') this.play.note = '';
          } else if (waited > SEEK_NOTE_TIMEOUT_MS) {
            logWarn(
              '[bili] seek 等首帧超时 ' + waited + 'ms（frames=' + st.frames +
              ' pos=' + st.positionMs + ' target=' + this._seekNoteTarget +
              ' pending=' + (st.seekPending === true ? 1 : 0) + '）→ 收起提示'
            );
            this._seekNoteAt = 0;
            this._seekSawPending = false;
            if (this.play.note === '定位中…') this.play.note = '';
          } else {
            seekWaiting = true; // 还在等首帧：保持提示、且不让「加载中」覆盖
          }
        }
        // v2.9.4：**只要还没有任何视频帧就提示「加载中」**。原因（真机诊断实测）：起播门 15s 会
        // 先放行音频（"极弱网先出声"），此时音频已响但画面仍是冻的 → 用户感受正是"画面卡住但声音
        // 正常"。原判据（首帧+位置0+无音频字节）在音频放行后就不成立了，于是画面冻着却没有任何提示。
        // 注意区分：resyncing（视频流重启）走上面的 2.5s 门槛保持静默自愈，避免提示刷屏。
        // seek 等首帧期间（seekWaiting）不覆盖「定位中…」：seek 后 frames 会短暂归零/停涨，
        // 若按这里判定就会把提示改成「加载中…」，用户看到的是"定位"变"加载"，语义错且更焦虑。
        if (seekWaiting) {
          // 保持「定位中…」原样
        } else if (liveReconnecting) {
          /* 直播重连期间画面必然停滞（frames 不再增长），若按下面判定就会挂
           * 「加载中…」，但真实情况是「等几秒就好」—— 语义不同，交给下面的
           * 重连提示统一处理，这里让位。 */
        } else if (st.state === 'playing' && st.frames === 0 && !st.resyncing) {
          if (this.play.note !== '加载中…') {
            this.play.note = '加载中…';
            logWarn('[bili] 无画面帧（起播/卡死）→ 加载中…');
          }
        } else if (this.play.note === '加载中…' && st.frames > 0) {
          this.play.note = '';
        }
        /* 位置回写：seek 等首帧期间保持本地乐观值（目标落点），否则 thumb 会被旧的
         * native 位置拉回去，表现为"拖完往左弹一下"。首帧到达后再交回 native。 */
        if (!seekWaiting) this.play.positionMs = st.positionMs;
        this.play.frames = st.frames;
        /* 直播「曾经播起来过」的事实记录（P1-4）：主播下播就在我们点进去的瞬间发生时，
         * 轮询（1s 一次）可能一次都抓不到 playing，只有 idle —— 若不留这条历史，
         * isLiveEnded() 就只能靠 frames>0 兜底（位置不行：直播位置是墙钟，一进页面就有值）。
         * 一旦确认 playing 就置位、会话内不清。 */
        if (isLivePlay && st.state === 'playing' && !this._livePlayed) {
          this._livePlayed = true;
        }
        /* ---------- 直播重连提示（P1-4 的 JS 侧配套） ----------
         * native 直播走 souphttpsrc（timeout=15s、retries=3），最坏 45 秒画面冻住而
         * state 仍是 playing，用户没有任何反馈。native2 在重连开始时 patch_error
         * (PB2_E_LIVE_RECONNECT=17)（**不动 state**），重连成功清零。
         *
         * 这里只负责翻译成提示：出现就挂「直播重连中…」，消失就撤。
         * 只撤自己挂的那句 —— 别把别人（音画恢复/加载中）的提示清掉。 */
        if (isLivePlay) {
          const reconnecting = liveReconnecting;
          if (reconnecting) {
            /* 只在「从没提示过」时挂提示 —— 不覆盖「定位中…」之类更紧急的实时状态
             * 倒是次要；关键是别让每秒一次的 tick 反复重写同一个字符串。 */
            if (this.play.note !== LIVE_RECONNECT_NOTE) {
              this.play.note = LIVE_RECONNECT_NOTE;
            }
            if (!this._liveReconnectLogged) {
              this._liveReconnectLogged = true;
              logWarn(
                '[bili] live reconnect（直播重连中）code=' + st.lastError +
                ' msg=' + (st.lastErrorMsg || '') + ' pos=' + st.positionMs
              );
            }
          } else {
            /* 只撤自己挂的那句 —— 别把「加载中…」「音画不同步…」的提示清掉。 */
            if (this.play.note === LIVE_RECONNECT_NOTE) this.play.note = '';
            /* 日志标志必须无条件复位：提示可能已被上面的「加载中…」覆盖，
             * 若只跟着 note 复位，下一次重连就再也打不出日志了。 */
            if (this._liveReconnectLogged) {
              this._liveReconnectLogged = false;
              logWarn('[bili] live reconnect 已恢复 pos=' + st.positionMs + ' frames=' + st.frames);
            }
          }
        }
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
      /* 字幕位置插值：native 每秒才回一次 positionMs，1s 粒度会让字幕明显不同步。
       * 用「上次状态位置 + 墙钟增量」在 250ms 节拍上推进；暂停/seek 时冻结。
       * 外推上限 1.2s：tick 被长任务拖住时不至于把字幕推到很远。 */
      this._ccClock = setInterval(() => {
        if (!this.ccBandVisible) {
          if (this.cc.posMs !== 0) this.cc.posMs = 0;
          return;
        }
        const a = this._ccAnchor;
        let p = a ? a.pos : this.play.positionMs;
        if (a && !a.frozen) p = a.pos + Math.min(1200, Math.max(0, Date.now() - a.at));
        if (p !== this.cc.posMs) this.cc.posMs = p;
      }, 250);
      this._intervals.add(this._ccClock);
    },
    /* ---------- 视频放大（双指捏合 1..4x） ---------- */
    async onToggleVideoZoom() {
      const on = this.videoSettings.videoZoom !== true;
      this.videoSettings = Object.assign({}, this.videoSettings, { videoZoom: on });
      saveVideoSettings(this.videoSettings);
      if (!on) {
        await this.applyZoom(1);
        this.stopPinch();
      } else if (this.mode === 'play') {
        this.startPinch();
      }
      logWarn('[zoom] enabled=' + (on ? 1 : 0));
    },
    async applyZoom(z) {
      const v = Math.max(1, Math.min(4, Number(z) || 1));
      /* 捏合时 50ms 一发，native 调用可能还没返回；这里只保留“最新目标”，
       * 由在途循环逐个落地，避免最后一帧被 _zoomBusy 吞掉（表现为放大不生效）。 */
      if (Math.abs(v - this.zoomValue) < 0.03 && this._zoomPending == null) return;
      this._zoomPending = v;
      if (this._zoomBusy) return;
      this._zoomBusy = true;
      while (this._zoomPending != null) {
        const target = this._zoomPending;
        this._zoomPending = null;
        try {
          await player.setZoom(target);
          this.zoomValue = target;
          logWarn('[zoom] set ' + target.toFixed(2) + 'x');
        } catch (e) {
          logWarn('[zoom] setZoom fail: ' + ((e && e.message) || e));
        }
      }
      this._zoomBusy = false;
    },
    /* 播放菜单「缩放」行：首次点击开启并 1.5x，之后 1.5→2→3→4→1 轮转。
     * 提供不依赖双指的手动放大入口（双指与“点画面开栏”容易互相打断）。 */
    async onZoomCycle() {
      if (this.videoSettings.videoZoom !== true) {
        this.videoSettings = Object.assign({}, this.videoSettings, { videoZoom: true });
        saveVideoSettings(this.videoSettings);
        this.startPinch();
        await this.applyZoom(1.5);
        return;
      }
      const levels = [1.5, 2, 3, 4, 1];
      let next = 1;
      for (let i = 0; i < levels.length; i++) {
        if (levels[i] > this.zoomValue + 0.01) { next = levels[i]; break; }
      }
      await this.applyZoom(next);
    },
    startPinch() {
      if (this.mode !== 'play' || this.videoSettings.videoZoom !== true) return;
      if (this._pinchTimer) return;
      if (typeof pinchStart !== 'function') return;
      try {
        pinchStart();
      } catch (e) {
        logWarn('[zoom] picpinch unavailable: ' + ((e && e.message) || e));
        return;
      }
      const tick = () => {
        if (this.mode !== 'play') {
          this.stopPinch();
          return;
        }
        this.pollPinch();
        const t = setTimeout(tick, 50);
        this._pinchTimer = t;
        this._timers.add(t);
      };
      const t = setTimeout(tick, 150);
      this._pinchTimer = t;
      this._timers.add(t);
      logWarn('[zoom] pinch started');
    },
    stopPinch() {
      if (this._pinchTimer) {
        clearTimeout(this._pinchTimer);
        this._timers.delete(this._pinchTimer);
        this._pinchTimer = 0;
      }
      this.pinch.active = false;
      this.pinch.miss = 0;
      if (typeof pinchStop === 'function') {
        try { pinchStop(); } catch (e) { /* 模块不存在时忽略 */ }
      }
    },
    pollPinch() {
      let text = '';
      try {
        text = String(pinchRead() || '');
      } catch (e) {
        return;
      }
      let count = 0;
      let distance = 0;
      const parts = text.split(' ');
      for (let i = 0; i < parts.length; i++) {
        if (parts[i].indexOf('n=') === 0) count = Number(parts[i].slice(2));
        else if (parts[i].indexOf('d=') === 0) distance = Number(parts[i].slice(2));
      }
      if (count >= 2 && distance > 1) {
        this.pinch.miss = 0;
        if (!this.pinch.active) {
          this.pinch.active = true;
          this.pinch.baseD = distance;
          this.pinch.baseZoom = this.zoomValue;
          /* 捏合开始：取消还没落地的单击定时器，避免 280ms 后误开栏。 */
          if (this._tapTimer) {
            clearTimeout(this._tapTimer);
            this._timers.delete(this._tapTimer);
            this._tapTimer = 0;
          }
          logWarn('[zoom] pinch start d=' + Math.round(distance) + ' base=' + this.pinch.baseZoom);
        }
        if (this.pinch.baseD > 0) {
          this.applyZoom(this.pinch.baseZoom * distance / this.pinch.baseD);
        }
      } else if (this.pinch.active) {
        /* 触摸板偶尔漏报一帧：连续 4 次（约 200ms）没凑齐两指才结束。 */
        this.pinch.miss = (this.pinch.miss || 0) + 1;
        if (this.pinch.miss >= 4) {
          this.pinch.active = false;
          this.pinch.miss = 0;
          this._suppressTapUntil = Date.now() + 800;
          logWarn('[zoom] pinch end -> ' + this.zoomValue.toFixed(2) + 'x');
        }
      }
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
      /* 暂停时控件必须可见（否则用户找不到继续键）；继续播放重新计时 5 秒。 */
      if (this.play.state === 'paused') {
        this.clearAutoHide();
        if (this.chromeHidden) await this.toggleChrome();
      } else {
        this.scheduleAutoHide();
      }
      logWarn('[bili] toggle → ' + this.play.state);
    },
    /* 全屏播放页：单击 = 显示/隐藏顶底栏；双击 = 暂停/继续（3.5.13 同步）。
     * 单双击共存：第一次点击延时 280ms 执行单击，期间第二击取消单击并切暂停。
     * 菜单打开时点空白处先关菜单（菜单按钮本身被隐藏，正常碰不到）。 */
    async onVideoTap() {
      if (this.playerMenuOpen) {
        await this.togglePlayerMenu();
        return;
      }
      /* 双指期间/刚结束时不要把手势尾部当成一次点击去开/关栏。 */
      if (this.pinch.active || Date.now() < (this._suppressTapUntil || 0)) return;
      if (this._tapTimer) {
        clearTimeout(this._tapTimer);
        this._timers.delete(this._tapTimer);
        this._tapTimer = 0;
        logWarn('[bili] double tap -> toggle play');
        await this.onTogglePlay();
        await this.showChromeForControl();
        return;
      }
      const t = setTimeout(() => {
        this._timers.delete(t);
        this._tapTimer = 0;
        this.toggleChrome();
      }, 280);
      this._tapTimer = t;
      this._timers.add(t);
    },
    /* 操作后确保栏可见：隐藏态先切回来；可见态只是重置 5 秒自动收起计时。 */
    async showChromeForControl() {
      if (this.chromeHidden) await this.toggleChrome();
      else this.scheduleAutoHide();
    },
    /* 隐藏/恢复状态栏：纯切换（v-if + native setRect），不做动画。
     * 隐藏时停掉标题跑马灯：栏都没了，滚动只会制造无谓的整页重绘。
     *
     * 3.9.2 修复「关栏后再次打开闪烁」：恢复栏必须**先**收窄 native 矩形、再切 UI。
     * 旧实现先切 UI、等 120ms 才 setRect，这 120ms 里 native 仍按全屏矩形每 33ms
     * 整屏覆写，Falcon 刚画好的顶/底栏被反复刷掉 → 用户看到闪烁。先收窄后，
     * native 只写视频带，栏区彻底交给 Falcon；末尾再重贴一次最后一帧，
     * 补回整页重绘（chromeTick）可能刷黑、暂停态又没有新帧可等的视频带。 */
    async toggleChrome() {
      const showBars = this.chromeHidden;
      if (showBars) {
        await this.applyVideoChromeRect(false); // 先停止整屏覆写，再让 Falcon 画栏
        this.chromeHidden = false;
        this.chromeTick = (this.chromeTick || 0) + 1;
        this.startTitleScroll();
        logWarn('[bili] chrome shown');
        await this.sleep(120);
        await this.applyVideoChromeRect(false); // 整页重绘之后重贴，避免视频带被刷黑
        this.scheduleAutoHide();
        return;
      }
      this.clearAutoHide();
      this.chromeHidden = true;
      this.chromeTick = (this.chromeTick || 0) + 1; // 触发整页重绘，清掉信箱区残影
      logWarn('[bili] chrome hidden (fullscreen)');
      this.stopTitleScroll();
      /* 等 Falcon 先把新布局画出来（约 1~2 帧），再让 native 用最后一帧重贴。
       * 顺序反了会看到「视频先铺满 → 被整页重绘刷黑 → 33ms 后补回」的闪跳。 */
      await this.sleep(120);
      await this.applyVideoChromeRect(true);
    },
    /* 把「栏显示/隐藏」对应的视频矩形下发给 native（不重启解码）。
     * hidden 显式传入目标态：恢复栏时要在 chromeHidden 翻之前先收窄矩形
     * （见 toggleChrome），不能依赖 this.chromeHidden 的当前值。
     * 起播前/无会话时 native 返回 PLAYER_NO_SESSION 属正常：open 会按
     * makeCtx().videoArea 直接开在正确几何上。 */
    async applyVideoChromeRect(hidden) {
      if (this.mode !== 'play') return;
      const fullscreen = typeof hidden === 'boolean' ? hidden : this.chromeHidden;
      const s = this.play.session || {};
      const rects = playVideoRects(
        s.videoWidth || 640,
        s.videoHeight || 360,
        playAreaFor(fullscreen, this.ccBandVisible)
      );
      const p = rects.physical;
      try {
        const r = await player.setVideoRect(p.x, p.y, p.width, p.height);
        logWarn('[bili] chrome rect ' + [p.x, p.y, p.width, p.height].join(',') +
          ' → ' + (r && r.ok ? 'ok' : ((r && r.code) || 'fail')));
      } catch (e) {
        logWarn('[bili] chrome rect fail: ' + ((e && e.message) || e));
      }
    },
    /* 切「样式」：清晰/流畅的解码尺寸不同，必须按当前位置重开一次解码管线
     * （native 的 scale 尺寸在 open 时固定，setRect 只改显示落点）。
     * 重开不重新取流：沿用 session.stream（同一条 URL），只在当前位置重启。
     * 显示矩形始终等比信箱 —— 不裁切、不拉伸，彻底修掉旧「铺满裁切」的变形。 */
    async onCyclePlayStyle() {
      const next = nextPlayStyle(this.videoSettings);
      this.videoSettings = Object.assign({}, this.videoSettings, { playStyle: next });
      saveVideoSettings(this.videoSettings);
      logWarn('[bili] playStyle -> ' + next);
      await this.applyVideoStyle();
    },
    async applyVideoStyle() {
      if (this.mode !== 'play') return;
      const s = this.play.session;
      if (!s) return;
      if (!s.stream) {
        // 老会话没有流信息（理论上不会发生）：只能改显示落点，解码尺寸保持不变
        await this.applyVideoChromeRect();
        return;
      }
      const pos = s.live ? 0 : (Number(this.play.positionMs) || 0);
      /* 重开解码会短暂没有 native 会话，旧 poll 若在空窗期读 status 会拿到错误态并
       * 触发看门狗。这里换一代 gen：先停旧 poll，成功后再用新 gen 重启轮询。 */
      const myGen = ++this.gen;
      this.stopPoll();
      const ctx = this.makeCtx(myGen);
      this.play.state = 'loading';
      this.play.note = '切换样式…';
      logWarn('[bili] style restart pos=' + pos + ' style=' + this.videoSettings.playStyle);
      await closeSession(ctx);
      if (this.gen !== myGen) return;
      const item = {
        kind: s.kind,
        bvid: s.bvid,
        cid: s.cid,
        roomid: s.roomid,
        title: s.title,
        up: s.up,
        cover: s.cover
      };
      const r = await openVideo(ctx, item, s.stream, pos);
      if (this.gen !== myGen) {
        if (r && r.ok) await closeSession(this.makeCtx(this.gen));
        return;
      }
      if (!r.ok) {
        this.play.state = 'error';
        this.play.note = describePlayError(r);
        logWarn('[bili] style restart fail stage=' + r.stage + ': ' + r.message);
        return;
      }
      this.play.session = r.session;
      this.play.state = 'playing';
      this.play.note = '';
      this.play.frames = 0;
      this._noFrameAt = Date.now(); // 重开解码有 1~3s 首帧，别让看门狗立刻判死
      /* 菜单还开着时，新会话默认会立刻贴帧把菜单盖掉；重新暂停 fb 输出，
       * 让菜单一直可见，等用户关菜单时再 resumeRender。 */
      if (this.playerMenuOpen) {
        try { await player.pauseRender(); } catch (e) { /* 仅影响菜单遮挡 */ }
      }
      this.startPoll(myGen);
      logWarn('[bili] style restart ok pos=' + pos + ' out=' + r.session.outWidth + 'x' + r.session.outHeight +
        ' decode=' + (r.session.decodePhysical ? r.session.decodePhysical.width + 'x' + r.session.decodePhysical.height : 'display'));
    },
    /* 右侧设置菜单（移植文件管理播放器）：打开时停 native 写 fb，
     * 否则下一帧视频会把 Falcon 画的菜单盖掉（文件管理播放器同款处理）。 */
    async togglePlayerMenu() {
      const open = !this.playerMenuOpen;
      this.playerMenuOpen = open;
      logWarn('[bili] playerMenu ' + (open ? 'open' : 'close'));
      /* 菜单打开期间暂停标题跑马灯：标题每 400ms 触发一次整页重绘，
       * 而 pauseRender 已停写 fb —— 重绘会把视频带刷黑后没人补帧，
       * 冻结的画面就丢了。关闭菜单后恢复滚动。 */
      if (open) this.stopTitleScroll();
      try {
        if (open) await player.pauseRender();
        else await player.resumeRender();
      } catch (e) {
        logWarn('[bili] playerMenu render ' + (open ? 'pause' : 'resume') + ' fail: ' + ((e && e.message) || e));
      }
      if (!open) this.startTitleScroll();
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
      this._seekFrames0 = this.play.frames || 0;
      this._seekNoteAt = Date.now();
      this._seekNoteTarget = 0; // 落点未定，靠帧数判完成
      this._seekSawPending = false;
      const r = await seekBy(this.makeCtx(myGen), this.play.positionMs, s.durationMs, deltaSec * 1000);
      if (this.gen !== myGen) return;
      if (!r.ok) {
        this._seekNoteAt = 0;
        this.play.note = r.message;
        logWarn('[bili] seek fail: ' + r.message);
        return;
      }
      this.play.positionMs = r.positionMs;
      this._seekNoteTarget = Number(r.target) > 0 ? Number(r.target) : Number(r.positionMs) || 0;
      // 「定位中…」交给 startPoll 的首帧判据清（见 tick 的 seekNote 段），不在这里清
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
      if (this.mode === 'article') return this.onBackArticle();
      /* 详情来自 UP 主页时，返回箭头回 UP（而不是一路回列表）——
       * 否则「搜索 → UP → 详情」按返回会跳回搜索页，上下文全丢。 */
      if (this.mode === 'detail' && this.detailFrom === 'up') {
        ++this.gen;
        this.stopTitleScroll();
        this.mode = 'up';
        logWarn('[bili] detail back to up');
        this.restoreListScroll('up');
        return;
      }
      this.onBack();
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
      // 简介来自 /x/web-interface/view 的 data.desc（列表项里没有），拿到就补进 session
      if (r.desc && this.play.session && !this.play.session.desc) {
        this.play.session = Object.assign({}, this.play.session, { desc: r.desc });
      }
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

    /* ---------- 可拖动进度条（任务 E：拖动中不下发 seek） ---------- */
    // falcon 运行时只有 changedTouches[0].pageX / pageY（offsetX/clientX/touches 全为 0），
    // 没有 panstart/panmove，所以自己按逻辑坐标算比例。
    // 几何统一由 screen.js 提供（seekRatioFromX）：轨道 = 中列 469 − 2×8 padding = 453。
    // pageX 是「相对被触摸元素」的坐标，不是整页坐标 —— 实测在 .seek 内点 x=37 得到的
    // pageX 就是 37，所以只需减 8px 左 padding，不用再减中列的绝对 x。
    seekRatioAt(x) {
      // 全屏底栏轨道 600px（物理 400），与 .fp-track 宽度一致
      return seekRatioFromX(x, FP_SEEK_TRACK_WIDTH);
    },
    /* 拖动中：**只更新本地 seekDragMs，绝不下发 seek**。
     *
     * 旧实现每次 touchmove 都走 onSeek → native seek，一次拖动会打出几十次 seek。
     * v1 的 seek 是"停掉双进程 + 重开 + 重过起播门"，实测阻塞 QuickJS 主线程 4961ms，
     * 拖一下就卡死；v2 的 seek 虽然只阻塞 0.1~25ms，但背靠背 seek 会互相干扰
     * （ADR §6.3：前一次还没 ASYNC_DONE 就来的新 seek 会拖到几秒）。
     * 所以拖动期间一次都不发，touchend 只发最后一次 —— 天然 debounce 到 0 次中间态。 */
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
    //
    // 下发纪律（P0-2 要求 1）：整个拖动期间 onSeekTouch 一次 seek 都不发，只更新本地
    // seekDragMs（thumb/fill 跟手）；touchend 再把**最后一次**落点排进 250ms debounce 队列
    // → 一次拖动最多一次 player.seek()。每次 seek 都要重建管线（实测 500ms+），连发会
    // 让画面连续黑闪，且 v2 背靠背 seek 会互相干扰（前一次没 ASYNC_DONE 就来的新 seek 拖到几秒）。
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
      /* 250ms 内再来的 release 直接顶掉前一次（不叠加、不排队两套）。 */
      if (this._seekDebounceT) {
        clearTimeout(this._seekDebounceT);
        this._timers.delete(this._seekDebounceT);
        this._seekDebounceT = 0;
      }
      // 取证：拖动期间 onSeekTouch 一次 seek 都没发，这里是唯一的下发入口。
      // 真机拖一次应当只看到 1 行 "seek 排队" + 1 行 "[bili] seekTo"（= 只调 1 次 player.seek）。
      logWarn('[bili] seek 排队 target=' + target + 'ms（debounce ' + SEEK_DEBOUNCE_MS + 'ms，拖动中下发 0 次）');
      this._seekDebounceT = setTimeout(() => {
        this._timers.delete(this._seekDebounceT);
        this._seekDebounceT = 0;
        this.seekTo(target).then(
          () => {},
          (err) => logWarn('[bili] seekTo 异常: ' + (err && err.message))
        );
      }, SEEK_DEBOUNCE_MS);
      this._timers.add(this._seekDebounceT);
    },
    /* 绝对定位（拖动结束/点击轨道时用）。P1-7 修复：直接下发目标毫秒。
     *
     * 旧实现 seekBy(positionMs, dur, goal - positionMs)，而 positionMs 来自每秒一次的
     * 轮询快照，最多旧 1s → 落点系统性偏 ±1000ms（defect-inventory P1-7）。
     * 现在走 play_session.seekTo(ctx, goal, dur)，钳制 [0, dur-1s] 在服务层做。
     *
     * 注意：这里**不等首帧**。v2 的 seek 立即返回 {ok,seekPending:true}，画面会在
     * 0.1~13.3ms（ADR §6.3）后自己换；await 只等 JSAPI 受理，不阻塞主线程。 */
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
      /* 立刻显示「定位中…」+ thumb 直接跳到目标位置（本地乐观值）。
       * 为什么不能等 native 回报位置：seek 后要到首帧才会更新 positionMs（500~600ms），
       * 期间如果继续显示旧位置，用户会觉得"拖了没反应"；而每帧轮询回来的位置是旧值，
       * 会让 thumb 往回弹。所以这里先本地写死 seekDragMs 的落点，等轮询追上再交回。 */
      this.play.note = '定位中…';
      this.play.positionMs = goal;
      this._seekFrames0 = this.play.frames || 0;
      this._seekNoteAt = Date.now();
      this._seekNoteTarget = goal;
      this._seekSawPending = false;
      const r = await seekToAbsolute(this.makeCtx(myGen), goal, dur);
      if (this.gen !== myGen) return;
      if (!r.ok) {
        this._seekNoteAt = 0;
        this.play.note = r.message;
        logWarn('[bili] seekTo fail: ' + r.message);
        return;
      }
      this.play.positionMs = r.positionMs;
      /* 「定位中…」不在这里清 —— 由 startPoll 的首帧判据清（见 tick 里的 seekNote 段）。
       * v2 的 seek 是"立即返回 + 0.1~13ms 后开始重建管线"，返回 ok 时画面还没换，
       * 立刻清提示会露出 500ms 的黑屏/旧画面。 */
      logWarn(
        '[bili] seekTo ' + goal + 'ms → pos=' + r.positionMs + 'ms' +
        (r.pending ? ' (pending)' : '') + ' frames0=' + this._seekFrames0
      );
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
    /* ---------- 评论输入（两步式，用户手绘稿 m11950）----------
     * 原 onWriteReply 是「弹系统输入法 → 直接发送」一步式；手绘稿要求右下方有常驻输入框，
     * 点框输入文字、点右侧按钮发送。所以拆成两步：
     *   onComposeReply：只负责唤起输入法并把文本回填到 cinText（不提交）
     *   onSendReply   ：把 cinText 真正 POST 出去
     * 两步共用 postReply(text)，保证校验/埋点/头插逻辑只有一份。 */
    async onComposeReply() {
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
      // 键盘避让（问题 2，goal-b29d60aa）：falcon 输入法是**独立的全屏系统 miniapp**，
      // 占满整个 960x266 逻辑画布（真机截图实测：键盘从逻辑 y=0 铺到 y=266，无留白）。
      // 而视频走 KMS overlay 平面，**盖在 falcon UI 之上** —— 所以只要平面还可见，
      // 无论缩到哪个位置都会遮住键盘的某一块（真机实测：缩到 y 0~260 后键盘顶部
      // 那一条仍被视频压住，用户根本没法点最上面一排候选词）。
      //
      // 结论：键盘弹出期间必须**把平面缩到 1x1**（等价隐藏），而不是缩小到上半部。
      // ⚠️ 不能用 w=h=0：native2 真机实测 kmssink 对零宽高**既不报错也不隐藏**，
      //    而是回退成「未设置矩形」⇒ 平面铺满 480x960，把键盘整个盖掉。
      //    1x1 才是真正不可见的合法几何（实测 `dst: pos[0,0] rect[1 x 1]`）。
      //    放左上角 (0,0)：面板上那一个像素在逻辑坐标 y=107 处的边缘，肉眼不可见。
      //
      // 关闭输入法后恢复原矩形。代价是输入时看不到画面，但这本来就是系统输入法的
      // 全屏行为（v1 老实现也是被完全遮住的，只是那时画面在下面透不出来而已）。
      //
      // 坐标系：传的 x/y 与 open 时下发的同系（视频逻辑区左缘，不含面板偏移）；
      // 面板偏移 PB2_PANEL_X_OFFSET 由 native 统一加（build_pipeline 与 SET_RECT 都加）。
      // 真机实测 orig.x=0 → 管线里是 107；1x1 时 native 不加偏移（见 session.c 防呆）。
      const phys = this.play.session && this.play.session.rectPhysical;
      let rectHidden = false;
      try {
        if (phys && phys.width > 0 && player.setVideoRect) {
          await player.setVideoRect(0, 0, 1, 1);
          rectHidden = true;
        }
      } catch (e) { /* v1 无 setRect 能力（PLAYER_UNSUPPORTED）：接受遮挡，不阻塞输入 */ }
      try {
        const inp = await input.open({
          value: this.cinText || '',
          placeholder: '友善评论，说点什么…',
          maxlength: 1000
        });
        if (this.gen !== myGen) return;
        if (inp && inp.error) {
          this.replyStatus = '输入法不可用(' + inp.error + ')';
          return;
        }
        // 取消（confirmed=false）时保留原文本，不清空用户已写的内容
        if (inp && inp.confirmed) this.cinText = (inp.text || '').trim();
        this.replyStatus = this.cinText ? '' : this.replyStatus;
      } finally {
        this.imBusy = false;
        if (rectHidden) {
          // 无论正常返回、取消还是出错，都把视频恢复到 open 时的原矩形。
          // 用 session 里存的 rectPhysical（含面板偏移的物理坐标）原样回传 ——
          // 与 native build_pipeline 拼出的 rect 完全一致（真机日志核对过
          // <107,299,240,427> 两次相同）。
          try { await player.setVideoRect(phys.x, phys.y, phys.width, phys.height); } catch (e) { /* 恢复失败只影响画面位置 */ }
        }
      }
    },
    async onSendReply() {
      const text = (this.cinText || '').trim();
      if (!text) {
        this.replyStatus = '请先输入评论内容';
        return;
      }
      const ok = await this.postReply(text);
      if (ok) this.cinText = '';
    },
    // 真正提交一条评论 → true 表示成功（发送失败时保留输入框内容，用户不必重打）
    async postReply(text) {
      const sess = await loadSession();
      const csrf = (sess.login && sess.login.bili_jct) || '';
      if (!csrf) {
        this.replyStatus = '请先在「我的」扫码登录后再评论';
        return false;
      }
      const ctx = this.replyCtx || { oid: 0, type: 1 };
      if (!(ctx.oid > 0)) {
        this.replyStatus = '当前稿件无评论上下文';
        return false;
      }
      const myGen = this.gen;
      this.replyStatus = '发送中…';
      const r = await addReply(this.client, ctx.oid, text, csrf, ctx.type);
      if (this.gen !== myGen) return false;
      if (!r.ok) {
        this.replyStatus = '发送失败：' + (r.message || '');
        logWarn('[bili] reply add fail stage=' + r.stage + ' code=' + r.code + ' ' + r.message);
        return false;
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
      return true;
    },
    // 图文态仍用一步式（那里没有常驻输入框）
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
        await this.postReply(text);
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

    /* 播放页返回（与详情/图文的 onBack 分开）：
     * 用户反馈「搜索 → UP 主页 → 详情 → 播放，返回却回到搜索页」——
     * 这里按来源回 UP 主页；其余情况回列表，并恢复列表滚动位置。 */
    async onPlayBack() {
      if (this.interact.visible) {
        await this.closeInteractPanel();
        return;
      }
      const myGen = ++this.gen;
      this.stopPoll();
      this.stopTitleScroll();
      this.clearAutoHide();
      this.clearAutoChain();
      this.clearChoiceCountdown();
      this.resetExtras();
      this.playerMenuOpen = false;
      this.chromeHidden = false;
      this.closeComments();
      // 主动离开播放页：停保活（真正该停的地方，不是 onHide）
      keepScreenOff();
      this._keepOn = false;
      this._bgPaused = false;
      await closeSession(this.makeCtx(myGen));
      this.play = idlePlay();
      if (this.playFrom === 'detail' && this.detailFrom === 'up') {
        this.mode = 'up';
        this.playFrom = '';
        logWarn('[bili] play back to up');
        this.restoreListScroll('up');
        return;
      }
      this.mode = 'browse';
      this.detailFrom = '';
      this.playFrom = '';
      logWarn('[bili] play back to browse');
      this.restoreListScroll(this.tab);
    },
    async onBack() {
      const myGen = ++this.gen;
      this.stopPoll();
      this.stopTitleScroll();
      this.clearAutoHide();
      this.clearAutoChain();
      this.clearChoiceCountdown();
      this.resetExtras();
      this.playerMenuOpen = false;
      this.chromeHidden = false;
      this.closeComments();
      // 主动离开播放页：停保活（真正该停的地方，不是 onHide）
      keepScreenOff();
      this._keepOn = false;
      this._bgPaused = false;
      await closeSession(this.makeCtx(myGen));
      this.mode = 'browse';
      this.play = idlePlay();
      this.detailFrom = '';
      this.playFrom = '';
      logWarn('[bili] back to browse');
      this.restoreListScroll(this.tab);
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

    /* ---------- 「我的 → 视频调节 / 直播」设置 ----------
     * falcon 没有原生下拉，四行设置全部是「点击轮转到下一个选项」。
     * 为什么不用弹层选择：播放页右栏已经被收藏夹覆盖层占过一次，再叠一层交互
     * 状态机会让「我的」页的分支爆炸；轮转零状态、零弹层，且当前值永远可见。
     * 每次改动立刻落盘（setStorage 才真正写），重启后仍然生效。 */
    async onCycleVideoQuality() {
      this.videoSettings = Object.assign({}, this.videoSettings, {
        videoQuality: nextOptionId(VIDEO_QUALITY_OPTIONS, this.videoSettings.videoQuality)
      });
      await saveVideoSettings(this.videoSettings);
      logWarn('[bili] videoQuality → ' + this.videoSettings.videoQuality);
    },
    async onCycleLiveQuality() {
      this.videoSettings = Object.assign({}, this.videoSettings, {
        liveQuality: nextOptionId(LIVE_QUALITY_OPTIONS, this.videoSettings.liveQuality)
      });
      await saveVideoSettings(this.videoSettings);
      logWarn('[bili] liveQuality → ' + this.videoSettings.liveQuality);
    },
    // 模板不能直接调模块函数 → 方法转发（两个 label）
    // 名字刻意与导入的 videoQualityLabel 等不同：方法内直接调同名导入虽然也能解析到
    // 模块作用域，但读代码时极易误以为递归，改名规避歧义。
    vqText() {
      return videoQualityLabel(this.videoSettings);
    },
    lqText() {
      return liveQualityLabel(this.videoSettings);
    },
    /* 播放页菜单的紧凑写法：大字号下「480p 30fps（省流）」会顶到菜单右边缘，
     * 去掉 " fps" 与空格（360p 30fps → 360p30），语义不变、省约 2 个字宽。
     * 「我的」页仍用完整写法（那一页有整行宽度）。 */
    vqMenuText() {
      return videoQualityLabel(this.videoSettings).replace(/\s+(\d+)fps/i, '$1');
    },
    lqMenuText() {
      return liveQualityLabel(this.videoSettings).replace(/\s+(\d+)fps/i, '$1');
    },

    /* ---------- 检查更新（GitHub Release，v3.5.5）----------
     *
     * 三条硬约束（都是真机实测，别按常识改）：
     *   1) 检测走**直连** api.github.com。镜像 v4.gh-proxy.org 访问 api.github.com
     *      会被共享出口 IP 限流（实测 403 rate limit exceeded），所以镜像只用于下载。
     *   2) 只用 /releases/latest —— 它天然排除 prerelease 与 draft，满足「不检测预览版」。
     *   3) 弹窗是**整页替换**不是浮层：falcon 的 position:absolute 不可靠，全仓零
     *      z-index，靠 upd.visible 让正常视图整体不渲染。
     *
     * 手动检测永远弹（用户主动点了就得有反馈，包括「已是最新」），
     * 自动检测要过 shouldAutoPrompt 三关（跳过版本 / 今天内不提示 / 不是更新版本）。
     */
    async checkUpdate(mode) {
      const m = mode === 'manual' ? 'manual' : 'auto';
      this.upd = {
        visible: true,
        mode: m,
        stage: 'checking',
        message: '正在连接 GitHub 检查最新正式版本…',
        release: null,
        lines: [],
        tip: ''
      };
      const r = await fetchLatestRelease();
      if (!r.ok) {
        logWarn('[bili] upd check fail: ' + r.stage + ' ' + (r.message || ''));
        this.upd = Object.assign({}, this.upd, {
          stage: 'error',
          message: (r.message || '网络错误'),
          tip: '检查失败不影响正常观看；稍后可再试一次'
        });
        return;
      }
      if (!isNewer(r.release.tag, APP_VERSION)) {
        this.upd = Object.assign({}, this.upd, {
          stage: 'latest',
          message: '当前 v' + APP_VERSION + ' 已是线上最新的正式版本（' + r.release.tag + '）',
          release: r.release,
          // 已是最新时也把**本版本**的发布说明摆出来 —— 用户点「检查更新」多半
          // 就是想看看这版改了什么，左栏空着一句占位反而像坏了。
          lines: bodyLines(r.release.body)
        });
        return;
      }
      this.updShowReady(m, r.release);
    },

    // 「发现新版本」：把发布说明切成行（<text> 里不能有换行，见 base.less 铁律）
    updShowReady(mode, release) {
      this.upd = {
        visible: true,
        mode: mode,
        stage: 'ready',
        message: '新版本 ' + release.tag + ' · 当前 v' + APP_VERSION,
        release: release,
        lines: bodyLines(release.body),
        tip: ''
      };
      logWarn('[bili] upd ready ' + release.tag + ' asset=' + (release.assetName || '-') +
              ' size=' + (release.assetSize || 0));
    },

    // 进入软件时的自动检测（静默失败：检查更新绝不能打断正常观看）
    async autoCheckUpdate() {
      try {
        const st = normalizeUpdateState(await getJson(KEYS.update, null));
        const r = await fetchLatestRelease();
        if (!r.ok) {
          logWarn('[bili] upd auto check fail: ' + r.stage + ' ' + (r.message || ''));
          return;
        }
        if (!shouldAutoPrompt(st, r.release.tag, APP_VERSION, Date.now())) {
          logWarn('[bili] upd auto suppressed (skip=' + (st.skipVersion || '-') +
                  ' quiet=' + (st.quietDay || '-') + ' remote=' + r.release.tag + ')');
          return;
        }
        this.updShowReady('auto', r.release);
      } catch (e) {
        logWarn('[bili] upd auto check error: ' + ((e && e.message) || e));
      }
    },

    updHide() {
      this.upd = Object.assign({}, this.upd, { visible: false, busy: false });
    },

    // 「暂不更新」/「关闭」：只关窗，不写任何持久状态（下次进软件还会提醒）
    onUpdClose() {
      if (this.updBusy) return;
      logWarn('[bili] upd close (mode=' + this.upd.mode + ' stage=' + this.upd.stage + ')');
      this.updHide();
    },

    // 「跳过此版本」：持久化到 storage，只有出现**更高**版本时才会再提醒
    async onUpdSkip() {
      const r = this.upd.release;
      if (!r) return;
      const st = normalizeUpdateState(await getJson(KEYS.update, null));
      const next = Object.assign({}, st, { skipVersion: r.tag });
      await setJson(KEYS.update, next);
      logWarn('[bili] upd skip ' + r.tag);
      this.updHide();
    },

    // 「今天内不提示」：按**本地日期**记一天（用户理解的「今天」是设备时区的今天）
    async onUpdQuiet() {
      const st = normalizeUpdateState(await getJson(KEYS.update, null));
      const next = Object.assign({}, st, { quietDay: dayKey(Date.now()) });
      await setJson(KEYS.update, next);
      logWarn('[bili] upd quiet until tomorrow (' + next.quietDay + ')');
      this.updHide();
    },

    // 「查看发布页」：词典笔上没有能唤起浏览器的 JSAPI（全仓 grep 确认），
    // 所以只能把地址显示出来让用户自己抄/扫码，不能假装能跳转。
    onUpdShowUrl() {
      const r = this.upd.release;
      const url = (r && r.htmlUrl) || REPO_URL;
      this.upd = Object.assign({}, this.upd, { tip: url });
      logWarn('[bili] upd release page: ' + url);
    },

    // 下载：先试用户选定的源；失败且没指定镜像时自动改用镜像再试一次。
    // 为什么自动回退：「我的」页手动检测只有 2 个按钮（用户在需求里明确不要选源），
    // 直连被墙时必须有兜底，否则「立即更新」在国内基本等于不可用。
    async onUpdInstall(useMirror) {
      const r = this.upd.release;
      if (!r || !r.assetUrl) {
        this.upd = Object.assign({}, this.upd, { tip: '该版本没有可用安装包，请到发布页手动下载' });
        return;
      }
      const order = useMirror ? [true, false] : [false, true];
      for (let i = 0; i < order.length; i++) {
        const mirror = order[i];
        const url = downloadUrl(r.assetUrl, mirror);
        if (!url) continue;
        const label = mirror ? '镜像' : '直连';
        this.upd = Object.assign({}, this.upd, {
          stage: 'downloading',
          message: '正在下载 ' + (r.assetName || '安装包') + '（' + label + '加速）…',
          tip: progressText(0, r.assetSize)
        });
        logWarn('[bili] upd download via ' + label + ': ' + url);
        const d = await downloadFileToDisk(url);
        if (!d.ok) {
          logWarn('[bili] upd download fail via ' + label + ': ' + (d.message || ''));
          if (i === order.length - 1) {
            this.upd = Object.assign({}, this.upd, {
              stage: 'error',
              message: '下载失败：' + (d.message || '未知错误'),
              tip: '两个下载源都失败了，可稍后重试或到发布页手动下载'
            });
            return;
          }
          continue;
        }
        logWarn('[bili] upd downloaded via ' + label + ' -> ' + d.filePath);
        this.upd = Object.assign({}, this.upd, {
          stage: 'installing',
          message: '下载完成，正在安装…',
          tip: d.filePath || ''
        });
        const ins = await installAmr(d.filePath);
        if (!ins.ok) {
          logWarn('[bili] upd install fail: ' + (ins.message || '') + ' rc=' + (ins.rc == null ? '?' : ins.rc));
          this.upd = Object.assign({}, this.upd, {
            stage: 'error',
            message: '安装失败：' + (ins.message || '未知错误'),
            tip: '安装包已下载到 ' + (d.filePath || SAVE_DIR)
          });
          return;
        }
        logWarn('[bili] upd install done ' + r.tag);
        this.upd = Object.assign({}, this.upd, {
          stage: 'done',
          message: '已安装 ' + r.tag,
          tip: '重新打开软件即可使用新版本'
        });
        return;
      }
      this.upd = Object.assign({}, this.upd, {
        stage: 'error',
        message: '没有可用的下载地址',
        tip: '请到发布页手动下载'
      });
    },

    /* ---------- 「我的」页手动检测更新 ---------- */
    onCheckUpdateTap() {
      this.checkUpdate('manual');
    },

    /* probeUpdate：检查更新全链路自证（种子 {"enabled":true,"autotest":true,"probeUpdate":true}）。
     * 为什么需要：http.downloadFile 与 pm.installPackage 的参数形态**没有文档**
     * （libfalcon.so 里只有 `[PMS]empty filepath.` 一条线索），只能靠真机日志确认；
     * 这条探针把检测/下载/安装三次调用的结论逐行打出来，一条 grep 就能判定。 */
    async runUpdateProbe() {
      logWarn('[bili] UPD-PROBE start local=' + APP_VERSION + ' repo=' + REPO_URL);
      const r = await fetchLatestRelease();
      logWarn('[bili] UPD-PROBE check ok=' + (r.ok ? 1 : 0) + ' stage=' + (r.stage || '-') +
              ' msg=' + (r.message || '-') + ' tag=' + (r.ok ? r.release.tag : '-') +
              ' asset=' + (r.ok ? (r.release.assetName || '-') : '-') +
              ' size=' + (r.ok ? r.release.assetSize : 0));
      if (!r.ok) {
        logWarn('[bili] UPD-PROBE FAIL at check');
        return;
      }
      const newer = isNewer(r.release.tag, APP_VERSION);
      logWarn('[bili] UPD-PROBE newer=' + (newer ? 1 : 0) + ' prompt=' +
              (shouldAutoPrompt(normalizeUpdateState(null), r.release.tag, APP_VERSION, Date.now()) ? 1 : 0));
      const link = downloadUrl(r.release.assetUrl, true);
      logWarn('[bili] UPD-PROBE mirror url=' + (link || '(none)'));
      if (!link) {
        logWarn('[bili] UPD-PROBE FAIL at url');
        return;
      }
      const d = await downloadFileToDisk(link);
      logWarn('[bili] UPD-PROBE download ok=' + (d.ok ? 1 : 0) +
              ' path=' + (d.filePath || '-') + ' msg=' + (d.message || '-'));
      if (!d.ok) {
        logWarn('[bili] UPD-PROBE FAIL at download');
        return;
      }
      const ins = await installAmr(d.filePath);
      logWarn('[bili] UPD-PROBE install ok=' + (ins.ok ? 1 : 0) +
              ' shape=' + (ins.shape || '-') + ' msg=' + (ins.message || '-'));
      logWarn('[bili] UPD-PROBE done ' + (ins.ok ? 'PASS' : 'FAIL'));
    },

    /* ---------- 生命周期：前后台 / 离开播放页 / 卸载 ----------
     *
     * 三种"离开"必须区分开（defect-inventory P0-6 的核心错误就是把它们混成一个 teardown）：
     *   1. 应用退到后台（onHide）—— 系统级，用户随时回来。**不能 teardown**：
     *      管线进程被杀掉后回来只会看到黑屏，得再点一次播放（用户报的"要点两次"）。
     *      正确处理：暂停播放 + 保留会话 + 停掉保活定时器，onShow 恢复。
     *   2. 离开播放页（onBack / 切 tab）—— 用户主动结束。teardown。
     *   3. 卸载（onUnload）—— 页面销毁。teardown（定时器必须清干净）。
     */
    teardown(note) {
      this.gen++;
      this.stopPoll();
      this.stopPinch();
      this._timers.forEach((t) => clearTimeout(t));
      this._timers.clear();
      this._seekDebounceT = 0; // 排队中的 seek 已随 _timers 一起清掉
      this._seekNoteAt = 0;
      cancelNativePosts(); // 丢弃在途 form POST（幂等）
      input.dispose(); // 退订系统输入法回调（幂等）
      // 真正离开播放页才停保活；定时器不清干净会在页面销毁后继续跑
      this.stopKeepScreen('teardown');
      this._bgPaused = false;
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
    /* 进入/维持播放态 → 启动 20s 屏幕保活（播放态和暂停态都要保活）。
     * 幂等：screen.js 内部是单例 keeper，重复调用只返回当前状态。
     *
     * 注意周期必须 < 30s：`hal-screen keep` 自身只续 30 秒窗口，且 30s 就是第一级
     * 调暗阈值（真机 /etc/input-event-daemon.conf 的 [Idle]）。KEEP_INTERVAL_MS=20000。 */
    startKeepScreen() {
      if (this._noKeep) return; // 诊断模式（noKeep）下不保活，保持息屏前提
      if (this._keepOn) return;
      this._keepOn = keepScreenOn();
      if (this._keepOn) {
        logWarn(
          '[bili] 屏幕保活启动 backend=' + (keepScreenBackend() || 'probe-pending') +
          ' interval=' + KEEP_INTERVAL_MS / 1000 + 's'
        );
      } else {
        logWarn('[bili] 屏幕保活不可用（brightness JSAPI 缺失）→ 30s 后系统会自动调暗，需 native 侧兜底');
      }
    },
    /* 停保活（onHide/teardown 共用）。必须打一行含 ticks 的日志：
     * 两次进出播放态后应看到 ticks=0 重新计数 —— 若看到两套 #N 交错，就是定时器泄漏。 */
    stopKeepScreen(where) {
      const wasOn = this._keepOn || isKeepingScreen();
      const ticks = keepScreenTicks();
      keepScreenOff();
      this._keepOn = false;
      if (wasOn) logWarn('[bili] 屏幕保活停止（' + where + '）ticks=' + ticks);
    },
    onShow() {
      if (this.mode === 'play') this.startPinch();
      /* 回前台：若退后台时暂停过播放，这里恢复。
       * 只恢复"我们自己暂停的那一次"，用户手动暂停的不动。
       * 不自动联网（列表/播放状态原样保留）。 */
      if (this.mode === 'play' && this._bgPaused) {
        this._bgPaused = false;
        this.startKeepScreen();
        const myGen = this.gen;
        togglePlay(this.makeCtx(myGen), false).then(
          (r) => {
            if (this.gen !== myGen) return;
            if (r && r.ok) {
              if (r.state === 'playing' || r.state === 'paused') this.play.state = r.state;
              logWarn('[bili] onShow → 恢复播放 state=' + this.play.state);
            } else {
              logWarn('[bili] onShow 恢复失败: ' + ((r && r.message) || '未知'));
            }
          },
          (e) => logWarn('[bili] onShow 恢复异常: ' + (e && e.message))
        );
        return;
      }
      // 后台期间保活被停过，回前台仍处播放态则重启
      if (this.mode === 'play') this.startKeepScreen();
      if (this.mode === 'play') this.startPinch();
    },
    onHide() {
      // 系统输入法拉起/关闭会触发 onHide —— 输入会话中不做任何处理
      //（否则 gen++ 会让输入确认结果被丢弃：表现为"返回后显示已停止且无内容"）
      if (this.imBusy) {
        logWarn('[bili] onHide during IM session → skip');
        return;
      }
      this.stopPinch();
      /* 应用退后台：**保留会话**，只暂停 + 停保活。
       *
       * 为什么暂停而不是让它后台播：本机音频输出是独占的（alsasink），后台继续播会
       * 抢走其他应用的音频；而且息屏后视频 vsink 不消费帧，白占 CPU。
       * 为什么不能 teardown：旧实现（defect-inventory P0-6）在这里 teardown，回来
       * 时播放器进程已经没了，用户看到黑屏必须再点一次 —— 这正是"要点两次"的一半成因。
       *
       * 保活必须**连定时器一起停**：应用在后台还每 20s 唤一次屏，等于永不熄屏（烧屏风险），
       * 也白耗电。onShow 会重新 startKeepScreen()，所以停是安全的。 */
      if (this.mode === 'play' && (this.play.state === 'playing' || this.play.state === 'paused')) {
        this.stopKeepScreen('onHide');
        if (this.play.state === 'playing') {
          this._bgPaused = true;
          const myGen = this.gen;
          // 不 await：onHide 是同步生命周期，await 会拖住系统
          togglePlay(this.makeCtx(myGen), true).then(
            (r) => {
              if (this.gen !== myGen) return;
              if (r && r.ok) {
                this.play.state = 'paused';
                logWarn('[bili] onHide → 暂停并保留会话（回前台恢复）');
              } else {
                logWarn('[bili] onHide 暂停失败: ' + ((r && r.message) || '未知'));
              }
            },
            (e) => logWarn('[bili] onHide 暂停异常: ' + (e && e.message))
          );
        }
        return;
      }
      // 非播放态（浏览列表等）：退后台无需保留任何资源
      this.teardown('');
    },
    onUnload() {
      this.teardown('');
    },

    /* ---------- 真机自检（storage 预置 bili_autotest 触发；
         全部复用按钮同一代码路径 —— 设备无触摸注入的替代取证） ---------- */
    // WBI 压测（诊断；种子 bili_autotest = {enabled:true, wbiStress:<N>}）：
    // 连打 N 次 nav 取密钥，统计失败率与成功时走的传输（jsapi / native）。
    /* 直播「下播」判据的引擎级自证（种子 bili_autotest = {enabled:true, autotest:true, probeLiveEnd:true}）。
     *
     * 为什么要在真机上跑这个：判据已有 Node 单测，但真机是 QuickJS，
     * `typeof` / `Number()` / 正则的行为与 V8 可能有细微差异；而真实下播无法随时复现
     * （要等主播关播，或断网等 souphttpsrc 重试耗尽）。用假 status 喂真函数，
     * 几毫秒跑完，结论可直接 grep。
     *
     * 不取流、不播放、不亮屏（屏幕保活都不会被这里触发），因此不影响防烧屏纪律。
     * 失败时额外打一行「判据数据源不一致」警告，但不中断后续流程。 */
    runLiveEndProbe() {
      try {
        const r = runLiveEndCases();
        for (let i = 0; i < r.lines.length; i++) logWarn(r.lines[i]);
        if (r.fail > 0) {
          logWarn(
            '[bili] LIVE-END PROBE 本机引擎结论与 Node 单测不一致：失败 ' +
              r.fail + ' 条 —— 判据不可信，「直播已结束」可能误判！' +
              ' 详情见上方 FAIL 行'
          );
          return { ok: false, pass: r.pass, fail: r.fail };
        }
        return { ok: true, pass: r.pass, fail: 0 };
      } catch (e) {
        // 自证代码自身抛错（比如模块加载失败）不能带崩首屏
        logWarn('[bili] LIVE-END PROBE 异常: ' + ((e && e.message) || e));
        return { ok: false, pass: 0, fail: -1 };
      }
    },
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
    /* 标签页探针（种子 {enabled:true, autotest:true, probeTab:'downloads'}）：
     * 启动后直接切到指定标签并停住，纯 UI 取证（不点按、不起播）。 */
    async runTabProbe(tab) {
      logWarn('[bili] probe tab begin ' + tab);
      try {
        this.downloads = (await loadDownloads()).items;
        await this.selectTab(tab);
        logWarn('[bili] probe tab done ' + this.tab + ' dl=' + this.downloads.length);
      } catch (e) {
        logWarn('[bili] probe tab FAIL: ' + ((e && e.message) || e));
      }
    },
    /* 搜索探针（种子 {enabled:true, autotest:true, probeSearch:'关键词'}）：
     * 跑一次真实搜索并打点耗时/条数/首尾标题 —— 用于验证「搜索慢」与
     * 「末尾结果不符」两件事，不在正常路径上执行。 */
    async runSearchProbe(kw) {
      logWarn('[bili] probe search begin kw=' + kw);
      try {
        await this.selectTab('rcmd');
        this.keyword = kw;
        this.searchResults = { video: [], article: [], live: [] };
        this.searchPages = { video: 1, article: 1, live: 1 };
        this.searchNoMoreMap = { video: false, article: false, live: false };
        const t0 = Date.now();
        await this.selectTab('search');
        const ms = Date.now() - t0;
        const n = this.searchItems.length;
        const first = n ? this.searchItems[0].title : '-';
        const last = n ? this.searchItems[n - 1].title : '-';
        logWarn('[bili] probe search result n=' + n + ' ms=' + ms + ' first=' + first + ' last=' + last);
      } catch (e) {
        logWarn('[bili] probe search FAIL: ' + ((e && e.message) || e));
      }
    },
    /* UP 主页探针（种子 {enabled:true, autotest:true, probeUp:true}）：
     * 首屏 → 第一条视频详情 → 点蓝色 UP 名 → 停在 UP 主页供截图 + 日志取证。
     * 只读链路（详情会取流解析，但不启动 native 播放）。 */
    async runUpProbe() {
      logWarn('[bili] probe up begin');
      try {
        await this.selectTab('rcmd');
        if (!this.rcmdItems.length) {
          logWarn('[bili] probe up FAIL: 推荐列表为空');
          return;
        }
        const item = this.rcmdItems[0];
        logWarn('[bili] probe up detail bv=' + item.bvid + ' itemMid=' + item.mid);
        await this.onOpenDetail(item);
        await this.sleep(600);
        logWarn('[bili] probe up detail ready mid=' + this.infoMid + ' up=' + this.infoUp);
        await this.onOpenUp();
        await this.sleep(1500);
        const c = this.up.card;
        logWarn('[bili] probe up result mode=' + this.mode + ' mid=' + this.up.mid +
          ' card=' + (c ? c.name : '-') + ' fans=' + (c ? c.fans : 0) +
          ' items=' + this.up.items.length + ' noMore=' + this.up.noMore +
          ' status=' + this.up.status);
        if (this.up.items.length) {
          logWarn('[bili] probe up first=' + this.up.items[0].bvid + ' title=' + this.up.items[0].title);
        }
      } catch (e) {
        logWarn('[bili] probe up FAIL: ' + ((e && e.message) || e));
      }
    },
    /* 播放返回探针（种子 {enabled:true, autotest:true, probePlayBackUp:true}）：
     * 推荐 → 详情 → UP 主页 → UP 投稿详情 → 播放 → onPlayBack，
     * 日志应出现 mode=up（而不是 browse/search）。用于钉住 3.9.5 的返回栈修复。 */
    async runPlayBackUpProbe() {
      logWarn('[bili] probe playback-up begin');
      try {
        await this.selectTab('rcmd');
        if (!this.rcmdItems.length) {
          logWarn('[bili] probe playback-up FAIL: 推荐列表为空');
          return;
        }
        await this.onOpenDetail(this.rcmdItems[0]);
        await this.sleep(600);
        await this.onOpenUp();
        await this.sleep(1200);
        if (!this.up.items.length) {
          logWarn('[bili] probe playback-up FAIL: UP 投稿为空 status=' + this.up.status);
          return;
        }
        await this.onOpenDetail(this.up.items[0]);
        await this.sleep(600);
        logWarn('[bili] probe playback-up detailFrom=' + this.detailFrom + ' mode=' + this.mode);
        await this.onStartPlayFromDetail();
        await this.sleep(2500);
        logWarn('[bili] probe playback-up playFrom=' + this.playFrom + ' mode=' + this.mode);
        await this.onPlayBack();
        await this.sleep(700);
        logWarn('[bili] probe playback-up result mode=' + this.mode +
          ' upItems=' + this.up.items.length + ' tab=' + this.tab);
      } catch (e) {
        logWarn('[bili] probe playback-up FAIL: ' + ((e && e.message) || e));
      }
    },
    /* 本地缓存起播探针（种子 {enabled:true, autotest:true, probeLocalPlay:true}）：
     * 进「下载」页播第一条缓存，验证「本地 mp4 → player.open → 出帧」整条链路。 */
    async runLocalPlayProbe() {
      logWarn('[bili] probe local begin');
      try {
        await this.selectTab('rcmd');
        this.downloads = (await loadDownloads()).items;
        await this.refreshDownloads();
        logWarn('[bili] probe local n=' + this.downloads.length);
        if (!this.downloads.length) {
          logWarn('[bili] probe local FAIL: 缓存列表为空');
          return;
        }
        await this.selectTab('downloads');
        await this.onPlayDownload(this.downloads[0]);
        const w = await this.waitForFrames(20000);
        logWarn('[bili] probe local play state=' + this.play.state + ' frames=' + (w.st && w.st.frames) +
          ' firstFrame=' + w.ms + 'ms note=' + this.play.note);
      } catch (e) {
        logWarn('[bili] probe local FAIL: ' + ((e && e.message) || e));
      }
    },
    /* 播放探针（缺陷 C / 屏幕保活取证专用）。
     *
     * 种子：{enabled:true, playProbe:<秒>, soakBv?:'BV...', pollEvery:1}
     * 行为：起播推荐首个稿件（或 soakBv 指定稿件），然后每 2s 打一行 [bili] probe，
     *       持续 N 秒后 onBack 收尾。**不做任何断言**，只输出证据。
     *
     * 为什么需要它：全链路 runAutotest 会主动 seek/暂停/切 tab，把"息屏期间管线是否
     * 被看门狗误杀"这件事的现场搅乱了。探针只做一件事——挂着播，让外部（关屏）干预。
     * 采样行包含 screen=亮/息屏、keep=保活后端、frames/stall，足够判定：
     *   · frames 是否持续增长（管线活着）
     *   · 息屏期间有没有 av/hard 自愈触发（日志里搜 "原地恢复"）
     */
    async runPlayProbe(seconds, bv) {
      logWarn('[bili] PROBE start ' + seconds + 's bv=' + (bv || 'auto'));
      try {
        await this.selectTab('rcmd');
        const item = bv ? { bvid: bv, title: 'PROBE ' + bv, up: '', cover: '', durationSec: 0 } : this.rcmdItems[0];
        if (!item) {
          logWarn('[bili] PROBE 推荐列表为空，无法起播');
          return;
        }
        await this.onPlayItem(item);
        if (this.play.state !== 'playing') {
          logWarn('[bili] PROBE 起播失败: ' + this.play.note);
          return;
        }
        const w = await this.waitForFrames(30000);
        logWarn('[bili] PROBE first frame ' + w.ms + 'ms frames=' + (w.st && w.st.frames) + ' pos=' + (w.st && w.st.positionMs));
        const t0 = Date.now();
        let lastFrames = (w.st && w.st.frames) || 0;
        while (Date.now() - t0 < seconds * 1000) {
          await this.sleep(2000);
          if (this.mode !== 'play') break;
          const st = await readStatus(this.makeCtx(this.gen));
          const rawScr = getScreenState();
          const scr = rawScr == null ? 'null' : screenStateName(rawScr) + '(' + rawScr + ')';
          /* 亮度/状态原始值（P0-1 要求 4）：两个 API 是两套编号，必须同时打原始数字才能
           * 判断"保活到底生效没"。真机实测语义见 screen.js 的 screenStateName 注释：
           *   getScreenState() 0=亮屏、3=调暗(DIM，能救)、1=熄灭(OFF，不强行点亮)
           *   getBrightness()  返回的是**亮度设置档位**（真机恒为 80），
           *                    屏幕被调暗到 bl_br=5 时它依然是 80 —— 所以不能只看它。 */
          const rawBright = getBrightness();
          const grew = (st.frames || 0) - lastFrames;
          lastFrames = st.frames || 0;
          logWarn(
            '[bili] probe t=' + Math.round((Date.now() - t0) / 1000) + 's screen=' + scr +
            ' bright=' + (rawBright == null ? 'null' : rawBright) +
            ' keep=' + (this._keepOn ? (keepScreenBackend() || 'probe') : 'off') +
            ' ticks=' + keepScreenTicks() +
            ' state=' + st.state + ' frames=' + st.frames + '(+' + grew + ')' +
            ' pos=' + st.positionMs + ' stall=' + (st.videoStallMs || 0) +
            ' ab=' + (st.audioBytes || 0) + ' plane=' + (st.planeActive ? 1 : 0) +
            ' alive=' + (st.pipelineAlive ? 1 : 0) +
            ' recover=' + (this._avRecoverCount || 0) + '/' + WATCHDOG_MAX_RECOVER
          );
        }
        logWarn('[bili] PROBE done（自愈触发 ' + (this._avRecoverCount || 0) + ' 次）');
        /* seek 探针：一次测三件事，缺一不可。
         *
         *   1) JS 侧往返耗时（callMs）—— 旧实现 seek 同步阻塞 4961ms
         *      （app 日志 `Msg process take too many time`）。>200ms 说明还在阻塞。
         *   2) 落点是否等于目标（landing）—— 曾经 `js_seek` 只返回 {ok:1}，
         *      seekToAbsolute 拿不到 seekTargetMs/positionMs，回落到 0，
         *      页面把 thumb 归零。所以只测耗时是假阳性。
         *   3) 到首帧耗时（firstMs）—— 从下发到 frames 增长。旧实现 6136ms 起播门。
         *
         * 关键：**每次 seek 之间必须等这条命令真的被 native 执行完再发下一个**。
         * v2 的 seek 是覆盖式 debounce（native 侧 PB2_SEEK_DEBOUNCE_MS=250），
         * 背靠背连发只会执行最后一个，前 N-1 个的耗时/落点全是假的。
         * 串行保证由 waitLanded 提供：它要求「seekPending 落回 false」+「位置靠近目标」
         * 同时成立才返回（见 play_session.js 的 waitLanded 注释），不再靠固定 sleep(300) 猜。 */
        if (this._probeSeek > 0) {
          const myGen = this.gen;
          const ctx = this.makeCtx(myGen);
          const dur = (this.play.session && this.play.session.durationMs) || 0;
          for (let i = 1; i <= this._probeSeek; i++) {
            if (this.gen !== myGen) break;
            if (this.mode !== 'play') break;
            const target = Math.floor((dur * i) / (this._probeSeek + 1));
            const t0 = Date.now();
            const r = await seekToAbsolute(ctx, target, dur);
            const callMs = Date.now() - t0;
            if (!r.ok) {
              logWarn('[bili] seekProbe target=' + target + ' FAIL ' + r.message + ' 耗时=' + callMs + 'ms');
              continue;
            }
            /* 等落点：必须先看到 seekPending 落回 false（= native 真的执行过这条命令，
             * do_seek 收尾才 pb2_stats_patch_seek(...,0,...)），再看 positionMs 靠近
             * target。只看位置会被"目标离当前位置本来就近"骗过去，就又是假阳性。
             * 判据细节与真机依据见 play_session.js 的 waitLanded 注释。 */
            const land = await waitLanded(
              function () { return readStatus(ctx); },
              target,
              { sleep: this.sleep.bind(this), failOnNewError: true }
            );
            /* 到首帧：seek 后 native 会把 frames 归零（pb2_probe_reset），所以
             * frames 第一次 >0 就是新管线真的出帧了。这是软指标（超时不算失败）。
             * 注意：若落点确认时 frames 已经 >0，说明 seek 完成前就已出新帧，
             * 再等下去只会白等一个轮询周期并把 first 谎报成 ~100ms —— 直接用
             * 落点耗时当"到首帧"的上界。 */
            const ff = !land.ok
              ? { ok: false, frameMs: -1, frames: 0, atLand: false }
              : land.frames > 0
                ? { ok: true, frameMs: land.waitedMs, frames: land.frames, atLand: true }
                : await waitFirstFrame(function () { return readStatus(ctx); }, { sleep: this.sleep.bind(this), deadlineMs: SEEK_FRAME_RETRY_DEADLINE_MS });
            logWarn(
              '[bili] seekProbe target=' + target +
              ' landing=' + (land.ok ? Math.round(land.landing) : 'FAIL') +
              /* 没落点就没有偏差可言 → n/a；不要打 0ms（那是"落点正好=目标"的意思） */
              ' drift=' + (land.ok ? Math.round(land.drift) + 'ms' : 'n/a') +
              ' call=' + callMs + 'ms' +
              ' pending=' + (r.pending ? 1 : 0) +
              ' 落点耗时=' + land.waitedMs + 'ms(采样' + land.samples + ')' +
              ' pending耗时=' + (land.sawPending ? land.pendingMs + 'ms' : 'n/a') +
              ' first=' + (ff.ok ? ff.frameMs + 'ms' : 'TIMEOUT') +
              (ff.atLand ? '(落点即已出帧)' : '') +
              (land.ok ? '' : ' 原因=' + land.error)
            );
            if (!land.ok) break; /* 落点没到，继续发只会更乱 */
          }
        }
        /* 暂停探针：量缺陷 A（暂停后恢复卡顿）。
         * 判据三项，缺一不可：
         *   1) 暂停期间 frames 完全冻结（旧实现靠 SIGSTOP 也能冻结，所以这项只是前提）
         *   2) 恢复后到下一帧的延迟（旧实现 2703ms）
         *   3) **恢复后 2s 窗口内的帧率**（旧实现掉到 10fps、有 1127/540/293ms 空档、
         *      要 10 秒才收敛；新实现应立刻满速 ~30fps）
         * 只有 3) 达标才算真的修好了——只看"能恢复"是假阳性。 */
        if (this._probePause > 0) {
          const myGen = this.gen;
          const ctx = this.makeCtx(myGen);
          for (let i = 1; i <= this._probePause; i++) {
            if (this.gen !== myGen || this.mode !== 'play') break;
            const before = await readStatus(ctx);
            if (!before.ok || before.state !== 'playing') break;
            const tPause0 = Date.now();
            await this.onTogglePlay();
            const pauseMs = Date.now() - tPause0;
            if (this.play.state !== 'paused') {
              logWarn('[bili] pauseProbe ' + i + ' FAIL 暂停未生效 state=' + this.play.state);
              break;
            }
            await this.sleep(1200);
            const held = await readStatus(ctx);
            const frozen = (held.frames || 0) - (before.frames || 0);
            const tResume0 = Date.now();
            await this.onTogglePlay();
            const resumeMs = Date.now() - tResume0;
            if (this.play.state !== 'playing') {
              logWarn('[bili] pauseProbe ' + i + ' FAIL 恢复未生效 state=' + this.play.state);
              break;
            }
            /* 到下一帧：轮询 100ms，最多 5s。旧实现 2703ms。 */
            let firstMs = -1;
            for (let k = 0; k < 50; k++) {
              await this.sleep(100);
              const s = await readStatus(ctx);
              if (!s.ok) break;
              if ((s.frames || 0) > (held.frames || 0)) { firstMs = Date.now() - tResume0; break; }
            }
            /* 恢复后 2s 的稳态帧率——这才是"卡顿有没有修好"的真判据。 */
            const win0 = Date.now();
            const s0 = await readStatus(ctx);
            await this.sleep(2000);
            const s1 = await readStatus(ctx);
            const winMs = Date.now() - win0;
            const fps = winMs > 0 ? (((s1.frames || 0) - (s0.frames || 0)) * 1000) / winMs : 0;
            logWarn(
              '[bili] pauseProbe ' + i +
              ' pause=' + pauseMs + 'ms resume=' + resumeMs + 'ms' +
              ' 暂停期新增帧=' + frozen + '(应=0)' +
              ' 恢复后到首帧=' + (firstMs < 0 ? 'TIMEOUT' : firstMs + 'ms') +
              ' 恢复后帧率=' + fps.toFixed(2) + 'fps' +
              ' 采样窗口=' + Math.round(winMs) + 'ms' +
              (fps >= 24 ? ' PASS' : ' SLOW')
            );
          }
        }
        /* SET_RECT 探针：验证键盘避让链路（JS → JSAPI → cmdq → worker → kmssink）。
         * 这一步不能靠"看代码对不对"，因为 SET_RECT 要穿过 4 层才到 kmssink，
         * 且 g_w.vsink 历史上存的是分支 bin（没有 render-rectangle 属性，会静默无效）。
         * 判据：native 日志出现缩小的 rect，再出现原 rect。
         * 注意这里**不能**用 input.open（会真的弹系统输入法、并触发宿主 onHide），
         * 直接调 player.setVideoRect 走同一条 native 路径。 */
        if (this._probeRect) {
          const phys = this.play.session && this.play.session.rectPhysical;
          if (!phys || !(phys.width > 0)) {
            logWarn('[bili] rectProbe SKIP 无 rectPhysical（会话未建立）');
          } else {
            const w = phys.width;
            const h = Math.round((w * phys.height) / Math.max(1, phys.width * 1.6));
            const shrinkH = Math.max(120, Math.min(h, 260));
            logWarn('[bili] rectProbe orig=' + JSON.stringify(phys) + ' shrink=<' + phys.x + ',0,' + w + ',' + shrinkH + '>');
            let r1 = null;
            try {
              r1 = await player.setVideoRect(phys.x, 0, w, shrinkH);
              logWarn('[bili] rectProbe shrink ret=' + JSON.stringify(r1));
            } catch (e) {
              logWarn('[bili] rectProbe shrink FAIL ' + ((e && e.message) || e));
            }
            /* 等 native 真的执行完再发恢复命令。
             * 1.5s 实测不够：探针紧跟着 onBack → closeSession → worker 收到
             * shutdown 时恢复命令还在队列里，被直接丢弃（真机日志只看到缩小那条
             * SET_RECT）。3s 留足 worker 空闲轮询的余量。 */
            await this.sleep(3000);
            /* 恢复原矩形——这一半同样重要：只缩不恢复会让视频永远停在半屏。 */
            try {
              const r2 = await player.setVideoRect(phys.x, phys.y, phys.width, phys.height);
              logWarn('[bili] rectProbe restore ret=' + JSON.stringify(r2));
            } catch (e) {
              logWarn('[bili] rectProbe restore FAIL ' + ((e && e.message) || e));
            }
          }
        }
        /* ================= 暂停态键盘避让探针 =================
         * 判据：native 日志出现 `SET_RECT <0,0,1,1> (hide) ... [paused->pulse PLAYING]`
         * 和 `SET_RECT: pulse done -> back to PAUSED`，且 debugfs 里平面真的变 1x1。
         * JS 侧只能证明命令发出去了，所以这里额外把暂停/恢复状态也打出来。 */
        if (this._probePauseRect) {
          const phys = this.play.session && this.play.session.rectPhysical;
          if (!phys || !(phys.width > 0)) {
            logWarn('[bili] pauseRectProbe SKIP 无 rectPhysical（会话未建立）');
          } else {
            logWarn('[bili] pauseRectProbe start state=' + this.play.state +
                    ' orig=' + JSON.stringify(phys));
            /* 先确保处于暂停态 */
            if (this.play.state === 'playing') {
              await this.onTogglePlay();
              await this.sleep(600);
            }
            logWarn('[bili] pauseRectProbe paused state=' + this.play.state);
            /* 暂停态隐藏平面 */
            try {
              const r1 = await player.setVideoRect(0, 0, 1, 1);
              logWarn('[bili] pauseRectProbe hide ret=' + JSON.stringify(r1));
            } catch (e) {
              logWarn('[bili] pauseRectProbe hide FAIL ' + ((e && e.message) || e));
            }
            await this.sleep(2500);
            /* 暂停态恢复原矩形 */
            try {
              const r2 = await player.setVideoRect(phys.x, phys.y, phys.width, phys.height);
              logWarn('[bili] pauseRectProbe restore ret=' + JSON.stringify(r2));
            } catch (e) {
              logWarn('[bili] pauseRectProbe restore FAIL ' + ((e && e.message) || e));
            }
            await this.sleep(2500);
            logWarn('[bili] pauseRectProbe done state=' + this.play.state);
          }
        }
        await this.onBack();
      } catch (e) {
        logWarn('[bili] PROBE 异常: ' + (e && e.message));
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
