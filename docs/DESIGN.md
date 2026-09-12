# Phase 0 设计 — ComfyUI on HarmonyOS 最小验证

> ⚠️ **本文是 Phase 0 的设计记录（2026-09-01/02），各 Step 均已完成**，保留作为决策过程。
> 已知取代点：① Step 0.2 实际走的不是 Python 3.13，而是切 **3.12.7**；② 文中复现链锚数字
> （条目数/字节数）为**时点值**，现行权威在 `config/externals.pins.tsv`。
> **当前状态见 `README.md` 与 `docs/status-and-next.md`。**

参考组织：`../wineohos`（Makefile 多目标 + `scripts/env.sh` + `entry/cpp|ets` 分层 + `docs/`）。
参考运行模型：`../qemuohos`（父 ArkUI 进程经 NCP 拉起 `.so` 子进程）。

## 目标
在鸿蒙 PC/Pad（arm64）上跑通"ComfyUI 后端子进程"的最小 NCP 链路，一次实测多个不确定点。

## 分步
- **Step 0.1（本工程已实现）**：`libcomfy_child.so` 作为 NCP 子进程，`Main(NativeChildProcess_Args)` 内验证
  ① 可被拉起  ② fork/exec 再派生子进程（ComfyUI 多进程 fan-out/DataLoader 前提）
  ③ 绑定 `127.0.0.1` 端口（给 ArkWeb 访问前提）  ④ 真实 CPU 基线
- **Step 0.2（待做）**：`Main` 里 dlopen `libpython3.13.so` + `Py_Initialize` 跑 ComfyUI，实测 libc++/SOABI 共存。

## 关键实现
- 父：`entry/src/main/cpp/napi_init.cpp` 暴露 `startComfyChild(entryParams)` → `OH_Ability_StartNativeChildProcess("libcomfy_child.so:Main", args, {NORMAL}, &pid)`（`NORMAL` 共享网络）。
- 子：`entry/src/main/cpp/child/comfy_child.cpp` 入口 `extern "C" void Main(NativeChildProcess_Args args)`。
- 工具链：OHOS SDK 6.1.1(API24)，`aarch64-linux-ohos` LLVM。设备 `hdc -s 192.168.1.8:33363`。

## 调研结论（subagent E/F，2026-09-01）
- **HAP 打包**（E）：NAPI 桥/NCP 子进程 `.so` 走 `libs/<abi>/`；但 **CPython venv 的 `libpython.so` + torch/site-packages `.so` 不能进只读 bundle**，须放**可写 `filesDir`** 由嵌入式 CPython dlopen/sys.path 加载。模型（数十 GB）不能进 HAP，须走"资源包后台下载/云存储"落到 `filesDir`；应用只稳定写自身沙箱，`cacheDir` 会被清、沙箱外无自由大文件区。
- **前端 ArkWeb**（F）：ComfyUI 前端（Vue3 SPA）可**直接加载、零改动**（ArkWeb=Chromium M132，支持 ES modules/Canvas2D/WebSocket/fetch/localStorage/WebGL/WASM）；唯一硬堵点 file/resource 跨域拦截——坚持 `http://`（跑后端）即绕开；留意"坚盾守护模式禁 WebGL/ServiceWorker（3D 预览）"与"`http://127.0.0.1` 明文真机实测"。

## Step 0.1 实测结果（2026-09-01 · aarch64 真机 192.168.1.8:33363）
全部 4 项验证通过，日志：
```
ComfyNapi  : startComfyChild entryParams=phase0|127.0.0.1|8188|--cpu
ComfyNapi  : OH_Ability_StartNativeChildProcess rc=0 pid=41251   ← NCP 拉起 OK
ComfyChild : Main ENTER pid=41251
ComfyChild : CPU baseline ~20M adds = 114266 us (aarch64)
ComfyChild : fork/exec check child pid=41274                      ← 可再派生子进程
ComfyChild : socket check bind=0 connect=0 accept=11 recv=14 'comfy-child-ok'  ← 绑 127.0.0.1 通
ComfyChild : Main EXIT
```
工程产物：2.7MB signed HAP；`libcomfy_child.so` 导出 `Main`；`libentry.so` 导出 `RegisterEntryModule`/`startComfyChild`。
要点：aarch64 设备**要求签名**（unsigned 被拒 `no signature file`），用 signed.hap；bundleName 复用 qemu 的 `app.hackeris.hium`（debug 签名）。*（历史时点：bundleName 已于 2026-09-07 改为 `app.fuqidian.magicflow`。）*
fork/exec read=0：`/system/bin/sh -c uname` 未回写输出（疑似 sh 不可用），但 fork+exec 成功（拿回 child pid）——已证明"能再派生"；后续换 toybox 确认输出。

## Step 0.2 实测结果（2026-09-01 · aarch64 真机）
`libpython3.13.so.1.0`（OHOS/musl 交叉编，SONAME 保持 CPython 原生 `.1.0`，未做任何 hack）经 CMake imported 打进 HAP `libs/arm64-v8a/`，NCP 子进程 `dlopen` 成功：
```
ComfyChild: dlopen libpython OK: libpython3.13.so.1.0        ← 按"so 必须打包进 HAP libs/<abi>/"约束，从包内加载
ComfyChild: Py_GetVersion(SOABI): 3.13.5 (main, Sep  1 2026) [Clang 15.0.4]  ← SOABI 正确、符号可解析
ComfyChild: Py_IsInitialized before init = 0
```
**结论（P0 命门②）**：dlopen + SOABI **验证通过**。libpython 仅 `NEEDED libc.so`（纯 C，无 C++），因此**不触发** `__h/__n1` libc++ 双命名空间——那是 torch._C.so 这类 C++ 扩展才涉及；本步证明 SOABI（cpython-313 / musl / clang15）正确 + 库可加载，是 torch 扩展可运行的**前置**条件。
`Py_Initialize()` 已于 2026-09-02 经 **stdlib 注入**（HAP rawfile→`filesDir/pyroot/lib/python3.13/`，`PYTHONHOME` 设该 pyroot）**完整成功**：`Py_IsInitialized after init=1`、`import os`/`import site` 成功、`PyRun_SimpleString rc=0`。**根因纪录**：曾现 `Fatal Python error: memory allocation failed`，非 OOM（`mem_probe` 证 malloc 到 128MB 成功），真凶是 **deprecated `Py_SetProgramName` 与 `Py_Initialize` 混用**（3.13 与内部 PyConfig 状态冲突）——移除后成功；嵌入式应改用 `PyConfig+Py_InitializeFromConfig`，禁混旧 API。完整 ComfyUI 需再装入 site-packages + torch。

**要点修正（用户实机结论，务必遵守）**：⚠️ 一切被加载的 `.so` 必须打包进 HAP `libs/<abi>/`（filesDir、`/data/local/tmp`、任意绝对路径 dlopen 均**不可行**），这否定了 subagent E"venv 的 .so 可放 filesDir"的旧定论。构建**仅 arm64**（`entry/build-profile.json5` 的 `abiFilters` 已去掉 `x86_64`）。

## Step 0.3a 实测结果（2026-09-02 · aarch64 真机）——P0 命门③ 机制全通
OHOS `aarch64-unknown-linux-ohos-clang++` 编最小 **C++** CPython 扩展 `comfymini`（必用 std::string），**显式 `-lc++_shared`（非 -lc++）链应用 libc++ → 默认 ABI 命名空间 `__n1`**。产物 `scripts/build_comfymini.sh`。
真机四环：① loader 短名 dlopen 从 libs/ 命中 C++ 扩展；② comfy_child NEEDED `libc++_shared(__n1)`+comfymini 仍正常启动（Py_Initialize=1）；③ **宿主注册桥 `register_libs_ext(fullname, soname)`**（dlopen 短名+PyInit_<tail>+塞 sys.modules），Python `import comfymini` **rc=0 真正可用**；④ 扩展内 `std::string`（走 `__n1`）执行无异常。**结论**：`import torch` 的接入机制（loader+__n1+宿主注册）全部打通，纯 .py 仍走 filesDir/site-packages 标准 import。
**教训**：imported SHARED 若**不设 SONAME** → DT_NEEDED 写成**构建机绝对路径**，loader 加载 comfy_child 按绝对路径找（真机不存在）而启动崩；**必须 `-Wl,-soname,<libs basename>`** 才能像 libpython 一样按短名从 libs/ 解析。

**torch 可得性（2026-09-02 已实测确认，字节证据）**：无官方 aarch64 musl wheel；**先例** [`openharmony-robot/thirdparty_pytorch`](https://gitcode.com/openharmony-robot/thirdparty_pytorch) 提供**现成 OpenHarmony aarch64 PyTorch 运行时**：`test/skh-run.tar.gz`（LFS `d6d6bfcfea`，**544835856 字节**，`gzip -t` 通过）= Python **3.12.7** + PyTorch **2.10.0** + TorchVision/Audio/SciPy/Pillow/SafeTensors 等，工具链 `aarch64-linux-ohos`，已确认 **CPU 推理在 BQ3588HM 板子 `import torch` 跑通**。子仓库 `build_in_harmonyos` 另有 libtorch C++(`BUILD_PYTHON=OFF`) CPU 构建。

**命门③命名空间实测（2026-09-02 真实 torch `_C`/`libtorch_cpu` 字节证据，推翻上述「`__h`/`__n1` 严禁混用」表述）**：OHOS 实际存在**两套互斥 libc++**，且**真实 OHOS torch 栈用的是 `__1` 不是 `__n1`**：
| 库 | 位置 | SONAME | 命名空间 |
|---|---|---|---|
| 应用侧 libc++ | SDK `native/llvm/lib/<triple>/libc++_shared.so` | `libc++_shared.so` | **`std::__n1`**（comfymini 链的正是它） |
| 系统 libc++ | skh-run `usr/lib/libc++.so.1`→`.so.1.0` | `libc++.so.1` | **`std::__1`**（标准，**真实 torch 栈链它**） |

- **实测 NEEDED**：`torch/_C.cpython-312-...so`(73KB)=`libtorch_python.so,libc.so`；`libtorch_python.so`=`libtorch.so,libtorch_cpu.so,libc10.so,libc++.so.1,libc++abi.so.1`；`libtorch_cpu.so`(205MB)=`libomp.so,libc10.so,libc++.so.1,libc++abi.so.1`；`torchvision/_C.so`=`libtorch.so,libtorch_cpu.so,libtorch_python.so,libc++.so.1,libc++abi.so.1`。符号命名空间全为 **`std::__1`**（mangled `St3__1`）。
- **含义**：复用一个编好的 OHOS torch 栈，**必须同步带上 skh-run 的 `libc++.so.1`+`libc++abi.so.1`+`libunwind.so.1`+`libomp.so`**（走 `__1`），**绝不能混入 `libc++_shared.so`（`__n1`）**——否则两个 libc++ 同进程共存致符号/类型冲突。`libpython3.12.so.1.0`(22MB) NEEDED 仅 `libintl.so.8,libc.so`（纯 C 不链 libc++），与任一套都兼容。

**版本夹角（待用户决策）**：skh-run 栈 SOABI=`cpython-312-aarch64-linux-ohos`（Python 3.12.7），与已打通的 **3.13** 不匹配。两条路：**A. 整体切 3.12**（复用 skh-run 的 `libpython3.12.so.1.0`+stdlib `usr/lib/python3.12`+site-packages+其 `libc++.so.1` 全套，pyroot 从 3.13 换成 3.12；命门③已被该栈解决）——**推荐**，风险最低；**B. 3.13 自编 torch**——成本极高。aiohttp 需确认 skh-run 是否带（PyPI `cp312-musllinux_aarch64` 是 musl ABI，与 OHOS 不通用，缺则需 OHOS 移植/自编）。

---

## 当前状态（2026-09-03）与复现链

已走 **路线 A（整体切 3.12）**：主链真机达成 `CF-OK-8188`（`/system_stats` 200，
comfyui_version 0.34.x）；ArkWeb 前端接通（官方 ComfyUI_frontend v1.54.1 dist）。aiohttp 悬案
已于 skh-run site-packages 实证覆盖。

**复现链**（从空机器 → 真机验证；详细命令与每步校验见 `README.md` §2 与 `Makefile`）：

```
fetch(外部输入 sha256 锚) → extract(skh 解包=唯一数据源契约) →
  stage(comfy 源码+patch+纯 py 依赖+官方前端, fail-fast) |
  zip(python312.zip 三锚:151,213,981B/28,080 条/sorted-namelist) |
  rust(submodule 源码编译两个 Rust 扩展, 产物 sha 与锚比对) |
  prebuilt(清单驱动 324 文件, sha/计数/NEEDED 闭包/死文件断言) →
  hap(hvigor 官方原版, libs/arm64-v8a == 236 元锚) → install/verify-device(真机 CF-OK-8188)
```

**hvigor 插件还原记录（2026-09-03 定谳）**：6.24.2 插件的 `generate-native-library.js` 曾被
本地修补（`findRuntimeFiles` 对 `-l:` prefix strip），2026-09-03 已删注入行还原为官方原版
（还原后 md5 = npm 官方 `705309b0d3e33e24f34c71391fa18d5d`）。构造上令新方案成立：无 SONAME 扩展
全部改走 **IMPORTED 绝对路径 + `-Wl,--as-needed`**（comfy_child 对其零符号引用 → NEEDED 被修剪,
绝不出现「构建机绝对路径」；hvigor 官方版对非 sysroot 绝对路径直接收集进 hap libs/）。
「-l: 短名 + 改插件」旧方案已退役。注意「禁止 patchelf」铁律不变——本方案从未触碰 .so。

## 版本与来源 pin（2026-09-03 基线，细节全文在 `config/externals.pins.tsv`）

| 资源 | 版本/锚 |
|---|---|
| skh-run.tar.gz | LFS `d6d6bfcfea…25406`（sha256 全值）, 544,835,856B；= Python 3.12.7 + PyTorch 2.10.0 + 环境自带 site-packages（**不含 aiohttp 生态**,由本链自补,见下一行） |
| ComfyUI_frontend | v1.54.1; dist.zip sha256 `a89cf5e8…248806`, 24,601,619B; index.html md5 `61a69562c29975642b296c553cd96d32` |
| ComfyUI 源码 | `03468f4`（workflow templates v0.11.52 #16024）+ `patches/comfyui-src-ohos-changes.patch`（551 行/349+/84-） |
| 纯 py 依赖 | `scripts/venv-requirements-port.txt`（pydantic 2.13.5/sqlalchemy 2.0.52/transformers 5.16.1 等；私有 comfy-kitchen 0.2.31/comfy-aimdo 0.4.15 = vendor wheel 入库）。其中 aiohttp 生态 9 盒（aiohttp 3.14.3/multidict 6.7.1/propcache 0.5.2/yarl 1.24.5/frozenlist 1.8.0/aiohappyeyeballs/aiosignal/attrs+attr/idna）为 **server.py 启动硬链**——skh 标准 tar 不含,需自 pypi 收（C 加速 .so 被滤,各包自动回退纯 py,与旧 zip run18 形态一致） |
| python312.zip | 151,213,981B / 28,080 条 / manifest 逐条目 sha256（`docs/manifests/`） |
| pydantic_core | 2.46.5 musllinux_1_1_aarch64 wheel sha `efd62a42…`, 2,158,408B（解包出 `_pydantic_core…musl.so` + `.libs/libgcc_s-0bf60adc.so.1`） |
| Rust 扩展 | tokenizers @7f1623b(v0.23.1)/safetensors @a406ca3（thirdparty/ submodule, cargo 交叉编译产物 sha 即锚；旧手工产物已弃） |
| 工具链基线 | CLT 6.1.1.280 / hvigor 6.24.2(官方原版) / SDK 6.1.1 API24 / hdc 3.2.0d / rustc 1.98.0（target `aarch64-unknown-linux-ohos` 已装） |

