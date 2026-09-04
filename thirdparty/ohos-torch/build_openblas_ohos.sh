#!/usr/bin/env bash
# build_openblas_ohos.sh —— OpenBLAS 0.3.29 交叉构建(OHOS aarch64, G4 定谳终版)。
# 证据链(2026-09-05, 真机实测, 全部踩过坑):
#   1) mbind(235)/set_mempolicy(238) 被 OHOS seccomp 以 SIGSYS 绝杀 —— dlopen 时 init 段
#      gotoblas_init→blas_thread_init→blas_memory_alloc→my_mbind 即死(cppcrash 实锤栈)。
#      → 补丁 13(common_linux.h, __OHOS__ 分支 no-op)。
#   2) torch 静态吸取 openblas 时**必须**给全 LAPACK(F77 符号: cgeev_/dgesv_… 60 个),
#      否则 libtorch_cpu.so BIND_NOW 在真机 musl 下 dlopen=EINVAL(实锤 errno=22)。
#      → netlib(Fortran LAPACK)必须编; NO_LAPACK=1 / 仅 C 级 lapack(5.7MB)产物必死。
#   3) OpenBLAS 的 prebuild/getarch 每次 make 都用【宿主】探测重写 Makefile.conf
#      (x86_64/GCC/COOPERLAKE) → FFLAGS 带 x86 flags(march=cooperlake 等)且 f2c 路径被
#      选择(C_LAPACK=1) → 混入宿主 x86 对象。→ 补丁 14(Makefile.system include 后
#      override ARCH=arm64/CORE=ARMV8/C_COMPILER=CLANG/F_COMPILER=GFORTRAN/NO_EXPRECISION=1)。
#   4) 教训(每条都是真机/实测踩坑):
#      a. make clean 不清 Makefile.conf; 增量 ar -u 会把第一次的错误对象(如宿主编的
#         lapacke_*.o)永久留在库中 → 本脚本一律【清空全部(.o/库/conf)+ 一次性编译】。
#      b. `make -j libs netlib` 并行 = ar 竞态: 两 target 同时对主 .a 追加, 撕裂 ar 库
#         (库中对象变全零 data) → **顺序**: 先 libs 后 netlib(各一次 -j)。
#      c. 必须给全套编译器: 漏传 RANLIB=llvm-ranlib(宿主无 aarch64-ohos-ranlib)或漏传
#         AR/FC → netlib 的 LAPACKE 回退宿主 cc/C 路径(x86 对象+系统 ar 混入)。
#      d. make.inc 生成时把 LAPACKE 的 CC 写成 `cc` → 编译后立即 sed 修正为 SDK clang。
#      e. 主库 libopenblas_armv8p-r0.3.29.a 与 lib/libopenblas.a(torch 引用路径)必须同步。
#      f. gfortran 交叉编译仅产纯 .o(无 glibc/libgfortran NEEDED), 但个别 LAPACK 例程的
#         错误路径引用 _gfortran_concat_string → 用独立 stub .c 编译进库(见
#         gfortran_concat_stub.c, G4 定谳: 缺它 = dlopen 最后 1 个未定义符号)。
# 依赖: 交叉 gfortran aarch64-linux-gnu-gfortran(apt install gfortran-aarch64-linux-gnu)。
# 产物: libopenblas_armv8p-r0.3.29.a(BLAS+LAPACK 完整, ≈23MB, 全 aarch64, 0 x86)。
# 用法: bash thirdparty/ohos-torch/build_openblas_ohos.sh <openblas-src-dir>
set -euo pipefail
SRC="$(cd "${1:?用法: $0 <openblas-src-dir>}" && pwd)"
TOOL_DIR="$(cd "$(dirname "$0")" && pwd)"
LLVM=/apps/harmony/sdk/default/openharmony/native/llvm/bin
PATCH_DIR="$TOOL_DIR/patches"
# gfortran 运行时 stub 源码随本仓库(thirdparty/ohos-torch/stubs/); 编入目标库
STUB="$TOOL_DIR/stubs/gfortran_concat_stub.c"

[ -d "$SRC" ] || { echo "FATAL: $SRC 缺失"; exit 1; }
command -v aarch64-linux-gnu-gfortran >/dev/null || { echo "FATAL: 缺 aarch64-linux-gnu-gfortran"; exit 1; }
[ -f "$STUB" ] || { echo "FATAL: 缺 gfortran stub: $STUB"; exit 1; }

cd "$SRC"
# ── 幂等打补丁(已应用则 skip, 状态异常则报错) ──
for p in 13-openblas-ohos-numa-noop.patch 14-openblas-cross-conf-override.patch; do
  if ! git -C "$SRC" apply --check "$PATCH_DIR/$p" 2>/dev/null; then
    if git -C "$SRC" apply --reverse --check "$PATCH_DIR/$p" 2>/dev/null; then
      echo "  [PATCH] already applied: $p"
    else
      echo "FATAL: 补丁 $p 无法应用(源码状态异常)"; exit 1
    fi
  else
    git -C "$SRC" apply "$PATCH_DIR/$p"
    echo "  [PATCH] applied: $p"
  fi
done

# ── 全量清空(教训 a: 无增量路径) ──
rm -f libopenblas_*.a lib/libopenblas.a Makefile.conf config.h Makefile.conf_last config_last.h \
      Makefile_kernel.conf config_kernel.h
find . -name "*.o" -delete 2>/dev/null
rm -f build_openblas.log

echo ">> OpenBLAS 交叉构建(ARMV8 + OpenMP + Fortran-LAPACK + mbind no-op) 开始"

# makefile override 在 patch14 已注入 Makefile.system(include Makefile.conf 后),
# 但 getarch 仍会写 conf —— override 保证 ARCH/CORE/C_COMPILER/F_COMPILER 最终生效。
# 教训 c: 全套工具链显式传参; 教训 b: 顺序构建(libs 完成后 netlib)。
# ⚠ G4 定谳(2026-09-05): 必须 USE_OPENMP=1 而非 pthread!
#   真机 512×4 实测: torch 侧 libomp 的并行区(attention/conv parallel_for)内部调用
#   OpenBLAS 时, pthread 版 OpenBLAS 检测 "OpenMP Loop" 后【拒绝并行/降级单线程】
#   (diag.log 反复 "OpenBLAS Warning: Detect OpenMP Loop and this application may hang"),
#   推理期 BLAS 单线程 ⇒ 全程慢 10× 以上(512×4 十五分钟无产出)。USE_OPENMP=1 让
#   OpenBLAS 与 torch 共享同一 libomp 运行时(符号 GOMP_* 由 HAP libs/libomp.so 提供,
#   已验证该库含 GOMP_parallel 等全套)。
make -j${MAX_JOBS:-16} \
  TARGET=ARMV8 HOSTCC=gcc BINARY=64 USE_THREAD=1 USE_OPENMP=1 NO_AFFINITY=1 NO_WARMUP=1 NUM_THREADS=12 \
  CC="$LLVM/aarch64-unknown-linux-ohos-clang" \
  CXX="$LLVM/aarch64-unknown-linux-ohos-clang++" \
  AR="$LLVM/llvm-ar" RANLIB="$LLVM/llvm-ranlib" NM="$LLVM/llvm-nm" \
  FC=aarch64-linux-gnu-gfortran F77=aarch64-linux-gnu-gfortran \
  libs 2>&1 | tee "$SRC/build_openblas.log"
echo ">> [1/3] BLAS 库完成, 开始 netlib(LAPACK, 顺序)"
make -j${MAX_JOBS:-16} \
  TARGET=ARMV8 HOSTCC=gcc BINARY=64 USE_THREAD=1 USE_OPENMP=1 NO_AFFINITY=1 NO_WARMUP=1 NUM_THREADS=12 \
  CC="$LLVM/aarch64-unknown-linux-ohos-clang" \
  CXX="$LLVM/aarch64-unknown-linux-ohos-clang++" \
  AR="$LLVM/llvm-ar" RANLIB="$LLVM/llvm-ranlib" NM="$LLVM/llvm-nm" \
  FC=aarch64-linux-gnu-gfortran F77=aarch64-linux-gnu-gfortran \
  netlib 2>&1 | tee -a "$SRC/build_openblas.log"

# ── git-clean 之后 make.inc 的 CC 可能是宿主 cc(教训 d): 检查并修正 ──
if [ -f "$SRC/Makefile.inc" ] && grep -q 'CC.*= *cc' "$SRC/Makefile.inc"; then
  echo "  [WARN] make.inc CC 为宿主 cc, 修正为 SDK clang"
  sed -i "s|^CC *=.*|CC = $LLVM/aarch64-unknown-linux-ohos-clang|" "$SRC/Makefile.inc"
fi

A="$SRC/libopenblas_armv8p-r0.3.29.a"
[ -s "$A" ] || { echo "FATAL: 主库缺失 $A"; exit 1; }

# ── gfortran stub 编入(教训 f) ──
STUB_OBJ="$SRC/gfortran_concat_stub.o"
"$LLVM/aarch64-unknown-linux-ohos-clang" -c -O2 -fPIC "$STUB" -o "$STUB_OBJ"
ar t "$A" | grep -qx gfortran_concat_stub.o || "$LLVM/llvm-ar" q "$A" "$STUB_OBJ"

# ── 同步 lib/libopenblas.a(torch 静态链路径, 教训 e) ──
mkdir -p "$SRC/lib" && cp -f "$A" "$SRC/lib/libopenblas.a"

# ══ 穷举校验(教训补强: 不止抽样) ══
echo ">> [2/3] 校验(穷举) ..."
echo "-- 对象架构扫描(全量) --"
X86=0; CNT=0
for o in $(ar t "$A"); do
  CNT=$((CNT+1))
  if ar p "$A" "$o" | file - | grep -q "x86-64"; then
    echo "  [X86-LEAK] $o"; X86=$((X86+1))
  fi
done
echo "  对象总数=$CNT, x86 泄漏=$X86"
[ "$X86" -eq 0 ] || { echo "FATAL: 仍有 x86 残留"; exit 1; }

echo "-- 关键符号集 --"
# ⚠ 教训: 不能用 grep -q 进管道! -q 命中即关 stdin → 上游 nm 收 SIGPIPE(141)
#   → set -o pipefail 下管道必判失败(真机级 bug: 好库被误杀)。全部用非 -q。
nm "$A" 2>/dev/null | grep -w dgemm_ && echo "  [OK] BLAS dgemm_" || { echo "FATAL: dgemm_ 缺失"; exit 1; }
# LAPACK 必备全族(G4 EINVAL 的 60 符号列表按前缀抽查关键 10 个)
for s in cgeev_ cgels_ cgelsd_ cgesdd_ cgesv_ dgesv_ dgesvd_ sgetrf_ zgesdd_ zgetrf_; do
  nm "$A" 2>/dev/null | grep -w "$s" || { echo "FATAL: LAPACK $s 缺失"; exit 1; }
done
echo "  [OK] LAPACK F77 全族(10 抽查)"
nm "$A" 2>/dev/null | grep -w _gfortran_concat_string || { echo "FATAL: gfortran stub 符号缺失"; exit 1; }
nm "$A" 2>/dev/null | grep -w gotoblas_init || { echo "FATAL: gotoblas_init 缺失"; exit 1; }

echo "-- syscall 扫描(mbind 必须为 0) --"
MBIND=$(nm "$A" 2>/dev/null | grep -c "SYS_mbind")
[ "$MBIND" -eq 0 ] || { echo "FATAL: 仍有 SYS_mbind 引用($MBIND); 补丁 13 未生效"; exit 1; }
echo "  [OK] mbind 引用=0"

echo ">> [3/3] DONE: $A ($(ls -la "$A" | awk '{print $5}') bytes, 全 aarch64, 无 x86/无 mbind)"
