# 前端 fork 化与项目结构重整(F口)

> 2026-09-05 依据用户 Goal 制定。背景:当前前端 = 官方 **dist 成品(NIGHTLY 渠道, 标 1.54.1)**,无源码、无构建、不可定制。
> 用户决策:**fork 官方前端, 基于稳定版本(非 nightly)定制, 自维护前端源码仓库**,主项目结构随之重整。

## 1. 事实盘点(已实测)

| 项 | 现状 | 证据 |
|---|---|---|
| 当前前端 | 官方 dist zip 成品, bundle 内大量 `NIGHTLY` 标记(版本号 1.54.1) | frontend_static/assets/main-*.js |
| 前端选择 | 官方稳定最新 tag **v1.54.4**(sha `8d24aed6`, 无 NIGHTLY) | git ls-remote tags |
| 引擎要求 | **node >=25 <26**, pnpm@11.13.1 | 源码 package.json engines/packageManager |
| 宿主工具 | node 22.22.1(太旧) → 已装独立 **node v25.9.0** 于 `/opt/node-v25.9.0-linux-x64` | — |
| 构建入口 | `pnpm build`(tsc 类型检查 + vite build → `dist/`) | package.json scripts |

## 2. 目标结构与接入(前端自维护仓库)

```
comfyui 主仓库
├── thirdparty/
│   ├── comfyui-frontend/        ★ 前端源码(submodule 化, 同 safetensors 先例);
│   │      上游 remote = 官方 Comfy-Org/ComfyUI_frontend@v1.54.4;
│   │      用户 fork 后 remote 换成自维护仓库(go 记录, fetch 链不依赖 remote 名);
│   │      定制改动落本树 feature 分支(与官方 tag 分叉, diff 可追踪)
│   ├── ohos-torch/ …(既有)  safetensors/ tokenizers/(既有 submodule)
├── scripts/
│   ├── fetch_externals.sh      + ⑤⑪ 前端源码 clone(@pins 中版本; 官方 dist zip 下载退场)
│   └── build_frontend.sh       ★ 新: 检环境(node25 路径) → pnpm install --frozen-lockfile
│                                  → pnpm build → 产物 dist/ 断言(index.html+assets) → 版本 stamp
├── config/externals.pins.tsv   + comfyui-frontend: type=git tag=v1.54.4 sha=8d24aed6…
└── (make 链) make stage 的 FE_SRC = build_frontend.sh 产物(替换官方 dist 解压段)
```

**接入语义**:
- 官方 `comfyui-frontend-dist.zip` 下载(现有 fetch ⑤ 段)删除,由 `fetch(源码)+build_frontend.sh(自产 dist)` 替代;
- stage `make_comfyui_stage.py` 1b 段校验锚改为: 自建 dist 的 `index.html` + FE 版本字符串(`window.__COMFYUI_VERSION__`/环境印记);
- 沉淀顺序: build 产物 → prebuilt(不入 zip? dist 已入 zip… dist 文件走 zip 的 stage 段,体积比官方 dist 相近;zip 锚/manifest 更新)。

## 3. 稳定版本策略

- 跟随官方 **release tag**(当前 v1.54.4 = 2026-09-05 稳定), 不用 main/nightly;
- 版本升级路径: 官方新 release → 主项目升级 tag(改 pins+rerun fetch/build) — 小/中等成本(构建分钟级);
- 用户定制散点: 在 `thirdparty/comfyui-frontend`(独立 git 分叉)记 commit; 官方 tag 升级时 rebase, 补丁冲突在定制的 diff 里消化。

## 4. 定制层设计(将来, 本项目无定制先例时守住)

1. **不改源码的方案优先**: custom node 前端扩展(web/index.js, 官方自动加载)/ ArkWeb 注入(壳层);
2. **真需求落源码**: 在该仓库改+`patches/`或独立 commit; 构建链自动带回;
3. 禁止直接改官方 dist bundle(前(未定)/手改无交付物)。

## 5. 风险与回退

| 风险 | 对策 |
|---|---|
| node26 换代导致 corepack/pnpm 崩 | 固定 PATH 用 /opt/node25(脚本写死路径+检查) |
| `pnpm build`(typecheck) 偶发与 upstream CI 不同 | build 失败即停(不产 dist), 报差异再议 |
| 自建 dist 与官方 dist 结构/内嵌资源差异 | 构建后 diff(dist 列表+index.html 锚), 若有出入逐项记入本文件 |
| 升级大版本同步成本 | 保持"稳定 tag + 独立 submodule"模式, 升级=rebase 定制 diff(有 git 历史) |
| 用户已下载官 dist 过渡期 | 旧的 `externals/comfyui-frontend-dist.zip` 可留档回退, 不入链 |

## 6. 验收(本 Goal 完结点)

- [x] `fetch` → `build_frontend.sh` → `make stage` → `make zip` 全绿(自建 dist 替换官方, 锚=自建产物);
- [x] 真机 v1.54.4 前端正常渲染(2026-09-05 实测截图), NIGHTLY 渠道徽章消失(旧版右上角有, 新版无);
- [x] `frontend_static/index.html` 锚更新(19064c6f…, 与 pins.tsv 一致);
- [x] 结构文档同步(AGENTS.md 快速定位 + 本文档)。

**升级中发现的关联 bug(已修)**: 解包重建(run58 指纹)只删 `lib/python3.12`,`comfyui/` 不在删除范围 →
zip 换版后新旧前端混装残留(实测 assets 1741 文件 vs 自建 ~1100)。**Stdlib.ets 补**: 指纹不匹配分支
额外整删 `pyroot/comfyui/frontend_static/`(零用户数据)后解包。教训: 新增于 zip 的非 stdlib 大目录,
解包清档必须同步覆盖。
