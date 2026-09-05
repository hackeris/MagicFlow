#!/usr/bin/env bash
# build_frontend.sh —— ComfyUI 前端"源码→自建 dist"(fork 定制链, docs/frontend-fork-plan.md)。
#
# 正确命令/目录/产物:
#   命令: bash scripts/build_frontend.sh [--skip-install]
#   目录: 源码 = thirdparty/comfyui-frontend(官方 taged 克隆/自维护 fork; 由 fetch_externals.sh 拉取)
#   产物: 源码树内 dist/(index.html + assets/**) → make_comfyui_stage.py 的 FE_SRC 取用
#   环境: node>=25(官方 engines: >=25 <26)固定使用 /opt/node-v25.9.0-linux-x64(不依赖系统 node);
#         pnpm@11.13.1(node25 下 npm -g 安装; corepack 在 node22 上崩, 不用)
#
# 用途/回退: 官方 dist zip 滚出链; 本脚本产物为唯一前端来源(定制在此树上 commit)。
set -eo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FE="$ROOT/thirdparty/comfyui-frontend"
NODE25=/opt/node-v25.9.0-linux-x64/bin
SKIP_INSTALL=0
[ "$1" = "--skip-install" ] && SKIP_INSTALL=1

# ── 环境预检(官方要求 node>=25; 系统 node22 不足, 亦会触发 corepack/pnpm 兼容坑) ──
[ -x "$NODE25/node" ] || { echo "FATAL: 缺 Node25($NODE25) — 需安装(见 docs/frontend-fork-plan.md §1)"; exit 1; }
export PATH="$NODE25:$PATH"
node -v | grep -q '^v25' || { echo "FATAL: node 版本不符: $(node -v)"; exit 1; }
command -v pnpm >/dev/null || { echo "FATAL: 缺 pnpm(node25 下 npm i -g pnpm@11.13.1)"; exit 1; }

[ -d "$FE" ] || { echo "FATAL: 源码树缺失 $FE — 先 bash scripts/fetch_externals.sh"; exit 1; }

# ── 版本 stamp(可追踪, 进 dist/VERSION.txt, 供 stage 锚/nightly 判定) ──
FE_DESC="$(git -C "$FE" rev-parse --short HEAD 2>/dev/null || echo unknown)"
FE_TAG="$(git -C "$FE" describe --tags --exact-match 2>/dev/null || echo '(no-tag)')"
echo "[frontend] source=$FE_DESC tag=$FE_TAG"

cd "$FE"
# ── 幂等快路径: dist 已构建且版本一致 → 跳过重构建(源码变更或 VERSION 缺才重建) ──
if [ -f dist/VERSION.txt ] && grep -q "\"commit\": \"$FE_DESC\"" dist/VERSION.txt 2>/dev/null \
   && [ -f dist/index.html ]; then
  echo "[OK] dist 已新鲜($FE_DESC), skip rebuild(删 dist 可强制重构建)"
  cat dist/VERSION.txt
  exit 0
fi

if [ "$SKIP_INSTALL" = 0 ] || [ ! -d node_modules ]; then
  echo "[install] pnpm install --frozen-lockfile ..."
  pnpm install --frozen-lockfile
fi

echo "[build] pnpm build(typecheck + vite)..."
pnpm build

# ── 产物断言(失败必须非零退出; 禁止"缺关键文件仍继续") ──
[ -f dist/index.html ] || { echo "FATAL: 产物缺 dist/index.html"; exit 1; }
[ -d dist/assets ] || { echo "FATAL: 产物缺 dist/assets"; exit 1; }
N_JS=$(find dist/assets -name '*.js' | wc -l)
[ "$N_JS" -gt 0 ] || { echo "FATAL: dist/assets 无 .js"; exit 1; }
grep -q "NIGHTLY" dist/index.html 2>/dev/null && echo "WARN: dist 含 NIGHTLY 标记(异常, 应无)"
echo "{ \"source\": \"assert\", \"tag\": \"$FE_TAG\", \"commit\": \"$FE_DESC\", \"buildJs\": $N_JS }" > dist/VERSION.txt
echo "[OK] dist 就绪: $(du -sh dist | awk '{print $1}') assets=$N_JS version=$FE_TAG@$FE_DESC"
