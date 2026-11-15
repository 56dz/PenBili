# 自适应单位（PStore 规则 2）

本文回答一个问题：**PenBili 的界面尺寸为什么这样写，换成别的机型会不会显示不全。**
结论先行：**布局尺寸全部是 `vw` / `vh`，只有 159 处刻意保留的 `px`，每一处都能指出理由。**

## 1. 换算基准

`App.setViewPort(w)` 声明设计画布宽，运行时会按 `scale = clientWidth / viewPort`
把画布缩放铺满屏幕。本项目的设计画布是**运行时探测到的视口尺寸**（见
`src/services/device_profile.js`），X6 Pro 上是 960×266：

| 单位 | 与 px 的关系 | 说明 |
| --- | --- | --- |
| `1vw` | `9.6px` | 视口宽的 1% = 960 × 1% |
| `1vh` | `2.66px` | 视口高的 1% = 266 × 1% |

换算公式（写代码时用）：

```
vw = px / 9.6        vh = px / 2.66
px = vw * 9.6        px = vh * 2.66
```

例：`font-size: 15px` → `font-size: 1.5625vw`；
    `height: 26px`  → `height: 9.7744vh`。

## 2. 什么时候用 vw、什么时候用 vh

| 场景 | 用哪个 | 理由 |
| --- | --- | --- |
| 宽度、左右 margin/padding、横向定位 | `vw` | 跟着画布宽等比缩放 |
| 高度、上下 margin/padding、纵向定位 | `vh` | 跟着画布高等比缩放 |
| **近正方形元素** | **两个方向都用 `vw`** | 混用 vw+vh 时，画布宽高比一变（960:266 ≈ 3.6:1 → 936:280 ≈ 3.34:1）方形就会变椭圆 |
| 整屏高 | `height: 100vh` | 不写死 266 |
| 整屏宽 | `width: 100vw` | 不写死 960 |

已经按「近正方形两个方向都用 vw」处理的元素：`.bar-logo`（30×30）、
`.ibtn-icon`（26×26）、`.ibtn-icon-big`（32×32）、`.avatar`（44×44）、
`.pback-icon`（22×22）、`.pbtn`（65×62）、`.pbtn-icon`（26×26）、`.seek-knob`（10×10）。

## 3. 刻意保留 px 的三类（共 159 处）

规则是：**凡是能随画布等比缩放的都转了；转不了或不该转的保留 px 并就地标注 `px-keep`。**

### 3.1 与硬件 KMS 平面逐像素重合的矩形（17 处）

视频不是画在 Falcon 画布上的，而是走 **DRM/KMS 硬件叠加平面**（`kmssink`，
`render-rectangle` 下发的是**物理面板坐标**）。UI 侧挖的洞必须和平面矩形
逐像素对齐，否则会出现"洞比画面大一圈"或"画面被洞裁掉一条"。

| 选择器 | 值 | 位置 |
| --- | --- | --- |
| `.pvideo` / `.video-hole` / `.play-idle` | `469px × 240px` | `src/styles/base.less:743-755` |
| `.pvideo-live` / `.video-hole-live` / `.play-idle-live` | `height: 266px` | `src/styles/base.less:771-779` |
| `.seek` | `width: 469px` | `src/styles/base.less:792` |
| `.seek-track` | `width: 453px` | `src/styles/base.less:801` |

这些值同时被 `src/services/screen.js` 的 `VIDEO_AREA` / `LIVE_VIDEO_AREA`
和 native 侧的 `render-rectangle` 使用，三处必须一致。

### 3.2 位图点阵量化（102 处）

二维码是按「每格 N 像素」逐行渲染的（`<div class="qr-row">` + `<div class="qr-seg">`），
格子宽度写在一组预生成类里（`.qr-l-1` … `.qr-l-65`、`.aq-l-1` … `.aq-l-37`）。
转成 vw 会引入亚像素误差，导致扫描器识别失败 —— 这是**能扫出来**和**扫不出来**的区别，
不是审美问题。容器尺寸同样固定：`.qr-box` 228×228、`.mine-about-qr` 74×74、
行高 `.qr-row` 4px / `.aq-row` 2px。

### 3.3 零值与 hairline（28 处）

- `flex-basis: 0px`（19 处）：falcon 运行时 **`flex: 1` 简写无效**，必须写
  `flex-grow/flex-shrink/flex-basis` 三连；`0px` 是「不占基础宽度」的语义常量，不是长度。
- `border-width: 1px`（9 处）：1 像素描边。转成 `0.1042vw` 后在非 960 画布上会
  变成 0.97 或 1.03 像素，被渲染器取整成**时有时无**的抖动边。

## 4. 验收方式

```bash
# 字号已零 px（PStore 最常抓的一条）
grep -rn "font-size:[^;]*px" src/ | wc -l     # 必须为 0
grep -rn "line-height:[^;]*px" src/ | wc -l   # 必须为 0

# 保留的 px 必须都能指出理由
grep -rn "[0-9]px" src/styles/*.less | grep -v "px-keep"
```

单测里钉住了这两条（`test/run.js` 的 `units:` 与首页样式用例）：
字号不得出现 px、`.upd-scroll` / `.upd-card` 的尺寸折回 960×266 后必须等于
150px / 432px / 760px（**断言比较折算后的像素值而不是字面单位**，这样换成 vw 也不会
让用例失效，但改错尺寸仍会红）。

## 5. 多机型验证状态

| 机型 | 视口 | 状态 |
| --- | --- | --- |
| X6 Pro | 960×266 | **实测通过**，5 个页面截图逐张目视比对，与改造前一致 |
| X7 Pro | 936×280 | 代码按 `vw`/`vh` 等比缩放 + 运行时画像，**无真机** |

X7 Pro 的视口比 X6 Pro 宽 24、高 14。`100vw` 会铺满 936，`vw` 单位的字号会略微
变小（960→936 是 ×0.975），但没有溢出风险 —— 这正是规则 2 想要的效果。
