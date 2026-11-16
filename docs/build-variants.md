# 两个包：X7 全系全量包 / 只含自检页的测试包

`3.5.6` 起一次构建产出两个 AMR，共用同一个 `appid`（`8002026110200001`）
与版本号（`3.5.6`）：

| 文件 | 内容 | 用途 |
| --- | --- | --- |
| `8002026110200001.3_5_6_x7.amr` | 首页 + 播放页 + 自检页（完整 UI） | 日常使用；X7P 936x280 写死 |
| `8002026110200001.3_5_6_diag.amr` | **只有** `pages/diag` 一页 | 逐台机器验屏幕/平面/解码链路 |

> 两个包 appid 相同，安装后一个会覆盖另一个。验完自检包记得重装全量包。

## 1. 为什么 X7 要单独出一版

X7 家族是**同芯片、不同屏幕尺寸**。旧代码里有三处按 `936x280` 这一台机器
总结出来的假设，换一台屏幕尺寸不同的 X7 就会错位：

| 位置 | 旧假设 | 现在 |
| --- | --- | --- |
| `device_profile.buildProfile` 的 env 闸门 | 宽高比 ≥ 2.2 才当成视口，否则丢弃 | 只排除已知物理面板 `480x960`；竖屏上报旋成横屏；其余按视口用 |
| `layout.gridColumns / cardWidth` | 最多 6 列、卡宽封顶 166 | 最多 10 列、卡宽封顶 220（1024/1280 宽不再空出半屏） |
| `layout.seekH` | `30 * H / 266`，480 高会算出 54px | 钳在 24~40px（280→32，480→40） |

其余布局本来就是按视口宽高比例算的（`layout.buildLayout(W,H)`），画布由
`app.js` 用 `profile.cfg.width` 调 `setViewPort`，所以只要 env 判对了，
三列宽度、视频洞、控制条、网格都会跟着屏幕走。

机型判定也不再按 `936x280` 认：`deviceModel` 含 `x7`、或视口高 > 270、
或宽 ≥ 1000 都走 X7 的平面策略（`planeId=-1` + `swScale=1`）。X6 Pro
（960x266）仍走原来的 `planeId=76` + 硬件缩放，逐字段不变（单测守着）。

如果某台机器的固件把**面板尺寸**当视口上报（自检页会显示 `(env 480x960)`
这种字样），用 storage 覆盖即可，不用重新打包：

```json
{"width": 1024, "height": 300, "panelWidth": 480, "panelHeight": 1024,
 "panelXOffset": 0, "planeId": -1, "swScale": 1}
```

写入 `bili_device_profile` 后重启小程序生效。

## 2. 自检包（diag-only）

构建时把 `src/app.json` 临时改成 **两个路由都指向自检页**：

```json
"pages": {
  "index": "pages/diag/diag.js",
  "diag": "pages/diag/diag.js"
}
```

并把 `src/build_flags.js` 的 `DIAG_ONLY` 置为 `true`（打包结束自动还原）。

> 为什么不能只写一条 `pages/diag`：CLI 的 `isSingleJsbundle()` 在
> `Object.keys(pages).length === 1` 时会把页面代码**内联进 app.js**，不再产出
> 独立的 `index.js.bin`；而真机启动时按路由名加载 `./index.js`，找不到就黑屏
> （真机实测：diag 单页包开机全黑）。两条路由才能让页面模块单独落盘。
> `index` 指向自检页，所以启动看到的仍然只有自检界面。

因此自检包：

- 开机直接进「链路自检」，没有首页/播放页可点；
- 「返回」按钮自动隐藏（没有首页可回）；
- 仍然带 `libjsapi_player2.so`（三份 SKU 目录），能完整跑播放链路自检；
- 顶部 `deviceText` 会显示生效 profile；当原始 env 与生效视口不一致时，
  会多印一个 `(env 480x960)`，一眼看出是面板当视口上报。

一台新机器拿到手先装自检包，看三行：

```
x7p 1024x300 7x162 off0 row0 | plane=auto vs1 buf100 · Esmart1-win0 ACTIVE
洞640x360 矩形... 缓冲... 档A 生效<x,y,w,h>
播放 playing · 00:12 / 02:29 · 帧 312 · 每秒刷新
```

第一行确认视口/列数/平面；第二行确认视频矩形与挖洞同源；第三行确认解码在跑。

## 3. 构建

WSL 里一条命令出两个包：

```bash
export NODE_BIN=/mnt/c/Users/lenovo/.workbuddy/binaries/node/versions/22.22.2-6/node.exe
bash tools/build-variants.sh
```

脚本做两件事：

1. `bash tools/build-wsl.sh` → 全量包 `8002026110200001.3_5_6.amr`，
   复制为 `..._x7.amr`；
2. `SKIP_NATIVE=1 PENBILI_VARIANT=diag bash tools/build-wsl.sh` → 只重打
   JS/manifest（native 复用上一步），输出 `..._diag.amr`。

单测：`node test/run.js`（新增 env 归一化与 1024/1280/800x480 布局断言）。
