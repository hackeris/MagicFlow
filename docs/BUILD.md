# 构建文档(BUILD.md)

> 2026-09-05 定稿。本文档描述的链路已做**全链从零复现验证**(2026-09-05):清空目录 → `git worktree` → 按本文步骤 → 产物装机出图字节级一致。
> 权威驱动:`thirdparty/ohos-torch/run_repro_chain.sh`;日常入口:`make hap`。

## 0. 产物一览(构建什么)

| 产物 | 位置 | 大小(实测) |
|---|---|---|
| 签名 HAP(最终) | `entry/build/default/outputs/default/entry-default-signed.hap` | ≈480MB |
| torch(OpenBLAS 版)安装树 | `build/torch-ohos-install/` | libtorch_cpu.so ≈200MB |
| OpenBLAS 交叉库 | `externals/openblas-src/lib/libopenblas.a` | 23.2MB |
| Rust 扩展 | `build/rust-out/tokenizers.abi3.so`、`_safetensors_rust.abi3.so` | 秒级 |
| python312.zip | `entry/src/main/resources/rawfile/python312.zip` | 211,691,431B(锚) |
| prebuilt 库集 | `entry/src/main/cpp/prebuilt/`(325 文件闭环) | collect 断言 |

## 1. 前置依赖(机器态,一次准备)

| 依赖 | 要求 | 自动处置 |
|---|---|---|
| OHOS NDK **SDK** | `/apps/harmony/sdk/default/openharmony/native/llvm/bin/aarch64-unknown-linux-ohos-clang++` 存在 | bootstrap 检查,FATAL 提示 |
| 交叉 gfortran | `aarch64-linux-gnu-gfortran` | bootstrap 缺失时 `apt install gfortran-aarch64-linux-gnu` |
| rust 工具链 | rustc + `aarch64-unknown-linux-ohos` target | 缺失时提示 `curl https://sh.rustup.rs \| sh`;target 自动 `rustup target add` |
| host Python 3.12 | `/opt/py312/bin/python3.12` | 缺失时从 pin 的 `Python-3.12.7.tgz` 自动 configure 编译(需 host 开发工具) |
| 签名机态 | `.ohos/`(密钥)+ `local.properties` | 非正主处:`bootstrap --source-dir <宿主区>` 桥接拷贝 |
| 宿主基础工具 | gcc/make/git/curl/unzip、python3 | 必须预装(build_torch 0.5/0.6 段用 host cc 自编 mkdisp/protoc) |
| 网络 | GitHub/GitCode(LFS)/python.org/PyPI | fetch 时完整校验 sha/size |

自产免装:`/opt/sleef-native`(5 个宿主工具)、`/opt/protobuf-host`(同源 3.13 protoc)——均由 build_torch_ohos.sh 分段自编落位;`/opt/py312`(bootstrap 代编)。

## 2. 从零全链(推荐 ≤ 10 分钟内启动)

```bash
# 0) 取得项目(带 submodule 条目; 已是当前仓库可跳)
git clone <repo> && cd <repo>
git submodule update --init --recursive     # Rust 扩展源码(huggingface 固定 commit)

# 1) 机器态预检/桥接(正主原地跑可省略 --source-dir)
bash scripts/bootstrap_worktree.sh [--source-dir /data/share/comfyui]

# 2) 全链驱动
bash thirdparty/ohos-torch/run_repro_chain.sh
```

分步等价(驱动即以下序列,每步日志 `build/repro/*.log`):

```bash
make fetch                                            # pins 全源(~0.5-1h, pytorch 浅克隆+37 子模块是大头)
make rust                                             # Rust 扩展(依赖 extract: skh 树解包)
bash thirdparty/ohos-torch/build_openblas_ohos.sh externals/openblas-src   # ≈25min
bash thirdparty/ohos-torch/build_torch_ohos.sh --clean                     # ≈30min(j16, 5903 目标)
make zip && make prebuilt && make hap                 # 锚/闭环/签名 HAP
```

## 3. 日常开发迭代

- 只改 `entry/src/main/cpp/`、`ets/`、`config/`、`scripts/`:`make hap` 即可(全增量,秒级~分钟级)。
- 改了 torch 侧或 openblas:
  - 库更新:`cp` 产物 → 更新 `config/prebuilt_manifest.tsv` 对应行 sha → `make hap`;
  - 重编:重跑 4 行序列;**改完必须让 `collect_prebuilt.sh` 325 文件闭环 PASS**,否则 hap 装不上(锚断言)。
- **探针注意**:COMFTEST-BLAS/MM/MM4x 在 `#ifndef RELEASE_BUILD` 段,Release 构建自动裁剪;`OPENBLAS_NUM_THREADS=12`+`OPENBLAS_CORETYPE=ARMV8` 是生产配置(置于 import 前)。

## 4. 各环节速查

| 环节 | 脚本 | 输入(pin/externals) | 校验 |
|---|---|---|---|
| fetch | `scripts/fetch_externals.sh` | `config/externals.pins.tsv`(skh-run LFS/前端 dist/comfyui-src 03468f4/zip 锚/pydantic whl/pytorch v2.10.0@449b176/openblas v0.3.29@8795fc7/py312 tar) | sha256+size+锚串(证据串 `_dynamo_disable` 等) |
| rust | `scripts/build_rust_exts.sh` | `thirdparty/{tokenizers,safetensors}` submodule | 产物 sha 与 prebuilt 锚比对 |
| openblas | `thirdparty/ohos-torch/build_openblas_ohos.sh` | `externals/openblas-src` | 全库 x86=0/mbind=0/符号族/gfortran stub |
| torch | `thirdparty/ohos-torch/build_torch_ohos.sh` | pytorch-src+patches 01-12+host py312+protobuf/mkdisp | `_C.so` ARM aarch64 |
| zip | `scripts/make_py312_zip.py` | stage+skh stdlib | 三锚(条目数/sorted_namelist sha/size) |
| prebuilt | `scripts/collect_prebuilt.sh` | manifest ts v | 325 文件闭环+NEEDED 闭包+死文件断言 |
| hap | Makefile→hvigor | 上述全部+`.ohos` 签名 | SignHap+BUILD SUCCESSFUL |

## 5. 已知坑(& 已修复决策,勿回退)

| 症状 | 根因 | 处置(已固化) |
|---|---|---|
| `ninja: error: '/bin/mkdisp' missing` | patch 08/11 假 done(旧循环吞错+无条件 touch,cmake.py 白名单缺失,NATIVE_BUILD_DIR 未透传) | 循环三分支幂等+失败 FATAL;08/11 合并为 `08-cmake-env-whitelist.patch` |
| openblas 校验段静默死 | `grep -c` 无匹配 rc=1 触发 set -e | `\| true`(要的语义=计数 0) |
| dlopen=EINVAL(22) | 缺 60 LAPACK F77 符号 | 完整 Fortran-LAPACK(gfortran 纯 .o) |
| dlopen=SIGSYS@mbind | OHOS seccomp | 补丁 13 no-op |
| 最后一未定义 | `_gfortran_concat_string` | `stubs/gfortran_concat_stub.c` 编入库 |
| 512 级 OOM(SIG9) | 模型 F32 5.2GB,常驻爆顶 | 机型上限见 §7;非构建问题 |
| 后台"exit 0"假象 | 尾管 `tail` 掩盖真退出码 | 全量日志文件+DRIVER-EXIT 显式输出 |

## 6. 验证判据(装机)

```bash
hdc -t <ip> install -r entry/build/default/outputs/default/entry-default-signed.hap
hdc -t <ip> shell aa start -a EntryAbility -b app.hackeris.hium
# 等 ~100s, 看 diag.log(沙箱):
#   /data/app/el2/100/base/app.hackeris.hium/haps/entry/files/pyroot/diag.log
```
- ✅ `COMFTEST-BLAS` 含 `BLAS_INFO=open`
- ✅ `COMFTEST-MM4x t=0.6x nthreads=12`(**判据 < 2.0s**)
- ✅ `CF-OK-8188 status=200`;256×256/SD-Turbo/steps=2 出图 ≈88s(字节 103,411B,seed=42)

## 7. 设备能力上限(重要决策记录)

MatePad 11.5 S / 11.8GB RAM:F32 模型常驻 5.2G,推理峰值超 5.1G 即内核 SIG9。**本机可跑上限: 256×256 级**;512 需模型 fp16 化(转换工具流式版 `fp32_to_fp16.py`)或换大内内存机型——判据"512×4 ≤6min"在本机**放弃**,见 `docs/local-inference-torch-parallel.md` §6-7。

## 8. 回退

- HAP/torch 版本:git 历史 + `build/openblas-backup-pthread/`(pthread 版 OpenBLAS 旧库参考)。
- 历史双区镜像已废弃(研究区 2026-09-05 清理; 其更早归档 = `/data/share/comfy-ohos-port.old`)。
