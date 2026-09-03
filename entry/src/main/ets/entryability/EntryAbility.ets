import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { ensureStdlib } from '../utils/Stdlib';

const DOMAIN = 0xD001;
const TAG = 'ComfyUI';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(DOMAIN, TAG, 'EntryAbility onCreate');
    // Step 0.2：唯一解压点 —— rawfile 里的 stdlib(python312.zip) 拷到 filesDir，解压成
    // <filesDir>/pyroot/lib/python3.12/ 目录树供子进程 Py_Initialize。fire-and-forget；
    // 页面侧经 ensureStdlib 拿到同一单例 Promise 再 await，任何地方都不会并发重复准备。
    ensureStdlib(this.context).catch((e: Error) => {
      hilog.error(DOMAIN, TAG, 'EntryAbility stdlib prepare failed: %{public}s', JSON.stringify(e));
    });
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'loadContent failed: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, TAG, 'loadContent succeeded');
      // 2026-09-04 沉浸窗口模式(Pad): 全屏布局 + 隐藏状态栏/底部导航指示器 —— Web 内
      //   ComfyUI 铺满整屏。参照 ../wineohos 的 PAD 桌面(WineWindowManager/DesktopAbility):
      //   setSpecificSystemBarEnabled('navigationIndicator', false) 才是隐藏底部手势条的正确
      //   接口(文档: setWindowSystemBarEnable 的 'navigation' 参数无效果, 状态栏/导航只由
      //   'status' 控制 —— 故不用它)。与 Index.ets 的 expandSafeArea 配合, 内容顶到边。
      try {
        const mw = windowStage.getMainWindowSync();
        mw.setWindowLayoutFullScreen(true);
        mw.setSpecificSystemBarEnabled('status', false);
        mw.setSpecificSystemBarEnabled('navigationIndicator', false);
      } catch (e) {
        hilog.error(DOMAIN, TAG, 'immersive window setup failed: %{public}s', JSON.stringify(e));
      }
    });
  }
}
