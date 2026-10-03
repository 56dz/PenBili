import { BasePage } from './base-page.js';

// 逻辑画布宽度来自设备 profile（profiles/youdao-x5.md，真机截屏证实）：
// 有道 X5 的 Falcon UI 合成面为横向条带 800x254，物理面板 254x800（direction=270）。
const DESIGN_WIDTH = 800;

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
