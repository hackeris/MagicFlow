#!/usr/bin/env bash
# nnrt-backend 扩展交叉编译(宿主 x86_64 → OHOS aarch64)。
#
# 用法(任意目录可执行, 脚本内部自定位):
#   bash nnrt-backend/scripts/build_ext.sh            # 构建全部扩展
#   bash nnrt-backend/scripts/build_ext.sh clean      # 毁灭性重建(删 build/ 后重来)
#
# 产物: nnrt-backend/build/*.so(逐个打印尺寸; 失败立即非零退出)
#   由 scripts/collect_prebuilt.sh(manifest tag=nnrt) 收进 entry/src/main/cpp/prebuilt/nnrt/,
#   再经 CMakeLists import_ext_so → HAP libs/<abi>/。
# 前置: build/skh-run-extract/(make extract 产物)、OHOS SDK(CLT)
#
# ⚠ 部署铁律(2026-09-11 定谳, 别再往 zip 里塞):
#   本扩展必须落 HAP libs/<abi>/, 不能随 python312.zip 进 pyroot(EL2 数据区):
#   1) 栈铁律: 一切被加载 .so 走 HAP libs/<abi>/, 纯 .py 才走 zip→filesDir;
#   2) 机制: comfy_child.cpp 的 _LibsFinder 只扫 libs/ 下**带 cpython tag** 的 .so 建
#      「短名→路径」映射; torch/__init__.py 末尾 _import_device_backends() 的
#      `import _nnrt_bootstrap` 靠它重定向到 libs/ 绝对路径;
#   3) 反例实证(1.5/9030, 09-11): 文件放 pyroot 时 Python **能** find 到
#      (报错路径即 pyroot 下), 但 dlopen 报
#      "Error loading shared library ...pyroot.../_nnrt_bootstrap.so: No such file or
#       directory"(无 "(needed by ..)" 后缀 = musl 顶层 open 失败, 非依赖缺失)。
#      ⇒ EL2 数据区的 .so 不被 loader 接受, 与文件在不在无关。
#   注: tag 后缀三种(.cpython-312-aarch64-linux-ohos.so / -musl.so / .abi3.so)任选其一,
#   本栈 skh 原生编译 → 用 -linux-ohos。
#
# ⚠ libc++ 命名空间纪律(沿袭 scripts/build_rust_exts.sh 的实证):
#   真实 torch 栈链的是 skh 树 usr/lib/libc++.so.1(⇒ std::__1);
#   SDK clang++ 默认 -lc++ 会解析到 native libc++_shared.so(⇒ std::__n1),
#   同进程双 libc++ = 命门③冲突。故一律 -L skh/usr/lib + -l:libc++.so.1(后到优先)。
set -eo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SKH="$ROOT/build/skh-run-extract/skh-run"
TORCH="$SKH/usr/lib/python3.12/site-packages/torch"
SDK="${OHOS_SDK:-/apps/harmony/sdk/default/openharmony}"
SYSROOT="$SDK/native/sysroot"
CXX="$SDK/native/llvm/bin/aarch64-unknown-linux-ohos-clang++"

CSRC="$ROOT/nnrt-backend/csrc"
OUT_DIR="$ROOT/nnrt-backend/build"

if [ "${1:-}" = "clean" ]; then
    echo "[CLEAN] rm -rf $OUT_DIR"
    rm -rf "$OUT_DIR"
fi

# ── 前置断言(失败即停, 不带错误继续) ──
[ -x "$CXX" ] || { echo "[FAIL] 缺交叉编译器: $CXX"; exit 1; }
[ -d "$SYSROOT" ] || { echo "[FAIL] 缺 SDK sysroot: $SYSROOT"; exit 1; }
[ -f "$TORCH/include/torch/extension.h" ] || { echo "[FAIL] 缺 torch 头文件(先 make extract): $TORCH"; exit 1; }
[ -f "$SKH/usr/lib/libc++.so.1" ] || { echo "[FAIL] 缺 skh libc++.so.1"; exit 1; }
[ -f "$SKH/usr/include/python3.12/Python.h" ] || { echo "[FAIL] 缺 Python.h"; exit 1; }

mkdir -p "$OUT_DIR"

# 扩展文件名后缀 = _LibsFinder 认的三种之一(见文件头部署铁律)。PYBIND11_MODULE 的模块名
#   参数仍是不带 tag 的短名(_nnrt_bootstrap), 文件名带 tag 是 CPython 扩展命名规范。
PYEXT=".cpython-312-aarch64-linux-ohos.so"

# 需要构建的扩展: 模块名 + 源文件列表(引擎层 nnrt_engine.cpp 与 torch 集成层
#   bootstrap.cpp 分文件 —— 引擎与 torch/torch API 解耦, 便于单独替换/复用)
build_ext() {
    local mod="$1"; shift
    local out="$OUT_DIR/$mod$PYEXT"
    echo "[BUILD] $mod  ← $*"
    "$CXX" \
        --target=aarch64-linux-ohos \
        --sysroot="$SYSROOT" \
        -shared -fPIC -std=c++17 -O2 \
        -fvisibility=hidden \
        -DTORCH_EXTENSION_NAME="$mod" \
        -DTORCH_API_INCLUDE_EXTENSION_H \
        -I"$TORCH/include" \
        -I"$TORCH/include/torch/csrc/api/include" \
        -I"$SKH/usr/include/python3.12" \
        -L"$TORCH/lib" -L"$SKH/usr/lib" \
        -Wl,-rpath-link,"$TORCH/lib" \
        -ltorch -ltorch_cpu -ltorch_python -lc10 \
        -l:libc++.so.1 -l:libc++abi.so.1 \
        -o "$out" "$@"
    # ── 产物断言(2026-09-11 事故教训: 仅 "[ -f $out ]" 不足以证明编译成功) ──
    #   ⚠ 实测: 函数签名改为 "$@" 后漏改此处输入(仍写 "$src"), 变量未定义 → 展开成空参数 →
    #     clang 无输入文件仍 rc=0 并生成 11,920B 空壳 .so(只有 _init/_fini, 无 PyInit) →
    #     "[ -f $out ]" 通过 → 报 [OK] 且同步 manifest → **静默产出废包并装机**。
    #   故两道硬断言: ①源文件逐个存在且非空 ②产物必须导出 PyInit_<mod>(真扩展的铁证)。
    for _s in "$@"; do
        [ -s "$_s" ] || { echo "[FAIL] 源文件缺失或为空: $_s"; exit 1; }
    done
    [ -f "$out" ] || { echo "[FAIL] $mod 产物缺失"; exit 1; }
    #   ⚠ 须用 llvm-readelf: 实测 GNU readelf --dyn-syms 对本工具链产出的 .so 列不出动态
    #     符号(返回 0 条且 rc=0, 静默) → 会造成"产物正常却断言失败"的误判。
    local _re="$SDK/native/llvm/bin/llvm-readelf"
    [ -x "$_re" ] || _re=readelf
    #   ⚠⚠ 此处**不可**写成 `"$_re" --dyn-syms "$out" | grep -q ...`: 在 set -o pipefail 下,
    #     grep 一旦匹配就提前退出 → 上游 readelf 写管道收到 SIGPIPE → 管道整体返回非零
    #     → 断言把**正常产物**判成失败。2026-09-12 实测: 同一份产物时而 PASS 时而 FAIL
    #     (433 条 dyn-syms 恰好在管道缓冲区边界附近), 排查耗时且误导方向。
    #     改变量捕获: 无管道, 即无此竞态。
    local _syms
    _syms="$("$_re" --dyn-syms "$out" 2>/dev/null || true)"
    case "$_syms" in
        *"PyInit_$mod"*) ;;
        *)
            echo "[FAIL] $mod 产物未导出 PyInit_$mod(空壳或链接失败)"
            exit 1
            ;;
    esac
    echo "[OK] $mod  $(stat -c%s "$out") bytes  (PyInit_$mod ✓)"
}

build_ext "_nnrt_bootstrap" "$CSRC/bootstrap.cpp" "$CSRC/nnrt_engine.cpp" "$CSRC/backend.cpp"

# ── 同步 manifest 的 nnrt 行(sha 随源码走) ──
#   本脚本是 nnrt 产物的唯一生成入口 → 在此同步, 清单永不滞后。
#   ⚠ 不同步的后果(2026-09-11 实测): collect_prebuilt.sh 的「快路径」看到 prebuilt/ 里
#     已有匹配 manifest 的文件就直接跳过 → 拿旧产物打包, 静默部署旧扩展(不报错!);
#     源码改了而 manifest 没改, 整轮构建白跑。
MAN="$ROOT/config/prebuilt_manifest.tsv"
EXTNAME="_nnrt_bootstrap$PYEXT"
NEW_SHA="$(sha256sum "$OUT_DIR/$EXTNAME" | awk '{print $1}')"
OLD_SHA="$(awk -F'\t' -v r="nnrt/$EXTNAME" '$1==r{print $3}' "$MAN")"
if [ "$OLD_SHA" != "$NEW_SHA" ]; then
    if [ -n "$OLD_SHA" ]; then
        awk -F'\t' -v r="nnrt/$EXTNAME" -v s="$NEW_SHA" 'BEGIN{OFS="\t"} $1==r{$3=s} {print}' \
            "$MAN" > "$MAN.tmp" && mv "$MAN.tmp" "$MAN"
    else
        printf 'nnrt/%s\tnnrt\t%s\n' "$EXTNAME" "$NEW_SHA" >> "$MAN"
    fi
    echo "[MANIFEST] nnrt/$EXTNAME  ${OLD_SHA:-<新>} → $NEW_SHA"
else
    echo "[MANIFEST] nnrt/$EXTNAME  sha 未变"
fi

echo "[DONE] 产物目录: $OUT_DIR"
