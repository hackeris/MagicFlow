import { fileIo, fileUri, picker } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0xD001;
const TAG = 'ComfyUI';

// ── W3 模型根(2026-09-05, docs/model-download.md)──────────────────────────
//   模型根 = 用户可见下载目录: DocumentViewPicker(DOWNLOAD 模式, 直达 Download/<bundle>/,
//   不弹界面 —— wineohos WineEnvService.ets:822-842 先例) → FileUri.path → root/models。
//   用户: 系统文件管理器可看/可放模型文件; App(含后端子进程, 同 uid)可读写。
//   契约: 进程内单例(仿 Stdlib); 失败降级 → 沙箱 <filesDir>/pyroot/comfyui/models
//   (用户不可见, 但下载/出图仍可用); 降级不落盘, 下次启动重试 picker。
//   只写不删: yaml 幂等写(内容同即 skip), 不影响解压链(comfyui/ 树为 zip 产物,
//   zip 换版重解压会重建树 —— yaml 不在 zip 内, 保活; 每次 launch 重写自愈)。

export interface ModelsRootInfo {
  root: string;         // 模型根(绝对, 进程视图; 子进程同 uid 同视图)
  viaPicker: boolean;   // true=用户可见目录; false=沙箱降级
}

const SUBDIRS = ['checkpoints', 'loras', 'vae', 'controlnet', 'upscale_models', 'embeddings'];
const ROOT_CACHE_FILE = 'models_root.json';
const YAML_REL = 'comfyui/extra_model_paths.yaml';

let rootPromise: Promise<ModelsRootInfo> | null = null;

function sandboxRoot(ctx: Context): string {
  return `${ctx.filesDir}/pyroot/comfyui/models`;
}

// ArkTS 禁 object-literal-as-type: 缓存文件的结构用显式 interface
interface ModelsRootJson {
  root: string;
}

async function pickModelsRoot(ctx: Context): Promise<ModelsRootInfo | null> {
  // 1) 持久化命中(上次授权的路径仍存在 → 复用, 不再走 picker)
  try {
    const saved = fileIo.readTextSync(`${ctx.filesDir}/${ROOT_CACHE_FILE}`);
    const j = JSON.parse(saved) as ModelsRootJson;
    if (j.root && fileIo.accessSync(j.root)) {
      const r: ModelsRootInfo = { root: j.root, viaPicker: true };
      return r;
    }
  } catch (_) { /* 无缓存/损坏 → 走 picker */ }
  // 2) DocumentViewPicker DOWNLOAD 模式授权(不弹界面); 异常/空结果 → null(降级)
  let res: string[] = [];
  try {
    const documentViewPicker = new picker.DocumentViewPicker(ctx);
    res = await documentViewPicker.save({ pickerMode: picker.DocumentPickerMode.DOWNLOAD });
  } catch (e) {
    hilog.warn(DOMAIN, TAG, 'models root picker failed: %{public}s', JSON.stringify(e));
    return null;
  }
  if (!res || res.length === 0) {
    hilog.warn(DOMAIN, TAG, 'models root picker empty result');
    return null;
  }
  let p = '';
  try {
    p = new fileUri.FileUri(res[0]).path;
    if (p.startsWith('file://')) { p = p.substring(7); }
  } catch (e) {
    hilog.error(DOMAIN, TAG, 'FileUri parse fail: %{public}s', JSON.stringify(e));
    return null;
  }
  const base = p.endsWith('/') ? p : `${p}/`;
  const root = `${base}models`;
  try {
    const fd = fileIo.openSync(`${ctx.filesDir}/${ROOT_CACHE_FILE}`,
      fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.WRITE_ONLY);
    fileIo.writeSync(fd.fd, JSON.stringify({ root }));
    fileIo.closeSync(fd);
  } catch (e) { hilog.warn(DOMAIN, TAG, 'models root cache write fail: %{public}s', JSON.stringify(e)); }
  const out: ModelsRootInfo = { root: root, viaPicker: true };
  return out;
}

/** 模型根唯一入口(进程单例): picker 授权 → 缓存复用; 失败降级沙箱。
 *  与 ensureModels 同享缓存: 先调方定论, 后调方读同一 Promise。 */
export function getModelsRoot(ctx: Context): Promise<ModelsRootInfo> {
  if (!rootPromise) {
    rootPromise = (async (): Promise<ModelsRootInfo> => {
      const r = await pickModelsRoot(ctx);
      if (r) {
        hilog.info(DOMAIN, TAG, 'models root (user-visible): %{public}s', r.root);
        return r;
      }
      const fb: ModelsRootInfo = { root: sandboxRoot(ctx), viaPicker: false };
      hilog.warn(DOMAIN, TAG, 'models root degraded to sandbox: %{public}s', fb.root);
      return fb;
    })();
  }
  return rootPromise;
}

function writeTextIfChanged(fp: string, content: string): void {
  try {
    if (fileIo.accessSync(fp) && fileIo.readTextSync(fp) === content) {
      hilog.info(DOMAIN, TAG, 'yaml/readme unchanged, skip write: %{public}s', fp);
      return;
    }
    const fd = fileIo.openSync(fp,
      fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.WRITE_ONLY);
    fileIo.writeSync(fd.fd, content);
    fileIo.closeSync(fd);
    hilog.info(DOMAIN, TAG, 'written: %{public}s', fp);
  } catch (e) {
    hilog.error(DOMAIN, TAG, 'write fail %{public}s: %{public}s', fp, JSON.stringify(e));
  }
}

const README_TXT = `ComfyUI 模型目录(本目录用户可见)

子目录用途:
  checkpoints/          完整模型(如 SD-Turbo sd_turbo.safetensors)
  vae/                  VAE 模型
  loras/                LoRA 模型
  controlnet/           ControlNet 模型
  upscale_models/       放大模型(Real-ESRGAN 等)
  embeddings/           文本嵌入

使用方法: 从 HuggingFace/Civitai 等下载 .safetensors/.ckpt 文件,
放入对应子目录; 回到 ComfyUI 刷新模型列表(或重启应用)即可使用。

应用内下载的模型也保存在本目录, 可在文件管理器中直接查看/删除。
`;

/**
 * 启动链入口(W3): 模型根 + 子目录 + extra_model_paths.yaml(is_default 置首) + README。
 * 幂等; yaml 仅 when viaPicker(沙箱是 core 默认路径, 无需配)。
 * 必须在 startComfyChild 之前 await(后端 main.py 启动时加载 yaml)。
 */
export async function ensureModels(ctx: Context, pyroot: string): Promise<ModelsRootInfo> {
  const info = await getModelsRoot(ctx);
  const mkDirs = (): void => {
    for (const d of [info.root, ...SUBDIRS.map((s: string) => `${info.root}/${s}`)]) {
      try {
        fileIo.mkdirSync(d, true);
      } catch (_) { /* 已存在等, 容错 */ }
    }
  };
  mkDirs();
  if (info.viaPicker) {
    // 幂等 yaml(绝对路径; 子进程同 uid 同视图 → 与 core apply_custom_paths 语义一致)
    const yamlContent = [
      '# OHOS 模型根(W3): 用户可见下载目录, is_default 置首位(下载落点=首位路径)',
      'ohos_models:',
      `  base_path: ${info.root}`,
      '  is_default: true',
      ...SUBDIRS.map((s: string) => `  ${s}: ${s}`),
      ''
    ].join('\n');
    writeTextIfChanged(`${pyroot}/${YAML_REL}`, yamlContent);
    writeTextIfChanged(`${info.root}/README.txt`, README_TXT);
  }
  hilog.info(DOMAIN, TAG, 'models ensured root=%{public}s viaPicker=%{public}s',
    info.root, String(info.viaPicker));
  return info;
}

/** ensureModel 用: 旧文件(开发期)迁移动态 — 产品不做转移; 直接查新根即可。 */
export function modelPath(info: ModelsRootInfo, sub: string, name: string): string {
  return `${info.root}/${sub}/${name}`;
}
