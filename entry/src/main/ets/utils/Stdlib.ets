import { fileIo } from '@kit.CoreFileKit';
import { zlib } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';

const DOMAIN = 0xD001;
const TAG = 'ComfyUI';

// 与 comfy_child.cpp 的 pyroot=<dir> 约定一致：
// CPython(POSIX) bootstrap 靠 STDLIB_SUBDIR=lib/python3.12/os.py 定位 stdlib，
// 因此必须把 stdlib 解成 <pyroot>/lib/python3.12/ 目录树，再设 PYTHONHOME=<pyroot>。
// ⚠ 铁律分流：此 zip 只装纯 .py 运行时文件（stdlib + site-packages，如 torch/typing_extensions）；
//   一切 .so（torch._C / 70 个 lib-dynload / libpython / libc++.so.1 等）已在 HAP libs/<abi>/，
//   由 comfy_child 的宿主注册桥 dlopen+PyInit+注册进 sys.modules，不随 zip 走。
const STDLIB_RAW = 'python312.zip';
const STDLIB_HOME = 'pyroot';
// zip 版本指纹（run58 修复）：内容 = 上次解压所用 zip 的字节数。解决「幂等=os.py 存在即跳过，
//  zip 更新了 pyroot 却永远停在首次解压」的问题（run57 实证：pyroot 无 main_sim.py →
//  FileNotFoundError → 主链 rc=-1，流程缺口而非内存墙）。判定改为「zip 尺寸指纹匹配才跳过；
//  不匹配则删除 pyroot/lib/python3.12 重建」，保证 python312.zip 更新自动传导到 pyroot。
const SYNCV_NAME = '.syncv';

// stdlib 的拷贝+解压只由本模块这个「唯一解压点」负责，任何地方都通过 ensureStdlib 拿结果，
// 绝不自己再准备一份 —— 并发写同一文件/目录会截断 zip 导致解压损坏(900003)。
// 单例 promise 天然串行 + 幂等：App 每次冷启动 onCreate 只触发一次，后到者复用同一结果。
let stdlibReady: Promise<string> | null = null;

/**
 * 确保 Python 标准库(.py)已解压到 <filesDir>/pyroot 下，返回 <filesDir>/pyroot。
 * 幂等：lib/python3.12/os.py 已存在即刻返回；首次调用负责拷贝+解压。
 * 返回的 pyroot 供子进程设 PYTHONHOME / PYTHONPATH。
 */
export function ensureStdlib(ctx: Context): Promise<string> {
  if (stdlibReady) {
    return stdlibReady;
  }
  stdlibReady = doEnsure(ctx).catch((e: Error) => {
    stdlibReady = null; // 失败允许下次重试
    throw e;
  });
  return stdlibReady;
}

async function doEnsure(ctx: Context): Promise<string> {
  const filesDir = ctx.filesDir;
  const dest = `${filesDir}/${STDLIB_RAW}`;
  const home = `${filesDir}/${STDLIB_HOME}`;
  const marker = `${home}/lib/python3.12/os.py`;
  const syncvPath = `${home}/${SYNCV_NAME}`;

  const rm = ctx.resourceManager;
  // getRawFdSync 拿到 rawfile(在 HAP 资源里)的 fd+offset+length。
  const rawFd = rm.getRawFdSync(STDLIB_RAW);
  hilog.info(DOMAIN, TAG, 'stdlib rawFd fd=%{public}d offset=%{public}d length=%{public}d',
    rawFd.fd, rawFd.offset, rawFd.length);

  try {
    // 版本指纹判定：zip 尺寸匹配才跳过（run58）。旧逻辑「os.py 存在即 skip」使 zip 更新
    //   永远不传导到 pyroot（run57 FileNotFoundError 根因）。size 相同而内容不同的残余
    //   风险：打包脚本每次产物尺寸几乎必然变化，且不匹配时本函数自愈（删树重建）。
    const syncv = fileIo.accessSync(syncvPath) ? fileIo.readTextSync(syncvPath).trim() : '';
    if (syncv === String(rawFd.length)) {
      if (fileIo.accessSync(marker) && fileIo.accessSync(dest)) {
        hilog.info(DOMAIN, TAG, 'stdlib tree ready + zip version match (%s bytes), skip -> %s',
          syncv, marker);
        rm.closeRawFd(STDLIB_RAW); // 按 path 关闭 rawfile 的 HAP fd（closeRawFd 签名是 path 而非 fd）
        return home;
      }
    }

    // 分块流式拷贝 rawfile → filesDir/python312.zip。
    // 整块 writeSync(大 ArrayBuffer) 会 EINVAL=13900020，必须分块；且这是唯一解压点，无并发，TRUNC 安全。
    const dst = fileIo.openSync(dest, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC);
    const CHUNK = 4 * 1024 * 1024;
    const buf = new ArrayBuffer(CHUNK);
    let pos = rawFd.offset;
    const end = rawFd.offset + rawFd.length;
    let total = 0;
    while (pos < end) {
      const toRead = Math.min(CHUNK, end - pos);
      const n = fileIo.readSync(rawFd.fd, buf, { offset: pos, length: toRead });
      if (n <= 0) break;
      fileIo.writeSync(dst.fd, buf, { length: n });
      pos += n;
      total += n;
    }
    fileIo.closeSync(dst);
    fileIo.closeSync(rawFd.fd);
    hilog.info(DOMAIN, TAG, 'stdlib zip written %{public}s (%{public}d bytes)', dest, total);

    // 解压到 filesDir/pyroot → 得到 lib/python3.12/ 目录树（zip 内条目即 lib/python3.12/*）。
    // 无并发时首次运行 home 尚不存在，mkdir 创建即可。CoreFileKit 无 rmSync，
    // 且 mkdirSync(recursive=true) 对已存在目录也可能抛 File exists，故容错。
    try {
      fileIo.mkdirSync(home, true);
    } catch (_) {
      // 目录已存在，忽略
    }
    // 版本不匹配走到这里：旧树作废，先删 pyroot/lib/python3.12（zip 条目 lib/python3.12/**
    //   的解压结果）再重建。pyroot 根的 diag.log/napi.log 不在 lib 下，不受影响。
    if (fileIo.accessSync(`${home}/lib/python3.12`)) {
      hilog.info(DOMAIN, TAG, 'stdlib zip version changed, remove stale tree -> %{public}s',
        `${home}/lib/python3.12`);
      fileIo.rmdirSync(`${home}/lib/python3.12`);
    }
    await zlib.decompressFile(dest, home, {});

    if (!fileIo.accessSync(marker)) {
      throw new Error(`stdlib tree missing ${marker} after decompress`);
    }
    // 指纹落盘：与本次解压所用 zip 尺寸同源（fingerprint = rawFd.length）。
    {
      const vf = fileIo.openSync(syncvPath,
        fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.WRITE_ONLY);
      fileIo.writeSync(vf.fd, String(rawFd.length));
      fileIo.closeSync(vf);
    }
    hilog.info(DOMAIN, TAG, 'stdlib decompress ok, pyroot=%{public}s', home);
    return home;
  } catch (e) {
    hilog.error(DOMAIN, TAG, 'stdlib FAILED code=%{public}s msg=%{public}s',
      JSON.stringify((e as BusinessError).code), JSON.stringify((e as BusinessError).message));
    throw e as Error;
  }
}
