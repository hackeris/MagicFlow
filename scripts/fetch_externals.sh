#!/usr/bin/env bash
# fetch_externals.sh —— 下载/准备并校验全部外部输入 → externals/（幂等，可重复执行）。
#
# 产品（均为 gitignore 的构建输入，仓库不存二进制）：
#   externals/skh-run.tar.gz             OHOS aarch64 Python3.12.7+torch2.10.0 栈（544835856B，sha256 pin）
#   externals/comfyui-src/               ComfyUI @03468f4 + patches 应用（OHOS 修改）
#   externals/comfyui-frontend-dist.zip  官方前端 dist v1.54.1（24601619B，sha256 pin）
#   externals/frontend-dist/             dist.zip 解压（stage 注入用；index.html md5 二次锚）
#   externals/py-site/                   纯 py 依赖集（venv-requirements-port.txt 驱动，离线两步）
#   externals/wheels/host/               pip download 缓存（离线可重放）
#
# 用法：
#   bash scripts/fetch_externals.sh           # 全量（已有且 stamp 命中则 SKIP）
#   bash scripts/fetch_externals.sh --offline # 仅用本地副本/缓存，缺即报错
# 本地副本优先级（env 覆盖，全部 size+sha256 校验后才采用）：
#   SKH_RUN_TARBALL=/tmp/tpp/test/skh-run.tar.gz
#   FRONTEND_DIST_ZIP=/tmp/comfyui_frontend_dist.zip
#   COMFYUI_SRC_DIR=/tmp/comfyui-src-p        # 已 patch 的脏目录（跳过 clone+apply，直接指纹校验）
# 任何一项失败 → 汇总报错 exit 1（绝不让后续 stage/zip 用半成品「成功」）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXT="$ROOT/externals"
PINS="$ROOT/config/externals.pins.tsv"
STAMP_DIR="$EXT/.stamps"
PY_REQ="$ROOT/scripts/venv-requirements-port.txt"
VENDOR_WHEELS="$ROOT/config/vendor-wheels"
OFFLINE=0
[ "${1:-}" = "--offline" ] && OFFLINE=1
mkdir -p "$EXT" "$STAMP_DIR"

log() { echo -e "\033[32m[FETCH]\033[0m $*" >&2; }
warn() { echo -e "\033[33m[FETCH:WARN]\033[0m $*" >&2; }
die()  { echo -e "\033[31m[FETCH:ERROR]\033[0m $*" >&2; exit 1; }

sha_of() { sha256sum "$1" | awk '{print $1}'; }
size_of() { stat -c %s "$1"; }

# pin 表读取:pin_get <name> <col>（col: url/sha256/size/type/note）
pin_get() {
    awk -F'\t' -v n="$1" -v c="$2" 'NR>1 && $1==n {print $c; exit}' "$PINS"
}
SKH_SHA="$(pin_get skh-run.tar.gz sha256)";      SKH_SIZE="$(pin_get skh-run.tar.gz size)"
FE_SHA="$(pin_get comfyui-frontend-dist.zip sha256)"; FE_SIZE="$(pin_get comfyui-frontend-dist.zip size)"
FE_HTML_MD5="$(pin_get comfyui-frontend-index.html sha256)"
COMFY_COMMIT="$(pin_get comfyui-src sha256)"
ZIP_SHA="$(pin_get python312.zip sha256)";        ZIP_SIZE="$(pin_get python312.zip size)"

# stamp 幂等:stamp_ok <name> | stamp_set <name>（内容=sha 等关键值,已命中即零成本跳过）
stamp_ok()   { [ -f "$STAMP_DIR/$1.ok" ]; }
stamp_set()  { printf '%s\n' "${2:-ok}" > "$STAMP_DIR/$1.ok"; }

# 通用文件校验：check_file <path> <sha> <size> —— sha 必过，size 为 "-" 时跳过字节校验
check_file() {
    [ -f "$1" ] || return 1
    local s="$(sha_of "$1")" n="$(size_of "$1")"
    if [ "${s}" = "$2" ] && { [ "$3" = "-" ] || [ "$n" = "$3" ]; }; then return 0; fi
    warn "$1 校验不符（exist sha=$(echo "$s"|cut -c1-10) size=$n want sha=$2 size=$3）"
    return 1
}

# ─────────────────────────────── ① skh-run.tar.gz ───────────────────────────────
fetch_skh() {
    log "① skh-run.tar.gz（544MB 级，优先本地副本）"
    if check_file "$EXT/skh-run.tar.gz" "$SKH_SHA" "$SKH_SIZE" 2>/dev/null && stamp_ok skh-run; then log "  [SKIP]"; return; fi
    local DEST="$EXT/skh-run.tar.gz" SRC=""
    if [ "${SKH_RUN_TARBALL:-}" != "" ] && [ -f "$SKH_RUN_TARBALL" ]; then SRC="$SKH_RUN_TARBALL";
    elif [ -f "$DEST" ]; then SRC="$DEST";
    elif [ -f /tmp/tpp/test/skh-run.tar.gz ]; then SRC=/tmp/tpp/test/skh-run.tar.gz; fi
    if [ -n "$SRC" ]; then
        check_file "$SRC" "$SKH_SHA" "$SKH_SIZE" || die "本地副本 $SRC 校验失败（损坏/不同版本）"
        if [ "$SRC" != "$DEST" ]; then cp "$SRC" "$DEST.part" && mv "$DEST.part" "$DEST"; fi
        stamp_set skh-run; log "  [OK] 副本命中: $SRC"; return
    fi
    [ "$OFFLINE" = 1 ] && die "① --offline 无本地副本可用（SKH_RUN_TARBALL=... 可指）/tmp/tpp/test/skh-run.tar.gz"
    if ! git lfs version >/dev/null 2>&1; then
        die "① 需要 git-lfs（仓库 LFS 对象）；且无本地副本。参考: git lfs clone https://gitcode.com/openharmony-robot/thirdparty_pytorch.git /tmp/tpp"
    fi
    # 定向取 LFS：只拉 test/skh-run.tar.gz 一个对象（仓库另有 3 个 sdk 大件）
    local CLONE="$EXT/.gitcode-pp"
    [ -d "$CLONE/.git" ] || git clone --depth 1 "$(pin_get skh-run.tar.gz url)" "$CLONE"
    ( cd "$CLONE" && git lfs fetch --include="test/skh-run.tar.gz" && git lfs checkout --include="test/skh-run.tar.gz" )
    check_file "$CLONE/test/skh-run.tar.gz" "$SKH_SHA" "$SKH_SIZE" || die "① LFS 取到的 skh-run.tar.gz sha/size 不符"
    cp "$CLONE/test/skh-run.tar.gz" "$DEST.part" && mv "$DEST.part" "$DEST"
    stamp_set skh-run; log "  [OK] gitcode LFS"
}

# ─────────────────────────────── ② 前端 dist ───────────────────────────────
fetch_frontend() {
    log "② ComfyUI 前端 dist v1.54.1"
    if check_file "$EXT/comfyui-frontend-dist.zip" "$FE_SHA" "$FE_SIZE" 2>/dev/null && \
       [ "$(md5sum "$EXT/frontend-dist/index.html" 2>/dev/null | awk '{print $1}')" = "$FE_HTML_MD5" ] && stamp_ok frontend
    then log "  [SKIP]"; return; fi
    local ZIP="$EXT/comfyui-frontend-dist.zip"
    if [ "${FRONTEND_DIST_ZIP:-}" != "" ] && [ -f "$FRONTEND_DIST_ZIP" ]; then ZIP="$FRONTEND_DIST_ZIP"; fi
    if check_file "$ZIP" "$FE_SHA" "$FE_SIZE" 2>/dev/null; then
        : # 已有好文件
    else
        [ "$OFFLINE" = 1 ] && die "② --offline 无前端 zip（FRONTEND_DIST_ZIP=/tmp/comfyui_frontend_dist.zip 可指）"
        curl -fsSL -o "$EXT/.fe.part" "$(pin_get comfyui-frontend-dist.zip url)"
        mv "$EXT/.fe.part" "$ZIP"
    fi
    check_file "$ZIP" "$FE_SHA" "$FE_SIZE" || die "② 前端 zip 校验失败"
    # 解压 + index.html 二次锚
    if ! [ -f "$EXT/frontend-dist/index.html" ]; then
        mkdir -p "$EXT/frontend-dist"
        ( cd "$EXT/frontend-dist" && unzip -q -o "$ZIP" )
    fi
    local m="$(md5sum "$EXT/frontend-dist/index.html" | awk '{print $1}')"
    [ "$m" = "$FE_HTML_MD5" ] || die "② 解压后 index.html md5=$m ≠ $FE_HTML_MD5（错误包？）"
    stamp_set frontend; log "  [OK] dist.zip + index.html 锚"
}

# ─────────────────────────────── ③ ComfyUI 源码 + patch ───────────────────────────────
fetch_comfyui_src() {
    log "③ ComfyUI 源码 @$COMFY_COMMIT + OHOS patch"
    local SRC="$EXT/comfyui-src"
    local SEED_REL="comfy/ldm/seedvr/model.py"
    if grep -q '_dynamo_disable' "$SRC/$SEED_REL" 2>/dev/null && stamp_ok comfyui-src; then log "  [SKIP]"; return; fi
    if [ "${COMFYUI_SRC_DIR:-}" != "" ] && [ -f "$COMFYUI_SRC_DIR/$SEED_REL" ]; then
        log "  [本地已 patch 目录] $COMFYUI_SRC_DIR"
        mkdir -p "$SRC" && cp -a "$COMFYUI_SRC_DIR"/. "$SRC/"
        if grep -q '_dynamo_disable' "$SRC/$SEED_REL" 2>/dev/null; then
            stamp_set comfyui-src; log "  [OK] patch 证据串命中（_dynamo_disable）"; return
        else
            warn "  $COMFYUI_SRC_DIR 中未命中 patch 证据 —— 视为未 patch，改用 clone+apply"
        fi
    fi
    if ! grep -q '_dynamo_disable' "$SRC/$SEED_REL" 2>/dev/null; then
        [ "$OFFLINE" = 1 ] && die "③ --offline 无 comfy 源码（COMFYUI_SRC_DIR=/tmp/comfyui-src-p 可指）"
        rm -rf "$SRC"; git clone "$(pin_get comfyui-src url)" "$SRC"
        ( cd "$SRC" && git checkout -q "$COMFY_COMMIT" )
    fi
    local had=""
    if ! grep -q '_dynamo_disable' "$SRC/$SEED_REL" 2>/dev/null; then
        ( cd "$SRC" && git apply --check "$ROOT/patches/comfyui-src-ohos-changes.patch" ) || \
            die "③ patch 应用校验失败（仓库可能与 03468f4 基线漂移）"
        ( cd "$SRC" && git apply "$ROOT/patches/comfyui-src-ohos-changes.patch" )
        had=1
    fi
    # patch 证据链
    ( cd "$SRC" && git diff --stat | grep -q '+' ) || die "③ patch 后无改动"
    grep -q '_dynamo_disable' "$SRC/$SEED_REL" || die "③ 证据串 _dynamo_disable 未命中（patch 内容不符）"
    stamp_set comfyui-src; log "  [OK] clone $(git -C "$SRC" rev-parse --short HEAD 2>/dev/null) + patch"
}

# ─────────────────────────────── ④ 纯 py 依赖站点 ───────────────────────────────
fetch_pysite() {
    log "④ 纯 py 依赖集（venv-requirements-port.txt 驱动）"
    [ -f "$PY_REQ" ] || die "④ 缺 scripts/venv-requirements-port.txt（S4 产物，需先行）"
    [ "$OFFLINE" = 1 ] || {
        if [ ! -f "$EXT/wheels/host/.complete" ]; then
            log "  pip download → externals/wheels/host/"
            rm -rf "$EXT/wheels/host"; mkdir -p "$EXT/wheels/host"
            python3 -m pip download -q --no-deps -r "$PY_REQ" -d "$EXT/wheels/host" \
                --find-links "$VENDOR_WHEELS" \
              || die "④ pip download 失败（无网络时可用 --offline 或预置缓存）"
            touch "$EXT/wheels/host/.complete"
        fi
    }
    # 干净重建 target（防增量漂移）
    rm -rf "$EXT/py-site"; mkdir -p "$EXT/py-site"
    python3 -m pip install -q --no-index --find-links "$EXT/wheels/host" --find-links "$VENDOR_WHEELS" \
        --target "$EXT/py-site" -r "$PY_REQ" \
      || die "④ pip install --target 失败"
    # 防呆:psutil/regex 绝不能混入（psutil 走 stub/psutil.py 注入；regex 有 C 扩展依赖,收纯 py 会带崩）
    for bad in psutil regex; do
        if [ -d "$EXT/py-site/$bad" ] || [ -f "$EXT/py-site/$bad.py" ]; then
            die "④ py-site 里出现了 $bad（应从 requirements 排除；若为 namespace 残留,拆包后清理）"
        fi
    done
    stamp_set py-site; log "  [OK] py-site（目标含 $(ls "$EXT/py-site" | wc -l) 顶层名）"
}

# ─────────────────────────────── ⑤ pydantic_core aarch64 musl wheel（prebuilt 源之一） ───────────────
fetch_pydantic_wheel() {
    log "⑤ pydantic_core-2.46.5 musllinux_1_1_aarch64 wheel"
    local WH="$EXT/wheels-aarch64/pydantic_core-2.46.5-cp312-cp312-musllinux_1_1_aarch64.whl"
    local SHA="$(pin_get pydantic_core-aarch64-musl.whl sha256)" SIZE="$(pin_get pydantic_core-aarch64-musl.whl size)"
    if check_file "$WH" "$SHA" "$SIZE" 2>/dev/null && [ -f "$EXT/pc-wheel-extract/pydantic_core/_pydantic_core.cpython-312-aarch64-linux-musl.so" ]; then
        log "  [SKIP]"; return
    fi
    if [ -f /tmp/wheels/pydantic_core-2.46.5-cp312-cp312-musllinux_1_1_aarch64.whl ]; then
        cp /tmp/wheels/pydantic_core-2.46.5-cp312-cp312-musllinux_1_1_aarch64.whl "$WH.part" && mv "$WH.part" "$WH"
    else
        [ "$OFFLINE" = 1 ] && die "⑤ --offline 无 wheel 副本"
        curl -fsSL -o "$WH.part" "$(pin_get pydantic_core-aarch64-musl.whl url)" && mv "$WH.part" "$WH"
    fi
    check_file "$WH" "$SHA" "$SIZE" || die "⑤ wheel 校验失败"
    [ -f "$EXT/pc-wheel-extract/pydantic_core/_pydantic_core.cpython-312-aarch64-linux-musl.so" ] || {
        rm -rf "$EXT/pc-wheel-extract"
        mkdir -p "$EXT/pc-wheel-extract"
        ( cd "$EXT/pc-wheel-extract" && unzip -q -o "$WH" )
    }
    stamp_set pc-wheel; log "  [OK] wheel + 解包"
}

# ─────────────────────────────── ⑥ 前置产物指纹（zip 锚存档） ───────────────────────────────
check_zip_anchor() {
    local ZP="$ROOT/entry/src/main/resources/rawfile/python312.zip"
    if [ -f "$ZP" ]; then
        check_file "$ZP" "$ZIP_SHA" "$ZIP_SIZE" || warn "  ⚠ 既存 python312.zip 与锚不符（将被 make_py312_zip.py 重建）"
    fi
}

# ───────────────────────────────────── 主流程 ─────────────────────────────────────
# 幂等在每个 fetch_* 内部完成（目标存在性+sha+stamp 全命中才跳）；失败即记录到 FAILED,末尾汇总 exit 1。
FAILED=""
run_or_fail() { local n="$1" f="$2"; $f && log "[DONE] $n" || FAILED="$FAILED $n"; }
run_or_fail skh          fetch_skh
run_or_fail frontend     fetch_frontend
run_or_fail comfyui-src  fetch_comfyui_src
run_or_fail py-site      fetch_pysite
run_or_fail pc-wheel     fetch_pydantic_wheel
check_zip_anchor

if [ -n "$FAILED" ]; then
    die "以下源失败:$FAILED —— 补齐后再继续 stage/zip（绝不带半成品构建）"
fi
log "全部外部输入就绪 ✓（externals/ 见下）"
ls -lh "$EXT" | grep -v '^total'
