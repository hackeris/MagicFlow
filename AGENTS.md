# AGENTS.md — 协作说明（给人和 AI agent 的快速指引）

> 项目全貌、架构、逐目录解释、复现链详见 `README.md`；设计与研究史（Phase 0 各步实测记录）见
> `docs/DESIGN.md`。**旧的「QEMU TCG 模拟器 PoC」文案已废弃** —— 本工程不是 QEMU 模拟器。

## 1. 项目身份

ComfyUI on HarmonyOS（arm64 真机）—— 一个 HarmonyOS 应用，复用**官方 ComfyUI 源码仓库**
（@03468f4，经 `patches/` 修改）与 **OHOS aarch64 PyTorch 运行时栈**（skh-run, LFS 锚定）。
程序本体 = ArkTS 前端 + NCP 原生子进程（嵌入式 CPython 3.12 解释器宿主的 ComfyUI 后端）。

## 2. 常用命令（仓库根）

```bash
make fetch          # 拉取+校验全部外部输入（幂等; --offline 支持本地副本）
make stage zip      # 重建 stage / rawfile python312.zip（三锚校验）
make rust prebuilt  # 重建两个 Rust 扩展（submodule 源码）/ 收集 prebuilt 二进制（清单驱动）
make hap            # 构建 signed HAP（libs/arm64-v8a==236 元锚）
make install        # 部署真机（hdc 目标见 scripts/env.sh 的 HDC_TARGET）
make verify-device  # 真机一轮观测（run_and_capture.sh 采集→部署→信号轮询→摘要）
make log clean clean-all
```

## 3. 铁律（违反即返工）

1. **一切被加载的 `.so` 必须打包进 HAP `libs/<abi>/`** —— filesDir/绝对路径 dlopen 均不可行；
2. **禁止 patchelf**（含对 .so 的任何 SONAME/DT_NEEDED 修补）；无 SONAME 扩展的
   链接与收集机制已内建 `entry/src/main/cpp/CMakeLists.txt`（IMPORTED 绝对路径 + `--as-needed`,
   见其机制注释块）；
3. **仅 arm64**（`abiFilters` 已去 x86_64）；真机 aarch64 必须签名 HAP（`.ohos/`, 不入库）；
4. **stub 的 `__spec__` 必须是真形 `ModuleSpec`**；树系 stub 名单带包名前缀；
   新探针关键词先加 `run_and_capture.sh` 白名单再部署；
5. git commit 不带 Co-Authored-By 尾行。

## 4. 版本与外部依赖

- 外部资源锚（sha256/commit/大小）**唯一真源** = `config/externals.pins.tsv`；
- 纯 py 依赖 pin = `scripts/venv-requirements-port.txt`（自 hostcv venv 的 dist-info 导出,勿手改；
  psutil/regex/typing_extensions 例外见文件头注释）；
- 构建产物（prebuilt 394M、python312.zip 152M、skh 544M、前端 24M、签名材料）**全部不入 git**,
  由脚本重建并锚校验；
- 工具链 = `/apps/harmony`（CLT 6.1.1.280）;**hvigor 6.24.2 官方原版（2026-09-03 已还原插件,
  勿再改 /apps/harmony 内文件）**。

## 5. 单区纪律（2026-09-05 起）

- `/data/share/comfyui` = 唯一权威代码库（**改这里**）；
- 早前研究区 `/data/share/comfy-ohos-port` 已于 2026-09-05 清理（git 提交即备份,
  更早一份归档 = `/data/share/comfy-ohos-port.old`）；不存在第二副本。

## 6. 快速定位

- 主链：`entry/src/main/cpp/child/comfy_child.cpp` 的 `Main`（PyConfig / meta_path finder / run_path）；
- zip 组装与三锚：`scripts/make_py312_zip.py`；stage 组装与校验：`scripts/make_comfyui_stage.py`；
- 模型层 stub 白名单：`stub/stub_global.py` 的 `STUB_SECT`（两份 stub 模板同步改）；
- 库收集机制：`CMakeLists.txt::import_ext_so` 注释块（2026-09-03 定谳）。
