import { BasePage } from './base-page.js';
import { buildProfile, readFalconEnv } from './services/device_profile.js';

/*
 * 逻辑画布宽度 = 设备视口宽度（1:1，不缩放）。
 *
 * 【3.6.1 关键改动】3.5.5 / 3.6.0 写死 setViewPort(960)：
 *   有道 X6 Pro：视口 960x266 → 正好铺满，1:1；
 *   X7 Pro    ：视口 936x280 → 960 个设计像素被压进 936 → 整页 0.975 缩放，
 *               画布高 266*0.975 = 259.35 → 屏幕底部约 21px 黑边；同时首页
 *               .grid 第 5 列在缩放后差 0.25px 放不下 → 换行，右侧空出 184px
 *               （真机截屏 bugs/主界面左侧，下侧都留有黑边，进行ui的适配.png）。
 *
 * 现在按 profile.cfg.width 设画布：960 → 960、936 → 936，永远 1:1。
 * 页面各栏尺寸由 src/services/layout.js 按视口算（见 layoutStyles），
 * 视频矩形、面板偏移、DRM 平面都走同一套数字，UI 与硬件平面不会错位。
 *
 * 注意：这里必须**同步**拿 profile —— setViewPort 只在 onLaunch 有效，
 * 而 ensureProfile() 要读 storage（异步）。readFalconEnv() 是同步读 $falcon.env，
 * 足够在启动瞬间定下画布宽度；storage 里的 bili_device_profile 覆盖只影响
 * 几何参数（偏移/平面/矩形），不影响画布宽度，改宽度需要重启小程序。
 */
const bootProfile = buildProfile({ env: readFalconEnv() });
const DESIGN_WIDTH = bootProfile.cfg.width;

class App extends $falcon.App {
  constructor() {
    super();
  }

  onLaunch(options) {
    super.onLaunch(options);
    this.setViewPort(DESIGN_WIDTH);
    $falcon.useDefaultBasePageClass(BasePage);
  }

  onShow() {
    super.onShow();
  }

  onHide() {
    super.onHide();
  }

  onDestroy() {
    super.onDestroy();
  }
}

export default App;
