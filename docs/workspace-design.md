# workspace 门设计(产品化体验对齐)

> 2026-09-05 设计稿(未实施)。目标:把「App 打开即起后端+自动加载画布」的 POC 行为,收敛为官方新版一致的门控体验——**用户创建/打开工作空间后,后端与内容才出现**。

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
| **W1 门户 UI + 延迟启动(不动前端 pin)** | Index.ets 改造(onAppear 去 launch/ensureModel; 门户页; 选择→launch; 显示"后端就绪"状态) | 打开 App:门户出现、**后端进程未起**(ps 验证);创建空间→起后端→8188→画布就绪;`make verify`(COMFY_SMOKE=1)仍 PASS |
| **W2 前端 pin 升级(workspace 化)** | comfyui-src + frontend-dist pin 更新;重跑 `make verify` + 手工空间流程 | workspace 门户在 Web 层原生呈现(本地前端与 app 门户二选一或融合, 见 W2 决策) |
| **W3 模型/规格选择进入空间体验** | startComfyChild 传规格(256/512 界限由 built-in 提示);ensureModel 延迟到新建空间动作 | 创建空间才触发模型检测/下载 |

**W1/W2 解耦可独立**:W1 用现有 1.54 前端(门户在 ArkTS 层),W2 再升级。

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
