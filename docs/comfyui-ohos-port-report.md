# Comfy-Desktop 移植 HarmonyOS 调研报告

> 调研对象：`github.com/Comfy-Org/Comfy-Desktop`
> 目标平台：HarmonyOS（OpenHarmony）· 设备形态：PC / Pad（arm64）
> 调研日期：2026-09-01
> 参考项目：`../wineohos`（WineHua）、`../qemuohos`（HiUM/HiVM）、`../wineohos/.temp/Termony`
> 素材目录：`.temp/`
>
> ⚠️ **本文是 Phase 0 的调研快照，多处结论已被后续实作推翻**，保留作为决策过程记录。
> 已知被取代的点：①全文以 Python 3.13 为前提，实际走的是切 3.12.7（见 `DESIGN.md:73`）；
> ②关于 libc++ 命名空间（`__h` / `__n1`）的分析被 `DESIGN.md:58-65` 的实测推翻；
> ③§8 的 Phase 0-4 路线已全部完成。**当前状态请看 `README.md` 与 `status-and-next.md`。**

---

## 1. 结论摘要（TL;DR）

**可行性结论：技术上可行，但属于"高难度、长周期、重投入"的移植，且有一条**本质性风险**——PyTorch 的 libc/ABI 与鸿蒙运行库的共存问题。**

Comfy-Desktop 的本质是 **三个东西拼在一起**：
1. **Electron 壳**（N+Vue renderer，装机向导 / 启动 / 云登录 / 模型管理）—— 鸿蒙没有 Electron，**必须整体重写**。
2. **ComfyUI Python 后端**（纯 Python + aiohttp，跑 `main.py`，加载 torch 等）—— 这是**可移植**的核心。
3. **ComfyUI 前端**（Vue SPA，托管在 Python server 的 aiohttp 下，不内嵌在 Electron）—— 鸿蒙端用 **ArkWeb 加载 `http://127.0.0.1:8188` 即可复用**，基本零改动。

因此"移植 Comfy-Desktop 到鸿蒙"= **重写壳 + 在其上原生跑起 ComfyUI Python 后端**。用户指定的三大子课题恰是难点所在：

| 子课题 | 结论 | 主要参考 |
|---|---|---|
| **1. Python 移植** | 可行。参考 Termony 的 **musl 交叉编译**思路（`--enable-shared` + `ANDROID_API_LEVEL=1` hack），但**构建为 `.so`** 而非 ELF 可执行，以便 NCP 子进程 dlopen | `Termony/build-hnp/python/Makefile` |
| **2. PyTorch 移植** | **有条件可行（P0 定论）**：`aarch64-linux-ohos` LLVM 全源码交叉编译 + **libc++ 双命名空间统一**（libpython.so 与 torch._C.so 同链 `libc++_shared.so`，应用侧 `__n1`）+ **显式 `-lpython` + SOABI 实测对齐**；已有 OHOS 板上 `import torch` 先例 | `thirdparty_pytorch`（完整 Python 绑定模板）、OHOS NDK |
| **3. 进程架构适配** | 鸿蒙 **NCP（Native Child Process）只支持将 `.so` 作为独立进程运行**，正与 wineohos/qemuohos 的 `lib*_child.so + Main(NativeChildProcess_Args)` 模式一致 | `wineohos`、`qemuohos` |

### 一句话技术路线
> **把 ComfyUI 后端（CPython + PyTorch）编译为鸿蒙 arm64 的 `.so` 组件，嵌入一个 `libcomfyui_child.so` 宿主进程，经 `OH_Ability_StartNativeChildProcess` 以独立子进程拉起，绑定 `127.0.0.1:8188`，UI 用 ArkTS 重写壳 + ArkWeb 加载 ComfyUI 前端。**
> **形态边界（已定论）**：该 NCP 方案**仅支持 PC/2in1 与平板**（API 14+，手机返回 `NCP_ERR_NOT_SUPPORTED`），与目标设备形态（PC/Pad）一致；`deviceTypes` 需含 `tablet`/`2in1`，应用需声明 `ohos.permission.INTERNET`。

---

## 2. Comfy-Desktop 技术栈剖析（已确认）

### 2.1 总体分层
```
┌─ Electron Renderer 进程（Chromium + Vue3 SPA）────────────┐
│  装机向导 / 启动面板 / 模型管理 / 云登录 / 设置             │
│  经 preload.ts 的 contextBridge 暴露 electronAPI（只走 IPC） │
├─ Electron Main 进程（Node.js）────────────────────────────┤
│  ComfyDesktopApp（总编排）→ AppWindow / ComfyServer /      │
│  InstallationManager / DesktopConfig / VirtualEnvironment │
│  进程 spawn、文件系统、安装生命周期、IPC 路由                │
├─ Python 子进程（venv Python + ComfyUI）────────────────────┤
│  python -s ComfyUI/main.py --cpu --port 8188 --enable-manager
│  aiohttp server，托管 ComfyUI 前端（Vue SPA → templates/）  │
└──────────────────────────────────────────────────────────┘
```

### 2.2 关键事实（源于源码，`src/main/**`）

- **引擎**：Electron 40.4.1、Node ≥22、TypeScript 5.9、Vue 3.5、Vite 7、pnpm。
- **Python 环境形态**（关键）：`install.ts` 显示安装的是 **预打包的可重定位 `venv/` artifact**（top-level `venv/` + `ComfyUI/`），来自 comfy-builder 的 tar.gz（含 SHA-256 校验）。即——**Python 解释器 + torch + ComfyUI 依赖已按平台预先打好包**，每个平台（win-x64 / mac-arm64 / linux-x64）一个 artifact。→ 移植本质 = **为 ohos-arm64 构建同样的 venv artifact（全部编译为鸿蒙可用的 `.so`）**。
- **环境管理与`uv`**：`pythonEnv.ts` 用 `standalone-env/bin/uv` 建 `.venv`（`getVenvPath`）。bootstrap 用 `python-build-standalone` 预编译 CPython **3.13.12**（`scripts/build-bootstrap-python.mjs` 的 `PLATFORM_MAP`：win-x64 / mac-arm64 / linux-x64 三种，均 **glibc/msvc**，无 OHOS/arm64 变体）。
- **后端启动命令行**（`launch.ts`）：`<venv>/bin/python3 -s ComfyUI/main.py  [--cpu] [--port N] [--enable-manager]`，cwd=installPath，port 默认从参数里 `extractPort`。**`-s`**（=`-S`）标志意味着**不自动导入 site**，由 venv 对齐。→ 这是后端进程的唯一口径。
- **前端承载**（重要，利好）：`executionTap.ts` 明确注释 `The launcher does not embed the ComfyUI frontend` —— 前端**不在 Electron 里**，由 Python server 的 aiohttp 在 `templates/` 目录托管。launcher 的 BrowserWindow 直接 `loadURL(http://127.0.0.1:8188)`。→ 鸿蒙端复用同样的前端只需 **ArkWeb 指向该本地端口**。
- **ComfyUI-Manager 为 v4**：`comfyui_manager` Python 包 + `--enable-manager` flag + `<install>/ComfyUI/user/__manager/config.ini`；安全等级 `block/high+/high/middle+/middle`；API 走 `/api/v2/...`。（见仓库 `AGENTS.md`。）
- **模型**：`comfybuilder/models.ts` + `comfyDownloadManager` 管理下载；模型落 `<install>/ComfyUI/models/`（checkpoints/loras/vae/controlnet 等）。
- **发布**：electron-builder + todesktop（`.todesktop.json`、`@todesktop/runtime`）。

### 2.3 移植触点清单（哪些不能直接用、必须换）

| Comfy-Desktop 组件 | 鸿蒙等价物 | 工作量 |
|---|---|---|
| Electron Main（Node） | ArkTS 应用壳 + **ArkTS `Ability`**（生命周期/窗口） | 重写 |
| Electron BrowserWindow | ArkUI 窗口 + `XComponent`/`ArkWeb` | 重写 |
| 预打包 venv (glibc/msvc) | 鸿蒙 arm64 的 venv（musl/libc++），**需自建** | **核心难点** |
| pygit2 bootstrap python (standalone) | 鸿蒙 arm64 python-build-standalone 变体 / 或去掉 | 中 |
| uv 二进制 / node-pty / 系统 API | 鸿蒙 NAPI（`Napi` / `libos`）/ NCP | 中 |
| localhost HTTP (UI↔后端) | 同，`http://127.0.0.1:8188`（见 §5 风险） | 复用 |
| 云登录 / 遥测 (Firebase/Datadog/Posthog) | 按合规裁剪或改用鸿蒙推送/分析 | 轻 |

---

## 3. 移植目标架构（鸿蒙等价物）

```
┌─ ArkUI 进程（libentry.so）──────────────────────────────────┐
│  ArkTS：壳 UI（向导/启动/模型/设置）+ 登录 + 配置持久化         │
│  Native(NAPI)：napi_init.cpp → 窗口/Surface 管理、IPC 路由     │
│  ArkWeb：加载 http://127.0.0.1:8188（ComfyUI 前端复用）        │
│╔═══════════════════════════════════════════════════════════╗│
│║ NCP（OH_Ability_StartNativeChildProcess）—— 每实例一个 so  ││
│║ 子进程 libcomfyui_child.so（NCP，一进程一个 ComfyUI 实例）   ││
│║   Main(NativeChildProcess_Args) {                          ││
│║     dlopen(libpython3.13.so) → Py_Initialize()             ││
│║     → import comfy.main；跑 aiohttp；绑 127.0.0.1:8188      ││
│║   }                                                        ││
│║   （内部：libtorch.so / torch._C.so / venv site-packages） ││
│╚═══════════════════════════════════════════════════════════╝│
└────────────────────────────────────────────────────────────┘
```

**与 wineohos / qemuohos 一一对应**：
- 父进程 = ArkUI 进程（`libentry.so`），负责生命周期/窗口/NAPI —— 同两者。
- 子进程 = `libcomfyui_child.so`，经 `OH_Ability_StartNativeChildProcess` 拉起，入口 `extern "C" void Main(NativeChildProcess_Args args)` —— 同 `wine_child.cpp:388` / `qemu_child.cpp`（`libwine_child.so` / `libqemu_child.so`）。
- 跨进程窗口/渲染（若要走原生侧）：`OH_NativeWindow_WriteToParcel`（qemuohos `DESIGN.md` §3 已验证）—— ComfyUI 走 ArkWeb 则多数场景不需要。

---

## 4. 三大子课题详解

### 4.1 Python 移植（构建为 `.so`）⭐

**目标**：把 CPython 交叉编译为鸿蒙 arm64 的**共享库 `libpython3.13.so`**（而非 Termony 那样的 ELF 可执行），供 `libcomfyui_child.so` dlopen 后嵌入式初始化。

**复用 Termony 的可行做法**（`Termony/build-hnp/python/Makefile`，已确认可行、能跑通）：
```makefile
# 关键构建参数（Termony 实际用的，目标即 arm64 musl）
--host $(OHOS_ARCH)-unknown-linux-musl --build=$(OHOS_ARCH)
--enable-shared            # 产出 libpythonX.Y.so
ANDROID_API_LEVEL=1        # HACK：让共享库链接 libpython 的关键
--with-build-python=<host python>   # 交叉编译需先有同版本 host python (build-native)
DESTDIR 安装到 --prefix=/data/app/... （Termony 用绝对 prefix）
sed 掉 pyconfig.h 的 HAVE_LIBINTL_H   # 缺 gettext
```
产物再 `llvm-strip`。**关键差异（我们要改的）**：
- Termony 目标是 `aarch64-unknown-linux-musl`，安装为 HNP 包 `/data/app/bin/python3` **ELF 可执行**；我们要的是 **`.so` + 嵌入宿主**，因此**不需要 `--enable-shared` 之外的 ELF 入口**，而是让宿主进程去 `dlopen(libpython.so)` + `Py_Initialize`/`Py_BytesMain` 以嵌入方式运行 `ComfyUI/main.py`。
- **前缀要可重定位**：嵌入 .so 后 `sys.prefix` / site-packages 定位依赖编译期 `--prefix`，需与运行时路径设计对齐（参考 wineohos `wine_env.cpp` 的 env 注入，把 `PYTHONHOME/PYTHONPATH` 注入子进程）。

**备注**：CPython 3.13 的**内建模块**（`spwd`/`gdbm`/`crypt` 等）对 glibc 的个别依赖、以及静态 vs 动态 stdlib 取舍，**未专项核实**——属 Phase 1 实现期事项（编译遇缺符号按 musl 补丁处理即可，非阻塞）。

### 4.2 PyTorch 移植 ⭐⭐（最大风险）

**P0 定论（subagent A · 2026-09-01）** —— **物理可行、已有上板先例，但有三条命门。**

**可行性**：`import torch` 在 OpenHarmony 板上已被真实跑通（OHOS 5.1 + aarch64 + PyTorch 2.10），不是臆测。但有两条**口径不同**的先例：
1. `thirdparty_pytorch`（OpenHarmony 板级生态）＝**完整 Python 绑定版**（等价 `BUILD_PYTHON=ON`）：Python 3.12 + torch 2.10 + torchvision 0.25 + torchaudio 2.10，板上 `import torch/torchvision/torchaudio/scipy/safetensors` 全部成功、CPU 推理通过。**可作为本移植的现成模板**（补丁系列 `series.openharmony-5.1.0-musl.txt`）。
2. `build_in_harmonyos`（OHOS PC，MR#5290）＝**仅 libtorch C++ core（`BUILD_PYTHON=OFF`）**，无 Python 绑定，**跑不了 ComfyUI**。

**① libc ABI 定论（"有条件共存"）**：关键不在"musl 能否配 libc++"，而在**是否用同一套 OHOS NDK 统一编译**。OHOS 的 C 库＝**musl**、C++ 库＝**libc++**、工具链＝`clang+libc+++musl+lld`、target＝`aarch64-linux-ohos`。
- Python↔C 扩展边界是**纯 C ABI**（`PyObject*`/CPython C-API，不跨边界传 STL），故 CPython(musl) 与 libtorch(libc++) **不需 C++ 运行时跨库互通**，只需各自内部自洽。
- **真正的坎＝OHOS libc++ 双命名空间**：系统库 `libc++.so`＝命名空间 **`__h`**；应用原生库 `libc++_shared.so`＝**`__n1`**；**两者严禁混用**（华为官方明确：同 app 包内多个动态库须同大版本 clang 编译、依赖同一 libc++_shared.so）。一旦 `torch._C.so` 与宿主 libpython 落到不同命名空间 → 进程内两份 libc++ 运行时：STL 跨 DSO mangled name 不同 → `undefined symbol`；异常 typeinfo 不同 → `std::terminate`；ODR 违规 → double-free。
- **满足共存的 5 硬性条件**：① libpython.so 与 torch._C.so **都当应用库**、用同一套 OHOS NDK clang（同大版本，与 SDK 匹配）编译；② **都链同一个 `libc++_shared.so`（`__n1`）**，`OHOS_STL=c++_shared`（官方：带动态库 HAP 必须 shared，`c++_static` 破坏 ODR）；③ 严禁链系统 `libc++.so`（`__h`）；④ torch._C.so **显式 `-lpython3.12`**（或 `patchelf --add-needed libpython3.12.so.1.0`，否则 `cannot locate symbol PyExc_*`），跨边界接口收敛为 C+`char*`；⑤ 与宿主 CPython 3.12 的 C-API/头文件/pyconfig **同源同配置**。
- **禁止**：拷桌面预编译 libtorch/torch._C.so（必段错误）；musl 缺 glibc 专属符号（`pthread_cancel`/`malloc_usable_size`/`backtrace`/`__cxa_thread_atexit_impl`/`dlinfo`），须源码打补丁替换；用 LLVM OpenMP（`libomp.so`），勿链 GNU `libgomp`；用 `RTLD_NOW|RTLD_LOCAL` + `LD_PRELOAD libpython:libomp` 控符号可见性。
- 跨平台最贴近先例：**Android**（Termux / python-for-android）——CPython 与 libtorch 都走 `c++_shared`（同一 libc++_shared.so）才共存。

**② 产物形态（决定性）**：thirdparty_pytorch 的 runtime **本体是 ELF `bin/python3`**（可独立解释器进程），`libpython3.12.so` 用 `LD_PRELOAD` 预加载，**不是被宿主 dlopen 的库**。两条互斥路线：
- **路线 A 嵌入 CPython**：宿主 dlopen `libpython.so` + `Py_Initialize` + `PyImport_ImportModule("torch")` —— **可行**，但要吞 5 类风险（`Py_Initialize` 内 `dlopen(NULL,RTLD_GLOBAL)` 注入全局符号冲突、TLS key 冲突、GIL 互斥量覆写、**双 OpenMP 运行时冲突**、扩展被外部 dlopen 时**不会自动调 `PyInit_*`** 须先有已初始化解释器）。
- **路线 B 纯 C++ libtorch core**（`BUILD_PYTHON=OFF`）：干净（宿主直接链 .so，无 Python/GIL），**但没有 `torch` Python 模块，跑不了 ComfyUI**。
- **⚠️ 执行模型兼容性（A 的关键提醒）**：ComfyUI 真正的并行靠"每 GPU 一个独立 python 进程"（PromptServer 每实例一进程串行队列）+ comfy-env 的 `SubprocessWorker` 持久子进程（`subprocess` spawn + `runpy` + socket/共享内存 IPC）——依赖 **fork/exec 出新的 python 进程**。**纯单宿主 dlopen「无 bin/python3」的形态跑不了多进程 fan-out、DataLoader 子进程复用。**⇒ **要完整跑 ComfyUI，实际需要 thirdparty_pytorch 那种「完整 python 进程环境」（一个能再派生子进程的 python 运行时），而非只能被 dlopen 的单一解释器。**（对 §3 架构的修正见 4.3 末"执行模型"注。）

**③ 依赖面（CPU-only，已确认/修正）**：
- **必留**：libc++ 工具链、**一种 BLAS/LAPACK**（OpenBLAS 或 `BLAS=Generic`，torch.linalg 必须，无 `USE_BLAS=0` 合法配置）、cpuinfo/fmt/nlohmann-json/pybind11（vendored；pybind11 仅 `BUILD_PYTHON=1`）、OpenMP(`libomp.so`)。
- **必砍**：GPU 族 `USE_CUDA/CUDNN/NCCL/MAGMA/HIP=0`；分布式 `USE_DISTRIBUTED/GLOO/MPI=0`（gloo 桌面默认依赖）；kineto/LLVM/gflags/ittapi/benchmark/gtest。
- **可选（体积-性能权衡）**：oneDNN（`USE_MKLDNN`，arm64 NEON/SVE 成熟，性能敏感建议留）、XNNPACK/FBGEMM/NNPACK/QNNPACK/SLEEF。
- **按功能**：protobuf+onnx（`USE_ONNX=0`，不需要 torch.onnx 导出则砍）、flatbuffers（vendored 小）。
- **修正**：**abseil 不是 PyTorch 核心依赖**（核心 `.gitmodules` 无 abseil，仅 ExecuTorch llama2 示例可选子模块）；真正 vendored 依赖是 cpuinfo/sleef/flatbuffers/XNNPACK/fmt。

**④ ABI tag（易踩坑）**：SOABI＝`cpython-<ver>-<platform>`；`-gnu`/`-musl` 段是编码 libc 的（bpo-43112）。**但 OHOS 是特例**：交叉编译用 `$build_os`(x86_64 glibc 主机) 判平台，config.guess 不识 `aarch64-linux-ohos` 为 musl → **OHOS 的 SOABI 后缀实际命名成 `-gnu` 却是 musl**，**不能靠后缀判 libc，须实测** `sysconfig.get_config_var('EXT_SUFFIX')` + `readelf -d <python>` 确认无 `GLIBC_2.x` 版本化符号。且交叉编译未设 `CMAKE_CROSSCOMPILING_EMULATOR` 时 `Python_SOABI` 会被留空 → `WITH_SOABI` 静默产出无 tag `_C.so` → `import torch._C` 失败（PyTorch PR#189388 修复：须从解释器/crossenv 一次性设 SOABI）。

**⑤ NPU（重要，改变预期）**：**libtorch 在 OHOS 上没有任何可直连的 NPU 后端**。要 NPU 必须**绕开 libtorch**，两条互斥技术栈：**瑞芯微 RKNN**（RK3588→`.rknn`，`rknn_set_core_mask` 三核；主/板端版本须一致、NPU 驱动升级，坑多）或 **华为 MindSpore Lite / Ascend-om**（`@ohos.ai.mindSporeLite`，`.ms/.om`，原生适配达芬奇+INT8/FP16混合量化）。`torch_npu` 面向**昇腾云侧**（Atlas 910B+CANN），**与端侧板卡生态无关**。⇒ CPU-only 起步、NPU 另绕道。

**⑥ 体积**：`libtorch_cpu.a`(OHOS armv8 静态)≈**392MB**；含 Python torch CPU aarch64 wheel≈**80–96MB**；lite interpreter 裁剪（`SELECTED_OP_LIST`/`operator.yaml`）≈**3MB**（代价：只能跑 TorchScript/JIT 序列化模型、维护算子白名单）。含 Python 生态完整运行时 + 板卡依赖 **≥5GB 磁盘**（thirdparty_pytorch 标注）。

**⑦ CPU 性能基准确数（无官方数）**：仓库只证实"功能可用"，无 latency/FPS 数。跨平台参考（骁龙 Hexagon）：SD v2.1 512² —— NPU 约 **10s/图**，CPU 回退 256² 约 **30s/图**；RK3588(6 TOPS) 上 RKNN NPU 约 **3.64 FPS vs ONNX CPU 约 1 FPS（~3.7×）**，INT8 再叠加 3-5×。

### 4.3 进程架构适配（NCP 独立 `.so` 子进程）⭐

**平台侧已定论（subagent C，经 skill-harmonyos-docs 核实）**

- **关键 API（能力确认）**：`OH_Ability_StartNativeChildProcess(entry="libxxx.so:Main", args, options, &pid)`；入口函数名可自定义但要导出 `void Main(NativeChildProcess_Args args)`，子进程按 `dlopen→dlsym("Main")→Main(args)→返回即退出` 执行。头文件 `AbilityKit/native_child_process.h`，链接 `libchild_process.so`，起始 API 12。
- **参数**：`NativeChildProcess_Args{ entryParams(≤150KB), fdList(≤16 fd) }`；`NativeChildProcess_Options{ isolationMode }` —— 模式 `NCP_ISOLATION_MODE_NORMAL=0`（父子**共享网络/沙箱**，**必须用这个**才能让子进程监听端口给 WebView 访问）/ `ISOLATED=1`。
- **子进程硬限制**：不支持 UI 界面、不支持 Context 相关接口、不能建 ArkTS 运行时（**纯 Native**）；**只能由主(UI)进程创建**、子进程不能再创建子进程；随父进程退出而退出。
- **数量上限**：应用内子进程总数 512；单进程最多 50（API 15+）。
- **设备形态（决定性）**：NCP **API 14+ 仅 PC/2in1 + 平板可用**，**手机返回 `NCP_ERR_NOT_SUPPORTED(801)`** —— 与用户目标（PC/Pad）一致，但**手机端不可行**。
- **两套入口不通用**：参数传递型用 `Main(NativeChildProcess_Args)`；IPC 型用 `OH_Ability_CreateNativeChildProcess` + `NativeChildProcess_OnConnect()`/`NativeChildProcess_MainProc()`（后者父子可 IPC，但与本方案无涉）。

**已完成验证**（被两个参考项目证实）：鸿蒙 App 里不能用 `fork`/`exec` 起一个 ELF 可执行进程，**只能用 `OH_Ability_StartNativeChildProcess` 把某个 `.so` 作为独立进程拉起**。这正匹配 ComfyUI 后端"run-once + 需求隔离"的特性。

- **入口约定**：子进程 `.so` 必须导出 `extern "C" void Main(NativeChildProcess_Args args)`（`wine_child.cpp:388`），`args.entryParams` 传参（"homeDir|binDir|arg0|..." + `__env=KEY=VALUE` 覆盖），`args.fdList` 传 fd。
- **参数解包/环境注入**：`wine_child.cpp` 把 `argv[0]=="wineserver"` 截获转本体、`__winehua_desktop__` 标记等 —— 我们同样可在 `Main` 里解析 entryParams，决定启动 ComfyUI 后端（含 `--cpu --port` 等），并注入 `PYTHONHOME/PYTHONPATH/MODELS_DIR`。
- **进程监督**：`wine_process.cpp` 用 `OH_Ability_KillChildProcess(pid)`、`OH_Ability_RegisterNativeChildProcessExitCallback(OnNcpChildExit)` 处理退出 —— 对应我们管理 ComfyUI 实例生命周期（重启/退出回收）。
- **抽象/分派**：wineohos `ncp_dispatch.cpp` 封装 `OH_Ability_StartNativeChildProcess`（含 `Phone_StartNativeChildProcess` 等运行时分派）——可直接借鉴，确保在 PC/Pad/机芯上行为一致。
- **跨进程窗口**（仅当不走 ArkWeb 需要）：qemuohos 已验证 `OH_NativeWindow_WriteToParcel` 跨进程传窗口句柄，**裸 surfaceId 跨进程不可用**（`DESIGN.md` §3 踩坑）。
- **IPC**：parent↔child 建议走 **unix socket**（qemuohos QMP 每条 `qmp-<vmId>.sock`，沙箱路径，无端口冲突）或**直接 localhost HTTP**（ComfyUI 后端本身就是 HTTP server，UI 直接吃）。
- **⚠️ 执行模型（修正 §3 架构的"单解释器"表述）**：NCP 子进程本质是**完整独立 Linux 进程**（具备普通 fork/exec 能力，仅不能递归再建 NCP 子进程）。因此其 `Main()` 里应初始化**完整的 Python 运行时进程环境**（`Py_Initialize` 完整版，非"只能被宿主 dlopen、不能再派生的单一解释器"），才能满足 ComfyUI 的**多进程 fan-out、comfy-env `SubprocessWorker` 持久子进程、PyTorch `DataLoader` 子进程复用**。形态上更贴近 `thirdparty_pytorch` 的"**完整 python 进程环境**"，而不是"单宿主 dlopen"。用例：`abiFilters=["arm64-v8a"]` 编译；**若要给手机用则需换非 NCP 方案（见 §7 P2 形态约束）**。

---

### 4.4 PyTorch / aiohttp 作为「Python 模块」打包进 HAP（方案可行性）

**用户的明确形态要求**：`torch` 与 `aiohttp` **不是**独立的原生库或 C++ core，而是**可作为 Python 模块 `import torch` / `import aiohttp`**，随 `libpython.so` 一起打进鸿蒙 HAP。这意味着 4.2 里的 `BUILD_PYTHON=OFF`（libtorch C++ core）只解决了「C++ 层」——**真正的 ComfyUI 后端需要的是 `torch` 的 Python 绑定**（`torch/_C.so`、`torchvision/_C.so` 等，带 CPython ABI 标记）。这点是 4.2 与 4.4 合起来判断的关键。

#### 运行形态（HAP 内）
```
libpython3.13.so              # → entry/libs/${abi}/
   ↑ dlopen（NCP 子进程 libcomfyui_child.so）
stdlib 库（encodings 等 *.py）  # → rawfile/（或打包进 .so 的 frozen 模块）
site-packages/
   ├── torch/  torch/_C.so（CPython ABI 绑定）  torchvision/  torchaudio/
   ├── aiohttp/（含 C 扩展 aiohttp/_http_parser.so, multidict, yarl）
   ├── numpy/  pillow/  safetensors/  ...
   └── comfyui 后端包（ComfyUI/ 源码 + main.py）
```
（`.so` 进 `entry/libs/`、`.py`/数据进 `rawfile/`，是 wineohos / qemuohos 已验证的 HAP 布局，非重新发明。）

#### ComfyUI 依赖全景（subagent B · ComfyUI v0.34.0）

ComfyUI 后端 = **一个 aiohttp + asyncio 进程**（非 uvicorn/ASGI），默认 `127.0.0.1:8188`，前端由 `web.static('/', web_root)` 托管 pip 包 `comfyui-frontend-package/static`。完整依赖按"原生扩展性质"分层（移植难度据此分档）：

| 依赖 | 性质 | 移植难度 / 备注 |
|---|---|---|
| **torch / torchvision / torchaudio / torchsde** | C++（含 CUDA），核心 | **必须换成 arm64 CPU（或 NPU）版**；硬依赖（P0 见 4.4） |
| transformers / einops / alembic / filelock / requests / simpleeval / comfyui-* 包 | 纯 Python | **直接随 site-packages 打包**，最低难度 |
| numpy / scipy / Pillow | C/C++ | 需 ohos wheel，多数已有 ohos 移植轮 |
| **safetensors / tokenizers / blake3 / pydantic-core** | **Rust 扩展** | **定论（subagent D）：有条件可行**。用 **`aarch64-unknown-linux-musl` target + OHOS SDK clang 作 linker**（`--sysroot`+`-D__MUSL__`）+ `cargo build --target` 直出 `.so`；`blake3`/`safetensors`（去numpy feature）低难度、`pydantic-core` 中、**`tokenizers` 最高**（onig C 依赖） |
| **aiohttp / yarl / multidict / aiosignal** | Cython/C 扩展 | **有纯 Python fallback**（`AIOHTTP_NO_EXTENSIONS=1`）可先退，性能稍降 → 可先不编 C 扩展 |
| av（PyAV/FFmpeg 绑定） | C | 仅音频/视频节点用；FFmpeg 需 ohos 编，**重**，可首期裁掉 |
| sentencepiece | C++ | 中（transformers 依赖） |
| SQLAlchemy(Cython) / pyyaml / psutil | C/Cython | 中 |
| **comfy-kitchen（fp8/fp4 ops）/ comfy-aimdo（DynamicVRAM）** | **CUDA 专属** | **必须剔除或条件导入**；尤其 `main.py:68 import comfy_aimdo.control` 是**无条件顶层导入**，无 CUDA 会崩，需改源码为 try/except 或移出必装清单 |
| xformers / flash-attn / triton | CUDA 专属 | CPU 模式**自动禁用**（`model_management.py:1689-1704`）/ 非必需，无需手动砍 |

**运行参数口径（必须）**：纯 CPU / aarch64 **必须显式加 `--cpu`**（否则默认 CUDA 路径直接 `Torch not compiled with CUDA`）；建议 `--cpu --port 8188 --enable-manager --front-end-root <本地前端>`。

**CPU-only 性能（subagent B 估算，注明不确定）**：CPU 通常比现代独立 GPU 慢 **~20–100 倍**。SD1.5 512² 桌面级 CPU ≈1–5 分钟/图（GPU 秒级）；**SDXL/FLUX 级 ≈10 分钟–数十分钟/图，接近不可交互**。⇒ **CPU/aarch64 可运行、可出图，但只适合离线/低分辨率/少量出图**；交互式生成必须 GPU（或 NPU，NPU 链未通）。

#### 可行性判断（分层）

| 层 | 判断 | 依据 / 风险 |
|---|---|---|
| **CPython 本体 → libpython.so** | ✅ 可行 | Termony 已用 musl 交叉编译 CPython 3.13 并在 OHOS 跑通；改为 `--enable-shared` 产 `.so`，由 NCP 宿主 dlopen（4.1） |
| **Python 的"可重定位 venv"语义** | ⚠️ 需专门处理 | Comfy-Desktop 的 `.so` 是**编译期 `--prefix`** 定位 `sys.prefix`/site-packages；嵌入后不能用 venv 相对路径，须经 `PYTHONHOME`/`PYTHONPATH` **运行时注入**（参考 wineohos `wine_env.cpp` 的 env 注入 + `entryParams __env=` 机制）。这是 4.1/4.4 的前提工程 |
| **torch → `import torch`（CPython ABI 绑定）** | ✅ 定论（有条件可行） | `torch._C.so` 须按目标 CPython(3.12/3.13) ABI 标记编译，并与 libpython **统一到同一 OHOS libc++ 命名空间（`__n1`）+ 同链 `libc++_shared.so`**；已有 OHOS 板 `import torch` 先例。详见 §4.2 命门② |
| **aiohttp → `import aiohttp`** | ⚠️ 中等 | aiohttp 主体纯 Python，但其 C 扩展（`_http_parser`）+ 依赖 `multidict`/`yarl`（也带 C 扩展）需为 ohos 编 wheel；有纯 Python fallback（`AIOHTTP_NO_EXTENSIONS=1`）可先退，性能稍降 |
| **其它 C 扩展依赖**（numpy/scipy/PIL/safetensors/…） | ⚠️ 中 | 大量 C/C++ 扩展 wheel 需逐一为 ohos 编译；多数有纯 Python 替代或已有 ohos 移植轮 |
| **打包进 HAP + 分发** | ⚠️ 体积瓶颈 | venv（CPython+stdlib+torch+numpy+…）裸大小数百 MB 至 ~1GB，超过 HAP 体积上限；**模型（数十 GB）绝不能进 HAP**，须首启下载（与 Comfy-Desktop 的 comfybuilder artifact 模型下载一致）。HAP 只装运行时，模型走用户目录 |

#### 结论（待 subagent 定论处已标注）
- **方案整体成立**，且"作为 Python 模块"比"libtorch C++ core"**更贴近 ComfyUI 实际用法**（ComfyUI 靠 `import torch` 而非 C++ API），因此 4.4 是**正确路线**。
- 落点风险集中于：**① torch Python 绑定与 musl python 的 ABI/libc 共存（P0）；② 可重定位 prefix 的运行时 env 注入（必做工程）；③ 体积/分发（模型外置）。**
- **兜底路线**（若 ① 长期无解）：见 §8 备选——用 Termony/qemu-vroot 的**整机 Linux 用户态 + 原生 CPython 可执行** 方案（HNP 包）跑 ComfyUI，牺牲"NCP 独立 so 进程"的整洁性，换取已证明可用的 glibc 生态。（注：该兜底与用户强调的"NCP 独立 so"形态不一致，仅作不放弃时的下策。）

### 4.5 Rust 扩展交叉编译到 OHOS（subagent D 定论）⭐

**定论：有条件可行**，且"条件"明确可满足。依据三层：① Rust 官方把 `aarch64-unknown-linux-ohos` 列为 **Tier 2（含主机工具）**，`rustup target add` 直接装；② **Termony 已用 `--host aarch64-unknown-linux-musl` 交叉编 CPython 3.13.5 并在 OHOS 跑通**（本地 `Makefile` 证据），证明 OHOS 属 musl 系 libc + clang/llvm 交叉工具链这条路走得通；③ maturin ≥1.6 原生支持 `--target` 交叉编译 + `--zig`。

#### 推荐做法（关键决策）
**target 用 `aarch64-unknown-linux-musl`，而非官方 `-ohos`** —— 因为 `target_env="ohos"` 会让下游 crates（`getrandom`/`cc`/`ring`）的 `cfg(target_env=...)` **不认识 "ohos" 而断**；而 `target_env="musl"` 对 crates 友好（正常命中）。但 **linker 用 OHOS SDK clang**（`--target aarch64-linux-musl --sysroot=<ohos-sysroot> -D__MUSL__`）拿真实 OHOS musl 头文件/库。即"**Termony 同款目标三元组 + 官方 SDK 链接器**"。

`~/.cargo/config.toml`（含 C 依赖的 crate 还需设 `CC_/CXX_/AR_<triple>` 指向 OHOS clang）：
```toml
[target.aarch64-unknown-linux-musl]
linker    = "<ohos-sdk>/native/llvm/bin/aarch64-unknown-linux-ohos-clang"  # 用作 linker
ar        = "<ohos-sdk>/native/llvm/bin/llvm-ar"
rustflags = ["-L", "<termony-sysroot>/lib", "-l", "python3.13",
             "-C", "link-arg=--sysroot=<ohos-sysroot>", "-C", "link-arg=-D__MUSL__"]
```
> OHOS SDK 自带 `aarch64-unknown-linux-ohos-clang`/`clang++` wrapper（Termony `Makefrag` 已确认），Rust 的 linker 直接指向它即可，无需再写封装脚本。**不推荐**裸 `ld`（无法自动处理 `--sysroot`/`-D__MUSL__`）；`zig cc` 是备选但缺 OHOS 专属符号、风险更高。

#### PyO3/maturin 关键卡点 = "目标解释器识别"
cross 时 maturin/PyO3 需要**目标 Python 的 sysconfig** 来算 ABI tag 和链接 libpython。设：
- `PYO3_CROSS_LIB_DIR=<termony-sysroot>/lib`（放大目标 `libpython3.13` + 头文件）
- `PYO3_CROSS_PYTHON_VERSION=3.13`（或 `PYO3_CONFIG_FILE` 自定 `abi_tag`/`ext_suffix`/`libs_dir`）

**落地推荐**：用**纯 `cargo build --target aarch64-unknown-linux-musl --release`** 直出 `.so`（绕过 maturin 的 wheel 平台 tag 生成，因 `-ohos`/`-musl` 不是 maturin 内置 wheel 平台），产物手动改名。

#### ABI tag 对齐
- 目标 CPython（Termony musl 3.13）的 `EXT_SUFFIX` 应为 `.cpython-313-aarch64-linux-musl.so`（bpo-43112）。
- **但未必非要匹配**：`importlib.machinery.EXTENSION_SUFFIXES` 恒含 **`.so`** 兜底后缀，所以把产物改名为纯 `safetensors.so`/`tokenizers.so` 也能被 import。建议要么用 `PYO3_CROSS_*` 生成正确 `ext_suffix`，要么 cargo 直出后改名为 `<mod>.<EXT_SUFFIX>`。
- **严禁**用 `-linux-gnu` tag（那是 glibc，ABI 与 musl 不兼容，装了必崩）。

#### 四个包难度评估

| 包 | 依赖特征 | OHOS 先例 | 难度 |
|---|---|---|---|
| **blake3** | 纯 Rust，无 C/系统依赖 | 无，但纯 Rust+musl 基本无坑 | **低** |
| **safetensors** | 纯 Rust + serde；可选 numpy(PyO3) feature | 无；去 numpy feature 即可 | **低~中** |
| **pydantic-core** | Rust + speedate/idna/strum，无 onig/ring | 无 OHOS wheel；getrandom/musl 是主痛点（meta-openembedded 有补丁） | **中** |
| **tokenizers** | 依赖 **onig（Oniguruma，C）+ onig_sys（捆绑 C）**，构建量最大 | 无；`onig_sys` 捆绑旧 C 在高版本 GCC 会编挂（Gentoo bug 944852） | **高（最大头）** |

#### 主要 crates 坑
- **`getrandom`**（最需警惕）：OHOS 的 `target_env="ohos"` 不在其 musl/空分支内，`/dev/urandom` 后端可能断。规避：① 用 `-musl` target 使 `target_env="musl"`；② 沿用 meta-openembedded musl patch；③ 依赖 C 链接加 `-D__MUSL__`。
- **`cc` crate**：任何含 C（onig_sys/ring/openssl-sys）的 crate 都要设 `CC_/CXX_/AR_<triple>` 指 OHOS clang + `--sysroot`。
- **`ring`/`openssl`**：这 4 包主链路基本不用 ring；openssl 需官方 `ohos-openssl` 且仅 reqwest/https 场景才要，先不带。
- **`-fPIC`**：cdylib 默认 PIC；个别 target 不 PIC 时加 `RUSTFLAGS="-C relocation-model=pic"`；libpython 本身要 `--enable-shared`（Termony 已满足）。
- **tokenizers**：设 `RUSTONIG_SYSTEM_LIBONIG=1`（用 OHOS 版系统 libonig）或 `--no-default-features --features=fancy-regex` 绕开 onig。

#### 风险优先级（Rust 子项）：① tokenizers（onig+构建重）＞② getrandom 的 cfg 断点 ＞③ `.so` ABI tag 对齐 ＞④ ring/openssl 场景。

---

## 5. 前端与 UI 承载（ArkWeb）⭐
**平台侧已定论（subagent C，经 skill-harmonyos-docs 核实）**

- ComfyUI 前端是 **Vue 3 + Vite 构建的 SPA**，由后端 aiohttp 托管，launcher 不内嵌 → 鸿蒙用 **ArkWeb（WebView）加载 `http://127.0.0.1:8188`** 即可复用，**无需用 ArkUI 重写**，极大省一档工作量。
- **加载方式**：`Web({ src, controller })`，**`src` 不能用 `@State` 动态改**，后端起来后必须用 `controller.loadUrl('http://127.0.0.1:8188')` —— 恰合"后端起来再用 loadUrl"的场景。
- **前提权限**：声明 `ohos.permission.INTERNET` 才能加载网络页面；`module.json5` 的 `deviceTypes` 需显式含 `"tablet"` 与 `"2in1"`（否则部分能力不生效）。
- **loopback 明文 http**：`127.0.0.1` 属网络地址而非本地文件（文件才需 `.fileAccess(true)`）；技能文档**未见对明文 `http://`/loopback 的禁止条目**。⚠️ **唯一未尽项**：真机对 `http://127.0.0.1` 明文加载的实测行为未在文档给出明确放行条目，**建议目标真机/模拟器实测确认**（模拟器回环语义注意，参考 `10.0.2.2` 网关）。
- **同设备命名空间**：NCP 后端子进程与 WebView 同处一设备命名空间，`http://127.0.0.1:8188` 直接互通，无需外部网络。
- 需要重写的只是"壳"本身：向导、启动/停止、模型管理、设置、账号——用 ArkTS/ArkUI 组件（`List`/`Navigation`/`Tabs`/弹窗等）。
- **渲染兜底（若不走 ArkWeb 需原生绘）**：XComponent(SURFACE)+EGL 官方支持直接上屏；因 NCP 子进程**不能建 XComponent**，须留主进程建好，经 `OH_NativeWindow_WriteToParcel`/surfaceId 传给子进程再 EGL 渲染（qemuohos 已验证）。

---

## 6. 参考项目复用方法汇总

| 参考 | 复用什么 | 对应到本移植 |
|---|---|---|
| **Termony** | Python **musl 交叉编译**（`--host ...-linux-musl` + `--enable-shared` + `ANDROID_API_LEVEL=1` + build-native for cross-compile + `llvm-strip`） | 4.1 的 CPython→so |
| **Termony/qemu-vroot** | 在鸿蒙里起 Linux 环境（proot/qemu user-mode） | 可作为**兜底**（见 §8 备选路线） |
| **wineohos** | NCP `.so` 子进程模式、entryParams/env 注入、`OH_Ability_KillChildProcess`/退出回调、`ncp_dispatch`、wayland/EGL 上屏 | 4.3 进程架构 |
| **qemuohos** | 跨进程窗口 `OH_NativeWindow_WriteToParcel`、unix-socket IPC、`libqemu_child` run-once 隔离、NCP 生命周期 | 4.3 进程架构 + 渲染（如需） |

---

## 7. 难点与风险（诚实排序）

**P0 · 单点命门（已定论：有条件可行，但有 3 处硬性门槛）**
> PyTorch 不再是"可能否决项"，而是"**有条件可行**"——`import torch` 已有 OHOS 板上先例（thirdparty_pytorch）。真正的命门是三处，缺一即崩：
1. **torch + torchvision + torchaudio 原生交叉编译**（工程量最大）：无 OHOS wheel，须 `aarch64-linux-ohos` LLVM 工具链 + musl sysroot + chroot/SDK **全源码**交叉编译，挂一串 musl/OHOS patch（FFmpeg、libjpeg/libpng、libomp）。
2. **libc++ 双命名空间统一（运行时生死线）**：libpython.so 与 torch._C.so **必须同为应用侧 `__n1`、同链同一 `libc++_shared.so`、同 clang 大版本**，否则进程内两份 libc++ 运行时 → `undefined symbol` / `std::terminate` / double-free，**加载期即崩**。
3. **显式链 libpython + SOABI 对齐**（最易被忽略）：`-lpython3.12` 或 `patchelf --add-needed libpython3.12.so`；torch 交叉构建须显式设 `Python_SOABI`（PR#189388）；OHOS 的 SOABI 后缀（`-gnu` 实为 musl）**不可信，须实测**。
> 附加硬约束：**拷桌面预编译 libtorch 必段错误**；musl 缺 glibc 符号须源码补丁；用 `libomp.so` 勿链 GNU libgomp。

**P1 · 高影响**
- **性能天花板（已量化）**：无 GPU/NPU（NPU 未通），**CPU 比独立 GPU 慢 ~20–100×**；SD1.5 ≈1–5 分钟/图，**SDXL/FLUX ≈10 分钟–数十分钟/图，接近不可交互** ⇒ 只能离线/低分辨率/少量出图。这是产品定位的硬约束，需在报告中明确向用户交底。
- **CUDA 专属包必须剔除**：`comfy-kitchen`（fp8/fp4 ops）、`comfy-aimdo`（DynamicVRAM）为 CUDA 专属，且 `main.py` 对 `comfy_aimdo.control` 是**无条件顶层 import**，无 CUDA 会崩 → 需改源码为 try/except 或移出清单。
- **Rust 扩展移植点（已定论，P1 中偏易）**：可用 **`aarch64-unknown-linux-musl` target + OHOS SDK clang linker** 交叉编（`target_env="musl"` 规避 `getrandom`/`cc` 在 `"ohos"` 的 cfg 断点）。难度：blake3/低、safetensors/低-中、pydantic-core/中、**tokenizers/高**（onig C + 构建重）——见 §4.5。
- **custom_nodes 生态**：大量第三方节点含 C/C++ 扩展且依赖 CUDA toolchain，aarch64/no-CUDA 只能在「纯 Python + torch CPU 算子」子集内选型，或做源码降级。

**P2 · 中影响**
- **形态约束**（已定论）：NCP **仅 PC/2in1 + 平板（API14+）**，**手机不支持**（`NCP_ERR_NOT_SUPPORTED`）。方案天然限定 PC/Pad，需在 `deviceTypes` 声明 `tablet`/`2in1`。
- ArkWeb 加载本地 http：**已定论可行**（须 `INTERNET` 权限 + `loadUrl`），仅剩"真机明文 http 实测"一项待确认。
- 依赖体积（venv + torch + models 可能 >5GB），HAP 分发策略（模型需首启下载，参考 Comfy-Desktop 的 comfybuilder artifact 下载模型）。

---

## 8. 分阶段实施路线（草案）

**Phase 0 · 地基验证（无它不动工）**
- 复现 Termony 的 Python→so：`OHOS_ARCH=aarch64` 交叉编出 `libpython3.13.so`，写 `libcomfyui_child.so`（NCP 子进程）`Main()` dlopen + `Py_Initialize` 跑一个最小 Python 脚本 → 真机打印 hello。
- 验证 NCP：`OH_Ability_StartNativeChildProcess` 拉起最小 `.so`、退出回调、unix-socket/HTTP 端口可监听。
- **Stage-gate：若 libpython 在 OHOS 无法由 NCP 子进程 dlopen 后正常初始化，则换 §8 备选路线。**

**Phase 1 · Python 后端最小可跑**
- full venv artifact（CPython so + stdlib + pip/site-packages）在鸿蒙编译、打包、部署；`Main()` 里嵌入跑 `ComfyUI/main.py --cpu`，确认 aiohttp 起、`http://127.0.0.1:<port>` 可访问、前端 ArkWeb 能加载。

**Phase 2 · PyTorch 接入**
- libtorch CPU-only 编译为 `.so`，`import torch`、基础 matmul 通过（对照 build_in_harmonyos smoke test）；接 torchvision/枕依赖。
- **Stage-gate：torch._C.so 能否在 musl python 进程里 import 成功（ABI 验证通过），否则调 libc 方案或改造 python 的 libc 对齐。**

**Phase 3 · 横向打通 ComfyUI**
- 模型目录、ComfyUI-Manager v4、模型下载、custom_nodes CPU 兼容子集。

**Phase 4 · 壳 UI 与体验**
- ArkTS 壳（向导/启动/模型/设置）+ ArkWeb 整合 + 进度/HAP 分发 + 签名上架。

---

## 9. 调研状态跟踪
> **全部 4 路 subagent（A/B/C/D）调研已完成，本报告已定稿。** 4 站结论：平台侧(C)可行、ComfyUI 依赖(B)可行、PyTorch(A)有条件可行、Rust(D)有条件可行。

- [x] 5 鸿蒙 NCP/端口/ArkWeb 平台侧定论（subagent C，已完成）
  → 主链路可行：NCP + `isolationMode=NORMAL` 绑 `127.0.0.1:8188` + ArkWeb `loadUrl`；仅 PC/Pad（API14+）；唯一未尽项="真机明文 http 实测"。
- [x] 4.2/4.4 PyTorch & libc-ABI 定论（subagent A，已完成）
  → **有条件可行**：`thirdparty_pytorch` 是完整 `import torch` 模板；命门=①torch 系列全源码交叉编 ②libc++ 双命名空间(`__h`/`__n1`)须同链 `libc++_shared.so` ③显式 `-lpython`+SOABI 实测；NPU 无 libtorch 直连(须 RKNN/MindSpore Lite)；体积 libtorch_cpu.a≈392MB / torch wheel≈80-96MB。
- [x] 4.1/4.4 ComfyUI 依赖全景 + CPU-only 可行性（subagent B，已完成）
  → aiohttp+asyncio（非 uvicorn）、必加 `--cpu`、CUDA 专属包(comfy-kitchen/aimdo)需剔除、Rust 扩展(safetensors/tokenizers/blake3)是新增移植点、CPU 性能~20-100×慢。
- [x] **Rust 扩展交叉编译到 OHOS**（subagent D，已完成）
  → 有条件可行：`aarch64-unknown-linux-musl` target + OHOS SDK clang linker + `cargo build --target`（绕过 maturin wheel tag）+ `PYO3_CROSS_LIB_DIR` 对齐 libpython；blake3/低、safetensors/低-中、pydantic-core/中、tokenizers/高（onig）；getrandom 的 `cfg(target_env)` 是主要坑。
