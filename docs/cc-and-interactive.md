# CC 字幕与互动视频（3.5.6）

本文记录本轮新增的两条能力、它们所依据的**上游 B 站接口**，以及在词典笔上落地时
绕过的设备限制。接口字段以 [bilibili-API-collect](https://github.com/SocialSisterYi/bilibili-API-collect)
的 `docs/player`、`docs/video/interact_video.md`、`docs/video/videostream_url.md` 章节为准；
实现里的纯函数都有对应单测（`test/run.js`）。

## 1. 上游接口对照

| 能力 | 接口 | 关键字段 |
| --- | --- | --- |
| 字幕轨道 + 互动图版本号 | `GET https://api.bilibili.com/x/player/v2?bvid=..&cid=..` | `data.subtitle.subtitles[]`、`data.interaction.graph_version` |
| 同上（风控回退） | `GET https://api.bilibili.com/x/player/wbi/v2?..&w_rid=..` | WBI 签名，参数与 v2 相同 |
| 字幕内容 | `GET <subtitle_url>`（`//aisubtitle.hdslb.com/...json`） | `body[]: { from, to, location, content }`，**根对象没有 `code`** |
| 互动节点 | `GET https://api.bilibili.com/x/stein/edgeinfo_v2?bvid=..&graph_version=..&edge_id=..` | `data.edges.questions[].choices[]`、`hidden_vars`、`story_list`、`is_leaf` |
| 片段取流 | `GET https://api.bilibili.com/x/player/wbi/playurl` | 用选项的 `cid` 覆盖 `view` 返回的首段 cid |

### 字幕轨道

`data.subtitle.subtitles[]` 单项：

```json
{ "lan": "ai-zh", "lan_doc": "中文", "ai_status": 1, "id": 1,
  "subtitle_url": "//aisubtitle.hdslb.com/bfs/ai_subtitle/prod/xxx.json?auth_key=.." }
```

- `subtitle_url` 常为**协议相对**地址，取数前补 `https:`；`auth_key` 有时效，不能缓存 URL。
- 匿名请求的字幕列表恒为空（B 站要求 `SESSDATA`），这是正常态而不是错误。
- 字幕文件是纯 JSON（无 `code` 字段），因此**不能**走 `client.request()`——那条通道按
  `code !== 0` 判失败。项目为此加了 `client.getRawJson()`。
- `from` / `to` 单位是秒（浮点），内部统一换算成毫秒；`to <= from` 的异常行给 1.2s 最小窗口。

### 互动节点（`edgeinfo_v2`）

- `graph_version` 取自 `x/player/v2` 的 `data.interaction.graph_version`；
  `edge_id` 为 0 或省略时返回**起始模块**。
- `data.story_list[]` 里 `is_current=1` 那项给出当前片段的 `cid`；
  `edges.questions[].choices[]` 里每个选项的 `cid` 与 `id` 分别是**下一片段的 cid**
  与**下一节点的 edge_id**。`cid` 缺失时从 `platform_action: "JUMP <edge_id> <cid>"` 兜底。
- `is_default=1` 是倒计时结束后自动走的选项；`is_hidden=1` 不显示。
- `condition` 形如 `$H7g_64_PG2EVS>=1.00 && $H7g_64_PG2EVS<=80.00`，
  变量来自 `hidden_vars[].id_v2`（`type=2` 是随机值，每个节点刷新）。
- `native_action` 形如 `$a=1;$b=$b+1`，是点选项后对隐藏变量的赋值语句。
- `is_leaf=1` 表示结局片段，没有 `questions`。

## 2. 设备侧的关键约束与解法

### 2.1 字幕不能做成 UI 浮层

X6 Pro / X7 Pro 的视频走 KMS overlay 平面（`zpos` 高于 Falcon UI），**UI 文字画不到
视频上面**；`base.less` 里「进度条必须落在视频矩形之外」说的就是这件事。同时本项目
不使用 `position:absolute` / `z-index`（falcon 上不可靠），所以叠层方案全部排除。

解法是**让出一块真实的 UI 像素**（3.5.6 起升级成四段模型，见
[`player-chrome.md`](player-chrome.md)）：

```
播放中列高度 = 顶部悬浮条 + 视频洞 + 字幕带 + 控制条 = 页高（恒等）
                  18        H-72      28      26
```

- `src/services/play_chrome.js::planBands()` 是唯一几何来源，页面的
  `videoBoxStyle` / `ccBarStyle` / `topBarStyle` / `ctrlBarStyle` 与
  `play_session.js::openVideo()` 的 `bandBox()` 都用它；
- 段落名改了：原来的 `ccReservePx` 拆成 `bandPx()`（`{top, bottom}`），
  `play_session` 仍兼容只有 `ccReservePx` 的旧 ctx；
- native 的 `render-rectangle` 与洞严格同源；
- 播放中切换字幕开关/语言、或展开/收起控件，都不重取流，只调
  `player.setVideoRect()` 热更新矩形。
- 字幕位置用「上次 `status().positionMs` + 墙钟增量」在 250ms 节拍上插值，
  避免 1s 轮询粒度导致明显不同步；暂停与 seek 期间冻结。

字幕文本渲染在 UI 平面的字幕带里，**不烧帧、不插 GStreamer 元素**，
不会重演 v2.7.0 弹幕 `drawtext×4` 拖垮解码的问题。

### 2.2 互动片段的连播

互动视频的「分 P」与普通分 P 不互通：`view` 返回的是首段 cid，后续每段 cid 只在
`edgeinfo_v2` 的选项里。因此 `resolveVideoUrl()` 增加了 `item.useCid` 通道——带这个
标记的 item 用 `item.cid` 取 `playurl`，`view` 仍照常请求（拿 `aid` / 显示宽高 / 标题）。

`view` 对 stein 稿件常返回 `duration: 0`，会让末尾判据永远不触发。所以
`parseHtml5Response()` / `parseDashResponse()` 都读 `playurl` 的 `timelength`（毫秒），
`openVideo` 把它写进 `session.durationMs`，轮询里再用 session 值兜底一次。

一次连播的完整时序（2026-11 按真实 `edgeinfo_v2` 抓包修正）：

1. 起播成功 → 异步 `x/player/v2`；`interaction.graph_version > 0` 即进入互动模式。
2. `edgeinfo_v2(edge_id=0)` → 归一化节点、合并隐藏变量、按条件过滤出本段可点选项。
3. **题面窗口**：`start_time_r` 实测恒为 `300`（= 片尾前 3.00s），到点才把选项
   面板摆到右栏；新片段一开始**不抢评论区**（整段都盖住评论区是错的）。
4. 末尾判据触发 → `handleSegmentEnd()`（判定逻辑在 `segmentEndPlan()`）：
   - `type=0`（自动跳转片段，选项文本是空的 `"A "`）→ 350ms 后直接连播；
   - `pause_video=1`（真实稿恒为 1）→ 片尾**停住等用户点**，不再 900ms 自动跳；
     `duration>0`（毫秒）才倒计时，到点走默认项；
   - `pause_video=0` 且有默认项 → 900ms 后自动连播；
   - `is_leaf` → 停在结局，提供回退 / 重播。
5. `playSegment()` 只重建播放会话（同一页面、同一评论区上下文），
   换段后按新 cid 重拉字幕、拉下一节点选项，历史快照压栈供「回退」。

> **踩坑（已修）**：`story_list` 是「进度回溯」链，不是当前模块。用
> `edge_id=48360986` 请求「1 第一循环」时，它仍返回「0 片头 / cid=245682070 /
> is_current=1」。拿 `story_list.is_current.cid` 当当前段会一直重播上一段；
> 现在 cid 只认调用方（起播 cid / 选项 cid / 回退快照）。

`condition` / `native_action` 用 `src/services/bili/interactive.js` 里的**极小递归下降
解析器**求值（数字、`$变量`、比较、`&&`/`||`、`+ - * /`、括号、赋值），不使用 `eval`。
解析不了的条件一律判 `true`：宁可多显示一个选项，也不能把用户锁死在没有出口的片段。

## 3. 代码位置

| 文件 | 职责 |
| --- | --- |
| `src/services/bili/player_v2.js` | `x/player/v2` 请求 + WBI 回退、字幕轨/互动图解析 |
| `src/services/bili/subtitle.js` | 设置 schema、URL 白名单、`body[]`→cues、二分取句、下载 |
| `src/services/bili/interactive.js` | `edgeinfo_v2`、节点归一化、变量表达式、选项/连播判据 |
| `src/services/bili/client.js` | `getRawJson()`（无 `code` 的纯 JSON GET） |
| `src/services/bili/playurl.js` | durl 回退补 `timelength` |
| `src/services/play_session.js` | `useCid` 覆盖、`bandPx()` 分段矩形、session 存宽高 |
| `src/pages/index/index.vue` | 字幕带、选项面板、插值时钟、连播状态机 |
| `src/styles/base.less` | `.cc-*` / `.ip-*` 样式 |

## 4. 真机验收

```bash
# 1) 装包（同版本号覆盖可能被拒，先卸载）
adb push 8002026110200001.3_5_6.amr /tmp/
adb shell "miniapp_cli uninstall 8002026110200001"
adb shell "miniapp_cli install /tmp/8002026110200001.3_5_6.amr"   # 必须返回 "ret":0
```

**CC 字幕**：登录后在「我的 → CC 字幕」打开开关 → 播一个带 CC 字幕的稿件。
期望「字幕语言」显示轨道名，视频画面等比缩小、下方出现白字字幕带，字幕随声音切换。
切换语言后字幕内容随之变化；关掉字幕后画面恢复原尺寸。

**互动视频**：打开一个互动视频稿件。期望片尾前 3 秒右栏才出现「第 N 段 · xxx」
和本段选项；`pause_video=1` 的片段播完会停在选项面板等点击（不再自动跳过），
点选项后直接播下一段（画面不回列表）；`type=0` 的过渡片段自动连播、不弹面板。
点「回退」回到上一段，点「重播本段」从头播当前段。

若字幕列表为空，状态行会提示「无字幕 · 登录后可看 AI 字幕」；匿名态 B 站本来就不返回
字幕轨道。互动视频匿名态 `story_list` 只有起始模块，回退功能由本地历史快照保证。

## 5. 已知取舍

- 不做 `preload.video[]` 预取流：设备带宽与内存有限，且 `playurl` 的 `auth_key` 有时效，
  提前取反而容易拿到过期 URL。换段时重新取流，实测起播耗时与普通点播一致。
- 不实现 `questions[].type=2` 的坐标定点选项：设备屏幕只有 936×280 逻辑像素，
  上游的 1920×1000 坐标映射后选项会互相重叠，统一用右栏列表呈现。
- 不实现 `no_evaluation` 相关的结尾评分，只保留回退 / 重播。
- 字幕只显示当前一句（`from <= pos < to`），不做多行堆叠：字幕带只有 28px（12px × 2 行）。
