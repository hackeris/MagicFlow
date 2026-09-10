#!/usr/bin/env bash
# fetch_externals.sh —— 下载/准备并校验全部外部输入 → externals/（幂等，可重复执行）。
#
# 产品（均为 gitignore 的构建输入，仓库不存二进制）：
#   externals/skh-run.tar.gz             OHOS aarch64 Python3.12.7+torch2.10.0 栈（544835856B，sha256 pin）
#   externals/comfyui-src/               ComfyUI @03468f4 + patches 应用（OHOS 修改）
#   thirdparty/comfyui-frontend/        前端源码(submodule) @稳定 tag v1.54.4 —— 自建 dist 之源
#                                        （build_frontend.sh; 官方 dist zip 已退场, 回退 pin 条目仍在）
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

# pin 表读取:pin_get <name> <col>（col: type/url/sha256/size/note; col 号=该列序号）
# 注: awk -F'\t' 转义在不同 awk 实现不可靠,用 python 读制表符列
pin_get() {
    python3 - "$1" "$2" "$PINS" <<'EOF'
import sys
name, colname, pins = sys.argv[1], sys.argv[2], sys.argv[3]
COLS = {"type": 2, "url": 3, "sha256": 4, "size": 5, "note": 6}
for line in open(pins, encoding="utf-8"):
    line = line.rstrip("\n")
    if not line or line.startswith("#"):
        continue
    f = line.split("\t")
    if f[0] == name:
        print(f[COLS[colname] - 1])
        break
EOF
}
SKH_SHA="$(pin_get skh-run.tar.gz sha256)";      SKH_SIZE="$(pin_get skh-run.tar.gz size)"
COMFY_COMMIT="$(pin_get comfyui-src sha256)"
ZIP_SHA="$(pin_get python312.zip sha256)"   # = manifest 的 sorted-namelist sha256(确定性锚; zip 字节 sha 跨次漂移, 不作锚)

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

# ─────────────────────────────── ② 前端源码(自维护 fork; 官方 dist zip 退场) ───────────────────────────────
fetch_frontend() {
    log "② ComfyUI 前端源码(C) @v1.54.4(稳定 tag, 非 nightly)"
    local FE="$ROOT/thirdparty/comfyui-frontend"
    local PIN="$(pin_get comfyui-frontend-src sha256)"
    # [SKIP] 条件: 树在且 HEAD=pin commit
    if [ -d "$FE/.git" ] && [ "$(git -C "$FE" rev-parse HEAD 2>/dev/null)" = "$PIN" ] && stamp_ok frontend-src; then
        log "  [SKIP] 前端源码 $PIN"
        return
    fi
    if [ -d "$FE/.git" ]; then
        # 已有树(可能由 submodule 链带出): 对齐 pin commit
        git -C "$FE" fetch --depth 1 origin "$PIN" 2>&1 | tail -1 || true
        git -C "$FE" checkout "$PIN" 2>&1 | tail -1 || die "② 前端源码 checkout $PIN 失败"
    else
        # 无树: 经 submodule 初始化(主仓 .gitmodules 条目; 深度 1)
        ( cd "$ROOT" && git submodule update --init --depth 1 thirdparty/comfyui-frontend ) || \
            die "② 前端源码 submodule 初始化失败"
        [ "$(git -C "$FE" rev-parse HEAD)" = "$PIN" ] || \
            ( git -C "$FE" fetch --depth 1 origin "$PIN" && git -C "$FE" checkout "$PIN" ) || \
            die "② 前端源码对齐 pin($PIN) 失败"
    fi
    local H="$(git -C "$FE" rev-parse HEAD)"
    [ "$H" = "$PIN" ] || die "② 前端源码 head=$H ≠ pin=$PIN"
    stamp_set frontend-src; log "  [OK] 前端源码 @${H:0:12}（构建见 make frontend → build_frontend.sh）"
}

# ─────────────────────────────── ③ ComfyUI 源码 + patch ───────────────────────────────
# 全部 OHOS patch(03 主/15 smoke/16 模型下载/17 模板路由/18 区域化 catalog)证据命中判定 ——
# SKIP 与"本地已 patch 目录"共用, 防旧树缺新 patch 被 SKIP 短路(2026-09-05 W3; 09-08 P0 +18)。
ohos_patch_applied() { # $1=SRC 树
    grep -q '_dynamo_disable' "$1/comfy/ldm/seedvr/model.py" 2>/dev/null \
        && grep -q 'OHOS_MODEL_DL v1' "$1/server.py" 2>/dev/null \
        && grep -q 'OHOS_TEMPLATES_DEFAULT_DIR v1' "$1/server.py" 2>/dev/null \
        && grep -q 'OHOS_DL_REGION v1' "$1/server.py" 2>/dev/null \
        && grep -q 'OHOS_DL_SSL v1' "$1/server.py" 2>/dev/null \
        && grep -q 'OHOS_DL_ESRGAN_MIRROR v1' "$1/server.py" 2>/dev/null \
        && grep -q 'OHOS_DL_SDTURBO_SRC v1' "$1/server.py" 2>/dev/null \
        && [ -f "$1/templates/index.json" ]
}
fetch_comfyui_src() {
    log "③ ComfyUI 源码 @$COMFY_COMMIT + OHOS patch"
    local SRC="$EXT/comfyui-src"
    local SEED_REL="comfy/ldm/seedvr/model.py"
    if ohos_patch_applied "$SRC" && stamp_ok comfyui-src; then log "  [SKIP]"; return; fi
    if [ "${COMFYUI_SRC_DIR:-}" != "" ] && [ -f "$COMFYUI_SRC_DIR/$SEED_REL" ]; then
        log "  [本地已 patch 目录] $COMFYUI_SRC_DIR"
        mkdir -p "$SRC" && cp -a "$COMFYUI_SRC_DIR"/. "$SRC/"
        if ohos_patch_applied "$SRC"; then
            stamp_set comfyui-src; log "  [OK] patch 证据全部命中"; return
        fi
        if grep -q '_dynamo_disable' "$SRC/$SEED_REL" 2>/dev/null; then
            # 缺新 patch 的旧树: 不提前 return, 落 ③c 逐缺补齐链(16/17/18 按缺 apply —— 09-08 P0)
            warn "  $COMFYUI_SRC_DIR 缺新 patch(证据不全), 走补齐链"
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
    # ③c W3 模型下载/模板(patch 16/17, docs/model-download.md): 逐 patch 幂等 —— 旧树(仅 03/15)
    #   在此补齐; 证据短板独立判定, 某 patch 失败必须死(防"缺功能仍继续"的假成功)。
    #   ⚠ 应用顺序: 17 必须先于 16 —— 17 是"自包含件"(server.py hunk 全含 16 的 OHOS 段 +
    #   templates 新文件, 2026-09-05 W3); 已 16 未 17 的树直接 apply 17 会在 server.py 上
    #   冲突(2026-09-08 从零重放演练实测)。故 16 实际只兜"已 17 且 v1 缺失"的旧树。
    if ! grep -q 'OHOS_TEMPLATES_DEFAULT_DIR v1' "$SRC/server.py" 2>/dev/null || \
       [ ! -f "$SRC/templates/index.json" ]; then
        ( cd "$SRC" && git apply "$ROOT/patches/17-ohos-workflow-templates.patch" ) || \
            die "③c patch 17(模板)应用失败"
        log "  [OK] 模板(17) applied"
    fi
    if ! grep -q 'OHOS_MODEL_DL v1' "$SRC/server.py" 2>/dev/null; then
        ( cd "$SRC" && git apply "$ROOT/patches/16-ohos-model-download.patch" ) || \
            die "③c patch 16(模型下载)应用失败"
        log "  [OK] 模型下载(16) applied"
    fi
    # ③d 区域化 catalog(P0, 2026-09-08, docs/comfyui-cn-cloud-strategy.md):
    #   主源换国内镜像(modelscope/hf-mirror) + 预量化变体条目; 幂等, 缺即 apply(独立于 16)。
    if ! grep -q 'OHOS_DL_REGION v1' "$SRC/server.py" 2>/dev/null; then
        ( cd "$SRC" && git apply "$ROOT/patches/18-ohos-catalog-cn.patch" ) || \
            die "③d patch 18(区域化 catalog)应用失败"
        log "  [OK] 区域化 catalog(18) applied"
    fi
    # ③e 下载 SSL 修复(P0.1, 2026-09-09 真机实锤): 设备无系统 CA 目录 → urlopen 默认
    #   context 证书校验失败; 显式 certifi bundle。幂等, 缺即 apply(独立于 18)。
    if ! grep -q 'OHOS_DL_SSL v1' "$SRC/server.py" 2>/dev/null; then
        ( cd "$SRC" && git apply "$ROOT/patches/19-ohos-dl-ssl.patch" ) || \
            die "③e patch 19(下载 SSL)应用失败"
        log "  [OK] 下载 SSL(19) applied"
    fi
    # ③f RealESRGAN 源换镜像(P0.2, 2026-09-10 真机实锤): github release 设备侧 ~36KB/s
    #   (67MB 需 30min) → hf-mirror 同名重传(12s); 幂等, 缺即 apply(独立于 18/19)。
    if ! grep -q 'OHOS_DL_ESRGAN_MIRROR v1' "$SRC/server.py" 2>/dev/null; then
        ( cd "$SRC" && git apply "$ROOT/patches/20-ohos-esrgan-mirror.patch" ) || \
            die "③f patch 20(esrgan 镜像源)应用失败"
        log "  [OK] esrgan 镜像源(20) applied"
    fi
    # ③g SD-Turbo 源档位修正(P0.3, 2026-09-10 真机实锤): MS 版=fp32 5.2GB(12G OOM 风险)
    #   → 主源换 hf-mirror fp16 2.6GB, MS fp32 降 url_alt(24G+ 快源); 幂等独立。
    if ! grep -q 'OHOS_DL_SDTURBO_SRC v1' "$SRC/server.py" 2>/dev/null; then
        ( cd "$SRC" && git apply "$ROOT/patches/21-ohos-sdturbo-src.patch" ) || \
            die "③g patch 21(sd-turbo 源档位)应用失败"
        log "  [OK] sd-turbo 源档位(21) applied"
    fi
    # ③b smoke 自检节点独立 patch(2026-09-05, docs/smoke-design.md): 幂等——节点文件在即 skip
    #   2026-09-05 补丁含 __init__.py(ComfyUI 目录型节点必带); 老版树(仅 custom_node.py)按产物补齐
    if ! grep -q 'OHOS_SmokeBench' "$SRC/custom_nodes/ohos_smoke/custom_node.py" 2>/dev/null; then
        ( cd "$SRC" && git apply "$ROOT/patches/15-ohos-smoke-bench-node.patch" ) || \
            die "③b smoke 节点 patch 15 应用失败"
        log "  [OK] smoke 节点(15) applied"
    elif [ ! -f "$SRC/custom_nodes/ohos_smoke/__init__.py" ] || \
         ! grep -q 'OUTPUT_NODE = True' "$SRC/custom_nodes/ohos_smoke/custom_node.py" 2>/dev/null; then
        # 老版 15 已打的树(缺 __init__.py / custom_node.py 无 OUTPUT_NODE): 按文件补齐
        git apply --include='custom_nodes/ohos_smoke/__init__.py' "$ROOT/patches/15-ohos-smoke-bench-node.patch" 2>/dev/null || true
        if ! grep -q 'OUTPUT_NODE = True' "$SRC/custom_nodes/ohos_smoke/custom_node.py" 2>/dev/null; then
            rm -f "$SRC/custom_nodes/ohos_smoke/custom_node.py"
            ( cd "$SRC" && git apply --include='custom_nodes/ohos_smoke/custom_node.py' "$ROOT/patches/15-ohos-smoke-bench-node.patch" ) || \
                die "③b smoke 节点 custom_node.py 升级失败"
        fi
        log "  [OK] smoke 节点(15) 增量补齐"
    fi
    stamp_set comfyui-src; log "  [OK] clone $(git -C "$SRC" rev-parse --short HEAD 2>/dev/null) + patch"
}

# ─────────────────────────────── ④ 纯 py 依赖站点 ───────────────────────────────
fetch_pysite() {
    log "④ 纯 py 依赖集（venv-requirements-port.txt 驱动）"
    [ -f "$PY_REQ" ] || die "④ 缺 scripts/venv-requirements-port.txt（S4 产物，需先行）"
    [ "$OFFLINE" = 1 ] || {
        # requirements 变更(mtime 新于完成戳)即重 download —— 防旧缓存缺新包
        if [ ! -f "$EXT/wheels/host/.complete" ] || \
           [ "$(stat -c %Y "$PY_REQ")" -gt "$(stat -c %Y "$EXT/wheels/host/.complete")" ]; then
            log "  pip download → externals/wheels/host/"
            rm -rf "$EXT/wheels/host"; mkdir -p "$EXT/wheels/host"
            python3 -m pip download -q --no-deps -r "$PY_REQ" -d "$EXT/wheels/host" \
                --find-links "$VENDOR_WHEELS" \
              || die "④ pip download 失败（无网络时可用 --offline 或预置缓存）"
            touch "$EXT/wheels/host/.complete"
        fi
    }
    # 干净重建 target（防增量漂移）
    # --no-deps: requirements 是精确全集(含所有传递依赖), 不做依赖树解析——
    #   否则 pip 会要求 typing-extensions 等"我们刻意不收集"的包(其由 skh 树提供)
    rm -rf "$EXT/py-site"; mkdir -p "$EXT/py-site"
    python3 -m pip install -q --no-index --no-deps --find-links "$EXT/wheels/host" --find-links "$VENDOR_WHEELS" \
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
    mkdir -p "$EXT/wheels-aarch64"
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
    # 锚 = manifest 的 sorted_namelist_sha256(确定性: 条目名集合 + 逐条内容 sha 可复现)。
    # zip 字节 sha 因 mtime/写入顺序跨次漂移 —— 2026-09-11 实测: 同源两次重建
    # 40b11f72/000349a0 不同, 而 sorted 锚 f28fdda2 一致; 故不做字节校验。
    local ZP="$ROOT/entry/src/main/resources/rawfile/python312.zip"
    local MF="$ROOT/docs/manifests/python312.zip.manifest.gz"
    [ -f "$ZP" ] || return 0
    [ -f "$MF" ] || { warn "  ⚠ 缺 $MF, python312.zip 锚无法校验"; return 0; }
    local got
    got="$(MF="$MF" python3 -c 'import gzip,json,os;print(json.loads(gzip.open(os.environ["MF"],"rb").read())["sorted_namelist_sha256"])' 2>/dev/null)"
    if [ "$got" != "$ZIP_SHA" ]; then
        warn "  ⚠ 既存 python312.zip 锚不符（manifest sorted=$(echo "$got" | cut -c1-10) want=$(echo "$ZIP_SHA" | cut -c1-10)）—— make_py312_zip.py 重建"
    fi
}

# ─────────────────────────────── ⑦ G4 源码三件套（pytorch/openblas/host-py312） ───────────────────────────────
# 2026-09-05 G4 定谳补 pin: build_torch_ohos.sh / build_openblas_ohos.sh 的源码输入,
#   原本只存在研究区 externals/ 本地状态(未 pin) → 全链复现断点; 现入表并自动 fetch。
fetch_pytorch_src() {
    log "⑦ pytorch-src v2.10.0 @$(pin_get pytorch-src sha256 | cut -c1-7)"
    local SRC="$EXT/pytorch-src"
    if [ -d "$SRC/.git" ] && git -C "$SRC" rev-parse HEAD 2>/dev/null | grep -q "$(pin_get pytorch-src sha256)" \
       && [ "$(git -C "$SRC" submodule status 2>/dev/null | wc -l)" = "37" ]; then
        log "  [SKIP]"; return
    fi
    [ "$OFFLINE" = 1 ] && die "⑦ --offline 无 pytorch 源码"
    rm -rf "$SRC"
    # ⚠ 必须浅克隆(主仓): pin commit=v2.10.0 tag 头, 浅 HEAD 即匹配;
    #   git apply 补丁不需历史。全量=4.1GB(实测), 浅克隆≈2GB(省一半——2026-09-05 用户纠偏)。
    git clone --branch v2.10.0 --depth 1 "$(pin_get pytorch-src url)" "$SRC"
    git -C "$SRC" submodule update --init --recursive --depth 1
    git -C "$SRC" checkout -q "$(pin_get pytorch-src sha256)" 2>/dev/null || {
        git -C "$SRC" fetch -q --tags && git -C "$SRC" checkout -q "$(pin_get pytorch-src sha256)"
    }
    log "  [OK] $(git -C "$SRC" rev-parse --short HEAD) submodules=$(git -C "$SRC" submodule status 2>/dev/null | wc -l)"
}

fetch_openblas_src() {
    log "⑦ openblas-src v0.3.29 @$(pin_get openblas-src sha256 | cut -c1-7)"
    local SRC="$EXT/openblas-src"
    if [ -d "$SRC/.git" ] && git -C "$SRC" rev-parse HEAD 2>/dev/null | grep -q "$(pin_get openblas-src sha256)"; then
        log "  [SKIP]"; return
    fi
    [ "$OFFLINE" = 1 ] && die "⑦ --offline 无 openblas 源码"
    rm -rf "$SRC"
    git clone --branch v0.3.29 "$(pin_get openblas-src url)" "$SRC"
    log "  [OK] $(git -C "$SRC" rev-parse --short HEAD)"
}

fetch_py312_host() {
    log "⑦ host py312 源码 tar（src 自编 → /opt/py312）"
    local WH="$EXT/py312/Python-3.12.7.tgz"
    if check_file "$WH" "$(pin_get py312-host sha256)" "$(pin_get py312-host size)" 2>/dev/null; then
        log "  [SKIP]"; return
    fi
    [ "$OFFLINE" = 1 ] && die "⑦ --offline 无 py312 tar"
    mkdir -p "$EXT/py312"
    curl -fsSL -o "$EXT/py312/.part" "$(pin_get py312-host url)"
    mv "$EXT/py312/.part" "$WH"
    check_file "$WH" "$(pin_get py312-host sha256)" "$(pin_get py312-host size)" || die "⑦ py312 tar 校验失败"
    log "  [OK] 27MB tar 落地 $WH（configure 编译由 bootstrap_worktree.sh 做）"
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
run_or_fail pytorch-src  fetch_pytorch_src
run_or_fail openblas-src fetch_openblas_src
run_or_fail py312-host   fetch_py312_host
check_zip_anchor

if [ -n "$FAILED" ]; then
    die "以下源失败:$FAILED —— 补齐后再继续 stage/zip（绝不带半成品构建）"
fi
log "全部外部输入就绪 ✓（externals/ 见下）"
ls -lh "$EXT" | grep -v '^total'
