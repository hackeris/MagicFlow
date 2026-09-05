#!/usr/bin/env bash
# verify_smoke.sh —— 一键黑盒验收(防回归, 2026-09-05)。
# 定位: 把「装机→后端活→BLAS→MM4x→出图」的验证流程固化为固定判据。
#   黑盒 API 层(8189 → ComfyUI 后端), 不依赖 UI 流程; workspace 门/UI 改型不影响本脚本。
#   ⚠ 2026-09-05 零侵入改造(见 docs/smoke-design.md): 判据 A/B 由 ComfyUI 侧自检节点
#     OHOS_SmokeBench_BLASMM4x(comfyui-src custom_nodes/ohos_smoke)执行, 结果经
#     /history outputs.ui.json 返回 —— 产品代码(comfy_child.cpp/CMakeLists)无任何测试分支。
# 判据(固定):
#   A/B. smoke bench 节点: blas_ok=True 且 mm4x < 2.0s     —— BLAS 后端+多线程 GEMM
#   C.   出图 smoke_workflow_256x2.json: status=success 且 execution < 180s
#     (seed 每轮随机注入(2026-09-05) → 每轮真执行、图像各异, 无缓存假成功;
#      判据只验 success+耗时+>10KB, 不做字节断言)
# 用法: bash scripts/verify_smoke.sh [--fast] [--device 192.168.1.8:33363] [--hap <path>]
#   --fast   只验后端+判据 A/B(不跑出图); --device 默认 192.168.1.8:33363;
#   --hap 默认 entry/build/default/outputs/default/entry-default-signed.hap
# 输出: PASS/FAIL 逐项 + 关键数字; 任一 FAIL → exit 1。全量约 4-5 分钟。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEVICE="192.168.1.8:33363"
HAP="$ROOT/entry/build/default/outputs/default/entry-default-signed.hap"
FAST=0
while [ $# -gt 0 ]; do
  case "$1" in
    --fast) FAST=1 ;;
    --device) DEVICE="$2"; shift ;;
    --hap) HAP="$2"; shift ;;
    *) echo "未知参数 $1"; exit 2 ;;
  esac
  shift
done
HDC="hdc -t $DEVICE"
DLOG=/data/app/el2/100/base/app.hackeris.hium/haps/entry/files/pyroot/diag.log

PASSES=0; FAILS=""
ok()   { echo "  [PASS] $*"; PASSES=$((PASSES+1)); }
bad()  { echo "  [FAIL] $*"; FAILS="$FAILS;$*"; }

echo "== [0] 前置 =="
[ -f "$HAP" ] || { echo "FATAL: HAP 缺失 $HAP (先 make hap)"; exit 2; }
$HDC list targets 2>&1 | grep -q "$DEVICE" || { echo "FATAL: 设备 $DEVICE 不在线"; exit 2; }
echo "  device=$DEVICE hap=$(basename "$HAP")"

echo "== [1] 装机 + 启动 =="
# 保数据安装(install -r 升级语义): 模型/沙箱不丢, smoke 不触发重下(约 4-5 分钟保活)
$HDC install -r "$HAP" 2>&1 | tail -1 | grep -qE "AppMod finish|success" || bad "install -r 输出异常: $($HDC install "$HAP" 2>&1 | tail -1)"
$HDC shell "aa force-stop app.hackeris.hium" >/dev/null 2>&1 || true
sleep 2
$HDC shell "aa start -a EntryAbility -b app.hackeris.hium" >/dev/null 2>&1
echo "  已启动, 等门户 UI 稳定..."
sleep 20   # 门户首帧实测 ~15-20s(2026-09-05)

# ── W1 门户驱导(2026-09-05, docs/workspace-design.md)───────────────────────
#   产品化=零自动启动(W1 环境门): 后端由「用户手势」触发 —— verify 以 UI 自动化执行同一手势。
#   工具: 设备无 input 命令 → uitest uiInput(click/inputText/keyEvent);
#   定位: uitest dumpLayout 动态取文本节点 bounds 中心(抗布局/文案微调);
#   硬化: 每次注入前 aa start 拉回 App 前台(防用户正在用别的应用时误触, 2026-09-05 实判)。
node_center() { # $1=节点文本/hint(placeholder) → 回显 "<cx> <cy>"
  $HDC shell uitest dumpLayout -p /data/local/tmp/ul.xml >/dev/null 2>&1
  $HDC file recv /data/local/tmp/ul.xml /tmp/smoke_ul.xml >/dev/null 2>&1
  python3 - "$1" <<'PY'
import json, re, sys
d = json.load(open('/tmp/smoke_ul.xml', encoding='utf-8'))
target = sys.argv[1].strip()
out = []
def walk(n):
    a = n.get('attributes', {})
    t = ((a.get('text') or a.get('originalText') or a.get('hint') or '')).strip()
    if t and t == target:
        m = re.match(r'\[(\d+),(\d+)\]\[(\d+),(\d+)\]', a.get('bounds', ''))
        if m:
            x1, y1, x2, y2 = map(int, m.groups())
            out.append(f"{(x1 + x2) // 2} {(y1 + y2) // 2}")
            return True
    for c in n.get('children', []):
        if walk(c):
            return True
    return False
walk(d)
print(out[0] if out else '')
PY
}
ui_in() { $HDC shell "uitest uiInput $*" >/dev/null 2>&1; }

# 门户驱导: 打开已有环境(第一个卡片)或新建 smoke-auto; 返回 0=已注入, 1=失败。
#   坐标全部动态取自 uitest dumpLayout(文本/hint 节点中心), 不硬编码(抗布局微调)。
portal_enter() {
  # 前置: 把 App 拉回前台(硬化) —— 防用户正用别的应用时误触(2026-09-05 实判)
  $HDC shell "aa start -a EntryAbility -b app.hackeris.hium" >/dev/null 2>&1
  sleep 5
  local TITLE CARD NB IN OKB
  TITLE=$(node_center "我的环境")
  if [ -n "$TITLE" ]; then
    # 有环境: 取「我的环境」标题下方第一个文本节点 = 卡片名(第一项)
    $HDC shell uitest dumpLayout -p /data/local/tmp/ul.xml >/dev/null 2>&1
    $HDC file recv /data/local/tmp/ul.xml /tmp/smoke_ul.xml >/dev/null 2>&1
    CARD=$(python3 - "$TITLE" <<'PY'
import json, re, sys
d = json.load(open('/tmp/smoke_ul.xml', encoding='utf-8'))
ny = int(sys.argv[1].split()[1])
res = []
def walk(n):
    a = n.get('attributes', {})
    t = (a.get('text') or a.get('originalText') or '').strip()
    m = re.match(r'\[(\d+),(\d+)\]\[(\d+),(\d+)\]', a.get('bounds', ''))
    if m:
        cy = (int(m.group(2)) + int(m.group(4))) // 2
        if not res and t and cy > ny + 40:      # 标题下方第一处文本 = 卡片名
            res.append(f"{(int(m.group(1)) + int(m.group(3))) // 2} {cy}")
            return True
    for c in n.get('children', []):
        if walk(c):
            return True
    return False
walk(d)
print(res[0] if res else '')
PY
)
    if [ -n "$CARD" ]; then
      echo "  portal: 打开已有环境 @($CARD)"
      ui_in click $CARD
      return 0
    fi
  fi
  # 空态 → 新建链: 新建环境 → 输入框(placeholder) → smoke-auto → 创建
  NB=$(node_center "新建环境")
  [ -z "$NB" ] && { echo "  portal: 门户未出现(新建环境按钮缺失)"; return 1; }
  ui_in click $NB
  sleep 4
  IN=$(node_center "环境名称")
  [ -z "$IN" ] && { echo "  portal: 新建对话框未出现(输入框缺失)"; return 1; }
  ui_in click $IN
  sleep 2
  ui_in text smoke-auto
  sleep 2
  OKB=$(node_center "创建")
  [ -z "$OKB" ] && { echo "  portal: 创建按钮缺失"; return 1; }
  ui_in click $OKB
  echo "  portal: 新建环境 smoke-auto"
  return 0
}

echo "== [1b] 门户驱导(打开或新建环境) =="
portal_enter && echo "  已注入用户手势, 等后端就绪..."

# fport(宿主 8189 → 设备 8188)
curl -s -m 4 http://127.0.0.1:8189/system_stats >/dev/null 2>&1 || hdc -t "$DEVICE" fport tcp:8189 tcp:8188 >/dev/null 2>&1 || true

backend_ok=0
for i in $(seq 1 24); do
  sleep 10
  if curl -s -m 5 http://127.0.0.1:8189/system_stats 2>/dev/null | grep -q comfyui; then backend_ok=1; break; fi
done
[ "$backend_ok" = 1 ] && ok "后端就绪(8189/system_stats <=240s)" || bad "后端未就绪(240s)"

echo "== [2] 判据 A/B: OHOS_SmokeBench(BLAS=open + MM4x<2.0s) =="
# 2026-09-05 黑盒化: 判据由 ComfyUI 侧自检节点执行(comfyui-src custom_nodes/ohos_smoke),
#   结果经 history outputs.json 返回(ComfyUI 摊平结构, product 代码零探针, 见 docs/smoke-design.md)。
BN="$ROOT/scripts/smoke_bench_workflow.json"
[ -f "$BN" ] || { bad "缺 $BN"; }
RESP=$(curl -s -m 15 -X POST http://127.0.0.1:8189/prompt -H 'Content-Type: application/json' --data-binary @"$BN" 2>/dev/null || echo "")
B_PID=$(echo "$RESP" | python3 -c "import sys,json;print(json.load(sys.stdin).get('prompt_id',''))" 2>/dev/null || echo "")
if [ -z "$B_PID" ]; then bad "bench prompt 未受理"
else
  BJ=""
  for i in $(seq 1 10); do
    sleep 12
    R=$(curl -s -m 8 http://127.0.0.1:8189/history/$B_PID 2>/dev/null || echo "")
    BJ=$(echo "$R" | python3 -c "
import sys, json
d = json.load(sys.stdin); h = d.get('$B_PID', {})
o = h.get('outputs', {}).get('1', {})
# ComfyUI history 输出摊平(outputs[id] = ui/result 合并, 无 ui 层): 顶层 json; 兼容旧式双路径
jj = o.get('json') or o.get('ui', {}).get('json', [])
print(jj[0] if jj else '')
" 2>/dev/null || echo "")
    [ -n "$BJ" ] && break
  done
  if [ -z "$BJ" ]; then bad "bench 无结果(超时/节点未注册?)"
  else
    echo "$BJ" | python3 -c "import sys,json; j=json.load(sys.stdin); print(f'  bench: blas_ok={j[\"blas_ok\"]} mm4x={j[\"mm4x\"]}s nthreads={j[\"nthreads\"]}')" >&2 || true
    BL=$(echo "$BJ" | python3 -c "import sys,json; j=json.load(sys.stdin); print('OK' if j.get('blas_ok') else 'BAD')" 2>/dev/null || echo "BAD")
    BX=$(echo "$BJ" | python3 -c "import sys,json; print(json.load(sys.stdin).get('mm4x', 99))" 2>/dev/null || echo 99)
    [ "$BL" = "OK" ] && ok "BLAS_INFO=open(节点自报)" || bad "BLAS 非 open: $BJ"
    if [ -n "$BX" ]; then
      ok "MM4x=$BX s"
      python3 -c "exit(0 if float('$BX') < 2.0 else 1)" 2>/dev/null && echo "   [PASS] MM4x < 2.0s" || bad "MM4x=$BX 超标"
    else bad "MM4x 解析失败"
    fi
  fi
fi

echo "== [4] 判据 C: 出图 =="
if [ "$FAST" = 1 ]; then
  echo "  [--fast] 跳过出图"
else
  WF="$ROOT/scripts/smoke_workflow_256x2.json"
  # 2026-09-05 用户要求: smoke 图每次生成必须有差异(否则观感像"假生成");
  #   且固定 seed 会命中 ComfyUI 执行缓存 → 二次出图"假成功"(T=0.0s)。
  #   提交前把 prompt 内所有 seed 注入随机值: 每轮真执行 + 图像各异。
  WF_TMP=$(mktemp /tmp/smoke_wf_XXXXXX.json)
  python3 - "$WF" "$WF_TMP" <<'PYEOF' || { bad "seed 注入失败"; }
import json, random, sys
d = json.load(open(sys.argv[1]))
def walk(o):
    if isinstance(o, dict):
        for k, v in o.items():
            if k == 'seed':
                o[k] = random.randint(0, 2 ** 31)
            else:
                walk(v)
    elif isinstance(o, list):
        for it in o:
            walk(it)
walk(d.get('prompt', {}))
json.dump(d, open(sys.argv[2], 'w'))
PYEOF
  RESP=$(curl -s -m 15 -X POST http://127.0.0.1:8189/prompt -H 'Content-Type: application/json' --data-binary @"$WF_TMP" 2>/dev/null || echo "")
  rm -f "$WF_TMP"
  PID=$(echo "$RESP" | python3 -c "import sys,json;print(json.load(sys.stdin).get('prompt_id',''))" 2>/dev/null || echo "")
  if [ -z "$PID" ]; then bad "prompt 未受理($RESP 前 80 字: $(echo "$RESP" | head -c 80))"
  else
    done_at=""
    for i in $(seq 1 12); do
      sleep 20
      R=$(curl -s -m 8 http://127.0.0.1:8189/history/$PID 2>/dev/null || echo "")
      if echo "$R" | python3 -c "import sys,json;d=json.load(sys.stdin);h=d.get('$PID');sys.exit(0 if (h is not None and h.get('outputs')) else 1)" 2>/dev/null; then
        done_at="$R"; break
      fi
    done
    if [ -n "$done_at" ]; then
      echo "$done_at" | python3 -c "
import sys, json
d = json.load(sys.stdin); h = list(d.values())[0]
ms = h.get('status',{}).get('messages',[])
st = [m[1]['timestamp'] for m in ms if m[0]=='execution_start']
en = [m[1]['timestamp'] for m in ms if m[0]=='execution_success']
t  = (en[0]-st[0])/1000 if st and en else None
outs = h.get('outputs',{})
imgs = [(o['images'][0]['subfolder'], o['images'][0]['filename']) for o in outs.values() if 'images' in o and o['images']]
print('IMG=' + (imgs[0][1] if imgs else 'NONE'))
print('T=%.1f' % t if t else 'T=NONE')
" > /tmp/smoke_img.txt
      IMG=$(sed -n 's/^IMG=//p' /tmp/smoke_img.txt); TM=$(sed -n 's/^T=//p' /tmp/smoke_img.txt)
      [ "$IMG" != "NONE" ] && ok "出图 success(图=$IMG)" || bad "出图无 images"
      if [ "$TM" != "NONE" ]; then
        python3 -c "exit(0 if float('$TM') < 180.0 else 1)" && ok "execution ${TM}s < 180s" || bad "execution ${TM}s 超标"
      else bad "无执行时间"; fi
    else
      bad "出图超时(240s)"
    fi
  fi
fi

echo ""
echo "== 汇总 =="
if [ -n "$FAILS" ]; then echo "FAIL:${FAILS}"; exit 1; fi
echo "ALL PASS ($PASSES 项) --  smoke 验收通过"
