#!/usr/bin/env bash
# bootstrap_worktree.sh —— 全链复现的「机器态」桥接与预检(2026-09-05 G4 复现链补)。
# 作用: git worktree 只含源码/脚本/config; 本脚本把【机器态前置】全部落实:
#   1) OHOS NDK /apps/harmony 存在性
#   2) 交叉 gfortran(aarch64-linux-gnu-gfortran)
#   3) rustc + aarch64-unknown-linux-ohos target(rust 扩展)
#   4) 签名机器态桥接: 拷贝 <宿主区>/.ohos + local.properties → 本 worktree
#   5) host py3.12: /opt/py312 缺失时从 externals/py312/Python-3.12.7.tgz 自编
#      (build_torch_ohos.sh 跑 setup.py 必需; SOABI=cpython-312 与设备解释器成套)
#   6) Rust 扩展源码 submodule(in .gitmodules): git submodule update --init --recursive
# 用法: bash scripts/bootstrap_worktree.sh [--source-dir /data/share/comfyui]
#   --source-dir 默认 /data/share/comfyui(当前宿主区/正主; 桥接 .ohos/local.properties 的来源)。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_SOURCE="${1:-}"
if [ -z "$SRC_SOURCE" ]; then [ "$ROOT" = "/data/share/comfyui" ] && SRC_SOURCE="" || SRC_SOURCE="/data/share/comfyui"; fi
OK="✓"; MISS="✗"
say() { echo -e "\033[32m[BOOT]\033[0m $*"; }
bad() { echo -e "\033[31m[BOOT:FATAL]\033[0m $*"; exit 1; }

say "worktree root = $ROOT"

# 1) OHOS NDK
LLVM=/apps/harmony/sdk/default/openharmony/native/llvm/bin
[ -x "$LLVM/aarch64-unknown-linux-ohos-clang++" ] || bad "缺 OHOS NDK: $LLVM/aarch64-unknown-linux-ohos-clang++(SDK=/apps/harmony)"
say "$OK OHOS NDK $LLVM"

# 2) gfortran 交叉器
command -v aarch64-linux-gnu-gfortran >/dev/null || {
  say "缺 aarch64-linux-gnu-gfortran → apt 安装..."
  apt-get install -y -q gfortran-aarch64-linux-gnu
}
say "$OK aarch64-linux-gnu-gfortran"

# 3) rust + ohos target
command -v rustc >/dev/null || bad "无 rustc —— 先 curl https://sh.rustup.rs | sh 再重跑"
if ! rustup target list --installed 2>/dev/null | grep -q '^aarch64-unknown-linux-ohos$'; then
  say "rust target aarch64-unknown-linux-ohos 缺失 → rustup target add ..."
  rustup target add aarch64-unknown-linux-ohos
fi
say "$OK rust $(rustc --version | awk '{print $2}') + ohos target"

# 4) 签名桥接(仅非正主 worktree 才需要)
if [ -n "$SRC_SOURCE" ] && [ "$SRC_SOURCE" != "$ROOT" ]; then
  for rel in .ohos local.properties; do
    if [ -d "$SRC_SOURCE/$rel" ] || [ -f "$SRC_SOURCE/$rel" ]; then
      cp -a "$SRC_SOURCE/$rel" "$ROOT/$rel"
      say "$OK 签名机器态桥接: $rel ← $SRC_SOURCE"
    else
      bad "宿主区缺 $SRC_SOURCE/$rel(签名/构建必需)"
    fi
  done
else
  say "$OK 本目录即正主(.ohos/local.properties 原位)"
fi

# 5) host py312(源码 tar 若缺失, 先 make fetch)
PY312=/opt/py312/bin/python3.12
[ -x "$PY312" ] || {
  TG=$ROOT/externals/py312/Python-3.12.7.tgz
  [ -f "$TG" ] || bad "缺 $TG —— 先运行 make fetch"
  say "自编 host py3.12(/opt/py312)from $TG ..."
  rm -rf /tmp/py312-build /opt/py312
  mkdir -p /tmp/py312-build && tar xzf "$TG" -C /tmp/py312-build
  ( cd /tmp/py312-build/Python-3.12.7 \
    && ./configure --prefix=/opt/py312 >/dev/null \
    && make -j"$(nproc)" >/dev/null && make install >/dev/null )
}
say "$OK /opt/py312 ($(/opt/py312/bin/python3.12 --version))"

# 6) rust 扩展源码 submodule
if git -C "$ROOT" submodule status 2>/dev/null | grep -q '^-'; then
  say "submodule 检出(thirdparty/tokenizers + safetensors)..."
  git -C "$ROOT" submodule update --init --recursive
fi
say "$OK submodules"

say "就绪清单: NDK✓ gfortran✓ rust✓ py312✓ 签名桥接✓ submodules✓ —— 可执行:"
say "  make fetch && make rust && (openblas: bash thirdparty/ohos-torch/build_openblas_ohos.sh externals/openblas-src) && bash thirdparty/ohos-torch/build_torch_ohos.sh && make hap"
