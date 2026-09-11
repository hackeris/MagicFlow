#!/usr/bin/env bash
# collect_prebuilt.sh —— 按 config/prebuilt_manifest.tsv 清单重建 entry/src/main/cpp/prebuilt/。
#
# 背景（2026-09-03 正规化定谳）:
#   prebuilt/（~394MB 二进制）.so 一律不入 git —— 全部可由清单重建:
#     source tag → 源目录
#     dynload   → build/skh-run-extract/skh-run/usr/lib/python3.12/lib-dynload
#     numpy     → .../site-packages/numpy
#     scipy     → .../site-packages/scipy
#     torch     → .../site-packages/torch/lib;  torch-root → .../site-packages/torch/_C...
#     usr-lib   → .../usr/lib（libc++.so.1 系）
#     pybind11  → .../usr/include/pybind11（72 头, 含 OHOS 定制 type_caster_pyobject_ptr.h）
#     pc-wheel  → externals/pc-wheel-extract（pydantic_core wheel 解包: _pydantic_core...musl.so + .libs/libgcc_s-0bf60adc.so.1）
#     rust      → build/rust-out（build_rust_exts.sh 产物）
#     nnrt      → nnrt-backend/build（build_ext.sh 产物; NPU 后端 torch C++ 扩展, 见其文件头部署铁律）
#   manifest 行: <目标相对路径> <tag> <sha256>；收集=按 basename 在 tag 源树内 sha 匹配定位。
#
# 校验（任一失败 → exit 1,先完成收集再统一报告）:
#   1) 清单逐条 sha（缺/不符即报错）
#   2) 计数断言:按 tag 行数计（dynload 70 / numpy 38 / scipy 113 / torch 6 / torch-root 1 /
#      usr-lib 20 / pybind11 72 / pc-wheel 2 / rust 2 —— 均从 manifest 实际行数自动得出）
#   3) 清单外文件检查:prebuilt/ 既有文件 ⊆ manifest（防 comfymini/libpython3.13/libc++_shared 等死文件回流）
#   4) CMakeLists 引用一致性:import_so"rel" 与 import_ext_so"rel" 引用的文件全部存在于 prebuilt
#   5) NEEDED 闭包:全部 .so 的 DT_NEEDED ⊆ prebuilt 名集 ∪ 系统白名单{libc,libm,libdl,libpthread}
#      （== 保证真机 loader 在 libs/ 内闭环;白名单外未收集即 FAIL）
#
# 用法: bash scripts/collect_prebuilt.sh [--force]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRE="$ROOT/entry/src/main/cpp/prebuilt"
MAN="$ROOT/config/prebuilt_manifest.tsv"
FORCE=0; [ "${1:-}" = "--force" ] && FORCE=1
EXT="${EXT_DIR:-$ROOT/externals}"
EXTRA="$EXT/wheels-aarch64"   # 预留

# tag → 源目录（构建树内;依赖 make extract / fetch）
src_dir() {
    local SKXY="$ROOT/build/skh-run-extract/skh-run"
    case "$1" in
        dynload)  echo "$SKXY/usr/lib/python3.12/lib-dynload" ;;
        numpy)    echo "$SKXY/usr/lib/python3.12/site-packages/numpy" ;;
        scipy)    echo "$SKXY/usr/lib/python3.12/site-packages/scipy" ;;
        torch)    echo "$SKXY/usr/lib/python3.12/site-packages/torch/lib" ;;
        torch-root) echo "$SKXY/usr/lib/python3.12/site-packages/torch" ;;
        # torch-ohos*: G2 自建 OpenBLAS 版(thirdparty/ohos-torch/build_torch_ohos.sh), 2026-09-04
        torch-ohos) echo "$ROOT/build/torch-ohos-install/usr/lib/python3.12/site-packages/torch/lib" ;;
        torch-ohos-root) echo "$ROOT/build/torch-ohos-install/usr/lib/python3.12/site-packages/torch" ;;
        # sdklib: OHOS NDK 自带库(libc++_shared.so, __n1 系 —— 新 torch 链的 C++ 运行时)
        sdklib)   echo "/apps/harmony/sdk/default/openharmony/native/llvm/lib/aarch64-linux-ohos" ;;
        usr-lib)  echo "$SKXY/usr/lib" ;;
        pybind11) echo "$SKXY/usr/include/pybind11" ;;
        pc-wheel) echo "$EXT/pc-wheel-extract" ;;
        nnrt)     echo "$ROOT/nnrt-backend/build" ;;
        rust)     echo "$ROOT/build/rust-out" ;;
        *) echo "??Unknown-tag!" ;;
    esac
}

[ -f "$MAN" ] || { echo "[PREBUILT] 缺 $MAN"; exit 1; }

# ── 收集 stage:读清单(逐行),源树内 sha 定位,逐文件落位 ──
declare -a MISSING MISMATCH
total=$(wc -l < "$MAN")
echo "== [prebuilt] 清单 $total 条 → $PRE"
mkdir -p "$PRE"
[ "$FORCE" = 1 ] && { rm -rf "$PRE"; mkdir -p "$PRE"; }
# collect: 目标绝对路径;若既有文件 sha 与清单一致 → 跳过;否则查找/拷贝
collect_one() {
    local rel="$1" tag="$2" want="$3"
    local dst="$PRE/$rel" src=""
    # 既有好文件:快路径
    if [ -f "$dst" ] && [ "$(sha256sum "$dst" | awk '{print $1}')" = "$want" ]; then return 0; fi
    local d="$(src_dir "$tag")"
    [ -d "$d" ] || { MISSING+=("src-missing:$tag:$d"); return 0; }
    local fname="$(basename "$rel")"
    local found=""
    if [ "$tag" = "rust" ]; then
        # rust 产物字节不可复现（链接元数据/增量差异）—— 锚 =「当前工具链从固定 commit 源码重建」;
        # 直接采用 build/rust-out 产物（非空断言代替 sha 比对）。
        found="$d/$fname"; [ -s "$found" ] || found=""
    else
        while IFS= read -r cand; do
            if [ "$(sha256sum "$cand" | awk '{print $1}')" = "$want" ]; then found="$cand"; break; fi
        done < <(find "$d" -name "$fname" 2>/dev/null)
    fi
    if [ -n "$found" ]; then
        mkdir -p "$(dirname "$dst")"; cp "$found" "$dst"
    else
        MISSING+=("$rel(sha-mismatch:no-hit in $tag)")
    fi
}
while IFS=$'\t' read -r rel tag sha; do
    collect_one "$rel" "$tag" "$sha"
done < "$MAN"

# ── 校验块(先执行完拷贝,统一汇总) ──
echo "== [prebuilt] 校验 & 断言 =="
FAIL=0
# 1) 逐条 sha 复验（rust 例外: 源码重建产物字节不可复现,只做强存在性+NEEDED 闭环检查,不做 sha 比对）
while IFS=$'\t' read -r rel tag sha; do
    if [ "$tag" = "rust" ]; then
        [ -s "$PRE/$rel" ] || { echo "[FAIL] rust 产物缺失/空: $rel"; FAIL=1; }
        continue
    fi
    if [ -f "$PRE/$rel" ] && [ "$(sha256sum "$PRE/$rel" | awk '{print $1}')" = "$sha" ]; then :; else
        echo "[FAIL] sha $rel"; FAIL=1
    fi
done < "$MAN"

# 2) 计数断言(从 manifest 行数自动得出)
declare -A EXPECT
while IFS=$'\t' read -r rel tag sha; do EXPECT["$tag"]=$(( ${EXPECT["$tag"]:-0} + 1 )); done < "$MAN"
for tag in "${!EXPECT[@]}"; do
    got=$(find "$PRE" -type f | while read -r f; do
        rel="${f#$PRE/}"
        awk -F'\t' -v r="$rel" '$1==r{print $2}' "$MAN"
    done | grep -c "^${tag}$" || true)
    if [ "$got" != "${EXPECT[$tag]}" ]; then echo "[FAIL] 计数 $tag: want=${EXPECT[$tag]} got=$got"; FAIL=1; fi
done

# 3) 清单外文件检查(死文件防线)
while read -r f; do
    rel="${f#$PRE/}"
    if ! grep -q -P "^$(printf '%s' "$rel" | sed 's/[.[\*^$+?{}|()]/\\&/g')\t" "$MAN"; then
        echo "[FAIL] 清单外文件: $rel（死文件回流? 请删或加清单）"
        FAIL=1
    fi
done < <(find "$PRE" -type f)

# 4) CMakeLists 引用一致性(import_so/import_ext_so 的 rel 与 GLOB 清单)
cmake="$ROOT/entry/src/main/cpp/CMakeLists.txt"
while read -r rel; do
    [ -f "$PRE/$rel" ] || { echo "[FAIL] CMake 引用缺文件: $rel"; FAIL=1; }
done < <(grep -oE '(import_so|import_ext_so)\([^)]*"[^"]+"[^)]*\)' "$cmake" | grep -oE '"[^"]+"' | tr -d '"' \
        | grep -vE '\$\{' | grep -vE '^(libhilog_ndk|libchild_process|libdl|libace_napi)' | sort -u)

# 5) NEEDED 闭包(系统白名单外必须 ∈ prebuilt 名集)
LLVM_READELF="$([ -x /apps/harmony/sdk/default/openharmony/native/llvm/bin/llvm-readelf ] && echo /apps/harmony/sdk/default/openharmony/native/llvm/bin/llvm-readelf || echo readelf)"
SYSLIB='^(libc|libm|libdl|libpthread)\.so'
declare -A PRE_NAMES
while read -r f; do PRE_NAMES["$(basename "$f")"]=1; done < <(find "$PRE" -type f)
while read -r f; do
    "$LLVM_READELF" -d "$f" 2>/dev/null | awk '/NEEDED/{print $NF}' | tr -d '[ ]' | while read -r n; do
        n="${n#Shared library: [}"; n="${n%]}"
        if echo "$n" | grep -qE "$SYSLIB"; then continue; fi
        if [ -z "${PRE_NAMES[$n]:-}" ]; then echo "[FAIL] NEEDED 未闭环: $f → $n"; FAIL=1; fi
    done
done < <(find "$PRE" -type f -name '*.so*')

if [ -z "${MISSING[*]}" ] && [ "$FAIL" = 0 ]; then
    echo "== [prebuilt] PASS: $(find "$PRE" -type f | wc -l) 文件全部闭环 =="
else
    for m in "${MISSING[@]:-}"; do echo "[MISS] $m"; done
    echo "== [prebuilt] FAILED =="
    exit 1
fi
