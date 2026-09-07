// EntryAbility — poc/npu: 最小ability, 仅承载 Index 页面(demo 无业务自启)。
import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0xD001;
const TAG = 'NPUPoc';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(DOMAIN, TAG, 'EntryAbility onCreate');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'loadContent failed: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, TAG, 'loadContent succeeded');
      try {
        const mw = windowStage.getMainWindowSync();
        mw.setWindowLayoutFullScreen(true);
      } catch (e) {
        hilog.error(DOMAIN, TAG, 'fullscreen failed: %{public}s', JSON.stringify(e));
      }
    });
  }
}
