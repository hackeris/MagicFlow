# workspace 门设计(产品化体验对齐)

> 2026-09-05 设计稿;**W1 已实施(2026-09-05 同日真机验证闭环)**。目标:把「App 打开即起后端+自动加载画布」的 POC 行为,收敛为官方一致的门控体验——**用户创建/打开环境后,后端与内容才出现**。
>
> **⚠ 2026-09-05 调研定谳(修正前提, 见 §7)**:官方 "Team Workspaces" 是**云账户/计费作用域**(cloud.comfy.org + Firebase 登录),本地开源后端无任何 workspace 端点(v0.34 server.py 全检 0 命中);官方桌面首屏门户 = **Electron 壳自绘的 Chooser(环境瓦片)**,webview 内前端无 Node 运行时、无法自起后端,壳也不监听前端 workspace 事件。**结论:门户=壳层 UI(与官方 Chooser 同构)是唯一正确形态;原 W2「前端升级出 Web 门户」的前提被推翻,W2 取消。**

## 1. 背景与差距

| | 官方新版(2026) | 我们当前 |
|---|---|---|
| 首屏 | 工作空间门户(新建/打开列表) | 直接画布(默认模板,有内容) |
| 后端启动 | 用户选择空间后(会话创建时刻) | App `onAppear` 立即 `launch()`(NCP+ComfyUI) |
| 模型 | 空间内配置/按需 | 启动即 `ensureModel`(缺则 rport 自动下载) |
| 前端版本 | workspace 化前端 | pin v1.54.1 + 0.34.0(**无 workspace 概念**,已 grep 证实) |

根因:pin 版本代差 + `Index.ets` onAppear 自动化(POC 验收便利,现为产品行为)。

## 2. 设计(三层)

```
┌─ ArkTS UI 层 ──────────────────────────────┐
│ ① 首页 = 工作空间门户(新建/打开列表)          │
│     新建 → 弹出参数(名称/规格 256/512 提示)   │
│     打开 → 列出已有空间(设备 localStorage/DB) │
│ ② 选中事件 → startComfyChild(仅此一次)       │
└──────────────┬──────────────────────────────┘
               │ startComfyChild(延迟到用户动作)
┌─ NAPI/后端层 ─┴─────────────────────────────┐
│ startComfyChild 签名不变(entryParams),       │
│ 只把"调用者"从 onAppear 改为 门户事件;         │
│ 保留"验证模式"入口: COMFY_SMOKE=1 时 onAppear  │
│ 仍自动启动(verify_smoke.sh 黑盒判据不依赖 UI)  │
└──────────────┬──────────────────────────────┘
└─ 前端(Web) ── workspace 版前端(升级 pin) ─────┘
```

**关键决策**:
1. **后端 API 不动**:`libentry.so` 的 `startComfyChild`/`stopChild` 原样,只改调用时机 → 最小侵入;
2. **验证模式**(`COMFY_SMOKE=1`):门户上线后 smoke 仍一条命令(保持 verify 判据);
3. **前端升级另设一阶段**:0.34/1.54.1 → 带 workspace 的新前端(版本号实证后定,见 §4 风险清单)。

## 3. 实施阶段

| 阶段 | 内容 | 验收 |
|---|---|---|
| **W1 门户 UI + 延迟启动(不动前端 pin)** ✅(2026-09-05 已实施) | Index.ets 改造:门户视图(标题/新建环境按钮/我的环境列表/新建对话框);onAppear 仅 loadWsList(零自动);launch/ensureModel 链移入「创建/打开环境」动作 | 打开 App:门户出现、**后端进程未起**(ps 验证 ✅);打开/新建环境→起后端→8188 就绪→画布(✅ 60s);`make verify` 经门户驱导(uitest)仍 PASS(待归档);回退=go Index.ets 保留段还原 |
| **W2 前端 pin 升级(workspace 化)** ❌ **取消** | 调研推翻前提:官方 Web 前端无本地 workspace 门户(云/壳所属);前端保持 pin 1.54.1(与后端 0.34.0 官方同期配套) | —(详见 §7) |
| **W3 模型/规格选择进入环境体验**(未实施) | startComfyChild 传规格;ensureModel 已随 W1 延迟到新建环境动作(建→拉取);规格选择 UI 待做 | 创建环境才触发模型检测/下载(状态:ensureModel 已随动,规格选项暂以内置文案 256 提示) |

**W1 实测要点(2026-09-05)**:结构=Index.ets 单文件双视图(else=门户/if active=Web),数据=filesDir/workspaces.json;[平台教训] UIAbility.onCreate 访问 want.parameters 即毒化启动(白屏死锁无日志)→ **外部信号一律走 UI 手势/文件,绝不进 onCreate**;verify 门户驱导=uitest(设备无 input 命令)+ dumpLayout 动态定位(文本/hint 节点中心),每次注入前 aa start 拉回前台(防误触)。

## 4. 风险与回归清单(升级前端时)

- [ ] patch 01-12 基线漂移(pin 树不同 commit → fetch 后 `run_repro_chain` 立即暴露, 失败即 FATAL);
- [ ] queue/WebSocket 接口(`/prompt`,`/history`,`/system_stats`)兼容 —— 探针/smoke 全覆盖:
      `make verify` 5 判据 + 手工出图(256, 字节比对);
- [ ] 前端 dist 与 comfyui 后端版本配对(官方 dist 版本表);
- [ ] `ensureModel`/模型下载接口不变(request.downloadFile, 与 rport 链无关);
- [ ] 镜像与复现链:pin 变更 → bless_manifest + 全量复现一次(worktree)验证。

## 5. 验收判据(最终)

1. 首屏=门户,**无自动后端**(`ps` 无 Native_libcomfy_child);
2. 创建空间 → 后端起 → 8188 画布场景就绪;模型缺失时"新建"动作触发下载(进度可见);
3. 打开已存在空间 → 秒开(无重新下载);
4. `make verify`(smoke 模式)全 PASS;
5. 出图判据保持:MM4x<2.0s、256 出图 <180s。

## 6. 回退

- W1:Index.ets git 还原即可;W2:pin 表还原 + `make fetch` 重拉(锚校验);
- 后端 API 从未变,风险集中在 UI/WEB 层。

## 7. 调研定谳(2026-09-05, 双人并行源码级调研)

| 问题 | 结论 | 证据 |
|---|---|---|
| 官方 workspace 门户 = Web 前端? | 否。Web 前端自托管形态首屏=`/` 画布;workspace 门户是(a) Comfy Cloud 云构建(isCloud,需 Firebase+计费)或(b) Desktop 壳的 Chooser | ComfyUI_frontend src/router.ts;ChooserView.vue |
| workspace 后端端点? | 本地开源后端无任何 workspace 概念(v0.34 server.py 全文 0 命中,无 /workspaces);前端 workspace API 全部指向 cloud.comfy.org | v0.34.0/server.py;workspaceApiUrl.ts |
| Desktop 门户怎么起后端? | Electron 主进程 spawn(chooser 点击→IPC launch→spawnComfy);webview 无 Node 运行时起不了进程;壳不监听前端 workspace 事件 | Comfy-Desktop src/main/lib/ipc/sessionActions/launch.ts、attach.ts |
| 官方术语 | "ComfyUI 环境(Environment)/New Install",非 workspace(后者=云账户作用域) | ChooserFamilyGrid.vue "New Install tile" |
| 升级组合 | 前端 1.54.1 + 后端 0.34.0 = 官方同期配套,无需升级 | docs.comfy.org/zh/changelog 版本-前端对照 |
| 可拦截信号(若走 Web 门户路线) | 官方无生命周期钩子;唯一确定性拦截点=Storage.setItem 的 Comfy.Workspace.*(sessionStorage/localStorage) | workspaceConstants.ts/workspaceAuthStore.ts(persist 写点) |

**对本项目的改变**:W2 取消;门户=ArkTS 壳(官方同构);"打开环境才起后端"= Chooser 点击→launch 的 ArkTS 对应物。W3 顺延(模型/规格配置未来进入环境体验)。
