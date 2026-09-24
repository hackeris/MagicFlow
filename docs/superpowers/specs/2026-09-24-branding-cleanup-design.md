# 品牌正名 + 账号登录面门控（保留 API Key 能力）

> 2026-09-24 起草，2026-09-25 定稿。
> 本文是 `docs/comfyui-branding-audit.md` 的执行设计，并对该清单的部分结论作修正。

## 1. 决策

1. **砍掉账号登录体系**（OAuth / 邮箱密码 / 注册 / 订阅 / 充值）——**门控切断触达，不删代码**
2. **保留 API Key 能力** —— 官方 259 个 API 节点照常可用
3. **品牌正名为「梦幻之流」**
4. **切生产配置**（`USE_PROD_CONFIG=true`）—— API Key 验证的前提，见 §3.3

### 1.1 关键区分：API Key ≠ 账号登录

这是本设计的立足点，有代码为证：

| | 账号登录 | API Key |
|---|---|---|
| 依赖 | Firebase（Google 服务）+ OAuth 弹窗 | 仅一个 `X-API-KEY` 头 |
| 存储 | Firebase session | `localStorage['comfy_api_key']`（`apiKeyAuthStore.ts:15`）|
| 判定 | `firebaseUser !== null` | `isApiKeyLogin = apiKeyStore.isAuthenticated && firebaseUser.value === null`（`useCurrentUser.ts:18-19`）|
| 相互关系 | — | **与 Firebase 互斥**（同上），全程不碰 Google |

且 `SignInContent.vue:101` 的 API Key 入口按钮本就包在 `v-if="!isCloud"` 里 ——
**官方专门为非云构建留的路径**。`turnstile.ts` 注释也写明 OSS/localhost 有
"server-side loopback exemption covers local signup"。

⇒ 砍掉账号登录**不会**伤及 259 个 API 节点。

## 2. 复核结论（审计清单的实际状态）

2026-09-24 对 `thirdparty/comfyui-frontend`（v1.54.4 + ohos 定制）逐项复核。

### 2.1 已消解 —— 审计文档需改状态

| 项 | 审计原文 | 实际状态 | 证据 |
|---|---|---|---|
| D1 Datadog RUM | "必改，隐私底线" | **已消解** | `src/bootstrap.ts` 调用被 `if (__DISTRIBUTION__ === 'cloud')` 包裹，我们是 localhost ⇒ **编译期死分支**。产物验证：`dist/assets/*.js` 搜不到 `prod-v2`、`resolveDeployEnv`、clientToken。另 `initDatadogRum.ts:64-83` 有 hostname 白名单兜底 |
| D2 遥测 providers | "确认随 D1 一并废除" | **已消解** | 9 个 provider 只在 `initTelemetry.ts` 内动态 import，该函数首行即 `if (!IS_CLOUD_BUILD) return`（`:9,19`）；`main.ts:52-54` 调用点也被 `isCloud` 包裹。**无一个是无条件执行的** |
| E1 云功能 UI | "确认 isCloud 门控" | **已消解** | `router.ts:25-28,61` 的 `cloudOnboardingRoutes` 非 cloud 下为空数组，`/cloud/*` 不注册 |

### 2.2 审计文档漏记项

`GraphCanvas.vue:594` → `releaseStore.initialize()`：**启动后自动发出**
`GET {api}/release-notes/…`（参数含 `current_version / form_factor / locale /
deploy_environment`）。默认开启（`coreSettings.ts:479-484`）⇒ localhost 下**启动即外发**。

### 2.3 门控对象 —— 这些在 localhost 下真实可达

| 项 | 位置 | 现象 |
|---|---|---|
| 顶栏登录按钮 | `TopMenuSection.vue:87`、`WorkflowTabs.vue:121` | **无门控**，点开即官方登录对话框。`main.ts:74` 还无条件初始化 Firebase |
| 登录对话框 | `src/components/dialog/content/signin/` | 含 OAuth / 邮箱密码 / 注册；**打开即探测** `cloud.comfy.org/cdn-cgi/trace`（`useRegionGate.ts:15-19`）|
| 对话框标题 | `dialogService.ts:245,251` → `ComfyOrgHeader.vue` | 标题栏是 ComfyOrg logo |
| User 设置面板 | `useSettingUI.ts:140-149` | **无条件注册**，未登录时显示「Sign in / Sign up」 |
| 订阅/充值区块 | `CurrentUserPopoverLegacy.vue:73,92-100,106-115,118` | 由 `canAccessSubscriptionFeatures`（localhost 恒 true）与 `showAddCredits` 控制 |
| "Update ComfyUI" 菜单 | `HelpCenterMenuContent.vue:404-415` | 门控 `!isDesktop && !isCloud` **恰好命中 localhost** |
| 窗口标题 | `useBrowserTabTitle.ts:12-13` | 兜底 `'ComfyUI'` + 后缀 `' - ComfyUI'` |
| 首屏大字 | `UserSelectView.vue:7` | `<h1>ComfyUI</h1>` |

**后端侧确认**：`init_api_nodes=not args.disable_api_nodes` 在
`patches/comfyui-src-ohos-changes.patch:527` 中是**上下文行**（非我们的改动），
启动参数未带 `--disable-api-nodes` ⇒ 官方 API 节点确实加载。

**本次明确不加 `--disable-api-nodes`** —— 它会卸载 259 个 API 节点并加 CSP 阻断外联
（`server.py:488-500`），与「保留 API Key 能力」直接冲突。

## 3. 改动设计

**总原则（用户指示 2026-09-25）**：让相关逻辑**无法生效、无法被用户触达**即可，
**不在代码里真删**。全部改动为「门控」，保持可逆、降低上游升级冲突。

### 3.1 账号登录面门控（4 处）

| # | 位置 | 改法 | 效果 |
|---|---|---|---|
| 1 | `SignInContent.vue:196` | `showApiKeyForm = ref(false)` → `ref(true)` | 对话框打开即显示 API Key 表单；`v-else` 整块（登录/注册/SSO/邮箱）**永不渲染** |
| 2 | `signin/ApiKeyForm.vue:69-72` | back 按钮加门控（`:70`） | `@back` 的目标是被砍掉的账号登录分支，按钮已无去处；留着即为坏 UX |
| 3 | `CurrentUserPopoverLegacy.vue:189` | `canAccessSubscriptionFeatures` 在该文件内覆盖为 `false` | **一处改动使 L73 / L106 / L118 三处订阅区块同时失效**。（不改其定义处 `useSubscription.ts:50-54`——那里被 cloud 组件共用，影响面过大） |
| 4 | `useSettingUI.ts:140-149` | User 面板从注册表摘除，位置改挂「关于」面板 | 移除账号面板入口，同时落实 §3.4 的合规声明 |

**保留不动**（它们是 API Key 能力的一部分）：
- 顶栏 `LoginButton` / `CurrentUserButton`（反映 key 是否已配置）
- `CurrentUserPopover` 的 Logout（`CurrentUserPopoverLegacy.vue:145`）—— 清除 key 的唯一入口
- `ApiKeyForm.vue` 主体
- `main.ts:74` Firebase 初始化（`initializeApp` 不发网络请求）
- `useCoreCommands.ts:1010` 的 `OpenSignInDialog` 命令（复用为 API Key 对话框入口）
- `signin/` 与 `platform/cloud/` 目录全部留原地

**已知残留（本次不处理）**：`SignInContent.vue:248` 的 `useRegionGate()` 在对话框挂载时
会探测 `cloud.comfy.org/cdn-cgi/trace` + google/baidu。它不含个人信息、失败即静默，
且与账号登录无关。留作观察项。

### 3.2 对话框标题

**该对话框的性质**：`dialogService.showSignInDialog()` 的容器（key = `'global-signin'`）
原本是**通用登录对话框**，5 个调用点中门控后仍可达 3 个（顶栏 LoginButton、
`ApiNodesSignInContent.onLogin`、`OpenSignInDialog` 命令）。由于 `SignInContent` 只剩
`ApiKeyForm` 分支（§3.1 #1），它实际已变成**「填 Comfy API Key」专用对话框**。

**标题组件的性质**：`ComfyOrgHeader.vue` 整个组件只有一张 32×32 的 ComfyOrg logo
（`comfy-logo-single.svg`，无文字）—— 是账号登录时代的**纯装饰**。
而 `ApiKeyForm` **自带完整内层内容**：

| key | 文案 |
|---|---|
| `auth.apiKey.title` | `API Key` |
| `auth.apiKey.description` | `Use your Comfy API key to enable API Nodes` |
| `auth.apiKey.generateKey` | `Get one here` → 链到 `{comfyPlatformBaseUrl}/login` |

**处置**：去掉外层 logo 标题（装饰，零信息损失）；**保留表单内的 comfy.org 引用**
（功能性 —— 用户需要知道 key 从哪来）。改法：`showSignInDialog` 的 `headerComponent`
不再指向 `ComfyOrgHeader`（组件保留原地不删）。

### 3.2.1 partner gate 文案残留（决策：不处理）

partner gate 链路仍可达（正确 —— 官方 API 节点保留）：工作流含 `api_node` 且未配 key 时，
Run 按钮显示 `Sign in to run`，点击弹 `ApiNodesSignInContent`（文案
`Sign in to run partner nodes`）。门控后已无 "sign in" 语义，实际动作是填 key。

**决策（2026-09-25）**：不改。功能是通的（点 Sign In → 打开 API Key 表单）；改 i18n 会
增加与上游的差异面，而该问题只在「含官方 API 节点 + 未配 key」时才撞见。

### 3.3 切生产配置

`scripts/build_frontend.sh` 的构建命令加 `USE_PROD_CONFIG=true`。

**为什么必做**：API Key 保存时前端调 `authStore.createCustomer()` →
`buildApiUrl('/customers')`，基址由 `getComfyApiBaseUrl()` 决定 =
`__USE_PROD_CONFIG__ ? 'https://api.comfy.org' : 'https://stagingapi.comfy.org'`
（`comfyApi.ts:15-17`）。当前构建落到 **staging**，用户的**生产 key 会被拒**。

注意两侧基址是**分开**的：

| 环节 | 走哪 | 受控于 |
|---|---|---|
| 前端校验 key（`createCustomer`） | 当前 staging ⇒ 需切 | `__USE_PROD_CONFIG__`（构建期） |
| 后端节点实际调用 | **已是 `api.comfy.org`**（`cli_args.py:262-265` 默认值） | `--comfy-api-base`（启动参数） |

**不受影响**：Turnstile 人机验证 —— `turnstile.ts:34-37` 中 `!isCloudBuild` 直接返回
`''`，localhost 构建不渲染该组件（服务端 loopback 豁免）。

### 3.4 品牌正名

| # | 位置 | 改法 |
|---|---|---|
| 1 | `useBrowserTabTitle.ts:12-13` | 兜底 `'ComfyUI'` → `'梦幻之流'`；后缀 `' - ComfyUI'` → `' - 梦幻之流'` |
| 2 | `UserSelectView.vue:7` | `<h1>ComfyUI</h1>` → `<h1>梦幻之流</h1>` |
| 3 | 设置面板（取代 User 面板注册位，见 §3.1 #4） | 「关于」项：「基于 ComfyUI 构建 · GPL-3.0」+ 后端版本号 |

#3 的理由：README 已声明衍生关系（`README.md:105,109-110`），但**设备上看不到 README**。
GPL-3.0 衍生作品分发时应有可达的版权声明。

### 3.5 关闭 release notes 自动拉取

`coreSettings.ts:479-484`：`Comfy.Notification.ShowVersionUpdates` 的 `defaultValue`
由 `true` 改为 `false`。

理由：非 API Key 能力的一部分；本应用前后端版本锁定（v1.54.4 / 0.34.0），提示了也无法更新。
API Key、节点、主动触发的请求均不受影响。

### 3.6 应用图标（程序生成）

现有资源全部是 DevEco 脚手架模板图（蓝色圆角方块 + 四个白色小方块，2026-08-20，
从未更换）：

| 文件 | 尺寸 | 用途 |
|---|---|---|
| `entry/src/main/resources/base/media/startIcon.png` | 144×144 | 启动窗口图标 |
| `entry/src/main/resources/base/media/{background,foreground}.png` | 1024×1024 | 桌面分层图标 |
| `AppScope/resources/base/media/{background,foreground}.png` | 1024×1024 | 同上 |

改用首页视觉语言（`Index.ets`）：深空渐变底 `#0A0E1E→#3B2A78` + 紫蓝渐变圆
`#8B5CF6→#4F46E5` + 星光符号。PIL 生成（环境已有 12.2.0）。程序生成的简洁图标，非专业设计。

## 4. 明确不做

- 不删除 `signin/`、`platform/cloud/` 等任何云组件代码（门控即可）
- 不加 `--disable-api-nodes`
- 不改 Help 菜单的官方文档/社区外链（用户主动点击的生态入口）
- 不动 `Index.ets` 的 47 处硬编码颜色
- 不改后端 Python 代码

## 5. 验证计划

| # | 验证 | 判据 |
|---|---|---|
| 1 | 前端构建 | `pnpm build` 通过；产物中可搜到 `api.comfy.org` |
| 2 | 打包 | `make hap` BUILD SUCCESSFUL + prebuilt 325 文件闭环 PASS |
| 3 | **API Key 链路（核心）** | 真机粘贴一个 comfy.org key → 保存成功、顶栏变已登录态；`/object_info` 中官方 API 节点在列；跑一个 API 节点确认 `X-API-KEY` 头生效 |
| 4 | 账号登录不可达 | 截图确认：对话框只有 API Key 表单；无 OAuth/注册/SSO 按钮；设置里无 User 面板；顶栏用户菜单无充值/订阅项 |
| 5 | 零回归 | `verify_smoke.sh --port 8191` 12 项 ALL PASS |
| 6 | 外发关闭 | 启动后确认无 `release-notes` 请求 |

**#3 需要你配合**：一个可用的 comfy.org API key（或你自行验证）。

## 6. 风险

| 风险 | 说明 | 应对 |
|---|---|---|
| `canAccessSubscriptionFeatures` 局部覆盖 | 只改 `CurrentUserPopoverLegacy.vue` 内引用，不改定义处 | 实施时确认该文件内无其它依赖此值的逻辑分支 |
| 门控点遗漏 | 账号登录可能有未发现的触发路径 | 验证 #4 以「运行时不可达」为准（截图 + 点击穷举），不以"改了代码"为准 |
| 前端 pin/锚同步 | dist 变化 ⇒ `config/externals.pins.tsv` 锚必须同升（fork 纪律：`fetch_externals` 会 checkout 回旧 pin，静默吞掉改动） | 按既有 pin 流程同步 |
| 上游升级冲突 | 门控改动落在官方文件上 | 每处改动加注释说明意图，降低下次升级的误删风险 |

## 7. 文档同步

- `docs/comfyui-branding-audit.md`：D1/D2/E1 改标已消解（附证据）；补录 release notes 项；
  **新增一节**记录「保留 API Key 能力是有意决策」，防止后来者照旧清单误清理
- `docs/status-and-next.md`：§3 表格 ⑦ 行更新
