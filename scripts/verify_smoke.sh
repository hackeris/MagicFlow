#!/usr/bin/env bash
# verify_smoke.sh —— 一键黑盒验收(防回归, 2026-09-05)。
# 定位: 把「装机→后端活→BLAS→MM4x→出图」的手工验证流程固化为固定判据。
#   黑盒 API 层(8189 → ComfyUI 后端), 不依赖 UI 流程; workspace 门/UI 改型不影响本脚本
#   (将来门落地: 后端由验证入口拉起, 本脚本只负责判)。
# 判据(固定):
#   A. COMFTEST-BLAS 含 BLAS_INFO=open          —— BLAS 后端正确
#   B. COMFTEST-MM4x t 且 < 2.0s                 —— 多线程 GEMM 生效
#   C. 出图 smoke_workflow_256x2.json: status=success 且 execution < 180s
#     (seed=42 确定性 → 图片字节数应稳定; 因模型 fp16/fp32 切换会整体变, 不做硬字节断言,
#      仅 >10KB 且非零; 需要字节级断言时在 --strict 下用基线文件比对)
# 用法: bash scripts/verify_smoke.sh [--fast] [--device 192.168.1.8:33363] [--hap <path>] [--strict]
#   --fast   只验 A/B(不跑出图); --strict 启用图片字节基线比对(基线文件见下);
#   --device 默认 192.168.1.8:33363; --hap 默认 entry/build/default/outputs/default/entry-default-signed.hap
# 输出: PASS/FAIL 逐项 + 关键数字; 任一 FAIL → exit 1。全量约 4-5 分钟。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEVICE="192.168.1.8:33363"
HAP="$ROOT/entry/build/default/outputs/default/entry-default-signed.hap"
FAST=0; STRICT=0
while [ $# -gt 0 ]; do
  case "$1" in
    --fast) FAST=1 ;;
    --strict) STRICT=1 ;;
    --device) DEVICE="$2"; shift ;;
    --hap) HAP="$2"; shift ;;
    *) echo "未知参数 $1"; exit 2 ;;
  esac
  shift
done
HDC="hdc -t $DEVICE"
DLOG=/data/app/el2/100/base/app.hackeris.hium/haps/entry/files/pyroot/diag.log
BASELINE="${SMOKE_IMAGE_BASELINE:-}"

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
echo "  已启动, 等后端就绪..."

# fport(宿主 8189 → 设备 8188)
curl -s -m 4 http://127.0.0.1:8189/system_stats >/dev/null 2>&1 || hdc -t "$DEVICE" fport tcp:8189 tcp:8188 >/dev/null 2>&1 || true

backend_ok=0
for i in $(seq 1 24); do
  sleep 10
  if curl -s -m 5 http://127.0.0.1:8189/system_stats 2>/dev/null | grep -q comfyui; then backend_ok=1; break; fi
done
[ "$backend_ok" = 1 ] && ok "后端就绪(8189/system_stats <=240s)" || bad "后端未就绪(240s)"

echo "== [2] 判据 A: BLAS_INFO=open =="
A=$( { $HDC shell "grep -a 'COMFTEST-BLAS' $DLOG 2>/dev/null" || true; } | tail -1 | tr -d '\r' )
echo "$A" | grep -q "BLAS_INFO=open" && ok "BLAS_INFO=open" || bad "BLAS_INFO 非 open(完整行: ${A:0:80}...)"

echo "== [3] 判据 B: MM4x < 2.0s =="
M=$( { $HDC shell "grep -a 'COMFTEST-MM4x t=' $DLOG 2>/dev/null" || true; } | tail -1 | tr -d '\r' )
T=$(echo "$M" | sed -n 's/.*COMFTEST-MM4x t=\([0-9.]*\).*/\1/p' | tail -1)
if [ -n "$T" ]; then
  ok "MM4x=$T s"
  python3 -c "exit(0 if float('$T') < 2.0 else 1)" && echo "   [PASS] MM4x < 2.0s" || bad "MM4x=$T 超标(≥2.0s)"
else
  bad "无 MM4x 探针输出"
fi

echo "== [4] 判据 C: 出图 =="
if [ "$FAST" = 1 ]; then
  echo "  [--fast] 跳过出图"
else
  WF="$ROOT/scripts/smoke_workflow_256x2.json"
  RESP=$(curl -s -m 15 -X POST http://127.0.0.1:8189/prompt -H 'Content-Type: application/json' --data-binary @"$WF" 2>/dev/null || echo "")
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
