# X6 Pro 适配记录

本文件记录把 PenBili 从**有道词典笔 X5**（逻辑 800×254）适配到 **X6 Pro**（逻辑 960×266）的过程、
真机测得的几何常量、以及踩到的坑。所有数字都来自真机截屏与像素测量，不是估算。

## 1. 屏幕几何（真机实测，权威）

X6 Pro 的 Falcon 逻辑画布是 **960×266**，物理面板 **480×960**，`direction = 270`。

```
cfg.json: {"width":960,"height":266,"xoffset":0,"yoffset":107,
           "direction":270,"tp_direction":270,"tp_xoffset":113}
物理 DSI 模式: 480×960
```

266 的逻辑宽度落在 480 宽的面板里，两侧各留 `(480-266)/2 = 107` —— 正好等于 cfg 的 `yoffset`。
**这个 107 由显示服务端负责，应用侧不需要加。**

### 关键结论：`setViewPort(W)` 是把 W 个设计像素映射到整块画布宽度

适配第一步本来想「保持 800 设计宽度，让运行时信箱留边」，实测证明这是错的：

| `DESIGN_WIDTH` | `.bar`（174 CSS px）实测物理宽 | 缩放 | 结果 |
| --- | --- | --- | --- |
| 800（旧值） | 209 px | **1.2×** | 页面 254 高 ×1.2 = 305 > 266 → **底部裁掉 39px** |
| 960（新值） | 173 px | **1.0×** | 无裁剪，1:1 更锐利 |

所以 `setViewPort(W)` 会把 W 设计宽度**拉伸铺满**整块画布，而不是留黑边。
X5 上 `setViewPort(800)` 恰好 1:1 只是因为它本来就等于屏宽。

**结论：`src/app.js` 的 `DESIGN_WIDTH` 必须是 960，CSS 设计画布就是 960×266。**

### 栏位骨架（960 宽怎么分）

按 iPad 版 B 站重做后的最终几何（v3.0.0）：

```
浏览态：  左导航栏 76 + 内容区 884 = 960（内容区 5 列封面卡片）
播放态：  左图标栏 64 + 间距 16 + 视频区 432 + 右栏 448 = 960
          视频区高度 240，其下 26px 给可拖动进度条
图文态：  左图标栏 64 + 正文区 896 = 960
```

对应 `src/styles/base.less` 顶部注释与 `src/services/screen.js` 的
`BAR_WIDTH = 76` / `ICON_BAR_WIDTH = 64` / `ICON_BAR_GAP = 16` / `VIDEO_COL_WIDTH = 432`。

早先版本按「176 / 608 / 176」三栏分，左导航栏 176 太宽（按钮文字只占约 60px），
视频区 608 又挤掉了右栏的空间，所以 v3 直接改成窄栏 + 大内容区。

## 2. 触控坐标映射（X6 Pro 用 USB HID 注入，没有 `input` 命令）

设备上没有 `input` 命令，触控只能自己注入。逻辑坐标 → 面板坐标：

```
px = 107 + ly
py = 479 - lx
```

（`107` = 266 宽的带在 480 面板里的居中偏移，`direction=270` 决定旋转方向。）
`dev/x6tap.c`（zig 交叉编译成 aarch64 静态 ELF）实现了 `tap` / `swipe` / `raw`，
`dev/tap.sh`、`dev/seq.sh`、`dev/shot.sh` 是配套脚本。

## 3. 踩到的坑

### 3.1 `flex: 1` 简写在 Falcon 运行时**不可用**

编译器 `falcon-styler` 的 validator 认 `flex`（产物里也能看到 `"flex":1`），但运行时会把
flex 子项算错：`.searchbar` 里的搜索按钮声明的宽度是 76px，真机上渲染成 **367px**，把关键词
输入框挤没了。

必须用三行长写法：

```less
flex-grow: 1;
flex-shrink: 1;
flex-basis: 0px;
```

`base.less` 里 11 处 `flex: 1` 全部改成长写法后，按钮回到 76px，全部布局对齐。
（`flex-direction` / `flex-wrap` / `justify-content` / `align-items` 这些**属性**是可靠的，
`falcon-ui` 的 `layout/col.vue` 本身也是用 `flex-grow/flex-shrink/flex-basis` 写网格列。）

### 3.2 封面图域名在本机解析不了

`i0/i1/i2.hdslb.com` 在 X6 Pro 上 `getaddrinfo` 失败（`Temporary failure in name resolution`），
`nslookup` 直连 223.5.5.5 也超时；按 IP 直连同样超时（SNI/Host 层面被拦）。

但 **`archive.biliimg.com` 可以访问，路径和 hdslb 完全一致**：

```
http://archive.biliimg.com/bfs/archive/<hash>.jpg   → 200
```

所以新增 `src/services/imgurl.js` 的 `normalizeImageUrl()`，把任意 `*.hdslb.com` 重写到
`http://archive.biliimg.com` 并保留 path/query；`feed.js`、`bili/article.js`、`history.js`
都走它。**注意：旧版本已写进设备存储的播放历史里是裸 hdslb 地址，`normalizeHistory()` 也要过一遍**，
否则历史封面还是灰块。

### 3.3 native 模块必须是 aarch64

X6 Pro 是 aarch64，`dynamic_load_jsapi` 直接拒绝 32 位库：

```
libjsapi_player_xxx.so: wrong ELF class: ELFCLASS32
```

即使设备上有 `/lib/ld-linux-armhf.so.3` 也不行。`libs/` 里的 `.so` 必须重编到
`aarch64-linux-gnu.2.17`（`EI_CLASS=2`、`machine=183`）。`tools/inspect_elf.py` 里对
`ei_class == 1` 的硬断言也要放宽。

### 3.4 `<text>` 的内容不能换行写

Falcon 的 `<text>` 标签，内容**写在新的一行就整段不渲染**（一个像素都不画），CSS 完全正确也没用：

```html
<!-- ✅ -->
<text class="mine-id">{{ profile.mid }}</text>

<!-- ❌ 真机空白 -->
<text class="mine-id">
  {{ profile.mid }}
</text>
```

「我的」页头卡第二行（UID）整行消失就是这么来的：编译产物里 `mine-id` 的
`width` / `fontSize` / `lines` 都在，问题纯在模板。改单行后立刻正常。

### 3.5 行容器里的纵向子列要给显式尺寸

`flex-direction: row` + `align-items: center` 的容器里，如果子列只靠 `flex` 撑宽
（`mine-idcol`），它内部的**第二行文字同样会消失**。给这一层显式 `width` / `height`
（现在是 `width: 740px; height: 36px`）就恢复正常。

### 3.6 播放态根节点不能无条件透明

`page-live`（根节点背景 `rgba(0,0,0,0)`）是为了让 DRM 视频平面透出来，但**只有视频平面真的存在时**才该透明。
原来写成「只要进播放态就透明」，起播失败时中间列会透出上一屏的残留像素（能看到半截旧封面）。
现在收紧成 `mode === 'play' && holeVisible()`（真的在 playing/paused）才加 `page-live`。

## 4. 真机验证

截图在 `docs/screenshots/`，全部 960×266，来自真机 `miniapp_cli capture`：

| 文件 | 画面 |
| --- | --- |
| `x6-home.png` | 推荐流（4 列封面卡片网格 + B 站粉圆角方形导航） |
| `x6-hot.png` | 热门 |
| `x6-search.png` | 搜索「图文」栏结果 |
| `x6-article.png` | 图文详情（正文列 592 宽） |
| `x6-comments.png` | 播放态评论区浮层 |
| `x6-mine.png` | 我的（登录态、播放历史卡片） |
| `x6-qr.png` | 扫码登录（二维码取景盒 228×228 居中） |
| `x6-diag.png` | 链路自检页 |

> `miniapp_cli capture` **只能截到 Falcon 画布，截不到 DRM 视频平面**（`captureFB` 在本机返回
> `{"ret":-1}`，没有 `/dev/fb0`）。视频画面本身要用
> `gst-launch-1.0 kmssrc plane-id=<N> num-buffers=1 ! videoconvert ! pngenc ! filesink location=...`
> 回读。所以上表没有「播放中」截图。

## 5. 回归测试

`src/services/screen.js` 的几何纯函数（`fitLogicalRect` / `logicalRectToPhysicalRect` /
`playVideoRects` / `probeVideoRects`）全部有单测，适配 960 时只改常量就能跑：

```bash
cd PenBili && node test/run.js     # 122 passed
```

新增的两条与本次适配直接相关：
- 16:9 视频在 960×266 下的物理矩形 → `{x:0, y:450, width:240, height:427}`
- 竖屏稿件在 432 列信箱后的物理竖柱矩形

## 6. 已完成项（v3.0.0）

- `libs/libjsapi_player.so` 已重编为 aarch64（`ELFCLASS64` / `e_machine=183`），
  视频走 MPP 硬解 + KMS 覆盖平面，旋转交给 `mppvideodec`。详见
  [`x6pro-video-playback.md`](x6pro-video-playback.md)。
- 直播改本地直连，云端转码服务器与「我的 → 直播设置」整体移除。
