import { BasePage } from './base-page.js';
import { buildProfile, readFalconEnv } from './services/device_profile.js';

/*
 * 逻辑画布宽度 = 本机视口宽度（1:1，不缩放）。
 *
 * 【合并 PR #5 的取舍】PR 把这里改成 buildProfile({})（不读 env），并把
 * device_profile 的 DEFAULT_DEVICE 写死成 'x7p' —— 那样一个包只能跑一种机型，
 * 换机器得改源码重打包。我们保留运行时探测：先读 $falcon.env，
 * env 不可信（报的是面板尺寸 480x960）时 buildProfile 内部会回退机型预设，
 * 认不出机型则按 X6 Pro 基线走。
 *
 * setViewPort(视口宽) 后 1 设计像素 = 1 物理像素，
 * 页面各栏尺寸由 src/services/layout.js 按视口算（见 layoutStyles）。
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
