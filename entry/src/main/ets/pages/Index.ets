import { startComfyChild } from 'libentry.so';
import { ensureStdlib } from '../utils/Stdlib';
import { ensureModels, getModelsRoot, modelPath } from '../utils/ModelsDir';
import { fileIo } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { webview } from '@kit.ArkWeb';
import { http } from '@kit.NetworkKit';

const DOMAIN = 0xD001;
const TAG = 'ComfyUI';

// ── 模型完整性自检(W3 产品语义, 2026-09-05)─────────────────────────────────
//   落位(W3): <模型根>/checkpoints/sd_turbo.safetensors — 模型根 = 用户可见下载目录
//   (ModelsDir.ets: picker 授权), 失败降级沙箱 pyroot/comfyui/models。
//   下载 = 用户经 UI/后端端点(见 ensureModel 体内注释); 此处只治 0B 残留。

async function ensureModel(actx: Context): Promise<boolean> {
  const info = await getModelsRoot(actx);
  const fp = modelPath(info, 'checkpoints', 'sd_turbo.safetensors');
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
  // 2026-09-05 W3 语义定谳(产品化): 自动下载退场 —— 模型下载由用户在 UI(缺模型条目/
  //   catalog 对话框)经后端端点驱动(文档 docs/model-download.md); ensureModel 只做
  //   「完整性自检 + 0B 残留清除」, 下载缺口交给 UI/verify(MODEL_DL=1 走端点)。
  //   证据链: 实测 request.downloadFile 遇媒体区授权目录时只留 0B(且 marker 缺失),
  //   自动链不可靠 + 与端点下载在同目录同目标并发冲突。
  if (fileIo.accessSync(fp)) {
    try {
      const st = fileIo.statSync(fp);
      if (st.size > 0) {
        md('hit');
        return true;
      }
      fileIo.unlinkSync(fp);
      md('0B-removed');
      hilog.warn(DOMAIN, TAG, '0-byte model removed (download via UI/endpoint): %{public}s', fp);
      return false;
    } catch (e) {
      // stat/unlink 失败(罕见): 保守当命中, 避免反复删写(有日志定位)
      hilog.warn(DOMAIN, TAG, 'ensureModel stat fail: %{public}s', JSON.stringify(e));
      md('hit');
      return true;
    }
  }
  // 缺失(从未下载): 只记录, 不自动下载 —— 用户经 UI 下载(MODEL_DL=1 的 verify 判据[2.5]亦走端点)
  try {
    fileIo.mkdirSync(`${info.root}/checkpoints`, true);
    md('missing-no-autodl');
  } catch (e) {
    md('1-mkdir-fail');
    hilog.error(DOMAIN, TAG, 'mkdir fail: %{public}s', JSON.stringify(e));
  }
  return false;
}

// run94（2026-09-03):ArkWeb 接线 —— NCP 子进程后端(127.0.0.1:8188)由 launch() 拉起,
//   但主链 import 需 ~90s 才就绪;页面轮询 /system_stats,200 后才让 Web 组件加载
//   http://127.0.0.1:8188(同源,前端静态由后端 --front-end-root 直接服务)。
const BACKEND_URL = 'http://127.0.0.1:8188';
const POLL_MS = 2500;
const POLL_MAX_MS = 240000;

// ── 单入口改造(2026-09-05 用户定谳)────────────────────────────────────────
//   W1 门户(官方 Chooser 同构: 环境列表/新建/环境名输入)整体撤出 —— 环境管理是
//   桌面时代虚构需求: 官方 workspace=云账户作用域(调研实证), 本地后端恒为单一。
//   保留「零自动启动」实质: 重设施(zip 解压/torch 全量装载/子进程)只在用户点击
//   「启动 ComfyUI」后才发生 —— onAppear 不拉起任何东西。
//   启动进度(2026-09-05): 4 步阶段, 信号源全为真: ①ensureStdlib/ensureModels 完成
//   ②startComfyChild 调用 ③poll /system_stats 200 = 后端就绪; torch 装载期后端
//   无信号, 以「已用时长」展示(首次约 90s, 判据 backend ready 不变)。
//   记录: 曾对比「hilog / child 心跳」做进度流, hilog 流不可靠(时有时无)故弃。
//   ⚠ 仍不读 want.parameters(2026-09-05 实测: 任何访问即毒化 UIAbility, 白屏死锁)。
@Entry
@Component
struct Index {
  @State pid: number = 0;
  @State active: boolean = false;        // 已点击启动(Web 常驻挂载, about:blank)
  @State backendReady: boolean = false;
  @State phase: number = 1;              // 1 准备环境 / 2 启动子进程 / 3 装载核心组件
  @State status: string = '';
  @State elapsed: number = 0;            // 启动已用秒数(mm:ss 展示)
  @State progress: number = 0;           // 单调钳制 0-100(官方 Brand Takeover 同构)
  @State narrIdx: number = 0;            // 装载期叙事文案游标(官方叙述式分阶段文案)
  @State logOpen: boolean = false;
  @State logText: string = '';
  controller: webview.WebviewController = new webview.WebviewController();
  pollTimer: number = -1;
  tickTimer: number = -1;
  logTimer: number = -1;
  steps: string[] = ['准备运行环境', '启动后端子进程', '装载核心组件', '后端就绪'];
  narrs: string[] = ['初始化运行时组件…', '注册模型与扩展…', '准备推理引擎…', '联机后端检查…'];

  startApp(): void {
    if (this.active) {
      return;
    }
    this.active = true;
    this.phase = 1;
    this.elapsed = 0;
    this.status = '准备运行环境(解压/校验)…';
    this.tickTimer = setInterval(() => {
      this.elapsed += 1;
      this.progress = this.calcProgress();            // 单调钳制(永不回退)
      if (this.phase === 3 && this.elapsed % 8 === 0) {  // 装载期叙事循环(官方 style)
        this.narrIdx = (this.narrIdx + 1) % this.narrs.length;
      }
    }, 1000);
    this.logTimer = setInterval(() => {
      if (this.logOpen) {
        this.logText = this.readLogTail();
      }
    }, 1000);   // 1s 刷新: 心跳日志 0.4s 一条, 保证"实时滚出"观感
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
    hilog.info(DOMAIN, TAG, 'started (single entry, user gesture)');
  }

  // 子进程(NCP)是独立进程：把 pyroot=<filesDir>/pyroot 塞进 entryParams，子进程据此设 PYTHONHOME/PYTHONPATH。
  // stdlib 的拷贝+解压统一由 Stdlib.ets 单例负责（EntryAbility.onCreate 唯一触发点）；
  // 这里只 await 同一 Promise 等它就绪，绝不自己再写一份，避免并发/路径不一致。
  async launch(actx: Context): Promise<void> {
    try {
      const pyroot = await ensureStdlib(actx);
      // W3: 模型根先于子进程 —— yaml(extra_model_paths)必须早于后端 main.py 加载;
      //   picker 授权在此串行, 不弹界面(DOWNLOAD 模式); 失败降级沙箱不影响启动。
      await ensureModels(actx, pyroot);
      this.phase = 2;
      this.status = '启动后端子进程…';
      const entryParams = `phase0|127.0.0.1|8188|--cpu|pyroot=${pyroot}`;
      this.pid = startComfyChild(entryParams);
      this.phase = 3;
      this.status = '装载核心组件(首次约 1 分钟)…';
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
      hilog.error(DOMAIN, TAG, 'launch chain fail: %{public}s', JSON.stringify(e));
    }
  }

  // run94:后端就绪轮询 —— 200 即 loadUrl;超时仍在等待则报告,轮询继续(240s 上限)。
  //   单入口改造: ready 同时停 tick(已用时长停止走秒)。
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
          if (this.tickTimer !== -1) {
            clearInterval(this.tickTimer);
            this.tickTimer = -1;
          }
          if (this.logTimer !== -1) {
            clearInterval(this.logTimer);
            this.logTimer = -1;
          }
          this.progress = 100;
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
        this.status = '启动超时(240 秒), 见 py-init.log';
        hilog.error(DOMAIN, TAG, 'backend wait timeout(%{public}d)', POLL_MAX_MS);
      }
    }, POLL_MAX_MS);
  }

  // 进度页: 步骤状态 0=未达 1=进行中 2=完成
  stepState(i: number): number {
    if (this.phase > i) {
      return 2;
    }
    if (this.phase === i) {
      return 1;
    }
    return 0;
  }

  fmtElapsed(): string {
    const m = Math.floor(this.elapsed / 60);
    const s = this.elapsed % 60;
    const p = (n: number): string => (n < 10 ? `0${n}` : `${n}`);
    return `${p(m)}:${p(s)}`;
  }

  // 官方 Brand Takeover 同构: 各阶段映射非均匀区间, 段内按耗时插值; 单调接管(取历史最大)。
  //   1(准备): 0-40 | 2(子进程): 45 | 3(装载, 无后端信号): 48→92 缓慢推进 | 就绪: 100。
  calcProgress(): number {
    let p = 0;
    if (this.phase === 1) {
      p = Math.min(40, 8 + this.elapsed * 3);
    } else if (this.phase === 2) {
      p = 45;
    } else if (this.phase === 3) {
      p = Math.min(92, 48 + this.elapsed * 0.3);
    } else {
      p = 100;
    }
    return Math.max(this.progress, p);
  }

  // 可折叠日志终端(官方同构, 2026-09-05): 源 = pyroot/diag.log(子进程诊断日志)
  //   + py-init.log(Py_Initialize 报错), 均 App 视图可读; 只读尾 16KB → 末 18 行,
  //   展开时 3s 刷新(心跳日志 0.4s 时可见滚出)。
  readLogTail(): string {
    const paths = [
      `${getContext(this).filesDir}/pyroot/diag.log`,
      `${getContext(this).filesDir}/pyroot/py-init.log`
    ];
    let s = '';
    for (const p of paths) {
      try {
        // ⚠ API24 ReadOptions 无 position/seek(2026-09-05 编译实错) → readTextSync 全读切尾;
        //   diag.log 心跳 0.4s 追加, 长挂(小时级)后文件变大 —— 将来若>8MB 再换分块读。
        const full = fileIo.readTextSync(p);
        // 尾 60 行(实时观感): 心跳日志 0.4s 一条, 展开期间 1s 刷新可见滚筒
        const tail = full.slice(-60000).split('\n').slice(-60).join('\n');
        s = `${s}── ${p.substring(p.lastIndexOf('/') + 1)} ──\n${tail}\n`;
      } catch (_) { /* 日志缺失/不可读: 终端留空 */ }
    }
    return s;
  }

  build() {
    // 全屏铺满(无标题/无边距)。未点击启动 → 单入口首页(零自动启动);
    //   已点击 → Web about:blank 常驻挂载, 就绪前全屏进度页覆盖, ready 后透出画布。
    Stack() {
      if (this.active) {
        // src 恒为 about:blank：加载源唯一 —— 后端就绪由 waitBackend() 的 controller.loadUrl(BACKEND_URL)
        //   触发（2026-09-04 全屏改版: src 随 backendReady 切换会与 loadUrl 双触发重复加载）。
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
          // ── 官方同构启动页(Brand Takeover, 2026-09-05): 品牌区 + 单调进度条 +
          //    4 步真实阶段 + 装载期叙事文案循环 + 可折叠日志终端 ──
          Column() {
            Text('ComfyUI')
              .fontSize(42)
              .fontWeight(FontWeight.Bold)
              .fontColor(0xffffff)
              .margin({ top: 56 })
            Text('HarmonyOS 本地 AI 推理')
              .fontSize(14)
              .fontColor(0x8899AA)
              .margin({ top: 4 })

            // 单调进度条(永不回退): 自绘 Stack bar, 段映射见 calcProgress()
            Stack() {
              Row().width('100%').height(5).backgroundColor(0x1E2937).borderRadius(3)
              Row().width(`${this.progress}%`).height(5).backgroundColor(0x3B82F6).borderRadius(3)
            }
            .width('78%')
            .margin({ top: 28 })

            ForEach(this.steps, (s: string, i: number) => {
              Row() {
                if (this.stepState(i + 1) === 2) {
                  // 完成态: SymbolGlyph(checkmark) 矢量绿色 —— 不用 ✓ 字形字符(防 emoji 视效)
                  SymbolGlyph($r('sys.symbol.checkmark'))
                    .fontSize(14)
                    .fontColor([0x2e9e4f])
                    .width(24)
                } else if (this.stepState(i + 1) === 1) {
                  LoadingProgress().width(20).height(20).margin({ right: 4 })
                } else {
                  Text('·').fontSize(16).fontColor(0x666666).width(24)
                }
                Text(s)
                  .fontSize(15)
                  .fontColor(this.stepState(i + 1) === 2 ? 0x2e9e4f :
                    (this.stepState(i + 1) === 1 ? 0xffffff : 0x777777))
              }
              .width('78%')
              .padding({ left: 16, top: 8, bottom: 8 })
            }, (s: string) => s)

            Text(this.phase === 3 ? this.narrs[this.narrIdx] : this.status)
              .fontSize(13)
              .fontColor(0x9CA3AF)
              .margin({ top: 18 })
            Text(`已用 ${this.fmtElapsed()}`)
              .fontSize(12)
              .fontColor(0x6B7280)
              .margin({ top: 6 })

            // 可折叠日志终端: 条形折叠头(Button 包裹 —— uitest 点击验证 Row onClick 不响应, 2026-09-06)
            //   + Scroll 内滚动(60 行, 1s 刷新)
            // 折叠头: SymbolGlyph 矢量图标(系统符号, 绝无 emoji 字形) + 文字
            //   ⚠ 2026-09-06 用户定谳: 字形符号字符(▶/▼ 等)会被字体渲染成 emoji 样式
            //     观感不统一 —— 一律用 sys.symbol(矢量), 收起=chevron_right / 展开=chevron_down。
            Button() {
              Row() {
                // ⚠ 图标固定 width/height: chevron_right 与 chevron_down 原生宽度不同,
                //   否则切状态时「启动日志」文字左右位移(2026-09-06 用户实判)。
                if (this.logOpen) {
                  SymbolGlyph($r('sys.symbol.chevron_down'))
                    .fontSize(12)
                    .fontColor([0x60A5FA])
                    .width(16)
                    .height(16)
                    .margin({ right: 6 })
                } else {
                  SymbolGlyph($r('sys.symbol.chevron_right'))
                    .fontSize(12)
                    .fontColor([0x60A5FA])
                    .width(16)
                    .height(16)
                    .margin({ right: 6 })
                }
                Text('启动日志').fontSize(13).fontColor(0xFFFFFF)
              }
              .width('100%')
              .justifyContent(FlexAlign.Start)
              .alignItems(VerticalAlign.Center)
            }
            .type(ButtonType.Normal)
            .width('78%')
            .height(36)
            .padding({ left: 12, right: 12 })
            .borderRadius(8)
            .backgroundColor(0x1A2230)
            .margin({ top: 24 })
            .onClick(() => {
              this.logOpen = !this.logOpen;
              if (this.logOpen) {
                this.logText = this.readLogTail();
              }
            })
            if (this.logOpen) {
              Scroll() {
                Text(this.logText)
                  .fontSize(11)
                  .fontColor(0xD1D5DB)
                  .width('100%')
                  .fontFamily('monospace')
              }
              .width('78%')
              .height(260)
              .padding(10)
              .backgroundColor(0x0B0F14)
              .borderRadius(8)
              .margin({ top: 8 })
              .align(Alignment.TopStart)
            }
          }
          .width('100%')
          .height('100%')
          .alignItems(HorizontalAlign.Center)
          .backgroundColor(0x101418)
        }
      } else {
        // ── 单入口首页(2026-09-05): 固定环境卡 +「启动 ComfyUI」; 无环境管理 ──
        Column() {
          Text('ComfyUI')
            .fontSize(34)
            .fontWeight(FontWeight.Bold)
            .fontColor(0x123b6d)
            .margin({ top: 80, bottom: 8 })
          Text('HarmonyOS 本地 AI 推理')
            .fontSize(14)
            .fontColor(0x888888)
          Column() {
            Text('ComfyUI 环境')
              .fontSize(20)
              .fontWeight(FontWeight.Medium)
            Text('SD-Turbo 256×256 · 低内存形态')
              .fontSize(14)
              .fontColor(0x999999)
              .margin({ top: 6 })
            Text('模型目录: Download/app.hackeris.hium/models')
              .fontSize(12)
              .fontColor(0xAAAAAA)
              .margin({ top: 6 })
          }
          .width('78%')
          .padding({ left: 20, right: 20, top: 22, bottom: 22 })
          .borderRadius(12)
          .backgroundColor(0xF2F6FB)
          .alignItems(HorizontalAlign.Start)
          .margin({ top: 36 })

          Button('启动 ComfyUI')
            .width('70%')
            .height(50)
            .fontSize(17)
            .margin({ top: 28 })
            .onClick(() => {
              // 唯一触发点: 用户手势 → 重设施才开始(零自动启动实质)
              this.startApp();
            })
          Text('后端仅在点击启动后运行; 退出应用即停止')
            .fontSize(12)
            .fontColor(0x999999)
            .margin({ top: 14 })
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
      // 零自动启动: 打开 App 不拉起子进程/解压/装载 —— ps 无 comfy_child 可证。
      hilog.info(DOMAIN, TAG, 'app opened, idle (no backend until start tap)');
    })
  }
}
