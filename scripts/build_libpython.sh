#!/bin/bash
# 交叉编译 libpython3.13.so（aarch64, OHOS/musl）—— 基于 Termony build-hnp/python/Makefile 精简。
# 只求 libpython3.13.so + SOABI 正确；OHOS sysroot 无 bz2/lzma/ssl/sqlite3 等头 → 用 ac_cv 禁用，
# 且仅 make 主库(libpython3.13.so)，跳过会因缺库失败的可选共享扩展模块。
set -euo pipefail
source "$(dirname "$0")/env.sh"

PY_VER=3.13.5
WORK="$BUILD_DIR/libpython"
DL="$WORK/download"
SRC="$WORK/Python-$PY_VER"

OHOS_CLANG="$OHOS_SDK/native/llvm/bin/aarch64-unknown-linux-ohos-clang"
OHOS_CLANGXX="$OHOS_SDK/native/llvm/bin/aarch64-unknown-linux-ohos-clang++"
LLVM_STRIP="$OHOS_SDK/native/llvm/bin/llvm-strip"
LLVM_READELF="$OHOS_SDK/native/llvm/bin/llvm-readelf"

log()  { echo -e "\033[32m[PY]\033[0m $*"; }
err()  { echo -e "\033[31m[PY][ERROR]\033[0m $*" >&2; exit 1; }

# OHOS sysroot 缺失的可选库 → 逐个禁用，让 configure 将其排除出模块列表
DISABLE_CC_DEPS="
  ac_cv_header_bzlib_h=no ac_cv_lib_bz2_BZ2_bzCompress=no
  ac_cv_header_lzma_h=no ac_cv_lib_lzma_lzma_version_string=no
  ac_cv_header_openssl_ssl_h=no ac_cv_header_openssl_evp_h=no ac_cv_lib_ssl_SSL_new=no
  ac_cv_header_readline_readline_h=no ac_cv_header_readline_history_h=no ac_cv_lib_readline_readline=no
  ac_cv_header_gdbm_h=no ac_cv_lib_gdbm_gdbm_open=no
  ac_cv_header_dbm_ndbm_h=no ac_cv_lib_dbm_dbm_open=no ac_cv_header_ndbm_h=no
  ac_cv_header_sqlite3_h=no ac_cv_lib_sqlite3_sqlite3_open=no
  ac_cv_header_ffi_h=no ac_cv_header_libffi_ffi_h=no ac_cv_lib_ffi_ffi_call=no
  ac_cv_header_ncurses_h=no ac_cv_header_panel_h=no ac_cv_lib_ncurses_initscr=no
  ac_cv_header_expat_h=no ac_cv_lib_expat_XML_ParserCreate=no
  ac_cv_header_libintl_h=no
  ac_cv_file__dev_ptmx=no ac_cv_file__dev_ptc=no
"

# ── 1) 下载 (仅首次, 华为云镜像) ──
mkdir -p "$DL"
if [ ! -f "$DL/Python-$PY_VER.tgz" ]; then
  log "download Python-$PY_VER (huaweicloud mirror) ..."
  wget --retry-connrefused --read-timeout=20 --timeout=15 \
       -O "$DL/Python-$PY_VER.tgz" \
       "https://repo.huaweicloud.com/python/$PY_VER/Python-$PY_VER.tgz"
fi

# ── 2) 解压 ──
rm -rf "$SRC" "$WORK/native" "$WORK/build"
cd "$WORK" && tar xf "$DL/Python-$PY_VER.tgz"

# ── 3) build-native：宿主 cc 先编 x86_64 host python3.13（cross build-python 必需）──
log "build-native host python3.13 ..."
mkdir -p "$SRC/build-native"
cd "$SRC/build-native"
HOST_TOOL="CC=$(command -v cc) CXX= LD='ld' AR=$(command -v ar) CFLAGS= CXXFLAGS= LDFLAGS= PKG_CONFIG_LIBDIR="
eval "$HOST_TOOL" ../configure --disable-test-modules >/dev/null
eval "$HOST_TOOL" make -j "$JOBS" >/dev/null
eval "$HOST_TOOL" make install DESTDIR="$WORK/native" >/dev/null
HOST_PY="$WORK/native/usr/local/bin/python3"
[ -x "$HOST_PY" ] || err "build-native python 未生成: $HOST_PY"
log "build-native python OK: $("$HOST_PY" -V)"

# ── 4) 交叉 configure ──
log "cross configure (host=aarch64-unknown-linux-musl) ..."
mkdir -p "$SRC/build"
cd "$SRC/build"
# shellcheck disable=SC2086
CC="$OHOS_CLANG" CXX="$OHOS_CLANGXX" \
../configure \
  ANDROID_API_LEVEL=1 \
  --prefix="$WORK/prefix" \
  --exec-prefix="$WORK/prefix" \
  --disable-test-modules \
  --enable-shared \
  --host=aarch64-unknown-linux-musl \
  --build=x86_64-unknown-linux-gnu \
  --enable-ipv6 \
  --without-ensurepip \
  --with-build-python="$HOST_PY" \
  $DISABLE_CC_DEPS >/dev/null

# 缺 gettext 的 libintl.h → 清除该 define
sed -i.bak 's/^#define HAVE_LIBINTL_H 1//' pyconfig.h

# ── 5) 交叉 make：只编主库（跳过 sharedmods，避开缺库的可选扩展）──
log "cross make (shared libpython 主库) ..."
make -j "$JOBS" libpython3.13.so >/dev/null

# ── 6) 收集产物（主库在构建树顶层）──
OUT="$BUILD_DIR/libpython-out"
rm -rf "$OUT" && mkdir -p "$OUT"
find "$SRC/build" -maxdepth 2 -name 'libpython3.13*.so*' -exec cp -v --preserve=links {} "$OUT/" \;
"$LLVM_STRIP" "$OUT"/libpython3.13.so* 2>/dev/null || true

log "产物 SONAME / NEEDED / SOABI:"
for f in "$OUT"/libpython*.so*; do
  echo "  $(basename "$f"):"
  "$LLVM_READELF" -d "$f" 2>/dev/null | grep -iE 'SONAME|NEEDED' || true
done
log "DONE → $OUT"
ls -la "$OUT"
