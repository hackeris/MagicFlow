// Index.ets — poc/npu 主页面: 一键运行全部 NPU 探针通道, 汇总显示并落盘报告。
//
// 通道(与 nnrt_probe.cpp 头注释一致):
//   CH-01 npuEnum         设备枚举 (C API, UIAbility 进程)
//   CH-07 (本文件)        MindSporeLite Kit ArkTS 侧设备枚举 —— 与 CH-01 通道对照
//   CH-02 npuOpsMatrix    SD 链 33 算子覆盖矩阵 (构图/编译/执行 rc + 耗时)
//   CH-03 npuPerfRepeat   Add 五轮时序 (缓存复用)
//   CH-04 npuOffline      离线 .omc buffer (build 预期 rc=1)
//   CH-05 npuSingleOpProbe 单算子直调候选库存在性
//   CH-06 npuStartChild   NCP 子进程内枚举 (结果落 filesDir/npu-child.log)
// 报告: filesDir/npu-poc-report.md (hdc 可拉)
import { npuEnum, npuOpsMatrix, npuPerfRepeat, npuOffline, npuSingleOpProbe, npuStartChild } from 'libentry.so';
import { fileIo } from '@kit.CoreFileKit';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { mindSporeLite } from '@kit.MindSporeLiteKit';

const DOMAIN = 0xD001;
const TAG = 'NPUPoc';

@Entry
@Component
struct Index {
  @State report: string = '就绪。点击「运行全部探测」(约 30-60s)。';
  @State running: boolean = false;

  private ctx(): common.UIAbilityContext {
    return getContext(this) as common.UIAbilityContext;
  }

  private async ch07ArkTs(): Promise<string> {
    // ArkTS 侧 MindSporeLite Kit 设备枚举(对照 CH-01; 本 Kit ArkTS-only, 无法被子进程/Python 使用)
    let s = `[CH-07] mindSporeLite Kit devices:\n`;
    try {
      const ds = mindSporeLite.getAllNNRTDeviceDescriptions();
      s += `  count=${ds.length}\n`;
      for (const d of ds) {
        // API 24: NNRTDeviceDescription 仅暴露 deviceID()
        s += `  - deviceID=${d.deviceID()}\n`;
      }
    } catch (e) {
      s += `  Exception: ${JSON.stringify(e)}\n`;
    }
    return s;
  }

  private async ch04Omc(): Promise<string> {
    // 读 rawfile 的 custom_graph.omc (6405B, 601 链产物) → ArrayBuffer → CH-04
    const ctx = this.ctx();
    try {
      const raw = await ctx.resourceManager.getRawFileContent('custom_graph.omc');
      const buf = raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.byteLength);
      return npuOffline(buf, `${ctx.filesDir}/nncache`);
    } catch (e) {
      return `[CH-04] rawfile read EX: ${JSON.stringify(e)}\n`;
    }
  }

  private async ch06Child(): Promise<string> {
    const ctx = this.ctx();
    const resultPath = `${ctx.filesDir}/npu-child.log`;
    // 清旧文件
    try {
      fileIo.unlinkSync(resultPath);
    } catch (e) {
      // 不存在 = 正常
    }
    const pid = npuStartChild(resultPath);
    let s = `[CH-06] native child start rc(pid)=${pid}\n`;
    if (pid <= 0) {
      s += `  StartNativeChildProcess failed (rc=${pid})\n`;
      return s;
    }
    // 轮询子进程结果文件(20 × 500ms)
    for (let i = 0; i < 20; i++) {
      await new Promise<void>((r) => setTimeout(r, 500));
      try {
        const data = fileIo.readTextSync(resultPath);
        if (data && data.length > 0) {
          s += data;
          return s;
        }
      } catch (e) {
        // 文件未出现
      }
    }
    s += `  child result file timeout\n`;
    return s;
  }

  private async runAll(): Promise<void> {
    if (this.running) return;
    this.running = true;
    this.report = '运行中…(CH-02 算子矩阵需 30-60s, 请耐心)';
    const ctx = this.ctx();
    const nnCache = `${ctx.filesDir}/nncache`;

    let r = `# NPU POC Demo Report\n# device-independent header\n`;
    let ch1 = '';
    try {
      ch1 = npuEnum();
    } catch (e) {
      ch1 = `[CH-01] EX: ${JSON.stringify(e)}\n`;
    }
    r += `\n=== CH-01 ===\n${ch1}\n`;

    let ch7 = '';
    try {
      ch7 = await this.ch07ArkTs();
    } catch (e) {
      ch7 = `[CH-07] EX: ${JSON.stringify(e)}\n`;
    }
    r += `=== CH-07 ===\n${ch7}\n`;

    // CH-02/03/05 是可能较长的 C++ 调用(无崩溃隔离 = 同一进程; 分次调用避免单通道全覆盖)
    let ch2 = '';
    try {
      this.report += `\n...CH-02 算子矩阵进行中...\n`;
      ch2 = npuOpsMatrix(nnCache);
    } catch (e) {
      ch2 = `[CH-02] EX: ${JSON.stringify(e)}\n`;
    }
    r += `=== CH-02 ===\n${ch2}\n`;

    let ch3 = '';
    try {
      ch3 = npuPerfRepeat(nnCache);
    } catch (e) {
      ch3 = `[CH-03] EX: ${JSON.stringify(e)}\n`;
    }
    r += `=== CH-03 ===\n${ch3}\n`;

    let ch4 = '';
    try {
      ch4 = await this.ch04Omc();
    } catch (e) {
      ch4 = `[CH-04] EX: ${JSON.stringify(e)}\n`;
    }
    r += `=== CH-04 ===\n${ch4}\n`;

    let ch5 = '';
    try {
      ch5 = npuSingleOpProbe();
    } catch (e) {
      ch5 = `[CH-05] EX: ${JSON.stringify(e)}\n`;
    }
    r += `=== CH-05 ===\n${ch5}\n`;

    let ch6 = '';
    try {
      ch6 = await this.ch06Child();
    } catch (e) {
      ch6 = `[CH-06] EX: ${JSON.stringify(e)}\n`;
    }
    r += `=== CH-06 ===\n${ch6}\n`;

    // 落盘
    const repPath = `${ctx.filesDir}/npu-poc-report.md`;
    try {
      const fd = fileIo.openSync(repPath, fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC |
        fileIo.OpenMode.READ_WRITE);
      fileIo.writeSync(fd.fd, r);
      fileIo.closeSync(fd);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'report write failed: %{public}s', JSON.stringify(e));
    }
    this.report = r;
    this.running = false;
    hilog.info(DOMAIN, TAG, 'npu-poc-report written to %{public}s', repPath);
  }

  aboutToAppear(): void {
    // 验证 demo 语义: 页面出现即自动跑全通道(aa start 后无需人工点击, hdc 可驱动)
    this.runAll();
  }

  build() {
    Column({ space: 8 }) {
      Text('NPU POC Demo — 独立工程验证通道')
        .fontSize(18)
        .fontWeight(FontWeight.Bold)
        .margin({ top: 12 })
      Text('包名 app.fuqidian.magicflow | 与主项目 HAP 互斥安装(覆盖) — 详见 poc/npu README')
        .fontSize(11)
        .fontColor('#888888')
      Button(this.running ? '运行中…' : '运行全部探测')
        .width('70%')
        .enabled(!this.running)
        .onClick(() => {
          this.runAll();
        })
      Scroll() {
        Text(this.report)
          .fontSize(11)
          .fontFamily('monospace')
          .textAlign(TextAlign.Start)
          .width('100%')
      }
      .layoutWeight(1)
      .width('96%')
    }
    .width('100%')
    .height('100%')
  }
}
