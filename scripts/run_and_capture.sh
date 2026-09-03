#!/bin/bash
# run82-3：真机一轮观测——「先持续采集，后分析」。
# 阶段1：启动独立后台采集（timeout CAPTURE_SEC，全覆盖部署+运行全程，不打断）
# 阶段2：部署 HAP（force-stop → send → install → start）
# 阶段3：轮询死亡/达成信号（NCP-EXIT / CF-OK-8188 / ASOM1001），或到 MAX_WAIT
# 阶段4：停采集，事后分析摘要（NCP-EXIT signal、WARM-TB 原始异常、CF 信号、死因）
# 用法：./scripts/run_and_capture.sh [输出前缀,默认 /tmp/run3]
# 注意：本脚本对 HAP 使用相对仓库根的路径 —— 已由 ROOT 推导，不必保证 CWD=仓库根。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"

OUT="${1:-/tmp/run3}"
CAPTURE_SEC="${CAPTURE_SEC:-900}"
MAX_WAIT="${MAX_WAIT:-600}"
HAP="$ROOT/entry/build/default/outputs/default/entry-default-signed.hap"

echo "== [1/4] 启动后台采集 ${CAPTURE_SEC}s → ${OUT}.log"
timeout "$CAPTURE_SEC" $HDC shell "hilog | grep -aE 'ComfyChild|ComfyNapi|NCP-EXIT|WARM-OK|WARM-FAIL|WARM-TB|CF-RUN|CF-OK-8188|CF-FAIL|PY-TB|Main EXIT|run_path returned|py-err|ASOM1001|APPCORE|libcomfy|CXA17|STUB-GLOBAL|UTILS-FOUND|UTILS-PROBE|TV-STUB|FERRY-STUB|AV-STUB|AVSUBFINDER'" > "${OUT}.log" 2>&1 &
CAP_PID=$!
sleep 4   # 采集管道先就位，确保 force-stop 之前就有基线

echo "== [2/4] 部署: force-stop → send → install → start"
$HDC shell "aa force-stop $BUNDLE" || true
$HDC shell "bm uninstall -n $BUNDLE" || true   # run85：卸载（默认清数据）→ rawfile 全量重解压，消除增量残存变量
$HDC file send "$HAP" /data/local/tmp/comfy.hap
$HDC shell "bm install -p /data/local/tmp/comfy.hap"
$HDC shell "aa start -a EntryAbility -b $BUNDLE"
echo "DEPLOYED $(date +%T)"
BASE=$(wc -l < "${OUT}.log")   # run83：部署后基线行号——轮询只看自此以后的新行，避免命中旧轮残留信号
echo "BASE_LINES=$BASE"

echo "== [3/4] 轮询关键信号（最多 ${MAX_WAIT}s，收到后补看 20s 即收尾）"
end=$((SECONDS + MAX_WAIT))
while [ $SECONDS -lt $end ]; do
    if tail -n +$((BASE + 1)) "${OUT}.log" 2>/dev/null | grep -qE "NCP-EXIT |CF-OK-8188|ASOM1001|APPCORE"; then
        echo "  收到关键信号，补观察 20s"
        sleep 20
        break
    fi
    sleep 5
done
kill "$CAP_PID" 2>/dev/null || true
wait "$CAP_PID" 2>/dev/null || true

echo "== [4/4] 事后分析：关键证据摘要 =="
echo "--- NCP-EXIT（死亡通道定谳）---"; grep -aE "NCP-EXIT" "${OUT}.log" || echo "(无)"
echo "--- WARM-FAIL / WARM-TB（tokenizer 根因）---"; grep -aE "WARM-FAIL|WARM-TB" "${OUT}.log" || echo "(无)"
echo "--- CF 信号 ---"; grep -aE "CF-RUN|CF-OK-8188|CF-FAIL" "${OUT}.log" || echo "(无)"
echo "--- 死因/进程层 ---"; grep -aE "ASOM1001|APPCORE|Main EXIT|run_path returned|py-err|PY-TB" "${OUT}.log" || echo "(无)"
echo "LOG=${OUT}.log 共 $(wc -l < "${OUT}.log") 行"
