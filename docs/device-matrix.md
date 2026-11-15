# 机型兼容性矩阵（PStore 规则 5）

本文回答：**PenBili 能装到哪些词典笔上，哪些功能在哪些机型上没有验证。**
判据分三层：JS 侧有没有设备专属能力、native 侧有没有写死硬件参数、开发机脚本会不会进包。

## 1. 结论

| 机型 | 装机 | 浏览/搜索/登录/图文 | 视频播放 | 直播 | 状态 |
| --- | --- | --- | --- | --- | --- |
| **X6 Pro**（RK3562，960×266） | ✅ | ✅ | ✅ **实测** | ✅ **实测** | 主要目标机型 |
| X7 Pro（960/936×280） | ✅ | ✅ 理论上 | ⚠️ 代码已适配，**无真机** | ⚠️ 同左 | 待验证 |
| 其它 Falcon 词典笔 | ⚠️ | ⚠️ | ❌ 大概率不行 | ❌ | 不建议上架 |

**建议上架子分类**：`词典笔` + `同类设备`（因为用了 native JSAPI 且依赖 Rockchip
MPP 硬解与 DRM/KMS 叠加平面）。**不建议**勾选「所有设备」—— 没有 MPP 与 KMS 平面的
设备上视频起不来。

## 2. 为什么不能「所有设备都能用」

`libjsapi_player2.so`（native 模块）依赖三样**设备专属**的东西：

| 依赖 | 用途 | 缺失后果 |
| --- | --- | --- |
| `/dev/mpp_service`（Rockchip MPP） | H.264/H.265 硬件解码 | 解码器起不来，`first frame failed` |
| `/dev/dri/card0` + 可用 overlay plane | 视频走硬件叠加平面（`kmssink`） | 只能退到 UI 层软件绘制，帧率不足 |
| `/usr/bin/ffmpeg` | 拉流 + 重封装成 Matroska | 取流阶段直接失败 |

前两项是 Rockchip 平台专有，第三项是可执行文件依赖。这三条都是**运行时探测**
（失败会在日志里给出明确原因并回落到错误态），所以装到别的设备上不会崩，只会
播不了视频 —— 但那样上架就等于给用户一个坏应用，因此按「同类设备」上架。

## 3. JS 侧：没有设备专属能力调用

除了 native 模块本身，JS 侧只用 Falcon 标准 JSAPI：
`http`（网络）、`storage`（持久化）、`brightness`（屏幕保活）、`input`（文本输入）、
`modal`（原生弹窗）、`nvue`（页面）。

- 没有调用 `pm`（应用安装）—— 安装走 native 侧的固定 argv 子进程，见 §4。
- 没有调用 `updater`（有道自家服务端升级通道）—— 检查更新是读公开的
  Release API（GitCode / GitHub），见 `src/services/updater.js`。
- `$falcon.env` 的读取做了可用性闸门：只有 `envW >= 600 && envH > 0 &&
  envW/envH >= 2.2` 才采信，否则整体丢弃并回落到机型预设
  （`src/services/device_profile.js` 的 `envUsable`）。原因是它可能返回默认值
  `800×480` 或 `model="HaaS UI"`。

## 4. native 侧：所有子进程都是固定 argv，没有 shell

PStore 规则 5 关注的是「应用里有没有内藏 shell」。逐条列出本项目调用的系统程序：

| 调用 | 位置 | 参数来源 | 判定 |
| --- | --- | --- | --- |
| `/bin/miniapp_cli install <amr路径>` | `native/csrc/jsapi_player2.c:533` | 路径由文件下载结果给出，`posix_spawn` 数组传参（不经过 shell，无引号注入面） | 应用自身功能（安装自己下载的更新包） |
| `ffmpeg`（拉流） | `native/csrc/source/src_http.c:607` | 全部是代码里拼的常量 + B 站 CDN URL（不经 shell，数组传参） | 应用自身功能（播放） |
| `amixer`（音量） | `native/csrc/session/session.c:1631` | 数组传参；控件名来自 `amixer scontrols` 的只读输出 | 应用自身功能 |
| `bluetoothctl`（找已连耳机） | `native/csrc/session/cmd.c:794,817` | `popen`，但 MAC 先做格式校验（长度 17、冒号位置） | 应用自身功能 |
| `hal-screen` / `hal-audio` | 仅开发期调试，**不在应用代码里** | — | 不适用 |

**没有任何一处把用户可控字符串拼进 shell 命令行。** 唯一的 `popen` 路径
（`bluetoothctl`）传入的是从 `bluetoothctl devices` 输出里解析出来、并经过
`strlen(mac)==17 && mac[2]==':'` 校验的 MAC 地址，用户无法通过 UI 影响它。

应用不接受任何形式的用户输入命令。文本输入框只接受评论内容，且只发给 B 站接口。

## 5. 开发机脚本不进包

`tools/` 下的脚本（`build-wsl.sh`、`pack_falcon.py`、`probe-x7p.sh`、`deploy.ps1`）
是**构建期/开发期工具**，不会被打进 `.amr`。`.amr` 的内容只有：
`manifest.json` + `index.js.bin`（qjsc 编译产物）+ `libs/*.so` + 图片资源。
其中 `tools/probe-x7p.sh` 含 `adb` 字样，那是开发机取证脚本，不是应用能力。

包内清单可以用下面命令复核（22~23 个条目）：

```bash
unzip -l 8002026110200001.3_5_8.amr
```

## 6. X7 Pro 的适配状态（诚实披露）

`docs/x7pro-adaptation.md` 记录了完整过程。要点：

- 视口是 **936×280**（X6 Pro 是 960×266），面板仍是 480×960 竖屏。
- X7 Pro 固件上 **`plane-id=76` 不可用**（`set_state(PLAYING)` 返回 FAILURE），
  必须 `planeId=-1` 让 kmssink 自选平面 + 打开软件缩放。
- `panelXOffset` 在 X7 上必须是 **0**（JS 产出的 x 已经是面板绝对列坐标，
  native 再加 100 会推出带外）。
- 这些参数**全部改成运行时探测 + 可被 `$falcon.env` 覆盖**
  （`src/services/device_profile.js` + `native/csrc/session/session.h` 的
  `panelOffset`/`planeId`/`swScale`/`bufScale` 四个字段）。

**但作者手上没有 X7 Pro 真机**，所有 X7 数值来自第三方实测记录。代码做到
「探测不到就回落到 X6 Pro 预设」，所以最坏情况是 X7 Pro 上按 X6 参数跑
（画面比例可能不对），不会黑屏崩溃 —— native 侧还有一层兜底：显式 `plane-id`
起不来时自动切 `planeId=-1` + 软件缩放重试一次。

## 7. 多机型验证清单（ reviewers 可用）

在目标机型上依次做，每一步都要看真机画面而不是只看日志：

1. 冷启动 → 首页五列卡片是否完整、不横向溢出
2. 点卡片 → 起播 3 秒内出画面；画面比例正确（16:9 不拉伸、不压扁）
3. 播放页三列：左列标题/简介不截断、中列视频+进度条不重叠、右列评论可滚
4. 点视频区暂停 → 进度条静止；再点继续 → 帧率恢复 30fps
5. 拖进度条前/后 → 音画不分叉
6. 「我的」页滚到底 → 关于区与二维码完整（二维码能扫）
7. 搜索页三分栏切换 → 卡片网格列数与机型匹配
8. 图文详情 → 正文可上下滚、标题在最上方
