#!/usr/bin/env bash
# verify_smoke.sh —— 一键黑盒验收(防回归, 2026-09-05)。
# 定位: 把「装机→后端活→BLAS→MM4x→出图」的验证流程固化为固定判据。
#   黑盒 API 层(8189 → ComfyUI 后端), 不依赖 UI 流程; 单入口启动/UI 改型不影响本脚本。
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
echo "  已启动, 等首页 UI 稳定..."
sleep 20   # 首页首帧实测 ~15-20s(2026-09-05)

# ── 门户驱导(2026-09-05 单入口化, docs/workspace-design.md)─────────────────
#   产品化=零自动启动: 后端由「用户手势」触发 —— verify 以 UI 自动化执行同一手势
#   (点首页「启动 ComfyUI」; W1 环境管理/多步新建链已撤出设备形态)。
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

# 门户驱导(单入口, 2026-09-05): 点首页「启动 ComfyUI」按钮; 返回 0=已注入, 1=失败。
#   坐标全部动态取自 uitest dumpLayout(文本节点中心), 不硬编码(抗布局微调)。
portal_enter() {
  # 前置: 把 App 拉回前台(硬化) —— 防用户正用别的应用时误触(2026-09-05 实判)
  $HDC shell "aa start -a EntryAbility -b app.hackeris.hium" >/dev/null 2>&1
  sleep 5
  local NB
  NB=$(node_center "启动 ComfyUI")
  [ -z "$NB" ] && { echo "  portal: 首页未出现(启动按钮缺失)"; return 1; }
  ui_in click $NB
  echo "  portal: 点击『启动 ComfyUI』"
  return 0
}

echo "== [1b] 门户驱导(单入口: 点启动) =="
portal_enter && echo "  已注入用户手势, 等后端就绪..."

# fport(宿主 8189 → 设备 8188)
curl -s -m 4 http://127.0.0.1:8189/system_stats >/dev/null 2>&1 || hdc -t "$DEVICE" fport tcp:8189 tcp:8188 >/dev/null 2>&1 || true
# rport(设备 18080 → 宿主 18001): 模型源 http.server(/tmp/models, 服务 sd_turbo)。幂等自建。
#   ⚠ 2026-09-05: 设备侧 18000 有残留隧道监听(hdcd 持有, 宿主侧通路已断), 重建会报
#   "TCP Port listen failed" → 改用 18080。旧 18000 残留不碍事, 仅占端口。
hdc -t "$DEVICE" rport tcp:18080 tcp:18001 >/dev/null 2>&1 || true

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

echo "== [2.5] Q2 模型下载端到端(可选, MODEL_DL=1) =="
#   W3 docs/model-download.md: 经后端端点下载(隧道 URL, 与 ensureModel 等价但走在端点上,
#   验 202+进度+completed+落盘 全链)。媒体区已有完整 sd_turbo → skip(重复下载无意义)。
#   ⚠ 2026-09-05 实测教训: skip 判据曾查 core /models/checkpoints(并集: 媒体区+沙箱),
#   沙箱遗留祖传模型命中 → Q2 永被跳过 → 主链从未验证。改为媒体区目录直查字节数
#   (下载端点落点 = 模型树首路径 = 媒体区; wc -c 失败/不足 1GB → 走下载链)。
#   ⚠ 路径视图: 必须用 hdc 卷视图 /storage/media/100/...(shell 可见); `/storage/
#   Users/currentUser/...` 仅 App 进程视图(w6 实测 shell No such file → 判据假 FAIL)。
if [ "${MODEL_DL:-0}" = 1 ]; then
  MEDIA_CKP="/storage/media/100/local/files/Docs/Download/app.hackeris.hium/models/checkpoints"
  MB=$(hdc -t "$DEVICE" shell "wc -c < '$MEDIA_CKP/sd_turbo.safetensors' 2>/dev/null" 2>/dev/null | grep -o '[0-9]\+' | head -1)
  if [ -n "$MB" ] && [ "$MB" -gt 1000000000 ]; then
    ok "Q2 目标已具备(媒体区完整模型 ${MB}B)"
  else
    TASK=$(curl -s -m 15 -X POST http://127.0.0.1:8189/models/download \
      -H 'Content-Type: application/json' \
      --data-binary '{"url":"http://127.0.0.1:18080/sd_turbo.safetensors","directory":"checkpoints","filename":"sd_turbo.safetensors"}' 2>/dev/null || echo "")
    TID=$(echo "$TASK" | python3 -c "import sys,json;print(json.load(sys.stdin).get('task_id',''))" 2>/dev/null || echo "")
    if [ -z "$TID" ]; then
      bad "Q2 下载任务未受理: ${TASK:0:120}"
    else
      FIN=""
      for i in $(seq 1 60); do
        sleep 10
        ST=$(curl -s -m 8 "http://127.0.0.1:8189/models/download/$TID" 2>/dev/null || echo "")
        S=$(echo "$ST" | python3 -c "import sys,json;d=json.load(sys.stdin);print(d.get('status',''))" 2>/dev/null || echo "")
        echo "  dl status=$S $(echo "$ST" | python3 -c "import sys,json;d=json.load(sys.stdin);print(f'{d.get(1) if False else d.get(\"bytes_received\",0)}/{d.get(\"bytes_total\",0)}')" 2>/dev/null || true)"
        [ "$S" = "completed" ] && { FIN=1; break; }
        [ "$S" = "error" ] && { echo "  error: $ST" | head -c 300; break; }
      done
      if [ -n "$FIN" ]; then
        # 状态机 completed ≠ 文件实锤: 媒体区直查大文件(≥1GB, 终点目录=模型树首路径)
        MB2=$(hdc -t "$DEVICE" shell "wc -c < '$MEDIA_CKP/sd_turbo.safetensors' 2>/dev/null" 2>/dev/null | grep -o '[0-9]\+' | head -1)
        if [ -n "$MB2" ] && [ "$MB2" -gt 1000000000 ]; then
          ok "Q2 端点下载完成(媒体区落盘 ${MB2}B)"
        else
          bad "Q2 状态 completed 但媒体区落盘不足(疑 staging/路径错): ${MB2:-无文件}"
        fi
      else
        bad "Q2 端点下载未完成(≤600s)"
      fi
    fi
  fi
fi

echo "== [3] W3: 模型下载端点 + 模型可见性 =="
#   2026-09-05 W3(docs/model-download.md): patch 16 端点 catalog 可达 + extra_model_paths
#   is_default 生效(模型树上可见 sd_turbo)。只验端点与本地可见性, 不依赖外网。
CAT=$(curl -s -m 8 http://127.0.0.1:8189/models/download/catalog 2>/dev/null || echo "")
echo "$CAT" | python3 -c "
import sys,json
try:
    d=json.load(sys.stdin)
    assert any(e.get('id')=='sd-turbo' for e in d.get('catalog',[]))
except Exception: print('BAD')
else: print('OK')
" 2>/dev/null | grep -q OK && ok "W3 catalog 可达(含 sd-turbo)" || bad "W3 catalog 不可达/缺条目: ${CAT:0:120}"
MEL=$(curl -s -m 8 http://127.0.0.1:8189/models/checkpoints 2>/dev/null || echo "")
echo "$MEL" | python3 -c "
import sys,json
try:
    l=json.load(sys.stdin)
    assert any('sd_turbo' in str(x) for x in l)
except Exception: print('BAD')
else: print('OK')
" 2>/dev/null | grep -q OK && ok "W3 模型可见(checkpoints 含 sd_turbo, is_default 首位生效)" || bad "W3 模型不可见(extra_model_paths 未生效?): ${MEL:0:120}"

echo "== [4.5] W3 模板(patch 17, Q4) =="
#   /templates/index.json 官方由 comfyui-workflow-templates pip 包提供(设备缺 → patch 17
#   的 web.static 补上 comfyui 根 templates/)。验: 200 + 内容含 sd-turbo 模板条目。
TMP=$(curl -s -m 8 http://127.0.0.1:8189/templates/index.json 2>/dev/null || echo "")
echo "$TMP" | python3 -c "
import sys,json
try:
    d=json.load(sys.stdin)
    assert isinstance(d,list) and len(d)>=1 and 'sd_turbo' in str(d)
except Exception: print('BAD')
else: print('OK')
" 2>/dev/null | grep -q OK && ok "W3 模板路由可达(含 sd-turbo 条目)" || bad "W3 模板不可达: ${TMP:0:120}"

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
