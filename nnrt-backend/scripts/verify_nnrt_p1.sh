#!/usr/bin/env bash
# verify_nnrt_p1.sh — NPU P1 后端真机验证链(一键: 编译 → 收集 → 打 HAP → 装机 → 驱导 → 采集 → 判据)
#
# 用法:
#   bash nnrt-backend/scripts/verify_nnrt_p1.sh              # 常规轮
#   bash nnrt-backend/scripts/verify_nnrt_p1.sh --rebuild    # 先删 build/ 再走全链(毁灭性重建演练)
#
# 产物/落点:
#   nnrt-backend/build/_nnrt_bootstrap.cpython-312-aarch64-linux-ohos.so  (扩展)
#   → collect_prebuilt.sh 收进 entry/src/main/cpp/prebuilt/ → 打进 HAP → 装机运行
#   设备日志: <pyroot>/nnrt-p0a.log(后端探针, append)、nnrt-p1.log(python 自检)
#
# 判据(P1-2 算子下沉 + P1-3 数值):
#   1. NNRT-ENG e8 execRun ok=1                    —— NNRt 执行成功
#   2. c16 mm-ok val=4.0000 / c18 matmul-ok val=4.0000
#        全 1 矩阵 4×4 相乘(期望恰好 4.0)。**特例也要断言**: 编译缓存串图那个 bug 就是被它
#        暴露的(当时输出恒为 2.0 = 1.0+1.0, 即别的图的结果)。
#   3. c23 exact-inputs maxAbsErr=0.000e+00 —— fp16 可精确表示的输入(0.5×0.25)必须零误差,
#        用于锁定"误差源自输入降精度"这一归因。
#   4. c21[K8/K64] npu-vs-fp64 的 mean < 5e-3 —— 相对误差判据。实测 mean 1e-3 量级、
#        max 1e-2 量级; 9030 的 NNRt MATMUL 在 fp32 声明下按 fp16 计算(见 c23), 故不能按
#        fp32 的 1e-6 去要求。SD 级出图对该量级噪声鲁棒。
#   任一不满足 → 非零退出。
#
# ⚠ 调试期踩过的坑(保留在此处, 免得重蹈):
#   1) 日志 append: 采集前必须记基线行数(`wc -l`, 在 aa start 之前取), 否则历史轮次混入 ——
#      曾把 3 条历史 NNRT-MM 误读成"本轮重试"。
#   2) 采集必须覆盖**全部** NNRT-* 前缀: kernel 侧日志是 NNRT-MM, 曾因只 grep P1D/FB 而漏掉,
#      导致"卡在 dispatch 前"与"卡在 kernel 内"无法区分, 白跑两轮。
#   3) 存活判据要看**子进程数**: comfy_child 与主应用同名(fork 不更新 comm),
#      `ps | head -3` 会把它切掉, "子进程已死"曾被读成"主进程活着 ⇒ 卡住"。
#   4) 驱导前必须 `aa start` 拉回前台: 设备前台曾被「纳百窗」占据, dumpLayout 拿到它的控件,
#      找不到启动按钮 → 后端从未启动, 整轮白跑。
#   5) 退出码不许被尾管掩盖; 本脚本用 `set -eo pipefail`。
set -eo pipefail

ROOT=/data/share/comfyui
cd "$ROOT"
# shellcheck disable=SC1091
source scripts/env.sh

P=/data/app/el2/100/base/app.fuqidian.magicflow/haps/entry/files/pyroot
HAP="$ROOT/entry/build/default/outputs/default/entry-default-signed.hap"
UL=/tmp/ul_verify_nnrt.xml
NEW=/tmp/nnrt_p1_new.log

if [ "${1:-}" = "--rebuild" ]; then
    echo "== [0/6] 毁灭性重建演练: 删产物目录 =="
    rm -rf "$ROOT/nnrt-backend/build"
    echo "  已删 nnrt-backend/build"
fi

echo "== [1/6] 编译扩展 =="
bash nnrt-backend/scripts/build_ext.sh 2>&1 | tail -3

echo "== [2/6] 收集 prebuilt =="
bash scripts/collect_prebuilt.sh 2>&1 | tail -1

echo "== [3/6] 打 HAP =="
hvigorw assembleHap --mode module -p product=default -p buildMode=debug --no-daemon 2>&1 | tail -1

echo "== [4/6] 装机 =="
$HDC install -r "$HAP" 2>&1 | tail -1
$HDC shell "aa force-stop app.fuqidian.magicflow" >/dev/null 2>&1 || true
sleep 2
# 日志基线(见头部坑 1): 必须在 aa start 之前取
BASE=$($HDC shell "wc -l < $P/nnrt-p0a.log" 2>/dev/null | tr -d ' \r' || echo 0)
BASE=${BASE:-0}
echo "  日志基线行数: $BASE"
$HDC shell "aa start -a EntryAbility -b app.fuqidian.magicflow" >/dev/null 2>&1 || true
sleep 20

find_btn() {
    $HDC shell uitest dumpLayout -p /data/local/tmp/ul.xml >/dev/null 2>&1 || true
    $HDC file recv /data/local/tmp/ul.xml "$UL" >/dev/null 2>&1 || true
    python3 - <<'PY'
import json, re
try:
    d = json.load(open('/tmp/ul_verify_nnrt.xml', encoding='utf-8'))
except Exception:
    print(''); raise SystemExit
hit = []
def walk(n):
    a = n.get('attributes', {})
    t = ((a.get('text') or a.get('originalText') or a.get('hint') or '')).strip()
    if t == '启动 梦幻之流':
        m = re.match(r'\[(\d+),(\d+)\]\[(\d+),(\d+)\]', a.get('bounds', ''))
        if m:
            x1, y1, x2, y2 = map(int, m.groups())
            hit.append(f"{(x1+x2)//2} {(y1+y2)//2}")
            return True
    for c in n.get('children', []):
        if walk(c):
            return True
    return False
walk(d)
print(hit[0] if hit else '')
PY
}

dump_texts() {
    python3 - <<'PY'
import json
try:
    d = json.load(open('/tmp/ul_verify_nnrt.xml', encoding='utf-8'))
except Exception as e:
    print('  读不到布局:', e); raise SystemExit
seen = []
def walk(n):
    a = n.get('attributes', {})
    t = ((a.get('text') or a.get('originalText') or a.get('hint') or '')).strip()
    if t and len(t) < 40 and t not in seen:
        seen.append(t)
    for c in n.get('children', []):
        walk(c)
walk(d)
print('  界面文本:', ' | '.join(seen[:15]))
PY
}

echo "== [5/6] 驱导(拉回前台 + 重试, 见头部坑 4) =="
NB=""
for attempt in 1 2 3; do
    $HDC shell "aa start -a EntryAbility -b app.fuqidian.magicflow" >/dev/null 2>&1 || true
    sleep 6
    NB=$(find_btn)
    [ -n "$NB" ] && break
    echo "  驱导重试 $attempt: 未找到『启动 梦幻之流』"
done
if [ -z "$NB" ]; then
    echo "[FAIL] 驱导失败(3 次), 当前界面:"
    dump_texts
    exit 1
fi
echo "  按钮坐标: $NB"
$HDC shell "uitest uiInput click $NB" >/dev/null 2>&1 || true

echo "== [6/6] 采集与判据(等 80s) =="
sleep 80
# 一锅端回本轮新增, 宿主分拣(见头部坑 2)
$HDC shell "tail -n +$((BASE + 1)) $P/nnrt-p0a.log" > "$NEW" 2>/dev/null || true
LINES=$(wc -l < "$NEW" 2>/dev/null || echo 0)
echo "  本轮新增行数: $LINES (基线 $BASE)"

echo "--- P1D 探针(末 24 行) ---"
grep NNRT-P1D "$NEW" | tail -24 || true
echo "--- MM kernel(末 6 行) ---"
grep NNRT-MM "$NEW" | tail -6 || true
echo "--- ENG 引擎分段(末 12 行) ---"
grep NNRT-ENG "$NEW" | tail -12 || true
echo "--- 进程(不截断, 见头部坑 3) ---"
$HDC shell "ps -ef 2>/dev/null | grep app.fuqidian | grep -v grep" || true
echo "--- 子进程数(=1 只剩主进程 ⇒ comfy_child 已死) ---"
$HDC shell "ps -ef 2>/dev/null | grep app.fuqidian | grep -v grep | wc -l" || true

echo "--- 判据 ---"
FAILED=0
check() {   # check <描述> <grep 模式>
    if grep -qE "$2" "$NEW" 2>/dev/null; then
        echo "  [PASS] $1"
    else
        echo "  [FAIL] $1  (未匹配: $2)"
        FAILED=1
    fi
}
check "NNRt 执行成功(e8 ok=1)"          'NNRT-ENG e8 execRun ret=0 ok=1'
check "mm 数值正确(val=4.0)"            'c16 mm-ok dim=2 val=4\.0000'
check "matmul polyfill 正确(val=4.0)"   'c18 matmul-ok dim=2 val=4\.0000'
check "fp16 精确输入零误差(归因)"        'c23 exact-inputs.*maxAbsErr=0\.000e\+00'

# mean 相对误差判据(需要解析浮点, 交给 python)
if python3 - "$NEW" <<'PY'
import re, sys
txt = open(sys.argv[1], encoding='utf-8', errors='ignore').read()
hits = re.findall(r'c21\[(K\d+)\] npu-vs-fp64 max=([\d.e+-]+) mean=([\d.e+-]+)', txt)
if not hits:
    print('  [FAIL] 未找到 c21 对拍行'); raise SystemExit(1)
bad = [f"{tag}:mean={mean}" for tag, _, mean in hits if float(mean) >= 5e-3]
for tag, mx, mean in hits:
    print(f"  [{'FAIL' if float(mean) >= 5e-3 else 'PASS'}] c21[{tag}] mean={mean} (判据 <5e-3)")
raise SystemExit(1 if bad else 0)
PY
then
    :
else
    FAILED=1
fi

if [ "$FAILED" -ne 0 ]; then
    echo "[RESULT] FAIL —— 完整日志: $NEW"
    exit 1
fi
echo "[RESULT] PASS —— P1-2 算子下沉 + P1-3 数值判据全部满足"
echo "  详细日志: $NEW"
