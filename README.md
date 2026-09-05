# ComfyUI on HarmonyOS（arm64）

把 ComfyUI 后端移植为 HarmonyOS 应用、在 aarch64 真机（PC/Pad）上跑起来的正交工程。
**现状（2026-09-03）**：后端子进程主链达成 —— 真机 `CF-OK-8188`（`/system_stats` 200，`comfyui_version 0.34.x`）；
ArkWeb 前端接通（官方 ComfyUI_frontend v1.54.1，截图实证完整 GUI）。

## 1. 是什么

```
┌─ HarmonyOS 应用 (app.hackeris.hium) ───────────────────────────────┐
│ 父进程 (ArkTS)             NCP 子进程 (原生 .so)                    │
│ Index.ets                  libcomfy_child.so  (entry)              │
│  ├─ 轮询 /system_stats     └─ Main(NativeChildProcess_Args)        │
│  └─ Web(ArkWeb)               ├─ PyConfig+Py_Initialize            │
│       ↑ 同源                  │   pyroot=<filesDir>/pyroot         │
│       http://127.0.0.1:8188  │    └─ python312.zip(rawfile 解压)   │
│                              │        = stdlib 3.12.7 + torch 2.10 │
│                              │          + ComfyUI 0.34 repo(patched)│
│                              │   └─ meta_path finder:libs/ 里的    │
│                              │      无 SONAME 扩展 dlopen+PyInit   │
│                              │        注册进 sys.modules           │
│                              └─ 后端 comfyui/main.py (HTTP :8188)  │
└────────────────────────────────────────────────────────────────────┘
```

- **父进程** `entry/src/main/ets`：ArkTS 拉起 NCP 子进程（`OH_Ability_StartNativeChildProcess`），
  后端就绪后 ArkWeb 加载 `http://127.0.0.1:8188`（同源，前端由后端 `--front-end-root` 直接服务）。
- **子进程** `entry/src/main/cpp/child/comfy_child.cpp`：嵌入式 CPython 宿主 ——
  `PyConfig` 初始化（禁 `Py_SetProgramName` 旧 API 混用）；`sys.meta_path` Finder 把
  HAP `libs/arm64-v8a/` 里「无 SONAME 的扩展 .so」按短名 `dlopen` + `PyInit_*` + 注册
  （torch._C、numpy、scipy、PIL、_pydantic_core、tokenizers、safetensors 同构）。
  随后 `run_path` 执行 `pyroot/comfyui/main.py`。
- **Python 3.12 栈**：来自 OHOS aarch64 PyTorch 运行时先例
  `openharmony-robot/thirdparty_pytorch`（`test/skh-run.tar.gz`，LFS `d6d6bfcfea`）——
  Python 3.12.7 + PyTorch 2.10.0 + 环境自带 site-packages。铁律：**一切 .so 只经
  HAP `libs/<abi>/` 加载**（禁止 filesDir/绝对路径 dlopen；禁止 patchelf）。
  OHOS 系统解释器无关,进程内嵌全是应用侧库。

## 2. 复现链（从空机器 → 真机验证）

环境要求（构建机）：
- Linux x86_64；HarmonyOS Command Line Tools **6.1.1.280**（`/apps/harmony`，含 hvigorw/hdc/SDK 6.1.1 API24）；
- rust 工具链（rustc + `aarch64-unknown-linux-ohos` target——编译两个 Rust 扩展）；
- 网络（仅 `fetch` 一步；gitcode/github/pypi，或本地副本 env 覆盖）。
- aarch64 真机（`hdc -t` 目标见 `scripts/env.sh` 的 `HDC_TARGET`）。

```bash
make fetch            # ① 外部输入→externals/(sha256 全校验,幂等;断网: SKH_RUN_TARBALL=/tmp/tpp/test/skh-run.tar.gz ...)
make extract          # ② skh-run.tar.gz → build/skh-run-extract/（唯一数据源契约）
make stage            # ③ ComfyUI 源码(+patch) + 纯 py 依赖 + 官方前端 → build/pyroot-stage/
make rust             # ④ thirdparty/ submodule 源码 → build/rust-out/（两个 Rust 扩展,sha 与锚比对）
make zip              # ⑤ stage+skh stdlib → rawfile/python312.zip（28,080 条/manifest 锚）
make prebuilt         # ⑥ 清单驱动收集 → entry/src/main/cpp/prebuilt/（324 文件,sha/计数/NEEDED 闭环）
make hap              # ⑦ hvigorw assembleHap → entry-default-signed.hap（libs/arm64-v8a == 236 元锚）
make verify-device    # ⑧ 部署真机 + run_and_capture.sh 一轮观测（Cf-OK-8188 是达成证词）
```

每步失败即停（绝无“占位/降级继续”的假成功路径）；校验锚见 `config/externals.pins.tsv`、
`config/prebuilt_manifest.tsv`、`docs/manifests/python312.zip.manifest.gz`。

### 关键机制（新伙伴必读）

| 机制 | 说明 |
|---|---|
| `stub/` 双注入通道 | `sitecustomize_tpl.py` → zip 内 `lib/python3.12/sitecustomize.py`（CPython 启动最深处注入）；`stub_global.py` → zip 内 `comfyui/stub_global.py`（方案 B）。二者同模板：为模型层缺依赖（torchvision 全树等 80 叶子）做空壳预置，使主链 `import` 不死。`__spec__` 必须真形 `ModuleSpec`（find_spec 反咬）；树 stub 名单必须带包名前缀（`torchvision.`）。|
| `stub/psutil.py` | OHOS 兼容层（非 PyPI 包）。真包带 `_psutil_linux.so`，滤 .so 后 import 崩 → 由 stage 注入,requirements 显式排除。|
| prebuilt 无 SONAME 扩展链 | `CMakeLists.txt`：每个扩展一个 IMPORTED target（绝对路径）并 `-Wl,--as-needed`（它们不进入 comfy_child 的 DT_NEEDED——否则记“构建机绝对路径”真机崩；收集靠 hvigor 官方版对非 sysroot 绝对路径直接收集）。**hvigor 6.24.2 官方原版无需任何修改**（曾改插件 strip `-l:` 的方案已废弃并还原）。|
| `comfy_api/input` | 官方源自带（input/{__init__,basic_types,video_types}.py）；打包排除只限根级 `input/`，`comfy_api/input` 必须完整放行。|
| regex | `transformers` 唯一主链引用点已由 zip 侧 `AUTODOC_STUB` 绕开；纯 py 树绝不收集 regex（其 `_regex` C 扩展不匹配会带崩主链）。|

## 3. 目录解释

```
config/                外部资源 pin 表 + vendor 私有 wheel + prebuilt 清单（全仓库配置真相）
externals/             fetch 产物: skh-run.tar.gz(544MB)/comfyui-src/frontend-dist/py-site/… (gitignored)
thirdparty/            Rust 扩展源码 submodule（tokenizers@7f1623b, safetensors@a406ca3）
stub/                  三个注入模板（sitecustomize_tpl/stub_global/psutil）——被 make_py312_zip/stage 打进 zip
scripts/               复现链脚本（fetch/extract/stage/zip/rust/prebuilt/run_and_capture...命令观察）
patches/               ComfyUI 源码 OHOS 修改（comfyui-src-ohos-changes.patch, 基于 03468f4）
docs/                  DESIGN.md（研究史+当前状态+pin）/ manifests/（zip 锚）/ 调研报告
entry/src/main/cpp/    NCP 子进程宿主 + CMakeLists(+prebuilt 收集品, gitignored)
entry/src/main/ets/    ArkTS 前端（Index.ets Web 接线 / Stdlib.ets 解压）
build/                 build 中间所有（skh-run-extract/pyroot-stage/rust-out/gitignored .cxx 等）
.ohos/                 debug 签名材料（p12/cer/p7b/csr, gitignored; DevEco 自动生成,再生见下）
```

### 签名材料（真机安装必须）

aarch64 真机拒绝 unsigned HAP。本工程 `.ohos/` 四件套 = DevEco Studio 自动生成的 debug 签名
（bundleName **app.hackeris.hium**，源自 qemu 工程复用）。**再生方法**：DevEco Studio →
File → Project Structure → Signing Configs → 自动签名（需华为账号, 保持 bundleName 一致）；
或复用 qemuohos 同一套 debug 材料。证书有有效期——过期报 `signature` 错误时按上面流程再生成。

### 外部资源 pin 摘要（全表 `config/externals.pins.tsv`）

| 资源 | 来源 | 锚 |
|---|---|---|
| skh-run.tar.gz (544,835,856B) | gitcode LFS `thirdparty_pytorch` | sha256 `d6d6bfcfea07…25406` |
| 前端 dist v1.54.1 (24,601,619B) | github Comfy-Org/ComfyUI_frontend release | zip sha `a89cf5e8…`; index.html md5 `61a69562…` |
| ComfyUI 03468f4 | github comfyanonymous/ComfyUI | commit `03468f4` + patch 应用 |
| pydantic_core-2.46.5 musllinux_1_1_aarch64 wheel | pypi | sha `efd62a42…` |
| 纯 py 依赖集 | pypi（pip download/install --target） | `scripts/venv-requirements-port.txt` |
| python312.zip | 本链产物 | 151,213,981B / 28,080 条 / sorted-namelist 锚 |

### 工具链基线

CLT 6.1.1.280 / hvigor 6.24.2（**官方原版**，md5 `705309b0…`）/ SDK 6.1.1 (API24) / hdc 3.2.0d / rustc 1.98.0 +
`aarch64-unknown-linux-ohos`。历史: hvigor 插件曾被本地修补过（`-l:` strip），2026-09-03 还原。

## 4. 已知缺口（用了才炸）

- 推理期功能：视频/av、部分 torchvision/scipy 算子、alembic 等 —— 均为惰性 import 点,主链不受影响
  （alembic = comfy 远程 sqlite 历史同步用,启动仅打 warning;blake3 同理）；
- 前端模板列表走远程,设备无外网时显示 0/0 为正常；
- 模型权重（数十 GB）不进 HAP,当前为演示最小链；后续走资源包/云落 filesDir 方案。
- **启动耗时（2026-09-04 定谳）**：设备端「安装→后端就绪」约 1~2 分钟属**正常范围**（28k+ 条目的树解压
  与 torch/依赖全量装载为架构固有成本,与桌面版不可比）。已入链的缓解：pyc pass（`make_py312_zip`
  预编字节码随 zip 落树, PEP552 unchecked-hash 头, 免启动期逐模块编译）——收益不设预期，
  不再逐轮回归测时（延续「单轮单变量/不折腾」纪律）。曾在设备上否定过的路线（留档）：运行期
  自动写 `__pycache__`（去 PYTHONDONTWRITEBYTECODE）→ 首启 ~5 分钟,已回退,注释在
  `comfy_child.cpp`。
- **本地推理并行限制（2026-09-04 定谳, 证据集见 `docs/local-inference-torch-parallel.md`）**：
  设备已可本地出图（SD-Turbo 256×256×1 步 7.6 分钟实测成功）,但 op 层单线程——根因 = skh torch
  构建 BLAS=Eigen(单线程 GEMM), `torch.set_num_threads` 不作用于 Eigen；修复候选 = 重编换
  `USE_BLAS=OpenBLAS`（skh 发行树自带 libopenblas.a+头）。同时 cpuset 机制：app 非焦点（后台）
  = cpuset:background（3 小核 + 10min 单核>80% 配额 == 系统杀进程两轮实锤）；焦点 = top-app（12 核）。

## 5. 单区纪律（2026-09-05 起）

- **唯一权威 = 本目录（`/data/share/comfyui`）**（git, 无大二进制, 全链可复现）。
- 早前的研究区 `/data/share/comfy-ohos-port` 已于 2026-09-05 清理（内容可由本库 git+复现链重建;
  更早一份归档保留在 `/data/share/comfy-ohos-port.old`）。
- 任何改动/实验/验证一律在本目录进行,不存在"哪边对"的二义性。
