# ohos-torch：OHOS torch 交叉构建正式化

2026-09-05 定稿。产链与验证见 `docs/local-inference-torch-parallel.md`(§5/§6)。

## 组成

| 文件 | 角色 |
|---|---|
| `build_torch_ohos.sh` | pytorch 2.10.0 交叉构建(G0-G2 全踩坑固化,patch 01-12 自动应用;产物 → `build/torch-ohos-install`) |
| `build_openblas_ohos.sh` | OpenBLAS 0.3.29 交叉构建终版(G4 全教训: 顺序构建/全套工具链/全清/grep 非-q/OpenMP=1/Fortran-LAPACK/gfortran stub/穷举校验) |
| `patches/` | 01-12 torch 移植源 + 13(mbind no-op) 14(Makefile.system override 交叉目标) |

## 关键参数(勿改,改动需先读证据)

- OpenBLAS: `USE_OPENMP=1 NUM_THREADS=12`(pthread 版会在 torch libomp 并行区内降级,见 §6.3)
- torch 静态吸取 `externals/openblas/lib/libopenblas.a`;改库后重链: `ninja libtorch_cpu.so`
- 从零复现: 仅需 SDK=`/apps/harmony` + `aarch64-linux-gnu-gfortran`

## 复位

```bash
# OpenBLAS(全量重建约 25 分钟, 校验含全库 x86=0/符号/mbind=0)
git clone --no-hardlinks externals/openblas build/openblas-rebuild
bash thirdparty/ohos-torch/build_openblas_ohos.sh build/openblas-rebuild
```
