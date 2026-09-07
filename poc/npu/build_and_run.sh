#!/usr/bin/env bash
# poc/npu 一键链: 构建 → (可选)安装+运行 → 拉报告。
#
# 用法(必须从仓库根 /data/share/comfyui 任意目录执行 — 内部自 cd):
#   ./poc/npu/build_and_run.sh          # 只构建(宿主编译验证)
#   ./poc/npu/build_and_run.sh run      # 构建+装机+启动+拉报告
#   ./poc/npu/build_and_run.sh clean    # 毁灭性重建: rm outputs 后从零构建(可复现演练)
#
# 正确命令+目录+产物: 本脚本 cwd = poc/npu(工程根); HAP 产物 =
#   poc/npu/entry/build/default/outputs/default/entry-default-signed.hap(签名=default_MagicFlow_*)
# 依赖: env.sh(HDC=$OHOS_SDK/toolchains/hdc -t $HDC_TARGET; TOOL_HOME/hvigorw);
#        设备 HDC_TARGET 默认 192.168.1.8:33363(可用 HDC_TARGET=... 覆盖)。
#
# ⚠ 同包名互斥: bm install 会覆盖主项目 HAP(不可共存); 需要回主项目时重装其 HAP 即可。
set -eo pipefail

# ⚠ 变量名纪律: env.sh 内部会覆盖 ROOT(=comfyui 根) —— 本脚本的工程根必须用独立名 POC,
#   否则 HAP 会指向主项目产物(2026-09-07 曾因此把主项目 482MB HAP 发到真机)。
POC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$POC/../../scripts/env.sh"     # HDC/SDK/TOOL_HOME/BUNDLE(会覆盖 ROOT, 勿再用 ROOT)

HAP="$POC/entry/build/default/outputs/default/entry-default-signed.hap"
REPORT_REMOTE="/data/app/el2/100/base/$BUNDLE/haps/entry/files/npu-poc-report.md"

cd "$POC"

case "${1:-}" in
clean)
    echo "[CLEAN] rm -rf entry/build"
    rm -rf entry/build
    echo "[CLEAN] rebuild from scratch"
    ;&
esac

echo "[BUILD] hvigorw assembleHap (debug)..."
"$TOOL_HOME/bin/hvigorw" assembleHap --mode module -p product=default -p buildMode=debug --no-daemon

[ -f "$HAP" ] || { echo "[FAIL] HAP not found: $HAP"; exit 1; }
echo "[BUILD] HAP OK: $(stat -c%s "$HAP" 2>/dev/null || ls -l "$HAP" | awk '{print $5}') bytes"

if [ "${1:-}" != "run" ]; then
    echo "[DONE] build only. Use '$0 run' to deploy+runtime."
    exit 0
fi

echo "[DEPLOY] send+install (覆盖同名包, 与主项目互斥)..."
"${HDC%% *}" -t "$HDC_TARGET" file send "$HAP" /data/local/tmp/npupoc.hap
"${HDC%% *}" -t "$HDC_TARGET" shell "bm install -p /data/local/tmp/npupoc.hap"

echo "[RUN] aa start ..."
# 清旧报告: 轮询判据=文件存在; 不清会读到上一次运行的旧报告(2026-09-07 踩过)
"${HDC%% *}" -t "$HDC_TARGET" shell "rm -f $REPORT_REMOTE /data/app/el2/100/base/$BUNDLE/haps/entry/files/npu-child.log"
"${HDC%% *}" -t "$HDC_TARGET" shell "aa start -a EntryAbility -b $BUNDLE"

echo "[WAIT] report polling (up to 300s; CH-02 matrix = 33 ops x {build+run}, 每 op 数秒)..."
for i in $(seq 1 300); do
    if "${HDC%% *}" -t "$HDC_TARGET" shell "ls $REPORT_REMOTE >/dev/null 2>&1"; then
        sleep 2   # 让写盘完成
        break
    fi
    sleep 1
done

if "${HDC%% *}" -t "$HDC_TARGET" shell "ls $REPORT_REMOTE >/dev/null 2>&1"; then
    echo "[REPORT] ---"
    "${HDC%% *}" -t "$HDC_TARGET" shell "cat $REPORT_REMOTE"
    echo "[REPORT] saved above; also fetch child log:"
    "${HDC%% *}" -t "$HDC_TARGET" shell "cat /data/app/el2/100/base/$BUNDLE/haps/entry/files/npu-child.log 2>/dev/null || echo (no child log)"
else
    echo "[FAIL] report not visible after 120s; check: $REPORT_REMOTE"
    exit 1
fi
echo "[DONE]"
