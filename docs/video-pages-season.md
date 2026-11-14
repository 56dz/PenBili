# 分P 与合集（3.5.6 第十七轮）

> 目标：播放页能切换多 P 稿件的每一段，也能查看并切换稿件所属的 UP 主合集。
> 数据全部来自 `x/web-interface/view` 的**同一次响应**，不额外发请求。

## 1. 数据在哪

```
x/web-interface/view?bvid=...
  ├─ pages[]           分P 列表（单P 也有一条）
  │    { cid, page, part, duration, dimension }
  ├─ videos            分P 数（老字段，与 pages.length 一致）
  ├─ duration          ⚠️ 多P 时是**所有 P 之和**
  └─ ugc_season        合集（不属于合集时字段不存在）
       { id, title, ep_count,
         sections: [ { id, title, type,
                       episodes: [ { aid, bvid, cid, title,
                                     arc: { pic, title, duration, desc },
                                     page: { cid, page, part, duration } } ] } ] }
```

解析在 `src/services/bili/video_meta.js`（纯函数）：

| 函数 | 作用 |
| --- | --- |
| `parsePages(view)` | → `[{ cid, page, title, durationSec }]`；`part` 空时用 `P<序号>` |
| `parseSeason(view)` | → `{ id, title, count, sections:[{ title, items:[...] }] }` 或 `null` |
| `pageIndexOf(pages, cid)` | 当前 P 的下标（-1 = 不在列表里） |
| `episodeIndexOf(season, bvid)` | 当前集在合集的 `{ si, ii }` |
| `fmtDur(sec)` | 行尾的 `mm:ss` / `h:mm:ss`，0 → 空串 |

`client.fetchView` 把它们挂到返回值上；`play_session.resolveVideoUrl` 再把
`pages / season / part / desc` 原样带进 `session`，页面不需要第二个接口。

## 2. 三个必须处理的坑

### 2.1 多P 的 `view.duration` 是总和

旧代码：

```js
const durationSec = v.duration;   // 3P 各 100s → 300
```

结果是播 P1 时进度条显示 `5:00`，永远走不到头，片尾自动连播也不会触发。
现在只要 `pages.length > 1` 就改用当前 P 自己的时长：

```js
const durationSec = currentPage && currentPage.durationSec > 0
  ? currentPage.durationSec
  : v.duration;
```

单测 `play_session: 多P稿件用当前P的时长` 直接守这条。

### 2.2 合集的 bvid 可能在 `arc` 里

不同版本的服务端响应形态不一致：有的 `episode.bvid` 在顶层，有的只在
`arc.bvid`，还有两边都有的。解析器两边都取，都取不到就**跳过这一集** ——
宁可少一行，也不能拿空 bvid 去请求（会 404 并在播放页弹错误）。

### 2.3 `episode.title` 可能只是序号

有些合集返回 `title: "1"`、`"2"`。这时用 `arc.title`。判据是
`/^\d+$/`，不是"长度小于 3"（真有叫「01」的正常标题）。

### 2.4 老响应的 `episodes` 在顶层

极少数老合集没有 `sections`，`episodes` 直接挂在 `ugc_season` 下。
解析器在没有解析出任何 section 时才走这条兜底，正常响应不受影响。

## 3. 界面：右栏页签

右列只有 278×280，没有地方做浮层（falcon 的 `position:absolute` / `z-index`
不可靠），所以沿用评论区那套「整栏替换」：

```
┌──────────────────────────┐
│ 评论 128 │ 分P 3 │ 合集 42 │  ← 22px，只在真的有得选时出现
├──────────────────────────┤
│ P1 第一段          12:31 │  ← 26px 一行
│ P2 第二段          08:04 │  ← 当前 P 高亮（▶ + 品牌粉）
│ P3 第三段          15:22 │
│ 共 3 P · 点任意一段切换   │
└──────────────────────────┘
```

高度账（X7：右列 253 + 输入框 27 = 280）：

| 页签 | 组成 | 合计 |
| --- | --- | --- |
| 评论 | 页签 22 + 评论体 231 + 输入框 27 | 280 |
| 分P / 合集 | 页签 22 + 列表 258 | 280 |

- 页签高 `EP_TAB_H = 22`，从评论体里扣（`replyBodyStyle`），总高恒等于视口高。
  不把右列撑高：那 22px 会从视频区抢。
- 单 P 且无合集的稿件**不渲染这条**，右栏与以前逐像素相同。
- 三个页签是三个独立 `v-if`（不是 `v-else-if` 链）：falcon 对跨标签名
  （`<template>` / `<scroller>`）的 v-else 链支持不稳。
- 合集多节时插一行节标题（正片 / 花絮…）；单节不显示，省一行可见。

`epTabNow` 是渲染用的真相：`ep.tab` 可能停在一个已经失效的值上
（切到没有分P的稿件），这时回落到 `reply`，右栏不会空一片。

## 4. 切换：复用完整的起播流程

```js
switchEp(kind, row) → onPlayItem({ bvid, cid, useCid: true, keepEp: true, ... })
```

为什么不做「轻量 seek」：换 cid 等于换一条流（时长 / 宽高 / 音轨都可能不同），
必须重新 `openVideo` —— 它会把 hole 矩形、`render-rectangle`、字幕轨道、
互动 `graph_version`、评论 oid 全部按新稿件重算。复用 `onPlayItem` 是唯一
不会漏项的路。

几个细节：

- `useCid: true` 让 `resolveVideoUrl` 用 `item.cid` 而不是 `view.cid`（互动视频
  早就在用同一条机制）。
- `title: ''`：让 `openVideo` 用 **view 的稿件标题**。分P 的名字由
  `session.part` 单独带回，左列标题显示「稿件名 · P2 第二段」。
- `keepEp: true`：切换期间右栏列表保留，失败时用户还能在原列表里再点一次。
  换稿件（从列表点新视频）不传它，旧的分P 列表会立刻清掉 —— 否则会拿旧 cid
  去请求新 bvid，必然 404。
- 起播成功后才更新 `ep.pages / ep.season`：失败时保留旧数据。
- 简介用 `rs.desc || item.desc`：切集时必须用新稿件的简介，不能沿用上一集。

## 5. 单测

`test/run.js`：

- `video_meta: 分P 解析（part 兜底 / 缺 cid 跳过 / 单P）`
- `video_meta: 合集解析（多节 / bvid 只在 arc / 纯数字标题 / 顶层兜底）`
- `video_meta: 当前 P / 当前集的定位与时长格式`
- `client: fetchView 带回分P与合集（页面页签的数据源）`
- `play_session: 多P稿件用当前P的时长（view.duration 是所有P之和）`

## 6. 已知限制

- 不记忆「看到第几 P」的播放进度（B 站会记），换设备/重启从 0 开始。
- 合集只认 `ugc_season`（UP 主合集），不认番剧 `season_id` 与「系列」
  （`x/series/archives`）—— 这两类在笔端的使用频率远低于前者。
- 合集列表不做封面缩略图：右列 278 宽，一行 26 高放封面只会让标题更短。
