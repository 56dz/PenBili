import { BasePage } from './base-page.js';
import { buildProfile } from './services/device_profile.js';

/*
 * 逻辑画布宽度 = 写死的机型视口宽度（1:1，不缩放）。
 *
 * 【3.5.6 第二十四轮】删掉自适应分辨率后，这里不再读 $falcon.env：
 * 机型参数只有 device_profile.js 里的 X7P（936x280）/ X6P（960x266）两张表，
 * 画布宽度直接取表里的 width。setViewPort(936) 后 1 设计像素 = 1 物理像素，
 * 页面各栏尺寸由 src/services/layout.js 按视口算（见 layoutStyles）。
 *
 * 换机器就改 device_profile.js 的 DEFAULT_DEVICE（一行），不要在页面里散写。
 */
const bootProfile = buildProfile({});
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
