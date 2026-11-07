import { BasePage } from './base-page.js';

// 逻辑画布宽度来自设备 profile（真机截屏 + cfg.json 证实）：
//   有道 X6 Pro ：Falcon UI 合成面横向条带 960x266，物理面板 480x960（direction=270）
// setViewPort(W) 把 W 个设计像素铺满合成面宽度，因此 W 必须等于 CSS 设计宽度，
// 否则整页被等比缩放且**纵向裁掉底部**。
//
// 【X7 Pro 适配】这里**刻意保持 960 不变**：X7 Pro 的视口是 936x280（部分固件
// 报 960x280），传 960 会让 CSS 960x266 整体等比缩放到 936x259.35，全部内容可见；
// 若改成 setViewPort(936)，CSS 的 960 宽会超出视口被裁掉右侧 24px。
// 缩放系数由 src/services/device_profile.js 的 scale 统一计算，视频矩形、
// 面板列偏移、DRM plane-id 都跟着它走，UI 与硬件平面才不会错位。
const DESIGN_WIDTH = 960;

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
