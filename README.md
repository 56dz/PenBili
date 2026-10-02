# PenBili

**有道词典笔 X5 上的哔哩哔哩播放器**（Falcon / QuickJS mini-app，Vue 2 SFC）。本发布以仓库 `v2.7.0` 源码为基线，版本号为 `2.7.1`。

在逻辑分辨率 800×254 的设备屏幕上提供推荐流、DASH 播放、评论浏览/发表、TV 协议扫码登录与播放历史。
弹幕已于 v2.7.0 移除，以避免弱设备上的逐帧文字渲染拖慢视频解码。

![version](https://img.shields.io/badge/version-2.7.1-blue)
![license](https://img.shields.io/badge/license-GPL--3.0-green)

## 截图

**主页（推荐流）**

![主页](docs/screenshots/home.png)

**播放中**

![播放中](docs/screenshots/playing.png)

**评论区**

![评论区](docs/screenshots/comments.png)

以上图片为 v2.7.0 时的真机截图，页面布局在本维护版本中未改动。此次无法连接测试设备，因此没有伪造或替换截图；重新连接设备后可用 `npm run deploy` 更新。

## 功能

- **播放**：DASH 分离视频与 AAC 音轨，当前请求 360p（id16）并优先选择 AVC；必要时回退单文件流；按视频实际宽高信箱适配，支持 ±20 秒定位与播放历史。
- **音画同步**：轻微漂移时平滑追赶；音频领先超过 1 秒时只重启视频流并重新对齐，音频不中断；首帧等待有 15 秒上限，失败后自动恢复。
- **网络抖动缓冲**：FFmpeg 输入线程预取压缩数据包（视频队列 300、音频队列 700）；它不是解码后的视频帧缓存，也不保证所有网络条件下固定 15 秒可用缓冲。
- **音频**：独立解码进程与音频环形缓冲，支持设备默认输出和 A2DP 音频路由。
- **评论**：分页浏览、子回复、系统输入法发表评论；评论面板打开时维持音频播放并避免视频闪烁。
- **登录**：TV 协议扫码登录、登录态持久化、过期检查与刷新。
- **列表与搜索**：推荐、热门和关键词视频搜索。

当前版本不提供直播和专栏图文阅读；相关功能不在本次 v2.7.0 基线发布内容中。

## 设备与兼容性

| 项目 | 值 |
| --- | --- |
| 机型 | 有道词典笔 X5 |
| 运行时 | Falcon mini-app / QuickJS 20200705 |
| 逻辑屏幕 | 800×254 |
| 视频区域 | 452×254，按源视频宽高信箱适配 |

设备画像、诊断日志、运行截图及本地环境信息不随此版本分发。更换设备或固件时应重新验证屏幕几何与解码能力。

## 性能诊断记录

一次约 4.5 分钟的播放采样（253 个样本）中，播放稳定期未观察到超过 1 秒的停帧；超过 1 秒的停顿出现在流启动或首帧恢复阶段。该轮中视频解码约占单核 55–81%，应用进程约 5%，全机约 25%。这说明该样本中的停顿更像首帧/流启动等待，而非持续的解码饱和；不同视频和网络下结果可能不同。

## 构建与测试

前置：Node.js 18+、有道小程序打包器 **aiot-vue-cli**（1.0.32 验证）；重建 native 库另需 Zig（`arm-linux-gnueabihf.2.23` target）。

```bash
npm ci
npm test                    # 82 条逻辑、协议与几何回归测试
node test/qr_login_test.js  # 21 条登录协议测试

# 打包 AMR（QuickJS 生产包）
npm run build

# 可选：从 native 源码重建库
# PowerShell: $env:ZIG_EXE = 'C:\path\to\zig.exe'; npm run native
```

## 安装与真机自检

具体命令以设备上的 `miniapp_cli --help` 为准：

```bash
adb push PenBili.amr /tmp/PenBili.amr
adb shell miniapp_cli install /tmp/PenBili.amr
adb shell miniapp_cli start 8002026091900004 --index
```

本地部署、自检与截图采集：`npm run deploy`（依赖 ADB 可访问设备）。

## 隐私与仓库内容

- `profiles/` 下的设备资料、真机日志和截图为本地文件，不应提交。
- 登录与搜索样例使用脱敏或占位数据；不要提交账号 Cookie、二维码登录数据、设备序列号、MAC 地址、内网地址或签名播放 URL。
- `.gitignore` 已排除 `profiles/`、部署状态及构建中间文件。

## 致谢

- [SocialSisterYi/bilibili-API-collect](https://github.com/SocialSisterYi/bilibili-API-collect)：B 站接口文档。
- 腾讯 **WorkBuddy**：开发环境与 Agent 工具链。
- **GLM-5.3-Flash**（智谱）、DeepSeek 与小米 MiMo：代码协作与排障。

## 免责声明

- 本项目为个人学习用途，与哔哩哔哩、有道（网易）均无关联；相关商标归其权利人所有。
- `native/iot-miniapp-sdk/include` 为有道小程序官方 SDK 头文件，仅作为 native 模块的编译依赖随仓库分发；版权归原权利人，不适用于本仓库 GPL-3.0 授权。
- 使用本软件访问服务时请遵守哔哩哔哩用户协议；账号安全责任自负。
- **无任何担保**；详见 LICENSE。

## 许可证

[GPL-3.0](LICENSE)（GNU General Public License v3.0）
