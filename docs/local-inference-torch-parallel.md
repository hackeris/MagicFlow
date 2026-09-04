# 本地推理 R1: torch 并行不生效调查(证据集)

**日期**: 2026-09-04  **状态**: 定谳(证据全闭环)  **结论**: 不是"torch 不支持并行"——这个 OHOS 构建把 BLAS 选成了 **Eigen(单线程)**,torch 的
`set_num_threads` 从不作用于 Eigen-GEMM;修复 = 重编换 `USE_BLAS=OpenBLAS`(skh 发行树自带 `libopenblas.a+头`)。

---

## 1. 现象(设备实测,可复现)
- `torch.get_num_threads() = 1`(初始);`oset_num_threads(12)`/`omp_set_num_threads(12)` 后 `get_num_threads() = 12` ✔ 配置层可调
- **op 层不变**: `matmul(2048×2048)×4 = 8.979s`(≈3.8 GF/s = 单核水平;n=12 时同样 8.98s)
- 全流程 CPU 采样: 加载期/KSampler 期均 ~0.9-1.2 核(41 线程存在但 op 不并行)

## 2. 证据链

### 2.1 构建参数(设备 `torch.__config__.show()`,thr-probe.log)
```
CXX_FLAGS= ... -DUSE_PTHREADPOOL -DUSE_PYTORCH_QNNPACK -DUSE_XNNPACK
           -DAT_BUILD_ARM_VEC256_WITH_SLEEF ... 
USE_OPENMP=ON, USE_MKL=OFF, USE_MKLDNN=OFF, USE_EIGEN_FOR_BLAS=ON
CXX_COMPILER=/usr/bin/aarch64-linux-ohos-clang++   COMMIT_SHA=449b176
```

### 2.2 官方源码语义(克隆 pytorch v2.10.0,tag 校验)
| 文件 | 内容 | 含义 |
|---|---|---|
| `cmake/Dependencies.cmake:178-179` | `if(BLAS STREQUAL "Eigen") → set(CAFFE2_USE_EIGEN_FOR_BLAS ON)` | BLAS=Eigen(显式)或 `MKL 缺失默认退 Eigen`(同文件 190-222) |
| `aten/src/ATen/ParallelOpenMP.cpp:28-49` | `init_num_threads` 读 `intraop_default_num_threads()`;`get_num_threads=omp_get_max_threads()` | OMP 后端正常,理论 n=12 |
| `aten/src/ATen/ParallelCommon.cpp:78-96` | `intraop_default_num_threads` 读 `OMP_NUM_THREADS`/`MKL_NUM_THREADS`,否则默认 | 与实测 env=12 自洽 |
| `aten/src/ATen/PTThreadPool.cpp`, `Parallel.cpp`, `c10/util/ThreadPool.cpp` | **无 `setNbThreads`/`initParallel`/Eigen 字样** | `torch.set_num_threads` 从不配 Eigen 线程数 |

### 2.3 运行时 libomp 健康(空口不说)
- `readelf -d libtorch_cpu.so` NEEDED 含 `libomp.so`; `libomp.so` SONAME=`libomp.so`(匹配); 1.23MB 全符号(`omp_get_max_threads`/`omp_set_num_thredeadth` 等)
- 探针 `ctypes.CDLL("libomp.so")`: 初始 `max=1 num=1 procs=8`;`omp_set_num_threads(6)` 后 `max=6` → 库本身可调、无 stub
- `.so` strings: 只有 `"ATen parallel backend: "` + `"OpenMP"` 分支(无 `"native thread pool"`)→ 编译态后端=OpenMP

### 2.4 早期快照时序(为何 init=1)
- `OMP-DIRECT max=1`(首测)且 env=12——libomp 首次初始化发生于**早期 env 单化(历史 run37 段,与本轮改动互见)时刻**;env 后改无效
- **与 op 单线程无关**:op 层瓶颈=Eigen-GEMM(2.2);init 快照只解释"配置默认 1"

### 2.5 修复弹药存在
```
研究区/正规区 skh 树: skh-run/usr/lib/libopenblas.a + skh-run/usr/include/openblas/
```
→ 重编 torch 时 `USE_BLAS=OpenBLAS` 即可(其余 cmake 配置不变,skh SDK 链现成)。

## 3. 结论与修复选项
- 修复 A'(推荐,预期好): 复刻 skh build.sh,改 `-DUSE_BLAS=OpenBLAS -DBLAS=OpenBLAS` 重编 torch → 12 核 GEMM,512×4 预期 3-6 分钟,再实测复核。
- 修复 C(当前可用): 单线程下轻模型(<1GB)5-20s/图可体验。
- 其他候选(未探): mobile-式 threadpool(USE_PTHREADPOOL)与 `THREADPOOL` backends 的切换,如 A' 不达预期再评估。

## 4. 复现材料
- 设备探针: `thr-probe.log` = `/data/app/el2/100/base/app.hackeris.hium/haps/entry/files/pyroot/thr-probe.log`(n/interop/affinity/env/OMP-DIRECT/MM4x/CONFIG-FULL)
- 探针代码: `scripts/make_py312_zip.py` STAGE 段 `comfyui/comfy/utils.py` 注入(run102-104)
- 源码: `/tmp/ptsrc`(pytorch v2.10.0 sparse); thirdparty_pytorch `/tmp/tpp`(build.sh + 7 patches)
