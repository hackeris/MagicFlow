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
    });
  }
}
