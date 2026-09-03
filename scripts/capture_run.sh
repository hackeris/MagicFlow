#!/bin/bash
# run82-3：真机一轮观测采集。
# 本地后台流式（timeout 250）抓 hilog，白名单过滤防 chromium 洪峰拖垮管道。
# 关键新证据：NCP-EXIT（退出回调 signal 定谳死亡通道）、WARM-TB（tokenizer 原始异常）。
# 用法：./scripts/capture_run.sh <输出文件>
set -euo pipefail
source "$(dirname "$0")/env.sh"

OUT="${1:-/tmp/run}"
HILOG="$HDC shell \"hilog | grep -aE 'ComfyChild|ComfyNapi|NCP-EXIT|WARM-OK|WARM-FAIL|WARM-TB|CF-RUN|CF-OK-8188|CF-FAIL|PY-TB|Main EXIT|run_path returned|py-err|ASOM1001|NCP'"
echo "采集 240s → ${OUT}.log（后台）"
eval "timeout 240 $HILOG" > "${OUT}.log" 2>&1 &
echo $! > "${OUT}.pid"
echo "PID=$(cat ${OUT}.pid)  采集结束自动停"
