# 共享环境变量 — 被所有构建脚本/子 Makefile source。
# 组织方式参考 ../wineohos/scripts/env.sh。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export OHOS_SDK="${OHOS_SDK:-/apps/harmony/sdk/default/openharmony}"
export TOOL_HOME="${TOOL_HOME:-/apps/harmony}"
export PATH="$TOOL_HOME/bin:$TOOL_HOME/tool/node/bin:$PATH"

# ── Native 层架构 (HAP .so 的目标; 设备 CPU) ──
NATIVE_ARCH="${NATIVE_ARCH:-arm64-v8a}"
case "$NATIVE_ARCH" in
    arm64-v8a) NATIVE_TARGET="aarch64-linux-ohos" ;;
    x86_64)    NATIVE_TARGET="x86_64-linux-ohos" ;;
    *) echo "不支持的 NATIVE_ARCH=$NATIVE_ARCH (arm64-v8a|x86_64)"; exit 1 ;;
esac
export NATIVE_ARCH NATIVE_TARGET

CLANG="$OHOS_SDK/native/llvm/bin/$NATIVE_TARGET-clang"
CLANGXX="$OHOS_SDK/native/llvm/bin/$NATIVE_TARGET-clang++"
SYSROOT="$OHOS_SDK/native/sysroot"
export CLANG CLANGXX SYSROOT

# 产物/构建路径
BUILD_DIR="$ROOT/build"
# 外部输入根（fetch_externals.sh 产物落位; 可被 env 覆盖, 默认仓库内 externals/）
export EXT_DIR="${EXT_DIR:-$ROOT/externals}"
export BUILD_DIR

# 测试设备（hdc 目标, 用实际连接设备; HDC_TARGET 可覆盖, 如 HDC_TARGET=192.168.1.6:33363）
# 2026-09-11: 默认设备改为 1.5(192.168.1.5:44959, 9030/KirinXE90)。
#   教训: 旧默认 192.168.1.8:33363 已不在线(设备 IP 变更), 而 hdc 对不在线的 -t
#   会静默 fallback 到其他在线设备 —— 装机/拉日志全落到 1.6 上, 排查方向被带偏。
#   改设备前先 `hdc list targets` 核对。
export HDC_TARGET="${HDC_TARGET:-192.168.1.5:44959}"
export HDC="$OHOS_SDK/toolchains/hdc -t $HDC_TARGET"
export BUNDLE="app.fuqidian.magicflow"

# 编译并行
if [ -z "${JOBS:-}" ]; then JOBS="$(nproc 2>/dev/null || echo 4)"; fi
export JOBS

log()  { echo -e "\033[32m[BUILD]\033[0m $*" >&2; }
warn() { echo -e "\033[33m[WARN]\033[0m $*" >&2; }
err()  { echo -e "\033[31m[ERROR]\033[0m $*" >&2; exit 1; }
