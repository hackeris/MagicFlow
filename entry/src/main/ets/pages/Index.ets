import { startComfyChild } from 'libentry.so';
import { ensureStdlib } from '../utils/Stdlib';
import { fileIo } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { webview } from '@kit.ArkWeb';
import { http } from '@kit.NetworkKit';

const DOMAIN = 0xD001;
const TAG = 'ComfyUI';

// run94（2026-09-03):ArkWeb 接线 —— NCP 子进程后端(127.0.0.1:8188)由 launch() 拉起,
//   但主链 import 需 ~90s 才就绪;页面轮询 /system_stats,200 后才让 Web 组件加载
//   http://127.0.0.1:8188(同源,前端静态由后端 --front-end-root 直接服务)。
const BACKEND_URL = 'http://127.0.0.1:8188';
const POLL_MS = 2500;
const POLL_MAX_MS = 240000;

@Entry
@Component
struct Index {
  @State pid: number = 0;
  @State backendReady: boolean = false;
  @State status: string = '正在拉起后端子进程…';
  controller: webview.WebviewController = new webview.WebviewController();
  pollTimer: number = -1;

  // 子进程(NCP)是独立进程：把 pyroot=<filesDir>/pyroot 塞进 entryParams，子进程据此设 PYTHONHOME/PYTHONPATH。
  // stdlib 的拷贝+解压统一由 Stdlib.ets 单例负责（EntryAbility.onCreate 唯一触发点）；
  // 这里只 await 同一 Promise 等它就绪，绝不自己再写一份，避免并发/路径不一致。
  async launch(actx: Context): Promise<void> {
    try {
      const pyroot = await ensureStdlib(actx);
      const entryParams = `phase0|127.0.0.1|8188|--cpu|pyroot=${pyroot}`;
      this.pid = startComfyChild(entryParams);
      // Py_Initialize 若内部 fatal，会把报错写进 <pyroot>/py-init.log（同 app 沙箱，父进程可读）。
      // 子进程在 Py_Initialize 里 exit，故由父进程读该文件并把真实报错打进 hilog，便于定位。
      setTimeout(() => {
        try {
          const log = fileIo.readTextSync(`${pyroot}/py-init.log`);
          hilog.info(DOMAIN, TAG, 'py-init.log >>>\n%{private}s', log);
        } catch (e) {
          hilog.info(DOMAIN, TAG, 'py-init.log not available (rc/empty): %{public}s', JSON.stringify(e));
        }
      }, 4000);
    } catch (e) {
      // ensureStdlib 失败时子进程仍会启动，但收到空 pyroot，Py_Initialize 依旧失败 —— 便于对照复现
    }
  }

  // run94:后端就绪轮询 —— 200 即 loadUrl;超时仍在等待则报告,轮询继续(240s 上限)。
  waitBackend(): void {
    this.pollTimer = setInterval(() => {
      if (this.backendReady) {
        return;
      }
      const req = http.createHttp();
      req.request(`${BACKEND_URL}/system_stats`, {
        method: http.RequestMethod.GET,
        connectTimeout: 2000,
        readTimeout: 2000
      }).then((resp: http.HttpResponse) => {
        req.destroy();
        if (resp.responseCode === 200) {
          this.backendReady = true;
          this.status = '后端就绪，加载 ComfyUI 前端…';
          if (this.pollTimer !== -1) {
            clearInterval(this.pollTimer);
            this.pollTimer = -1;
          }
          hilog.info(DOMAIN, TAG, 'backend ready (system_stats 200), loadUrl %{public}s', BACKEND_URL);
          this.controller.loadUrl(BACKEND_URL);
        }
      }).catch((e: Error) => {
        req.destroy();
        if (!this.backendReady) {
          this.status = '等待后端 127.0.0.1:8188…';
          hilog.info(DOMAIN, TAG, 'backend not ready yet: %{public}s', JSON.stringify(e));
        }
      });
    }, POLL_MS);
    setTimeout(() => {
      if (this.pollTimer !== -1) {
        clearInterval(this.pollTimer);
        this.pollTimer = -1;
        hilog.error(DOMAIN, TAG, 'backend wait timeout(%{public}d)', POLL_MAX_MS);
      }
    }, POLL_MAX_MS);
  }

  build() {
    // 全屏铺满（无标题/无边距）：Web 常驻撑满整个窗口（含状态栏/导航栏 safe area），
    //   后端就绪前仅底部悬浮一行状态小字（调试可读,不占布局）。
    Stack() {
      // src 恒为 about:blank：加载源唯一 —— 后端就绪由 waitBackend() 的 controller.loadUrl(BACKEND_URL)
      //   触发（2019-09-04 全屏改版：src 随 backendReady 切换会与 loadUrl 双触发重复加载）。
      Web({ src: 'about:blank', controller: this.controller })
        .javaScriptAccess(true)
        .domStorageAccess(true)
        .fileAccess(true)
        .onPageBegin((event) => {
          hilog.info(DOMAIN, TAG, 'Web page begin: %{public}s', event.url);
        })
        .onPageEnd((event) => {
          hilog.info(DOMAIN, TAG, 'Web page end: %{public}s', event.url);
        })
        .onErrorReceive((event) => {
          hilog.error(DOMAIN, TAG, 'Web error: url=%{public}s err=%{public}s',
            event.request.getRequestUrl(), JSON.stringify(event.error));
        })
        .width('100%')
        .height('100%')
      if (!this.backendReady) {
        Text(this.status)
          .fontSize(12)
          .fontColor(0xCCCCCC)
          .backgroundColor(0x66000000)
          .borderRadius(8)
          .padding({ left: 10, right: 10, top: 4, bottom: 4 })
          .position({ x: '40%', y: '94%' })  // 底部居中悬浮,不遮挡 Web 操作区
      }
    }
    .width('100%')
    .height('100%')
    .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.TOP, SafeAreaEdge.BOTTOM])
    .onAppear(() => {
      // 自动化验证：启动即拉起 NCP 子进程（entryParams 供 Main 打印）+ 轮询后端
      this.launch(getContext(this));
      this.waitBackend();
    })
  }
}
