# 品牌正名与云登录面切除

> 2026-09-24 设计稿。本文是 `docs/comfyui-branding-audit.md` 的**执行设计**与**部分推翻**——
> 该审计清单写于 2026-09-08，多项结论已被代码演进推翻，需一并更新。

## 1. 决策

**切除官方账号登录面**（登录入口、User 面板登录块、充值入口、partner gate、
官方 API 节点），**品牌正名为「梦幻之流」**。

**用户的外部 API 需求不靠这条路满足**：W4 已交付自研 `OHOS_API_Text/Image/HTTP`
三节点 + 自研密钥端点（`GET/POST /ohos/apikeys`）。用户自己填第三方 key（含
api.comfy.org 的 key），**由后端发请求**，网络可达性由用户自行解决。这条路与前端
登录 UI 无关，因此切除登录面**不损失外部 API 能力**。

**因此不切生产配置**（`USE_PROD_CONFIG=true`）——该开关的唯一价值是让 comfy.org
正式账号能登录；登录既然不保留，切它无意义。

## 2. 复核结论（审计清单的实际状态）

2026-09-24 对 `thirdparty/comfyui-frontend`（v1.54.4 + ohos 定制）逐项复核。

### 2.1 已消解 —— 审计文档需改状态

| 项 | 审计原文 | 实际状态 | 证据 |
|---|---|---|---|
| D1 Datadog RUM | "必改，隐私底线" | **已消解** | `src/bootstrap.ts` 的调用被 `if (__DISTRIBUTION__ === 'cloud')` 包裹，我们是 localhost ⇒ **编译期死分支**。产物验证：`dist/assets/*.js` 搜不到 `prod-v2`、`resolveDeployEnv`、clientToken。另 `initDatadogRum.ts:64-83` 有 hostname 白名单兜底 |
| D2 遥测 providers | "确认随 D1 一并废除" | **已消解** | `src/platform/telemetry/providers/cloud/` 下 9 个 provider 只在 `initTelemetry.ts` 内动态 import，该函数首行即 `if (!IS_CLOUD_BUILD) return`（`:9,19`）；`main.ts:52-54` 调用点也被 `isCloud` 包裹。**无一个是无条件执行的** |
| E1 云功能 UI | "确认 isCloud 门控" | **已消解** | `src/router.ts:25-28,61` 的 `cloudOnboardingRoutes` 非 cloud 下为空数组，`/cloud/*` 不注册。残留仅"设置里 PlanCredits 面板的旧版账单 UI" |

### 2.2 审计文档漏记项

`GraphCanvas.vue:594` → `releaseStore.initialize()`：**启动后自动发出**
`GET {api}/release-notes/…`，参数含 `current_version / form_factor / locale /
deploy_environment`（`src/platform/updates/common/releaseStore.ts:249-284`）。
拦截条件 `if (!isCloud && !showVersionUpdates) return`，而
`Comfy.Notification.ShowVersionUpdates` 默认 **true**
（`src/platform/settings/constants/coreSettings.ts:479-484`）⇒ localhost 下**启动即外发**。
审计文档未记录此项。

### 2.3 切除对象 —— 这些在 localhost 下真实可达

| 项 | 位置 | 现象 |
|---|---|---|
| 顶栏登录按钮 | `TopMenuSection.vue:87`、`WorkflowTabs.vue:121` | **无门控**，点开即官方登录对话框（Google/GitHub/邮箱 + Turnstile）。`main.ts:74` 还无条件初始化 Firebase |
| 登录对话框 | `src/components/dialog/content/signin/` | 内含 comfy.org 条款/隐私/hello@comfy.org；**打开即探测** `cloud.comfy.org/cdn-cgi/trace`（`useRegionGate.ts:15-19` → `networkUtil.ts:17`） |
| User 设置面板 | `useSettingUI.ts:140-149` | **无条件注册**，未登录时显示「Sign in / Sign up」 |
| 充值入口 | `CurrentUserPopoverLegacy.vue:92-100` | 可打开 `TopUpCreditsDialogContentLegacy`（comfy.org/cloud/enterprise 链接） |
| partner gate / 教育卡 | `usePartnerNodesRunGate.ts`、`GraphView.vue:28` | `PartnerNodesEducationCard v-if="!isCloud"`——**官方专门在非云构建显示**；`partnerRunGateEnabled` 默认 true ⇒ 图含官方 API 节点时 Run 按钮变 "Sign in to run" |
| "Update ComfyUI" 菜单 | `HelpCenterMenuContent.vue:404-415` | 门控 `!isDesktop && !isCloud` **恰好命中 localhost** |
| 窗口标题 | `useBrowserTabTitle.ts:12-13` | 兜底 `'ComfyUI'` + 后缀 `' - ComfyUI'` |
| 首屏大字 | `UserSelectView.vue:7` | `<h1>ComfyUI</h1>`（我们后端非 multi-user ⇒ 实际不可达，但文案仍在） |

**后端侧确认**：`init_api_nodes=not args.disable_api_nodes` 在 `patches/comfyui-src-ohos-changes.patch:527`
中是**上下文行**（非我们的改动），启动参数 `phase0|127.0.0.1|8188|--cpu|pyroot=…`
未带 `--disable-api-nodes` ⇒ **官方 37 组 API 节点确实加载**，上述 partner gate 真能撞上。

## 3. 改动设计

### 3.1 后端参数层 —— 一个参数解决三项

`entry/src/main/ets/pages/Index.ets:162` 加参数：

```ts
const entryParams = `phase0|127.0.0.1|8188|--cpu|--disable-api-nodes|pyroot=${pyroot}`;
```

官方为离线部署设计的开关（`comfy/cli_args.py:214`），三层防护：

| 层 | 机制 | 解决 |
|---|---|---|
| 后端 | 不加载 37 组官方 API 节点 | ⇒ 前端 `usePartnerNodesInGraph` 检测不到 `api_node` ⇒ **partner gate / 教育卡 / "Sign in to run" / 价格徽标全部自然失效，无需为此改前端** |
| 网络 | CSP `connect-src 'self' data:`（`server.py:488-500`） | ⇒ 前端 JS **内核级禁止外联**：兜住 `cloud.comfy.org` 地区探测、`api.comfy.org` 注册表、`media.comfy.org` 视频 |
| 前端 | `releaseStore.ts:260` 读 argv 后 `return` | ⇒ release notes 拉取**主动跳过** |

**已验证不影响**：
- 自研 `OHOS_API_*` 节点落在 `custom_nodes/ohos_external_api/`，走 `init_custom_nodes`
  路径 ⇒ 不受 `init_api_nodes` 影响
- 自研节点由**后端** Python 发请求 ⇒ 不受前端 CSP 影响
- `verify_smoke.sh` 的 W4 判据查 `/object_info` 含 `OHOS_API_Text|Image|HTTP` ⇒ 零回归

**代价**：节点面板少 37 组官方 API 节点——它们在本地**本就是死路**（需 comfy.org 账号
token，而 token 由前端登录态注入）。

### 3.2 前端 fork 改动

| # | 项 | 位置 | 改法 |
|---|---|---|---|
| 1 | 顶栏登录按钮 | `TopMenuSection.vue:87`、`WorkflowTabs.vue:121` | 不渲染 |
| 2 | User 设置面板 | `useSettingUI.ts:140-149` | 摘除注册，腾出的位置改挂**「关于」面板** |
| 3 | 充值入口 | `CurrentUserPopoverLegacy.vue:92-100` | 不渲染 |
| 4 | "Update ComfyUI" 菜单 | `HelpCenterMenuContent.vue:404-415` | 不渲染 |
| 5 | 窗口标题 | `useBrowserTabTitle.ts:12-13` | `'ComfyUI'` → `'梦幻之流'`；后缀 `' - ComfyUI'` → `' - 梦幻之流'` |
| 6 | 首屏大字 | `UserSelectView.vue:7` | `ComfyUI` → `梦幻之流` |
| 7 | 「关于」面板（新建） | 取代 #2 腾出的注册位 | 「基于 ComfyUI 构建 · GPL-3.0」+ 后端版本号 |

**#7 的落点说明**：复用 User 面板腾出的注册位（设置 → General 组），既移除登录块，
又给合规声明一个用户可达的入口，无需新造入口。

**「不渲染」的实现约定**：在 fork 源码中**移除挂载点/注册项**，而非运行时
`v-if="false"` 隐藏 —— 后者会在升级上游时留下难以察觉的死代码。命令层同理：
`useCoreCommands.ts:1010-1017` 的 `Comfy.User.OpenSignInDialog` 在入口全堵死后已无
触发点，一并移除注册。

**保留**：Help 菜单的 `docs.comfy.org` / Discord / GitHub / Forum / Support 外链 ——
用户**主动点击**的生态入口，去掉伤可用性；且 `window.open` 不受 `connect-src` 限制
（CSP 管资源加载，不管导航）。

### 3.3 关闭 release notes 自动拉取（双保险）

`src/platform/settings/constants/coreSettings.ts:479-484`：
`Comfy.Notification.ShowVersionUpdates` 的 `defaultValue` 由 `true` 改为 `false`。

理由：3.1 的 argv 检查已覆盖，但那是**依赖后端参数**的间接机制；显式改默认值使其
在参数缺失时也不会外发。改动一行，值得。

### 3.4 应用图标（程序生成）

现有资源全部是 DevEco 脚手架模板图（蓝色圆角方块 + 四个白色小方块，2026-08-20 时间戳，
从未更换）：

| 文件 | 尺寸 | 用途 |
|---|---|---|
| `entry/src/main/resources/base/media/startIcon.png` | 144×144 | 启动窗口图标 |
| `entry/src/main/resources/base/media/{background,foreground}.png` | 1024×1024 | 桌面分层图标 |
| `AppScope/resources/base/media/{background,foreground}.png` | 1024×1024 | 同上 |

改用首页视觉语言（`Index.ets`）：深空渐变底 `#0A0E1E→#3B2A78` + 紫蓝渐变圆
`#8B5CF6→#4F46E5` + 星光符号。PIL 生成（环境已有 12.2.0）。

注：程序生成的简洁图标，非专业设计。

## 4. 明确不做

- 不物理删除 `signin/`、`platform/cloud/` 等云组件目录（保持"精准切除"）
- 不改 Help 菜单的官方文档/社区外链
- 不动 `Index.ets` 的 47 处硬编码颜色
- **不切生产配置**（`USE_PROD_CONFIG`）——见 §1
- 不改后端 Python 代码（本次改动限于：启动参数一行 + 前端 fork + 图标资源）

## 5. 验证计划

| # | 验证 | 判据 |
|---|---|---|
| 1 | 后端参数效果（宿主预验证，快） | 加参数跑后端：`curl -I` 确认响应含 CSP 头；`/object_info` 中官方 API 节点消失、`OHOS_API_*` 仍在 |
| 2 | **真机熔断点：WebSocket** | CSP 的 `connect-src 'self'` 理论上覆盖同源 `ws://127.0.0.1:8188/ws`，**必须实测**。若被误伤 ⇒ 退回"只关 API 节点 + 改前端 gate"路线（此时外联阻断改由 §3.3 与 fork 门控承担，失去内核级兜底） |
| 3 | 前端构建 | `pnpm build` 通过 |
| 4 | 打包 | `make hap` BUILD SUCCESSFUL + prebuilt 325 文件闭环 PASS |
| 5 | 真机 UI | 截图：顶栏无登录按钮、窗口标题正确、设置里有「关于」项、图标已换 |
| 6 | 零回归 | `verify_smoke.sh --port 8191` 12 项 ALL PASS |
| 7 | 外发关闭 | 启动后确认无 `release-notes` 请求 |

## 6. 风险

| 风险 | 说明 | 应对 |
|---|---|---|
| CSP 误伤 WebSocket | 核心链路，断了整个应用废掉 | 列为**熔断点**（验证 #2），先于其他改动验证 |
| CSP 误伤其他前端能力 | `img-src 'self'` 阻断外部图片、`frame-src 'self'` 阻断外部 iframe | 验证阶段观察；如有需要，可在 fork 中调整该中间件的 CSP 串 |
| 前端 pin/锚同步 | dist 变化 ⇒ `config/externals.pins.tsv` 锚必须同升（fork 纪律：`fetch_externals` 会 checkout 回旧 pin，静默吞掉改动） | 按既有 pin 流程同步 |

## 7. 文档同步

- `docs/comfyui-branding-audit.md`：D1/D2/E1 改标已消解（附证据）；补录 release notes 项；
  B 类按 §2.3 更新为"确认可达"
- `docs/status-and-next.md`：§3 表格 ⑦ 行更新
