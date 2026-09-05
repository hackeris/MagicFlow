# 梦幻之流(MagicFlow)—— ComfyUI on HarmonyOS

把 ComfyUI 后端以 NCP 子进程形式内嵌为 HarmonyOS 应用,在 aarch64 真机(PC/Pad)上本地运行。
当前形态:单入口首页「启动 梦幻之流」→ 4 步启动进度页 → ComfyUI 画布(本地 CPU 推理,SD-Turbo 256×256)。

## 1. App 形态

```
ArkTS 父进程                NCP 子进程                          ArkWeb
Index.ets(单入口)  ──OH_Ability_StartNativeChildProcess──▶  comfy_child.cpp   ◀── http://127.0.0.1:8188
 点「启动」→ 拉起            ├─ 内嵌 CPython 3.12(python312.zip)     (同源,前端由后端
 ├─ 4 步进度页(真实信号)     ├─ torch 2.10 + ComfyUI 0.34(patched)    --front-end-root 服务)
 └─ 就绪 → ArkWeb 加载       └─ 后端 comfyui/main.py (HTTP :8188)      (官方面板/模板/下载库)
```

- 后端只在用户点「启动」后运行;退出 App 即停止(零常驻)。
- 一切 `.so` 只经 HAP `libs/<abi>/` 经 meta_path Finder(`dlopen` + `PyInit_*`)加载(禁 `patchelf`/绝对路径)。
- 模型已可下载到系统可见目录 `Download/app.hackeris.hium/models/`(前端模型库一键下载),也可手动放入。

## 2. 复现链(空机器 → 真机验证)

环境要求:Linux x86_64;CLT 6.1.1.280(`/apps/harmony`);rust 工具链(+`aarch64-unknown-linux-ohos`);
网络(仅 fetch 一步);aarch64 真机(`scripts/env.sh` 的 `HDC_TARGET`,默认 `192.168.1.8:33363`)。

```bash
make fetch      # ① 外部输入 → externals/(sha256 全校验,幂等)
make extract    # ② skh-run.tar.gz → build/skh-run-extract/(唯一数据源契约)
make stage      # ③ ComfyUI 源码(+patch 16/17) + 纯 py 依赖 + 自建前端 dist → build/pyroot-stage/
make rust       # ④ thirdparty/ submodule 源码 → build/rust-out/(两个 Rust 扩展,sha 比对)
make zip        # ⑤ stage+skh stdlib → rawfile/python312.zip(28,080 条,manifest 锚)
make prebuilt   # ⑥ 清单驱动收集 libs/ → entry/src/main/cpp/prebuilt/(324 文件,sha/NEEDED 闭环)
make hap        # ⑦ hvigorw assembleHap → entry-default-signed.hap
make install    # ⑧ 部署真机(bm install + aa start)
make verify     # ⑨ 一键黑盒验收: 装机→门户驱导→后端→BLAS/MM4x→出图(全量 8 项,~5min)
                #    `make verify SMOKE_ARGS="--fast"` = 只验后端+BLAS/MM4x
```

每步失败即停(无"占位/降级继续"的假成功);外部资源锚见 `config/externals.pins.tsv`,
预置库清单见 `config/prebuilt_manifest.tsv`,zip 锚见 `docs/manifests/python312.zip.manifest.gz`。

## 3. 目录

| 目录 | 内容 |
|---|---|
| `config/` | 外部资源 pin 表 / vendor wheel / prebuilt 清单 |
| `externals/` | fetch 产物(gitignored): skh-run.tar.gz、comfyui-src、py-site、前端源码树 |
| `thirdparty/` | submodule: 前端 fork(hackeris/ComfyUI_frontend,ohos 分支)、tokenizers、safetensors、ohos-torch |
| `stub/` | zip 内注入模板(sitecustomize_tpl / stub_global / psutil)— 模型层缺依赖空壳预置 |
| `scripts/` | 复现链脚本 + verify_smoke.sh(黑盒判据) |
| `patches/` | ComfyUI 源码 OHOS 修改(03 主 patch + 16 下载端点 + 17 轻量模板) |
| `entry/src/main/` | cpp/ NCP 子进程宿主、ets/ ArkTS 前端、resources/ 图标与字符串 |
| `build/` | 中间产物(全部 gitignored) |

## 4. 工具链基线

CLT 6.1.1.280 / hvigor 6.24.2(官方原版)/ SDK 6.1.1 (API24) / hdc 3.2.0d /
rustc 1.98.0 + `aarch64-unknown-linux-ohos` / node 25(`scripts/build_frontend.sh` 自建前端 dist)。

签名材料:真机拒绝 unsigned HAP;`.ohos/` 四件套为 debug 签名(bundleName `app.hackeris.hium`)。
过期时报 `signature` 错误 → DevEco Studio 自动签名再生(保持 bundleName 一致)。

## 5. 已知缺口

- **设备上限 = 256 级**(F32 模型峰值触平台限制,512 必 SIG9;fp16 模型已定谳试过,512 仍触顶);
- 启动(安装→后端就绪)1~2 分钟属正常(28k 条目解压 + torch 全量装载为架构固有成本);
- 惰性缺失(启动仅打 warning,不影响主链):`alembic`(远程同步)、`blake3`、`pydantic_settings`
  (pyproject 解析降级)、upscale/canny 类节点缺 `spandrel`/`kornia`/`comfy_angle` → 对应节点降级不可用;
- 视频/av、部分 torchvision/scipy 算子未支持(惰性 import 点)。

## 6. 约定

- 唯一权威仓库 = 本目录(git + 无大二进制,全链可复现);变更/验证一律在本目录进行。
- 详细设计见 `docs/`: DESIGN.md(研究史+pin)、BUILD.md(复现链)、smoke-design.md、
  model-download.md、workspace-design.md、frontend-fork-plan.md、local-inference-torch-parallel.md。
