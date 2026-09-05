#!/usr/bin/env python3
"""Step 0.4b — 收集「OHOS 形态启动链」第二层运行时文件（comfyui 根 + 新纯 py 依赖）到 staging。

产物落位 build/pyroot-stage/：
  comfyui/**                        ← ComfyUI 0.34.0 仓库根（已打 transformers patch），出 zip 加 comfyui/ 前缀
  lib/python3.12/site-packages/**   ← 新收纯 py 依赖（pydantic/sqlalchemy/tqdm/...），出 zip 并入既有站点包段

铁律：一切 .so 不进 stage（pydantic_core._pydantic_core / _yaml 等走 HAP libs/<abi>/ 链）。
依赖来源 = host 预演 venv（/tmp/hostcv，H3「OHOS 形态启动链」已验证 UP 的那套包版本）。
"""
import hashlib
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 全部外部输入统一来自 externals/（fetch_externals.sh 产物，gitignored）。
# 不再有任何 /tmp 硬编码 —— 可复现：新机器只需先跑 scripts/fetch_externals.sh。
EXT = os.environ.get("EXT_DIR", os.path.join(ROOT, "externals"))
STUB_DIR = os.environ.get("STUB_DIR", os.path.join(ROOT, "stub"))
VENV_SP = os.path.join(EXT, "py-site")          # fetch 的 pip --target 产物（纯 py 依赖集）
COMFY_SRC = os.path.join(EXT, "comfyui-src")    # fetch 的 clone+patch 仓库
FE_SRC = os.path.join(ROOT, "thirdparty/comfyui-frontend/dist")  # 自建 dist: build_frontend.sh 产物
#   (2026-09-05 「F 口」: 官方 dist zip 退场, 前端=fork 源码树 stable tag v1.54.4 构建; docs/frontend-fork-plan.md)
FE_INDEX_MD5 = "570f65255453096137adece937512eca"  # 自建 index.html 锚(源码 v1.54.4+ohos 758f5ba; 与 pins.tsv 一致)
OUT = os.path.join(ROOT, "build/pyroot-stage")

# stage 依赖包（venv site-packages 内顶层名 → 目标 zip 相对名）。psutil 单文件特例见下。
# 全部经 H3「OHOS 形态」启动链验证必需；版本即 venv 实测版本（纯 py，跨 3.12/3.14 通用）。
TOP_PKGS = [
    "pydantic",            # comfy_config / comfy_execution 校验
    "pydantic_core",       # 只收 .py；_pydantic_core CPython-312-.so 走 libs/（铁律）
    "annotated_types",
    "typing_inspection",
    "annotated_doc",
    "sqlalchemy",          # 2.0.52 纯 py（C 扩展已删）——app.assets/services/ingest.py 裸 import
    "tqdm",
    "torchsde",
    "trampoline",          # torchsde 依赖（纯 py 单文件包）——run75：brownian_interval.py:16 裸 import
    "yaml",                # 只收纯 py（_yaml 目录排除；PyYAML 无 C 版时纯 py 回退）
    "comfy_kitchen",       # py3-none-any：backend eager 纯 py 可用
    "comfy_aimdo",         # py3-none-any：ctypes aimdo.so 缺失自动降级
    "comfyui_embedded_docs",
    # run82 — transformers 生态（model 层 text_encoders 硬依赖，5.16.1）：
    #   纯 py 全收；C 层 tokenizers/safetensors 走 libs/（铁律）。
    "transformers",
    "huggingface_hub",
    "tokenizers",          # 剔除 tokenizers.abi3.so（SP_EXCLUDE_SUFFIXES 命中）；纯 py 子包们
    "safetensors",         # 0.8.0 纯 py（.so 额外剔除），rust 侧用 prebuilt 同名 abi3.so
    "typer",               # transformers METADATA 硬依赖
    "click",               # typer 依赖
    "rich",                # typer 依赖（rich 15 纯 py）
    "shellingham",         # typer 依赖
    # run82-2 — requests 生态：generation/utils.py:76 → continuous_batching/cache_manager →
    #   requests.py 顶格 `import requests`（GenerationMixin 主链）。全纯 py（charset_normalizer
    #   的 C 加速 .so 被 SP_EXCLUDE_SUFFIXES 过滤，自动降级纯 py；urllib3 2.x 纯 py）。
    "requests",
    "urllib3",
    "certifi",
    "idna",
    "charset_normalizer",
    # run82-3 — aiohttp 生态（2026-09-03 正规化补齐：旧 zip 内含其 133 条/run18 达成;
    #   标准 skh tar 不含,pypi 同版本(v=venv 实测)收纯 py —— attr/ 由 attrs wheel 自带）
    "aiohttp",
    "aiohappyeyeballs",
    "aiosignal",
    "attrs",
    "attr",   # attrs wheel 同时发 attr/ 兼容目录（aiohttp/client.py:32 `import attr`）——
               #   2026-09-03 真机死点实证:只收 attrs 时 import attr 直接 ModuleNotFoundError
               #   （attrs 26.x 仍保双名发行;旧 zip 亦有 attr/ 14 条）
    # 2026-09-03 真机 run 补齐（aiohttp 硬依赖; 缺 .so 时各包自动回退纯 py）:
    #   multidict/propcache/yarl/frozenlist（版本见 venv-requirements-port.txt）
    "multidict",
    "propcache",
    "yarl",
    "frozenlist",
    # regex ⚠ 绝不收集：2026.9.3 _main.py:429 硬 import C 扩展 _regex（纯 py 不可用、
    #   仅 9 边缘文件用；若收纯 py 树不匹配 so，import regex 反而带崩主链）——由 zip 侧
    #   AUTODOC_STUB（transformers/utils/auto_docstring.py 空壳替换）绕开其唯一主链引用点。
]

# comfyui 仓库根排除项（运行时不需要）；根级权重目录 models 也是
# （⚠ run70 教训：copytree_filter 按目录名匹配，"models" 曾把 comfy/ldm/models
#   子目录一并误杀 → ModuleNotFoundError: comfy.ldm.models → rc=-1。
#   ⚠ run96（2026-09-03）：root_only 只拦根级同名，非根级同名子目录（comfy_api/input/
#   官方转换层包）必须放行 —— 此前 "input" 在全局排除里把 comfy_api/input 整树误杀
#   → run17 死点 'No module named comfy_api.input'（run93 的自写兼容层已废弃，
#   回归官方原版：input/{__init__,basic_types,video_types}.py））
SRC_EXCLUDE_DIRS = {".git", "__pycache__", ".github"}
# 仅仓库根级排除（不受同名子目录影响；含根级 input/output/user 数据目录与 models 权重）
SRC_ROOT_ONLY = {"input", "output", "user", "models"}
# 站点包内排除后缀（铁律 + 杂项）
SP_EXCLUDE_SUFFIXES = (".so", ".a", ".o", ".pyc", ".pyo", ".pth")
SP_EXCLUDE_DIRS = {"__pycache__", "libs", "include", "tests", "_yaml", ".libs", "data"}


def copytree_filter(src, dst, exclude_dirs, exclude_suffixes, root_only=()):
    """带过滤的 copytree：保留目录结构，跳过排除项；root_only 仅根层生效。"""
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        if rel == ".":
            dirs[:] = [d for d in dirs if d not in exclude_dirs and d not in root_only]
        else:
            dirs[:] = [d for d in dirs if d not in exclude_dirs]
        tgt = os.path.join(dst, rel) if rel != "." else dst
        os.makedirs(tgt, exist_ok=True)
        for f in files:
            if f.endswith(exclude_suffixes):
                continue
            shutil.copy2(os.path.join(root, f), os.path.join(tgt, f))


def copy_dist_info(sp, pkg):
    """拷贝 .dist-info/METADATA（版本可查），大件忽略。"""
    for d in os.listdir(sp):
        if d.startswith(f"{pkg}-") and d.endswith(".dist-info"):
            md = os.path.join(sp, d, "METADATA")
            if os.path.exists(md):
                dstdir = os.path.join(OUT, "lib/python3.12/site-packages", d)
                os.makedirs(dstdir, exist_ok=True)
                shutil.copy2(md, os.path.join(dstdir, "METADATA"))
            return


def main():
    if not os.path.isdir(VENV_SP):
        print(f"FATAL: 纯 py 依赖集不存在 {VENV_SP}\n"
              f"       请先: bash scripts/fetch_externals.sh", file=sys.stderr)
        return 1
    if not os.path.isdir(COMFY_SRC):
        print(f"FATAL: comfy 源码不存在 {COMFY_SRC}（fetch_externals.sh 的 clone+patch 产物）", file=sys.stderr)
        return 1
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)

    # 1) comfyui 仓库根
    copytree_filter(COMFY_SRC, os.path.join(OUT, "comfyui"), SRC_EXCLUDE_DIRS, (".pyc",), SRC_ROOT_ONLY)
    # 1b) frontend_static —— 注入自建 dist（thirdparty/comfyui-frontend/dist, 源码 stable tag 构建）。
    #   ⚠ fail-fast：官方 dist 缺失即整体失败（不存在「占位页继续」的降级 —— 那会导致
    #   HAP 装上后白板站点，是最难排查的假成功）。
    fe_dir = os.path.join(OUT, "comfyui/frontend_static")
    fe_index = os.path.join(FE_SRC, "index.html")
    if os.path.isfile(fe_index):
        shutil.rmtree(fe_dir, ignore_errors=True)
        shutil.copytree(FE_SRC, fe_dir)
        m = hashlib.md5(open(fe_index, "rb").read()).hexdigest()
        if m != FE_INDEX_MD5:
            print(f"  FATAL: frontend_static/index.html md5={m} ≠ 锚 {FE_INDEX_MD5}（错误包? 重跑 fetch_externals.sh）",
                  file=sys.stderr)
            return 2
        print(f"  [OK] frontend_static = 自建 frontend dist（index.html 锚 ✓）")
    else:
        print(f"  FATAL: 前端 dist 缺失 {FE_SRC}（请先: bash scripts/build_frontend.sh --skip-install）", file=sys.stderr)
        return 2
    # 2) 纯 py 依赖 → 站点包段
    spout = os.path.join(OUT, "lib/python3.12/site-packages")
    for pkg in TOP_PKGS:
        src = os.path.join(VENV_SP, pkg)
        if not os.path.exists(src):
            print(f"  !! 缺 {pkg}（venv 没有，跳过）", file=sys.stderr)
            continue
        copytree_filter(src, os.path.join(spout, pkg), SP_EXCLUDE_DIRS, SP_EXCLUDE_SUFFIXES)
        copy_dist_info(VENV_SP, pkg)
        print(f"  [OK] {pkg}")
    # 3) 单文件包特例
    #    psutil：⚠ 绝不能从 py-site 收（真包带 _psutil_linux.so,滤 .so 后 import 崩）。
    #    走仓库 stub/psutil.py（OHOS 兼容层,原 /tmp/hostcv/stub_psutil.py 入库）—— 与
    #    zip 侧注入的 sitecustomize/stub_global 同源,都在 stub/ 一目了然。
    stub_psutil = os.path.join(STUB_DIR, "psutil.py")
    if os.path.isfile(stub_psutil):
        shutil.copy2(stub_psutil, os.path.join(spout, "psutil.py"))
        print("  [OK] psutil.py（stub/ 注入）")
    else:
        print(f"  FATAL: stub/psutil.py 缺失", file=sys.stderr)
        return 2
    FILE_PKGS = ["simpleeval.py"]  # venv 单文件实现（requirements 已 pin 1.0.7）
    for fn in FILE_PKGS:
        src_f = os.path.join(VENV_SP, fn)
        if os.path.exists(src_f):
            shutil.copy2(src_f, os.path.join(spout, fn))
            print(f"  [OK] {fn}")
        else:
            print(f"  !! 缺 {fn}", file=sys.stderr)
    # 4) typing_extensions.py：skh 栈已有（python312.zip 校验项），不重复收集。
    #    （stage 侧若曾重复收集会导致 zip 重复条目 Warning——以 skh 版为准。）

    # 校验
    bad = []
    for root, dirs, files in os.walk(OUT):
        for f in files:
            if f.endswith(".so"):
                bad.append(os.path.join(root, f))
    checks = {
        "comfyui/main.py": os.path.isfile(os.path.join(OUT, "comfyui/main.py")),
        "comfyui/server.py": os.path.isfile(os.path.join(OUT, "comfyui/server.py")),
        "comfyui/comfy/__init__.py 不存在（namespace）":
            not os.path.isfile(os.path.join(OUT, "comfyui/comfy/__init__.py")),
        # run96 教训:comfy_api/input 官方兼容层包必须进（_io.py 引用 comfy_api.input）
        "comfyui/comfy_api/input/__init__.py（官方 input 包）":
            os.path.isfile(os.path.join(OUT, "comfyui/comfy_api/input/__init__.py")),
        "frontend_static/index.html（自建 dist 锚, v1.54.4）":
            os.path.isfile(os.path.join(OUT, "comfyui/frontend_static/index.html")),
        # W3 轻量模板(patch 17, docs/model-download.md): 模板目录随源码树拷入,
        #   缺失说明 patch 17 未应用/旧树缺包 —— 必须 fail-fast
        "comfyui/templates/index.json（W3 轻量模板, patch 17）":
            os.path.isfile(os.path.join(OUT, "comfyui/templates/index.json")),
        "site-packages/psutil.py（stub 注入）":
            os.path.isfile(os.path.join(OUT, "lib/python3.12/site-packages/psutil.py")),
        "0 个 .so 混入（铁律）": not bad,
    }
    for k, v in checks.items():
        print(f"  [{'OK' if v else 'FAIL'}] {k}")
    if bad:
        print("  !! 铁律违反，.so 名单：", bad, file=sys.stderr)
    return 0 if all(checks.values()) else 2


if __name__ == "__main__":
    sys.exit(main())
