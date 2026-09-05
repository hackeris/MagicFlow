import { startComfyChild } from 'libentry.so';
import { ensureStdlib } from '../utils/Stdlib';
import { fileIo } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { webview } from '@kit.ArkWeb';
import { http } from '@kit.NetworkKit';
import { request } from '@kit.BasicServicesKit';

const DOMAIN = 0xD001;
const TAG = 'ComfyUI';

// ── 本地推理验证(2026-09-04): 模型自动拉取 ─────────────────────────────────
//   通道: hdc rport tcp:18000 tcp:18001(设备→宿主反向隧道, 同机回环, 无外网依赖;
//   宿主侧 python3 -m http.server 18001 暴露 SD-Turbo 2.6GB checkpoint)。
//   落位: <filesDir>/pyroot/comfyui/models/checkpoints/sd_turbo.safetensors
//   (ComfyUI 默认 models/checkpoints 即此, 安装重启后扫码即出)。
const MODEL_URL = 'http://127.0.0.1:18000/sd_turbo.safetensors';
const MODEL_REL = 'pyroot/comfyui/models/checkpoints/sd_turbo.safetensors';

async function ensureModel(actx: Context): Promise<boolean> {
  const fp = `${actx.filesDir}/${MODEL_REL}`;
  // 磁盘标记点(2026-09-04): hilog 流不可靠(实测时有时无), 用文件系统 marker 做阶段落点,
  //   hdc shell(只读)可 ls 验证: mdf.1 mkdir-ok / mdf.2 dl-started / mdf.3 done / mdf.E-rc 失败码
  const mdfDir = `${actx.filesDir}/mdf`;
  const md = (s: string): void => {
    try {
      fileIo.mkdirSync(mdfDir, true);
      const fd = fileIo.openSync(`${mdfDir}/${s}`,
        fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.WRITE_ONLY);
      fileIo.writeSync(fd.fd, new Date().toISOString());
      fileIo.closeSync(fd);
    } catch (_) { /* marker 本身失败不阻断 */ }
  };
  hilog.info(DOMAIN, TAG, 'ensureModel called fp=%{public}s', fp);
  // ⚠ accessSync 返回 boolean(存在=true/不存在=false), 不抛异常; 只有权限失败才抛 13900012。
  //   必须判断返回值(先前直接裸调用 → 永远返回 hit, 下载永不触发 —— 2026-09-04 实锤)。
  if (fileIo.accessSync(fp)) {
    md('hit');
    return true;
  }
  try {
    fileIo.mkdirSync(`${actx.filesDir}/pyroot/comfyui/models/checkpoints`, true);
    md('1-mkdir-ok');
  } catch (e) {
    md('1-mkdir-fail');
    hilog.error(DOMAIN, TAG, 'mkdir fail: %{public}s', JSON.stringify(e));
  }
  try {
    md('2-dl-start');
    const task = await request.downloadFile(actx, { url: MODEL_URL, filePath: fp });
    return await new Promise<boolean>((resolve) => {
      // 事件签名(API24): progress:(receivedSize,totalSize)=>void; fail:(err)=>void
      task.on('progress', (receivedSize: number, totalSize: number) => {
        hilog.info(DOMAIN, TAG, 'model download %{public}ld/%{public}ld',
          receivedSize, totalSize);
      });
      task.on('complete', () => {
        md('3-done');
        hilog.info(DOMAIN, TAG, 'model download complete: %{public}s', fp);
        resolve(true);
      });
      task.on('fail', (err: number) => {
        md(String('E-' + err));
        hilog.error(DOMAIN, TAG, 'model download fail rc=%{public}d', err);
        resolve(false);
      });
    });
  } catch (e) {
    md('EX-err');
    hilog.error(DOMAIN, TAG, 'model download ex: %{public}s', JSON.stringify(e));
    return false;
  }
}

// run94（2026-09-03):ArkWeb 接线 —— NCP 子进程后端(127.0.0.1:8188)由 launch() 拉起,
//   但主链 import 需 ~90s 才就绪;页面轮询 /system_stats,200 后才让 Web 组件加载
//   http://127.0.0.1:8188(同源,前端静态由后端 --front-end-root 直接服务)。
const BACKEND_URL = 'http://127.0.0.1:8188';
const POLL_MS = 2500;
const POLL_MAX_MS = 240000;

// ── W1 ComfyUI 环境门(2026-09-05, docs/workspace-design.md)───────────────────
//   官方同构: 门户 = 壳层 UI(对标 Comfy-Desktop 的 Chooser 环境瓦片选择器, shell 搭建;
//   官方术语: "ComfyUI 环境(Environment)", 非 workspace —— 后者为云账户作用域(调研实证))。
//   用户创建/打开环境后才 launch()。
//   回退: 删本段改动并把 onAppear 恢复为 launc+waitBackend+ensureModel 即还原 POC。
//   ⚠ 2026-09-05 实测教训: UIAbility.onCreate 任何 want.parameters 访问即毒化启动
//   (白屏死锁, 无日志); 故本页所有数据源均为 filesDir 文件 + 用户手势, 绝不读 want。
interface Workspace {
  id: string;
  name: string;
  createdAt: number;
  lastOpened: number;
}
const WS_FILE = 'workspaces.json';

@Entry
@Component
struct Index {
  @State pid: number = 0;
  @State backendReady: boolean = false;
  @State status: string = '正在拉起后端子进程…';
  @State active: boolean = false;        // 已进入工作空间(Web 视图)
  @State wsList: Workspace[] = [];
  @State creating: boolean = false;      // 新建对话框开关
  @State wsName: string = '';
  controller: webview.WebviewController = new webview.WebviewController();
  pollTimer: number = -1;

  // ── W1:workspace 数据(filesDir/workspaces.json)──────────────────────────────
  wsPath(ctx: string): string {
    return `${ctx}/${WS_FILE}`;
  }

  loadWsList(): void {
    try {
      const t = fileIo.readTextSync(this.wsPath(getContext(this).filesDir));
      this.wsList = JSON.parse(t) as Workspace[];
    } catch (_) { this.wsList = []; }
  }

  saveWsList(): void {
    try {
      const fp = this.wsPath(getContext(this).filesDir);
      const fd = fileIo.openSync(fp,
        fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.WRITE_ONLY);
      fileIo.writeSync(fd.fd, JSON.stringify(this.wsList));
      fileIo.closeSync(fd);
    } catch (e) { hilog.error(DOMAIN, TAG, 'saveWsList fail: %{public}s', JSON.stringify(e)); }
  }

  startWorkspace(nm: string): void {
    // W1: 用户动作(新建/打开)才触发 —— 原 POC 的 launch+waitBackend+ensureModel 链
    this.active = true;
    this.creating = false;
    this.status = '正在拉起后端子进程…';
    void this.launch(getContext(this));
    this.waitBackend();
    void ensureModel(getContext(this)).then((ok) => {
      if (ok) {
        hilog.info(DOMAIN, TAG, 'model in place, refresh UI');
        setTimeout(() => {
          try { this.controller.refresh(); } catch (_) { /* ignore */ }
        }, 1500);
      }
    });
    hilog.info(DOMAIN, TAG, 'workspace entered: %{public}s', nm);
  }

  createWorkspace(): void {
    const nm = this.wsName.trim();
    if (nm.length === 0) { return; }
    this.wsList.push({
      id: String(Date.now()), name: nm,
      createdAt: Date.now(), lastOpened: Date.now()
    });
    this.saveWsList();
    this.wsName = '';
    this.startWorkspace(nm);
  }

  openWorkspace(w: Workspace): void {
    w.lastOpened = Date.now();
    this.saveWsList();
    this.startWorkspace(w.name);
  }

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
    // 全屏铺满（无标题/无边距）。W1 门控: 未进入工作空间 → 门户(零自动);
    //   已进入 → Web 常驻撑满窗口, 后端就绪前底部悬浮状态小字。
    Stack() {
      if (this.active) {
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
      } else {
        // ── 门户视图(W1, 官方 Chooser 同构): 用户创建/打开工作空间后才启动后端 ──
        Column() {
          Text('ComfyUI on HarmonyOS')
            .fontSize(30)
            .fontWeight(FontWeight.Bold)
            .fontColor(0x123b6d)
            .margin({ top: 60, bottom: 8 })
          Text('创建或打开 ComfyUI 环境后, 后端才会启动')
            .fontSize(14)
            .fontColor(0x888888)
            .margin({ bottom: 36 })

          Button('新建环境')
            .width('70%')
            .height(48)
            .fontSize(16)
            .onClick(() => { this.creating = true; })

          if (this.creating) {
            Column() {
              TextInput({ placeholder: '环境名称' })
                .height(44)
                .margin({ top: 20 })
                .onChange((v: string) => { this.wsName = v; })
                .onSubmit(() => { this.createWorkspace(); })  // 键盘 Enter 提交(verif 自动化经 keyevent 66)
              Text('规格: 默认 256×256 输出 (512 受设备内存限制不可用)')
                .fontSize(12)
                .fontColor(0x999999)
                .margin({ top: 8 })
              Row() {
                Button('创建')
                  .enabled(this.wsName.trim().length > 0)
                  .onClick(() => { this.createWorkspace(); })
                Button('取消')
                  .margin({ left: 16 })
                  .onClick(() => { this.creating = false; this.wsName = ''; })
              }.margin({ top: 16 })
            }.width('70%').alignItems(HorizontalAlign.Start)
          }

          if (this.wsList.length > 0) {
            Text('我的环境')
              .fontSize(14)
              .fontColor(0x666666)
              .margin({ top: 32, bottom: 8 })
            Column() {
              ForEach(this.wsList, (w: Workspace) => {
                Column() {
                  Text(w.name).fontSize(16).fontWeight(FontWeight.Medium)
                  Text(this.fmtTs(w.lastOpened)).fontSize(12).fontColor(0x999999)
                }
                .width('100%')
                .padding({ left: 16, right: 16, top: 10, bottom: 10 })
                .borderRadius(8)
                .backgroundColor(0xF2F6FB)
                .margin({ bottom: 8 })
                .onClick(() => { this.openWorkspace(w); })
              }, (w: Workspace) => w.id)
            }.width('70%')
          } else if (!this.creating) {
            Text('还没有环境, 先新建一个')
              .fontSize(14)
              .fontColor(0xAAAAAA)
              .margin({ top: 32 })
          }
        }
        .width('100%')
        .height('100%')
        .alignItems(HorizontalAlign.Center)
      }
    }
    .width('100%')
    .height('100%')
    .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.TOP, SafeAreaEdge.BOTTOM])
    .onAppear(() => {
      // W1 门户化: 零自动启动 —— 只读列表; 子进程由 create/open 动作触发。
      this.loadWsList();
    })
  }

  fmtTs(t: number): string {
    const d = new Date(t);
    const p = (n: number): string => (n < 10 ? `0${n}` : `${n}`);
    return `${p(d.getHours())}:${p(d.getMinutes())}`;
  }
}
