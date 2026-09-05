#!/usr/bin/env bash
# bless_manifest.sh —— 产物变更后,把 prebuilt_manifest.tsv 的 sha 校准到当前实际产物。
# 定位: OpenBLAS/torch 重编后所有 torch 系 sha 都会变 → 手工逐行替换易错;
#   本脚本按 manifest 的 tag↔src_dir 映射逐行重算(只动 sha 列; 行其余不动),列出变更。
# 用途: 重编产物后 [bash scripts/bless_manifest.sh], 再 make hap(collect 225 闭环通过)。
#   只信任"构建链产物"; 不适用于人为篡改(那是另一回事, collect 会拒绝烂锚)。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
M="$ROOT/config/prebuilt_manifest.tsv"
TAGS=(torch-ohos torch-ohos-root)

# 与 collect_prebuilt.sh src_dir 同源(不要复制逻辑, 来源只此一处)
src_dir() {
    case "$1" in
        torch-ohos) echo "$ROOT/build/torch-ohos-install/usr/lib/python3.12/site-packages/torch/lib" ;;
        torch-ohos-root) echo "$ROOT/build/torch-ohos-install/usr/lib/python3.12/site-packages/torch" ;;
        *) echo "" ;;
    esac
}

python3 - "$ROOT" "$M" <<'EOF'
import hashlib, os, sys
ROOT, M = sys.argv[1], sys.argv[2]
TAGS = {"torch-ohos": f"{ROOT}/build/torch-ohos-install/usr/lib/python3.12/site-packages/torch/lib",
        "torch-ohos-root": f"{ROOT}/build/torch-ohos-install/usr/lib/python3.12/site-packages/torch"}
lines = open(M).read().splitlines()
out, changed = [], 0
for ln in lines:
    parts = ln.split("\t")
    if len(parts) >= 3 and parts[1] in TAGS:
        f = os.path.join(TAGS[parts[1]], os.path.basename(parts[0]))
        if os.path.isfile(f):
            s = hashlib.sha256(open(f,'rb').read()).hexdigest()
            if s != parts[2]:
                print(f"  [BLESS] {parts[0]} {parts[2][:12]} → {s[:12]}")
                parts[2] = s; changed += 1
                ln = "\t".join(parts)
    out.append(ln)
open(M,'w').write("\n".join(out)+"\n")
print(f"done. updated={changed}")
EOF
