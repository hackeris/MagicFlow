#!/usr/bin/env bash
# run_repro_chain.sh —— 全链复现 driver(2026-09-05 复现验证用, 依序: fetch→rust→openblas→torch→zip/prebuilt/hap)。
# 在 git worktree 目录(bootstrap 已跑)里执行; 任一步失败即停(输出指向对应日志)。
# 用法: bash thirdparty/ohos-torch/run_repro_chain.sh
#   每步日志: build/repro/step_NN_<name>.log
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOG_DIR="$ROOT/build/repro"
mkdir -p "$LOG_DIR"
step() { echo -e "\n\033[32m==== STEP $1: $2 ====\033[0m"; }
# ⚠ 每步显式判失败(管道 tee + pipefail 已够,但再防一层): 失败即停并留可 grep 的标记
run() { local tag="$1"; shift; echo ">>> $*"; "$@" 2>&1 | tee "$LOG_DIR/$tag.log" || {
    echo -e "\033[31mSTEP $tag FAILED (见 $LOG_DIR/$tag.log)\033[0m"; exit 1; }; }

step "1/7" "fetch(外部输入)"
run 01_fetch   make fetch

step "2/7" "rust 扩展(thirdparty submodules)"
run 02_rust    make rust

step "3/7" "OpenBLAS 交叉构建(v0.3.29 → externals/openblas-src)"
run 03_openblas bash thirdparty/ohos-torch/build_openblas_ohos.sh externals/openblas-src

step "4/7" "torch 2.10.0 全量交叉构建(OpenBLAS 静态链, --clean)"
run 04_torch   bash thirdparty/ohos-torch/build_torch_ohos.sh --clean

step "5/7" "python312.zip(stage+stdlib+锚)"
run 05_zip     make zip

step "6/7" "prebuilt 收集(325 文件闭环+NEEDED 闭包)"
run 06_prebuilt make prebuilt

step "7/7" "HAP(签名)"
run 07_hap     make hap

echo -e "\n\033[32m==== REPRO CHAIN DONE ====\033[0m"
ls -la "$ROOT/entry/build/default/outputs/default/entry-default-signed.hap"
