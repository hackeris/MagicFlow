#!/usr/bin/env bash
# 路线 A —— OHOS torch 2.10.0 交叉重建(OpenBLAS 多线程),正规化构建脚本。
# 证据为什么重建: docs/local-inference-torch-parallel.md(BLAS=Eigen 单线程, 设备实测 MM 8.98s 单核)
# 构建链复刻: gitcode/openharmony-robot/thirdparty_pytorch 的 pytorch-2.10.0/build.sh(7 个 OHOS patch),
#            差异: (1) 不用 chroot/unshare/proot(用户令 + 容器无 CAP) → 纯交叉;
#                  (2) BLAS=Eigen(现. so 的默认) → OpenBLAS(静态 libopenblas.a, 零新增 NEEDED)。
# 工具链: /apps/harmony/sdk/default/openharmony/native/llvm(aarch64-unknown-linux-ohos-clang, musl)
# 宿主 py: /opt/py312(Python 3.12.7, SOABI=cpython-312 与设备解释器成套)
# 用法: bash thirdparty/ohos-torch/build_torch_ohos.sh [--clean]  (BUILD_KEEP=1 增量保留 build/)
# 产物: build/torch-ohos-install/usr/lib/python3.12/site-packages/torch/...
# 注意: SDK 的 libc++.so 是 linkerscript `INPUT(-lc++_shared)`(SONAME=libc++_shared.so, std::__n1);
#       G3 需打包 SDK 的 libc++_shared.so 进 HAP libs(与 skh 链的 libc++.so.1=__1 并存, 详见环境段注释)。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/externals/pytorch-src"                     # fetch_externals.sh 产物(全量 clone v2.10.0 + submodules); $ROOT 随 worktree 漂移(2026-09-05 修正 hardcode)
PATCH_DIR="$ROOT/thirdparty/ohos-torch/patches"
LLVM=/apps/harmony/sdk/default/openharmony/native/llvm/bin
NATIVE=/apps/harmony/sdk/default/openharmony/native
PY=/opt/py312/bin/python3.12
OUT="$ROOT/build/torch-ohos-install"
# 宿主编译的 protoc(与 third_party/protobuf 同版本 3.13.0.0), 见 0.6 段
HOST_PROTOC=/opt/protobuf-host/bin/protoc
# OpenBLAS: 官方 v0.3.29 交叉自建(HOSTCC=gcc TARGET=ARMV8 USE_THREAD=1; skh 树老 .a 在设备上
# MM 必崩(alloc_mmap→ld-musl SIGSEGV, G4 run 实锤) 且无 pthread=纯串行版本 → 弃用)
# 前置: bash thirdparty/ohos-torch/build_openblas_ohos.sh externals/openblas-src
#   (产物 externals/openblas-src/lib/libopenblas.a; 2026-09-05 修正 worktree 漂移 hardcode)
OB_OURS_SRC="$ROOT/externals/openblas-src"
OB_SYSROOT=/apps/harmony/sdk/default/openharmony/native/sysroot
# sleef 交叉分支: NATIVE_BUILD_DIR 为宿主机已编译的工具目录(CMakeLists 用 IMPORTED 加该路径下 mkdisp)
SLEEF_NATIVE=/opt/sleef-native
[ -x "$SLEEF_NATIVE/bin/mkdisp" ] || { echo "FATAL: 缺少宿主机侧 mkdisp(期望 $SLEEF_NATIVE/bin/mkdisp)"; exit 1; }

[ -d "$SRC/.git" ] || { echo "FATAL: 源码树缺失 $SRC (先 scripts/fetch_externals.sh)"; exit 1; }
[ -x "$LLVM/aarch64-unknown-linux-ohos-clang++" ] || { echo "FATAL: OHOS NDK 缺失(期望 /apps/harmony/sdk/default/openharmony/native/llvm)"; exit 1; }
[ -x "$PY" ] || { echo "FATAL: host py3.12 缺失(期望 /opt/py312/bin/python3.12)"; exit 1; }
[ -f "$OB_OURS_SRC/lib/libopenblas.a" ] || { echo "FATAL: libopenblas.a 缺失"; exit 1; }

# ── 0) patch 应用(幂等; -p0 锚定 'pytorch/' 目录名 → externals/ 下建同名 symlink) ──
EXT_DIR="$(dirname "$SRC")"
if [ ! -e "$EXT_DIR/pytorch" ]; then ln -s "$(basename "$SRC")" "$EXT_DIR/pytorch"; fi
cd "$EXT_DIR"
for p in "$PATCH_DIR"/*.patch; do
  [ -f "$EXT_DIR/pytorch/.patch-$(basename "$p").done" ] && continue
  patch -p0 -d "$EXT_DIR" --forward < "$p" -r - || true   # 已应用报 skip 视为完成
  touch "$EXT_DIR/pytorch/.patch-$(basename "$p").done"
  echo "  [PATCH] applied $(basename "$p")"
done

# ── 0.4) host py 依赖(numpy; CMake 的 Python check 缺 Development.Module/NumPy 时强制 BUILD_PYTHON=OFF
#          → 不编 libtorch_python.so → _C.so 无法链接; G2 实测复现; 仅构建期用, 不进产物) ──
if ! "$PY" -c "import numpy" 2>/dev/null; then
  "$PY" -m pip install numpy || { echo "FATAL: host py numpy 装失败"; exit 1; }
fi
echo "  [HOSTPY] $($PY -V) numpy=$($PY -c 'import numpy; print(numpy.__version__)')"

# ── 0.5) sleef 宿主工具(交叉分支 add_host_executable 为 IMPORTED 指向 NATIVE_BUILD_DIR/bin/*;
#       工具由宿主 cc 编译源码树 third_party/sleef/src/libm/, 幂等重编) ──
SLEEF_TOOLSRC="$SRC/third_party/sleef/src/libm"
mkdir -p "$SLEEF_NATIVE/bin"
for t in mkrename mkrename_gnuabi mkmasked_gnuabi mkdisp mkalias; do
  # 工具与架构无关(纯文本转换), 宿主编译器即可
  cc -O2 -o "$SLEEF_NATIVE/bin/$t" "$SLEEF_TOOLSRC/$t.c" -I"$SLEEF_TOOLSRC" || { echo "FATAL: 宿主编 $t 失败"; exit 1; }
done
echo "  [HOSTTOOL] sleef 宿主工具就绪: $SLEEF_NATIVE/bin/"
ls "$SLEEF_NATIVE/bin/"

# ── 0.6) 宿主 protoc(protobuf 3.13.0.0 同源; CAFFE2_CUSTOM_PROTOC_EXECUTABLE 用它做 onnx 生成;
#         幂等: 已就绪则跳过; 构建引 third_party/protobuf 源码, CMake 4 需 CMAKE_POLICY_VERSION_MINIMUM=3.5) ──
if [ ! -x "$HOST_PROTOC" ] || ! "$HOST_PROTOC" --version 2>/dev/null | grep -q "3.13.0"; then
  PB_SRC="$SRC/third_party/protobuf/cmake"
  PB_BUILD=/tmp/protobuf-host-build
  rm -rf "$PB_BUILD"
  cmake -S "$PB_SRC" -B "$PB_BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -Dprotobuf_BUILD_PROTOC_BINARIES=ON \
    -Dprotobuf_BUILD_TESTS=OFF \
    -Dprotobuf_BUILD_SHARED_LIBS=OFF \
    -Dprotobuf_WITH_ZLIB=OFF \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_INSTALL_PREFIX=/opt/protobuf-host \
    || { echo "FATAL: 宿主 protobuf configure 失败"; exit 1; }
  cmake --build "$PB_BUILD" --target protoc -j${MAX_JOBS:-16} \
    || { echo "FATAL: 宿主 protoc 编译失败"; exit 1; }
  mkdir -p /opt/protobuf-host/bin
  cp -f "$PB_BUILD/protoc" "$HOST_PROTOC"
fi
echo "  [HOSTPROTOC] $HOST_PROTOC = $("$HOST_PROTOC" --version)"

# ── 1) OpenBLAS 放置到 SDK sysroot(FindOpenBLAS 默认查找位; 幂等) ──
mkdir -p "$OB_SYSROOT/usr/lib" "$OB_SYSROOT/usr/include/openblas"
cp -f "$OB_OURS_SRC/lib/libopenblas.a" "$OB_SYSROOT/usr/lib/libopenblas.a"
cp -f "$OB_OURS_SRC/include/openblas/cblas.h" "$OB_SYSROOT/usr/include/openblas/cblas.h"
cp -f "$OB_OURS_SRC/include/openblas/cblas.h" "$OB_SYSROOT/usr/include/cblas.h" 2>/dev/null || true
cp -f "$OB_OURS_SRC/include/openblas/lapack.h" "$OB_SYSROOT/usr/include/openblas/lapack.h" 2>/dev/null || true

# ── 2) 交叉环境(与 skh build-utils.sh 同构, 但不用 chroot) ──
export CC="$LLVM/aarch64-unknown-linux-ohos-clang"
export CXX="$LLVM/aarch64-unknown-linux-ohos-clang++"
export LD="$LLVM/ld.lld"
export AR="$LLVM/llvm-ar"
export NM="$LLVM/llvm-nm"
export STRIP="$LLVM/llvm-strip"
export RANLIB="$LLVM/llvm-ranlib"
export READELF="$LLVM/llvm-readelf"
export OBJCOPY="$LLVM/llvm-objcopy"
export OBJDUMP="$LLVM/llvm-objdump"
export AS="$LLVM/llvm-as"
# CMake 交叉(Finder 会读这些全局变量)
export CMAKE_SYSTEM_NAME=Linux
export CMAKE_SYSTEM_PROCESSOR=aarch64
export Protobuf_PROTOC_EXECUTABLE=$HOST_PROTOC
export PROTOBUF_PROTOC_EXECUTABLE=$HOST_PROTOC
export ONNX_PROTOC_EXECUTABLE=$HOST_PROTOC
# skh 同款: 交叉时 protoc 必须宿主二进制(ProtoBuf.cmake 的官方交叉路径, 否则 third_party protoc
# 会被交叉编译成 aarch64 而在 onnx 生成步骤无法执行 — G2 实测复现)。
# 版本必须与 protobuf 子模块一致: 系统 protoc 3.21 生成头与 protobuf 3.13 头不兼容(cmake 实测错),
# 故宿主编译同源的 protobuf-3.13.0.0 protoc(见 0.6 段)。
export CAFFE2_CUSTOM_PROTOC_EXECUTABLE=$HOST_PROTOC
# skh 同版选项(仅 BLAS 一项换掉)
export USE_CUDA=OFF
# skh 同款: OFF 跳过 generate_linker_script(该脚本 ld -verbose 仅 GNU-ld 语义, lld 必炸)
export USE_PRIORITIZED_TEXT_FOR_LD=OFF
# WA: skh 用 USE_SYSTEM_SLEEF=ON(SDK 带系统 sleef);/apps/harmony sysroot 无 sleef → 用源码树自带(同功能)
export USE_SYSTEM_SLEEF=OFF
export BLAS=OpenBLAS
export USE_BLAS=OpenBLAS
# torch 的 FindOpenBLAS.cmake 只搜硬编码路径+$OpenBLAS_HOME → 显式给根(含 include/openblas + lib/libopenblas.a)
export OpenBLAS_HOME="$OB_OURS_SRC"
export USE_OPENMP=ON
export MAX_JOBS=${MAX_JOBS:-16}
# 链接库目录修正(核心!G2 第 16-22 次全部失败的真根因):
# driver(clang-15, SDK 默认)在命令中注入宿主 `-L/usr/lib/x86_64-linux-gnu`(且在其它 -L 之前)
# → 链接 `-lm/-lc` 时**先命中 host x86_64 的 libm.so**, lld 据 x86 输入判定 emulation
# → "--fix-cortex-a53-843419 is only supported on AArch64 targets"(链接器根本没到 aarch64 态)。
# 修: 在链接 flags 最前加 aarch64 sysroot 的 -L → -lm/-lc 先命中 aarch64 libm.a/libc.so。
# (fail2 复现命令 + 前置这两个 -L → 链接成功, 已验证)
OB_SYSROOT=/apps/harmony/sdk/default/openharmony/native/llvm/bin/../../sysroot
export CMAKE_EXE_LINKER_FLAGS="-L$OB_SYSROOT/usr/lib/aarch64-linux-ohos -L$OB_SYSROOT/usr/lib"
export CMAKE_MODULE_LINKER_FLAGS="-L$OB_SYSROOT/usr/lib/aarch64-linux-ohos -L$OB_SYSROOT/usr/lib"
export CMAKE_SHARED_LINKER_FLAGS="-L$OB_SYSROOT/usr/lib/aarch64-linux-ohos -L$OB_SYSROOT/usr/lib"
# CMake 4.2 对 lld 默认启用 LINK_DEPENDS_USE_LINKER(--dependency-file)+ 校验(lwyu)步骤;
# 交叉下校验对 aarch64 库必败(宿主 ldd/ld.lld 误解 --fix-cortex-a53 flag "only supported on AArch64"),
# G2 实锤(第 16-18 次构建均复现) → 关闭以禁用全部链接后校验。
export CMAKE_LINK_DEPENDS_USE_LINKER=OFF
export CMAKE_LINK_WHAT_YOU_USE=OFF
# C++ 运行时落点: SDK 原路(不加 -L 覆盖)。结论(实测钉死): SDK 的 libc++ 是 `std::__n1`(SONAME=
# libc++_shared.so)而 skh 树 libc++.so.1 是 `std::__1`(LLVM 原版)——両者不互通(undefined __n1 实锤)。
# 我们对象全部 __n1(SDK 头编译) → 必须匹配 SDK libc++_shared.so; G3 打包时将其(连同 abi/unwind 依赖,
# 若 NEEDED)入 HAP libs, 与既有 libc++.so.1(__1, 供旧 skh 链)并存; 两套 STL 无跨模块对象传递(桥=纯 C)。
# 警示: 后人勿再把 skh 树 libc++.so.1 注入 -L(会导致 __n1 undefined symbol 爆炸, G2 已踩坑)
# cmake.py 08 补丁把该变量纳入 build_options 白名单 → 传给 CMake(sleef 交叉分支 IMPORTED 工具路径)
export NATIVE_BUILD_DIR="$SLEEF_NATIVE"
# CMake4 交叉模式: FindPython 不查宿主 Development 组件(CMP0190/CMake 4.1 新模式) — 显式给输入变量,
# 已 DEFINE 则跳过查找(最小实验验证: 交叉下 components Interpreter/Development.Module/NumPy 全过)。
# 仅宿主构建期检查/头生成使用, 不进产物(_C.so 不链 libpython; skh libtorch_python.so NEEDED 已证)。
export Python_INCLUDE_DIR=/opt/py312/include/python3.12
export Python_LIBRARY=/opt/py312/lib/libpython3.12.a

# ── 3) setup.py build(等价 skh 的 pip3 install .;--no-build-isolation 用宿主已装依赖) ──
if [ "${BUILD_KEEP:-0}" != "1" ]; then
  rm -rf "$OUT" "$SRC/dist" "$SRC/torch.egg-info" 2>/dev/null || true
  # build/ 有 IDE clangd(.cache)并行写, rm -rf 会竞态失败 — 先 mv 轮换再后台删
  if [ -d "$SRC/build" ]; then
    mv "$SRC/build" "$SRC/build.old.$BASHPID" 2>/dev/null || rm -rf "$SRC/build" || true
    rm -rf "$SRC/build.old.$BASHPID" 2>/dev/null || true
  fi
fi
cd "$SRC"
echo ">> torch cross-build start: $($PY -V)  CC=$CC"
export TORCH_CROSS_COMPILE=1   # 双保险标记(版本无此宏时无害)
# 直接走 pip(不要 setup.py install: 它会 redirect 为 `pip install .` 并把 --root 丢掉 → 装进宿主 site-packages)
# --prefix=/usr: 让树内路径为 usr/lib/...(默认继承宿主 sys.prefix=/opt/py312, 会多一层 opt/py312)
$PY -m pip install --no-build-isolation --root="$OUT" --prefix=/usr --verbose . 2>&1 | tee "$ROOT/build/torch-build.log"
echo ">> torch cross-build done -> $OUT"

# ── 5) 产物收尾: _C 扩展按靶机 SOABI 命名(pip 用宿主 python 命名 x86_64; 内容却是 aarch64 ELF) ──
TARGET_SOABI="cpython-312-aarch64-linux-ohos"   # 靶机解释器 sysconfig SOABI(与 skh 发行命名一致)
TORCH_SITE="$OUT/usr/lib/python3.12/site-packages/torch"
_C_X86=$(find "$TORCH_SITE" -maxdepth 1 -name "_C.cpython-312-x86_64-linux-gnu.so" 2>/dev/null)
if [ -n "$_C_X86" ]; then
  mv "$_C_X86" "$TORCH_SITE/_C.$TARGET_SOABI.so"
fi
[ -f "$TORCH_SITE/_C.$TARGET_SOABI.so" ] || { echo "FATAL: $TORCH_SITE/_C.$TARGET_SOABI.so 缺失"; exit 1; }
file "$TORCH_SITE/_C.$TARGET_SOABI.so" | grep -q "ARM aarch64" || { echo "FATAL: _C.so 非 aarch64"; exit 1; }
echo "  [FINAL] _C.so = $(file "$TORCH_SITE/_C.$TARGET_SOABI.so" | grep -oE 'ARM aarch64')"
