#!/usr/bin/env bash
# build_rust_exts.sh —— 从 thirdparty/ 子模块源码交叉编译两个 Rust 扩展到 build/rust-out/。
#
#   tokenizers.abi3.so        (tokenizers @7f1623b / v0.23.1, bindings/python, pyo3=0.28.2 abi3)
#   _safetensors_rust.abi3.so (safetensors @a406ca3, bindings/python, pyo3=0.28 abi3-py310)
#
# 背景: 这两个 .so 原为 /tmp 手工交叉编译产物,无构建脚本 → 复现链盲区。
#   现改为「源码 submodule + 本脚本重编译」(用户决策 2026-09-03)。
#
# 前置要求(首次运行):
#   1) rust 工具链: rustup 与 stable 已装(+ target aarch64-unknown-linux-ohos)
#      —— 本机已有(rustc 1.98.0);新机器: curl https://sh.rustup.rs | sh
#      rustup target add aarch64-unknown-linux-ohos
#   2) submodule 已检出: git submodule update --init --recursive
#   3) OHOS SDK 路径经 source scripts/env.sh(本脚本自动 source)
#
# 产物校验: 若 build/ 旁已有 prebuilt 同名文件(首发锚),必须 sha256 一致——
#   「从源码重建 == 锚」才是可复现;不一致会留档并告警(collect_prebuilt.sh 会拒绝不一致)。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/env.sh" 2>/dev/null || true   # OHOS_SDK/CLANG 等(无则环境里给)
export PATH="$HOME/.cargo/bin:$PATH"
command -v rustc >/dev/null 2>&1 || { echo "[RUST] 无 rustc(先 rustup 安装)"; exit 1; }
if ! rustup target list --installed | grep -q '^aarch64-unknown-linux-ohos$'; then
    echo "[RUST] 缺 target aarch64-unknown-linux-ohos —— 执行: rustup target add aarch64-unknown-linux-ohos"
    exit 1
fi

LLVM="${OHOS_SDK:-/apps/harmony/sdk/default/openharmony}/native/llvm/bin"
CLANG="$LLVM/aarch64-unknown-linux-ohos-clang"
CLANGXX="$LLVM/aarch64-unknown-linux-ohos-clang++"
[ -x "$CLANGXX" ] || { echo "[RUST] 缺 SDK 交叉 clang++: $CLANGXX"; exit 1; }

OUT_DIR="$ROOT/build/rust-out"
mkdir -p "$OUT_DIR"

# cargo 会从子模块工作树读 .cargo/config.toml(镜像源 + OHOS linker);
# 子模块工作树在这个 untracked 目录不污染其 gitlink 状态(只记 commit)。
cargo_cfg() {
    local sub="$1"
    mkdir -p "$sub/.cargo"
    cat > "$sub/.cargo/config.toml" <<EOF
[source.crates-io]
replace-with = 'mirror'
[source.mirror]
registry = "sparse+https://mirrors.ustc.edu.cn/crates.io-index/"
[target.aarch64-unknown-linux-ohos]
linker = "$CLANGXX"
EOF
}

export CC_aarch64_unknown_linux_ohos="$CLANG"
export CXX_aarch64_unknown_linux_ohos="$CLANGXX"
export CARGO_TARGET_DIR=/tmp/rust-ext-target-oci   # 共享:两次构建复用依赖编译缓存

log() { echo -e "\033[32m[RUST]\033[0m $*" >&2; }

# ── ① tokenizers.abi3.so ──
build_tokenizers() {
    local sub="$ROOT/thirdparty/tokenizers"
    [ -f "$sub/bindings/python/Cargo.toml" ] || { echo "[RUST] submodule 未检出 thirdparty/tokenizers(git submodule update --init)"; exit 1; }
    log "tokenizers @ $(git -C "$sub" rev-parse --short HEAD)"
    cargo_cfg "$sub"
    ( cd "$sub/bindings/python" && cargo build --release --target aarch64-unknown-linux-ohos --quiet )
    local so="$CARGO_TARGET_DIR/aarch64-unknown-linux-ohos/release/libtokenizers.so"
    [ -f "$so" ] || { echo "[RUST] 产物缺失: $so"; exit 1; }
    cp "$so" "$OUT_DIR/tokenizers.abi3.so"
    log "  -> $OUT_DIR/tokenizers.abi3.so"
}

# ── ② _safetensors_rust.abi3.so ──
build_safetensors() {
    local sub="$ROOT/thirdparty/safetensors"
    [ -f "$sub/bindings/python/Cargo.toml" ] || { echo "[RUST] submodule 未检出 thirdparty/safetensors(git submodule update --init)"; exit 1; }
    log "safetensors @ $(git -C "$sub" rev-parse --short HEAD)"
    cargo_cfg "$sub"
    ( cd "$sub/bindings/python" && cargo build --release --target aarch64-unknown-linux-ohos --quiet )
    local so="$CARGO_TARGET_DIR/aarch64-unknown-linux-ohos/release/libsafetensors_rust.so"
    [ -f "$so" ] || { echo "[RUST] 产物缺失: $so"; exit 1; }
    cp "$so" "$OUT_DIR/_safetensors_rust.abi3.so"
    log "  -> $OUT_DIR/_safetensors_rust.abi3.so"
}

build_tokenizers
build_safetensors

# ── 锚比对(能对必对,否则告警,collect 阶段裁决) ──
PRE="$ROOT/entry/src/main/cpp/prebuilt"
for f in tokenizers.abi3.so _safetensors_rust.abi3.so; do
    if [ -f "$PRE/$f" ]; then
        a="$(sha256sum "$OUT_DIR/$f" | awk '{print $1}')"
        b="$(sha256sum "$PRE/$f" | awk '{print $1}')"
        if [ "$a" = "$b" ]; then log "  [OK] $f 与 prebuilt 锚一致 ($(echo "$a" | cut -c1-12)…)";
        else
            warn "  [DIFF] $f 重建产物 ≠ prebuilt 锚(禁止 patchelf 类产物进链;请核实工具链/commit 后决定是否更新锚)"
        fi
    else
        warn "  [NO-ANCHOR] prebuilt/$f 不存在(全新构建?产物可用,由 collect_prebuilt.sh 收走)"
    fi
done
log "完成 build/rust-out/ —— scripts/collect_prebuilt.sh 会从 $OUT_DIR 采样"
