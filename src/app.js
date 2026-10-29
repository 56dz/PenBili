import { BasePage } from './base-page.js';

// 逻辑画布宽度来自设备 profile（真机截屏 + cfg.json 证实）：
//   有道 X5     ：Falcon UI 合成面横向条带 800x254，物理面板 254x800（direction=270）
//   有道 X6 Pro ：Falcon UI 合成面横向条带 960x266，物理面板 480x960（direction=270）
// setViewPort(W) 把 W 个设计像素铺满合成面宽度，因此 W 必须等于合成面宽度，
// 否则整页被等比缩放（800 → 960/800=1.2 倍）且**纵向裁掉底部**。
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
