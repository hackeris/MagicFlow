#!/bin/bash
# Step 0.3a 烟囱：交叉编译最小 C++ CPython 扩展 comfymini。
# 关键：必须设 SONAME=短名（linker 才会把 DT_NEEDED 记成短名而非 imported 绝对路径，
#       否则被 comfy_child NEEDED 时 loader 会去真机不存在构建机路径加载而崩——libpython 正是靠 SONAME 才活的）。
# 产物链应用 libc++_shared.so(__n1)，复现命门③。
set -euo pipefail
source "$(dirname "$0")/env.sh"

PY_SRC="$BUILD_DIR/libpython/Python-3.13.5"
LIBCPP_DIR="$OHOS_SDK/native/llvm/lib/aarch64-linux-ohos"
RE="$OHOS_SDK/native/llvm/bin/llvm-readelf"
NM="$OHOS_SDK/native/llvm/bin/llvm-nm"
CXX="$OHOS_SDK/native/llvm/bin/aarch64-unknown-linux-ohos-clang++"
SOV="cpython-313-aarch64-linux-musl"
SO_NAME="comfymini.$SOV.so"          # 与 HAP libs/ 里的 basename 一致
OUT="$BUILD_DIR/comfymini"
SRC="$ROOT/entry/prebuilt/comfymini.cpp"
mkdir -p "$OUT"

log()  { echo -e "\033[32m[CFM]\033[0m $*"; }

log "编译 $SO_NAME (链 libc++_shared.so => __n1)"
"$CXX" -shared -fPIC -std=c++17 \
  -Wl,-soname,"$SO_NAME" \
  -I"$PY_SRC/Include" -I"$PY_SRC/build" \
  -L"$LIBCPP_DIR" \
  -o "$OUT/$SO_NAME" \
  "$SRC" \
  -lc++_shared -lc++abi

log "拷贝到 prebuilt/ 供 CMake imported 打进 HAP libs/<abi>/"
cp -f "$OUT/$SO_NAME" "$ROOT/entry/src/main/cpp/prebuilt/$SO_NAME"

log "== NEEDED / SONAME =="
"$RE" -d "$OUT/$SO_NAME" | grep -iE 'NEEDED|SONAME' || true
log "== libc++ 命名空间 (__h=系统 / __n1=应用) =="
"$NM" -D "$OUT/$SO_NAME" 2>/dev/null | grep -oE '__[hn][0-9a-z]*' | sort -u | head
log "== PyInit 导出 =="
"$NM" -D "$OUT/$SO_NAME" 2>/dev/null | grep -i PyInit || true
log "DONE -> $SO_NAME"
