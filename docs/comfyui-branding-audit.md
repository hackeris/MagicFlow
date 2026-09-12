# 前端 ComfyUI 品牌/官方云/外链面排查清单(2026-09-08)

> 背景:本项目是基于 ComfyUI 的衍生作品(自建 fork 前端 + 国产设备适配),前端里仍带了
> 官方品牌、Comfy 云登录入口、comfy.org 外链、官方遥测等"与官方绑定"的展示面,
> 可能让用户误以为这是官方出品/官方账号可用。本文为**排查清单**,每项含:位置、
> 什么时候会展示给用户、建议处置。
> **处理状态(2026-09-12 核查)**:A2(桌面 label)已随品牌更名完成;其余各项经 fork 源码
> 核实**仍未处理**(A1/A3/A4/B/C/D/E/F)—— 其中 **D1 遥测是隐私底线,建议优先**。
> 清单前提:前端构建非 isCloud(web 构建),云计费/订阅等大多已门控隐藏;以下均为
> 实测可以在我们的构建里出现的面。

## A. 品牌标识(让用户以为这是"ComfyUI"本身)

| # | 位置 | 展示时机 | 建议 |
|---|---|---|---|
| A1 | `src/views/UserSelectView.vue:7` `<h1>ComfyUI</h1>` | **启动后第一屏**(本地用户选择页)大字标题 | 改为本产品名(如 MagicFlow) |
| A2 | 应用图标/桌面标签(label) | 系统桌面/任务栏 | ~~改为本产品名~~ **✅ 已完成(2026-09-12 核查)**:`entry/src/main/resources/base/element/string.json` 中 EntryAbility_label 已是"梦幻之流" |
| A3 | 窗口标题(workflow name 渲染逻辑引用 ComfyUI) | 应用内页面标题栏 | 改标题模板 |
| A4 | 浏览器/UI 里的 "ComfyUI" 文案(菜单项 "ComfyUI" 字样) | 各处 | 逐项替换 |

## B. Comfy 官方云登录入口(点了登录不了,误导最重)

| # | 位置 | 展示时机 | 建议 |
|---|---|---|---|
| B1 | `src/components/dialog/content/signin/*`(SignInContent/SignInForm/PasswordFields) | 登录对话框(用户主动点"Sign in"触发) | 移除登录入口触发点+组件(本地版无登录语义) |
| B2 | `src/components/dialog/content/ApiNodesSignInContent.vue` + `actionbar/PartnerNodesEducationCard.vue`(GraphView 挂载)+ `LocalRunButtonWrapper.vue` | **主画布**:提示"伙伴节点"的教育卡;本地运行按钮点击若工作流含 Comfy 云 API 节点 → 弹"登录 Comfy 云"对话框 | 隐藏教育卡;本地运行按钮去掉 Partner 弹窗 |
| B3 | `UserPanel.vue`(设置→用户面板,`useSettingUI.ts:147` 注册)含 support@comfy.org 链接 | 设置页"用户"面板 | 整块移除/替换为本产品支持说明 |
| B4 | 登录对话内 comfy.org/terms/privacy 链接 + hello@comfy.org | 登录对话框内 | 随 B1 移除 |

## C. 外部网页入口(comfy.org / GitHub 等)

| # | 位置 | 建议 |
|---|---|---|
| C1 | `SignInContent.vue` hrefs(comfy.org 条款/隐私) | 随登录移除 |
| C2 | `TopUpCreditsDialogContentLegacy`(comfy.org/cloud/enterprise 链接,官方充值入口) | 确认 isCloud 门控后确认不再可达;若存在即屏蔽 |
| C3 | 帮助/文档类入口(main.ts 等注册的 openDocs 类菜单,源码注释中多个 comfyanonymous 链接) | 注释链接无需处理;UI 层的 docs/github 打开入口改为自建帮助页或无 |

## D. 遥测上报(隐私 + 官方关联,重点)

| # | 位置 | 说明 | 建议 |
|---|---|---|---|
| D1 | `src/bootstrap.ts:2-4` **无条件 `initDatadogRum()`** + `initDatadogRum.ts:16-24` 仅当 `X-Frontend-Version == __COMFYUI_FRONTEND_COMMIT__` 才正式上报 | 我们的自建 dist 若保留了与 comfyui-frontend 发布相同的 commit 标识,设备上运行时**会向 Datadog 上报 RUM 数据**(请求会发出,目标域名不可达时静默失败)——既涉隐私又关联官方 | **必改**:直接删除 boot 调用或把 initDatadogRum 门控改为显式 false |
| D2 | Mixpanel/PostHog 等 providers(platform/telemetry/providers/cloud/*) | 同类官方云遥测 | 确认随 D1 一并废除 |

## E. 官方云功能 UI(多为 isCloud 门控,确认即可)

| # | 位置 | 判断 |
|---|---|---|
| E1 | PricingTable / subscription / TopUpCredits / CloudAuthTimeoutView / UploadModelFooter 等 `src/platform/cloud/*` | isCloud 构建为 false 时大多不展示;保留代码无害;若打包体积敏感可 tree-shake 掉 |

## F. 待确认项(需要看运行时机)

- 登录入口按钮在非 isCloud 构建下不出现?还是仍显示?(authStore/router 的顶部触发逻辑未逐行确认——建议处理时在真机 dump 一次页面 DOM/截图核实)
- 设备后端 `init_builtin_api_nodes()` 默认加载 37 组官方 API 节点 → 节点面板上会有一堆 "OpenAI/Claude/…” 但认证(api_key_comfy_org)传不进 → **工作流里用了必然报"需云账号"错误**。是否保留展示/禁用,需产品决定(与 B2 联动)。

## 处置优先级建议

1. **D1**(遥测)— 隐私底线,首个处理;
2. **B2**(主画布伙伴节点卡/运行弹窗)— 用户最常撞见;
3. **A1/A2**(启动首屏/桌面名)— 品牌正名;
4. **B1/B3/B4 + C**(登录体系)— 随产品化一起清;
5. E/F 按产品决定。

> 处理方式:全部在前端 fork(thirdparty/comfyui-frontend)改源码 + 重新构建 dist,
> 与既有前端构建链一致;改动记录证据串/注释,避免下次 fetch_externals 覆盖。
