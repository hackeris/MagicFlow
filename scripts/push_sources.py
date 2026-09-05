#!/usr/bin/env python3
"""双区源码镜像同步:研究区(comfy-ohos-port) ↔ 正规区(/data/share/comfyui)。

白名单镜像：仅同步以下子树/文件;白名单区内「源端不存在即删除」(镜像语义)。
默认 dry-run;--apply 真干。产物(prebuilt/python312.zip/externals/.ohos/local.properties)
全部排除 —— 构建产物一律在正规区侧由脚本重建(锚校验),绝不跨区拷贝。

⚠ 方向裁决（2026-09-03 S13 达成后）:正规化/复现链已完成且修复都在正规区侧
  （run86 注入/aiohttp pin/zip 新锚等），此后一律用 `--reverse`(正规区 → 研究区)
  让调试现场追平正主；默认方向(研究区→正规区)仅在研究区有新调试成果才用,
  且会覆盖正规区侧新修复,慎用。自身脚本双区互备(见删除豁免)。

用法: python3 scripts/push_sources.py [--apply] [--reverse]
"""
import os
import shutil
import sys

REVERSE = "--reverse" in sys.argv
if REVERSE:
    SRC = "/data/share/comfyui"          # 正主
    DST = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # comfy-ohos-port
else:
    SRC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # comfy-ohos-port
    DST = "/data/share/comfyui"
APPLY = "--apply" in sys.argv

# 白名单(目录前缀/文件) —— 与仓库 .gitignore 边界一致
KEEP_DIRS = (
    "entry/src/main/",     # 子树排除见下
    "thirdparty/ohos-torch/",  # G4 定谳: OHOS torch/OpenBLAS 交叉构建脚本+补丁+stub(2026-09-05)
    "stub/",
    "scripts/",
    "patches/",
    "docs/",
    "AppScope/",
    "hvigor/",
    "config/",
)
KEEP_FILES = (
    "Makefile", "build-profile.json5", "code-linter.json5", "hvigorfile.ts",
    "oh-package.json5", "oh-package-lock.json5", "AGENTS.md", ".gitignore",
    ".gitmodules",
)
# 白名单内的排除(产物/机器态)
SKIP_SUBPATHS = (
    "entry/src/main/cpp/prebuilt/",
    "entry/src/main/resources/rawfile/python312.zip",
    "entry/src/main/resources/rawfile/bin/",
    "entry/src/test/", "entry/src/ohosTest/", "entry/src/mock/",
)


def in_whitelist(rel, is_dir):
    """rel(目录或文件)属于白名单/白名单父链(entry/、entry/src/ 等中间目录)即 True。"""
    if rel in KEEP_FILES:
        return True
    for d in KEEP_DIRS:
        d0 = d.rstrip("/")
        if rel == d0 or rel.startswith(d) or d0.startswith(rel.rstrip("/") + "/"):
            return True
    # entry 顶层 json5/hvigorfile 等文件
    if rel.startswith("entry/") and rel.count("/") == 1 and not is_dir:
        return True
    return False


def skipped(rel, is_dir):
    if is_dir:
        return rel.rstrip("/") + "/" in SKIP_SUBPATHS or rel + "/" in SKIP_SUBPATHS
    return rel in SKIP_SUBPATHS or any(rel.startswith(s) for s in SKIP_SUBPATHS)


def collect(src_root):
    """收集 SRC 白名单 rel 集。"""
    src_rel = set()
    for root, dirs, files in os.walk(src_root):
        dirs[:] = [d for d in dirs if not d.startswith(".") and d != "__pycache__"]
        rel = os.path.relpath(root, src_root)
        if rel != ".":
            # 目录级别过滤
            r = rel.replace(os.sep, "/") + "/"
            if skipped(r, True):
                dirs[:] = []
                continue
            if not in_whitelist(r, True):
                dirs[:] = []
                continue
        for f in files:
            r = (rel.replace(os.sep, "/") + "/" + f) if rel != "." else f
            if skipped(r, False) or not in_whitelist(r, False):
                continue
            src_rel.add(r)
    return src_rel


def main():
    src_rel = collect(SRC)
    changes = 0
    # 推送
    for r in sorted(src_rel):
        sp = os.path.join(SRC, r)
        dp = os.path.join(DST, r)
        if os.path.isfile(dp) and os.path.getsize(dp) == os.path.getsize(sp) and \
           open(dp, "rb").read() == open(sp, "rb").read():
            continue
        changes += 1
        print(f"{'[DRY]' if not APPLY else 'PUSH'}: {r}")
        if APPLY:
            os.makedirs(os.path.dirname(dp), exist_ok=True)
            shutil.copy2(sp, dp)
    # 删除感知:DST 白名单区内的多余文件
    for root, dirs, files in os.walk(DST):
        dirs[:] = [d for d in dirs if d not in (".git", "build", ".hvigor", ".ohos", "externals", ".cxx")
                   and d != "__pycache__"]
        rel = os.path.relpath(root, DST).replace(os.sep, "/")
        for f in files:
            r = (rel + "/" + f) if rel != "." else f
            if not in_whitelist(r, False) or skipped(r, False):
                continue
            if r == "scripts/push_sources.py":
                continue  # ⚠ 自身豁免:同步脚本双区互备,不得因源端暂缺被镜像删除
            if r not in src_rel and not os.path.exists(os.path.join(SRC, r)):
                changes += 1
                print(f"{'[DRY]' if not APPLY else 'DEL '}: {r}")
                if APPLY:
                    os.remove(os.path.join(root, f))
    print(f"done. （{'DRY-RUN' if not APPLY else 'APPLIED'}）{changes} 项")


if __name__ == "__main__":
    main()
