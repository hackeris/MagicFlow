# 品牌正名 + 账号登录面门控 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让应用内的账号登录体系（OAuth / 邮箱密码 / 注册 / 订阅 / 充值）不可触达，同时保留 comfy.org API Key 能力（259 个官方 API 节点照常可用），并把应用品牌正名为「梦幻之流」。

**Architecture:** 全部改动落在自维护前端 fork（`thirdparty/comfyui-frontend`，分支 `ohos`），**只做门控、不删代码**——相关组件与逻辑原地保留，只切断注册表/渲染分支/初始值，使账号登录无法生效。改动经 `scripts/build_frontend.sh` 重建 dist，再由既有 HAP 链打包验证。

**Tech Stack:** Vue 3.5 + TypeScript + Pinia + Vite（前端 fork）；ArkTS/ArkWeb + Makefile（宿主工程）；Python 3 + PIL（图标生成）。

**参考 spec:** `docs/superpowers/specs/2026-09-24-branding-cleanup-design.md`

---

## 执行前必读（三个坑）

1. **工作目录**：本计划在**两个 git 仓库**里操作——主仓库 `/data/share/comfyui` 和 fork 子模块 `/data/share/comfyui/thirdparty/comfyui-frontend`。每个 git 命令一律用 `git -C <绝对路径>`，不要依赖 cwd（本次会话已因此失败两次）。
2. **绝不执行 `fetch_externals`**：它会把 fork checkout 回旧 pin，静默吞掉本次改动（见 `docs/frontend-fork-plan.md`）。Task 9 会主动升级 pin 来避免这个陷阱。
3. **验证文化**：改动是 UI 门控，官方 vitest 覆盖不到"用户能否触达"，因此**判据以真机运行时为准**（截图 + 点击），不以"代码改了"为准。

---

## 文件结构

| 文件 | 责任 | 本次动作 |
|---|---|---|
| `src/components/dialog/content/SignInContent.vue` | 登录对话框内容（账号登录 + API Key 两种模式共存） | 门控：初始即 API Key 模式 |
| `src/components/dialog/content/signin/ApiKeyForm.vue` | API Key 输入表单 | 门控：隐藏 back 按钮 |
| `src/services/dialogService.ts` | 对话框装配（内容组件 + 标题组件） | 去掉装饰性标题组件 |
| `src/platform/settings/composables/useSettingUI.ts` | 设置面板注册表 | 门控：不注册账号/订阅类面板 |
| `src/components/topbar/CurrentUserPopoverLegacy.vue` | 顶栏用户菜单 | 门控：隐藏订阅/充值区块 |
| `src/composables/useBrowserTabTitle.ts` | 窗口标题 | 品牌正名 |
| `src/views/UserSelectView.vue` | 多用户选择页 | 品牌正名 |
| `src/components/dialog/content/setting/AboutPanel.vue` | 设置→关于 面板 | 新增衍生声明 |
| `src/locales/en/main.json` | 英文文案 | 新增声明文案 key |
| `src/platform/settings/constants/coreSettings.ts` | 设置项默认值 | 关闭 release notes 拉取 |
| `scripts/build_frontend.sh`（主仓库） | 前端构建 | 切生产配置 |
| `scripts/gen_app_icons.py`（主仓库，新建） | 应用图标生成 | 新建 |

---

## Task 1: 前置 —— 确立基线

**Files:** 无改动（只读）

- [ ] **Step 1: 确认两个仓库的当前位置与状态**

```bash
git -C /data/share/comfyui status -sb | head -5
git -C /data/share/comfyui/thirdparty/comfyui-frontend status -sb | head -5
git -C /data/share/comfyui/thirdparty/comfyui-frontend log --oneline -1
```

预期：主仓库在 `master`，工作区只有 `thirdparty/safetensors`、`thirdparty/tokenizers` 两个未跟踪目录；
fork 工作区干净，HEAD 为 `bef1b1d`。
**若 fork 工作区不干净 → 停下，先问清楚那些改动是什么。**

- [ ] **Step 1b: 确保 fork 在 `ohos` 分支上（不是 detached HEAD）**

```bash
git -C /data/share/comfyui/thirdparty/comfyui-frontend branch --show-current
```

**若输出为空**（`git status` 显示 `## HEAD (no branch)`）—— 这是常态：`fetch_externals`
按 pin 执行 `checkout <commit>`，会把 fork 留在 detached HEAD 上，**此时提交会丢失**。切回分支：

```bash
git -C /data/share/comfyui/thirdparty/comfyui-frontend checkout ohos
git -C /data/share/comfyui/thirdparty/comfyui-frontend status -sb | head -2
```

预期：`## ohos`。`ohos` 与 pin 指向同一 commit，切换不改变工作区内容。

- [ ] **Step 2: 记录当前 dist 与锚（改动后的对比基准）**

```bash
md5sum /data/share/comfyui/thirdparty/comfyui-frontend/dist/index.html
cat /data/share/comfyui/thirdparty/comfyui-frontend/dist/VERSION.txt
grep -n "comfyui-frontend-index.html\|comfyui-frontend-src" /data/share/comfyui/config/externals.pins.tsv
```

预期：md5 = `641e0fec1e0c4f77676c1aead7fb210e`；pins 两行的 commit 均为 `bef1b1d`。
把这三行输出记到工作笔记里——Task 9 要拿它们做对照。

- [ ] **Step 3: 抓门控前的测试基线**

```bash
cd /data/share/comfyui/thirdparty/comfyui-frontend && pnpm test:unit --run src/components/dialog/content/SignInContent.test.ts src/components/dialog/content/signin/ApiKeyForm.test.ts 2>&1 | tail -20
```

预期：**全绿**。若有失败，先记录——那是既有问题，不是本次引入的（后续步骤的判断依据）。

---

## Task 2: 账号登录面门控（4 处）

**Files:**
- Modify: `src/components/dialog/content/SignInContent.vue:196`
- Modify: `src/components/dialog/content/signin/ApiKeyForm.vue:69-72`
- Modify: `src/platform/settings/composables/useSettingUI.ts:253-262`
- Modify: `src/components/topbar/CurrentUserPopoverLegacy.vue:187-195`

### 2A. 登录对话框只剩 API Key 表单

- [ ] **Step 1: 改初始值**

`SignInContent.vue:196`，把：

```ts
const showApiKeyForm = ref(false)
```

改为：

```ts
// 2026-09-25 ohos: 账号登录面整体门控 —— 初始即进入 API Key 模式, 使 v-else 分支
// (SignInForm/SignUpForm/SSO)永不渲染。门控而非删除, 详见
// docs/superpowers/specs/2026-09-24-branding-cleanup-design.md §3.1
const showApiKeyForm = ref(true)
```

- [ ] **Step 2: 验证改动效果（静态）**

```bash
cd /data/share/comfyui/thirdparty/comfyui-frontend && grep -n "showApiKeyForm = ref" src/components/dialog/content/SignInContent.vue
```

预期：输出 `ref(true)`。

- [ ] **Step 3: 检查 `v-else` 分支的其它入口是否已被覆盖**

```bash
cd /data/share/comfyui/thirdparty/comfyui-frontend && grep -n "showApiKeyForm" src/components/dialog/content/SignInContent.vue
```

预期：只剩三处——`ref(true)`（Step 1）、模板 `v-if="showApiKeyForm"`、`toggleState()` 里的 `showApiKeyForm.value = false`。
`toggleState` 只被 `v-else` 分支里的链接调用（该分支已不可达），故无需处理。

### 2B. 隐藏 API Key 表单的 back 按钮

**为什么**：`ApiKeyForm` 的 `@back` 把 `showApiKeyForm` 置回 `false`，那会**重新打开账号登录分支**——这是门控的漏洞，必须堵。而该按钮的语义（"返回账号登录"）已无去处。

- [ ] **Step 4: 门控 back 按钮**

`signin/ApiKeyForm.vue:69-72`，把：

```html
      <div class="mt-4 flex items-center justify-between">
        <Button type="button" variant="textonly" @click="$emit('back')">
          {{ t('g.back') }}
        </Button>
```

改为：

```html
      <div class="mt-4 flex items-center justify-end">
        <Button
          v-if="false"
          type="button"
          variant="textonly"
          @click="$emit('back')"
        >
          {{ t('g.back') }}
        </Button>
```

说明：`v-if="false"` 是**门控**（组件与 emit 链全部保留），`justify-between` → `justify-end` 使唯一的 Save 按钮靠右，维持原有视觉。

- [ ] **Step 5: 同时堵住 `@back` 的处理（双保险）**

`SignInContent.vue:3-7`，把：

```html
    <ApiKeyForm
      v-if="showApiKeyForm"
      @back="showApiKeyForm = false"
      @success="onSuccess"
    />
```

改为：

```html
    <ApiKeyForm
      v-if="showApiKeyForm"
      @back="() => {}"
      @success="onSuccess"
    />
```

说明：即使将来有人恢复了 back 按钮，也不会退回账号登录分支。

### 2C. 设置面板不注册账号/订阅类面板

- [ ] **Step 6: 加门控开关并过滤 `panels`**

`useSettingUI.ts:253-262`，把：

```ts
  const panels = computed<SettingPanelItem[]>(() => [
    aboutPanel,
    creditsPanel,
    userPanel,
    ...visibleWorkspacePanels.value,
    keybindingPanel,
    extensionPanel,
    ...(isDesktop ? [serverConfigPanel] : []),
    ...(shouldShowSecretsPanel.value ? [secretsPanel] : [])
  ])
```

改为：

```ts
  // 2026-09-25 ohos: 账号/订阅类设置面板整体门控 —— 定义全部保留(creditsPanel /
  // userPanel / planCreditsPanel 等仍在下方), 仅不注册进面板列表。
  // 详见 docs/superpowers/specs/2026-09-24-branding-cleanup-design.md §3.1
  const showAccountPanels = false

  const panels = computed<SettingPanelItem[]>(() => [
    aboutPanel,
    ...(showAccountPanels
      ? [creditsPanel, userPanel, ...visibleWorkspacePanels.value]
      : []),
    keybindingPanel,
    extensionPanel,
    ...(isDesktop ? [serverConfigPanel] : []),
    ...(shouldShowSecretsPanel.value ? [secretsPanel] : [])
  ])
```

说明：`creditsPanel`（积分）、`userPanel`（用户）、`visibleWorkspacePanels`（含 PlanCredits 账单界面）三者由**同一开关**统一门控——其中后者在用户配了 API Key 后会因 `shouldShowWorkspacePanel = isLoggedIn` 而出现，是容易被漏掉的一处。

- [ ] **Step 7: 确认无遗留引用**

```bash
cd /data/share/comfyui/thirdparty/comfyui-frontend && grep -rn "userPanel\|creditsPanel" src/ --include=*.ts --include=*.vue | grep -v "useSettingUI.ts"
```

预期：**空输出**（先前已核实这两个 const 仅在本文件使用）。

### 2D. 用户菜单隐藏订阅/充值区块

- [ ] **Step 8: 覆盖订阅能力标志**

`CurrentUserPopoverLegacy.vue:187-195`，把：

```ts
const {
  canAccessSubscriptionFeatures,
  tier,
  subscription,
  balance,
  isLoading,
  fetchBalance
} = useBillingContext()
```

改为：

```ts
const { tier, subscription, balance, isLoading, fetchBalance } =
  useBillingContext()

// 2026-09-25 ohos: 订阅/计费面门控 —— 原值来自 useBillingContext().canAccessSubscriptionFeatures,
// 该值在 localhost 构建下恒为 true(useSubscription.ts:50-54)。此处就地覆盖为 false,
// 使本文件内 L73/L106/L118 三处订阅区块同时失效。不改定义处(那里被 cloud 组件共用)。
const canAccessSubscriptionFeatures = computed(() => false)
```

- [ ] **Step 9: 确认 `computed` 已导入**

```bash
cd /data/share/comfyui/thirdparty/comfyui-frontend && grep -n "^import.*vue'" src/components/topbar/CurrentUserPopoverLegacy.vue
```

预期：输出中含 `computed`。若无，把该 import 行补上 `computed`。

- [ ] **Step 10: 跑受影响的单测**

```bash
cd /data/share/comfyui/thirdparty/comfyui-frontend && pnpm test:unit --run src/components/dialog/content/SignInContent.test.ts src/components/dialog/content/signin/ApiKeyForm.test.ts 2>&1 | tail -25
```

预期：可能失败（官方测试按 `showApiKeyForm` 初始为 `false` 编写）。
**处理原则**：若失败原因是"断言账号登录界面可见"，**改测试以反映新行为**（这是我们主动改变的产品行为），但改动要最小（只改断言，不删测试用例）。
把测试改动记进 Task 8 的提交。

- [ ] **Step 11: 提交（若在本步提交，遵循执行器选择的提交策略）**

```bash
git -C /data/share/comfyui/thirdparty/comfyui-frontend add -A src/
git -C /data/share/comfyui/thirdparty/comfyui-frontend commit -m "feat(ohos): 门控账号登录面, 保留 API Key 能力"
```

---

## Task 3: 品牌正名 —— 窗口标题与首屏大字

**Files:**
- Modify: `src/composables/useBrowserTabTitle.ts:11-13`
- Modify: `src/views/UserSelectView.vue:7`

- [ ] **Step 1: 改窗口标题**

`useBrowserTabTitle.ts:11-13`，把：

```ts
const DEFAULT_TITLE =
  typeof document !== 'undefined' && document.title ? document.title : 'ComfyUI'
const TITLE_SUFFIX = ' - ComfyUI'
```

改为：

```ts
// 2026-09-25 ohos: 品牌正名(详见 spec §3.4)
const DEFAULT_TITLE =
  typeof document !== 'undefined' && document.title
    ? document.title
    : '梦幻之流'
const TITLE_SUFFIX = ' - 梦幻之流'
```

- [ ] **Step 2: 改首屏大字**

`UserSelectView.vue:7`，把：

```html
      <h1 class="my-2.5 mb-7 font-normal">ComfyUI</h1>
```

改为：

```html
      <h1 class="my-2.5 mb-7 font-normal">梦幻之流</h1>
```

- [ ] **Step 3: 确认改动生效面**

```bash
cd /data/share/comfyui/thirdparty/comfyui-frontend && grep -rn "ComfyUI" src/composables/useBrowserTabTitle.ts src/views/UserSelectView.vue
```

预期：仅剩注释或 `index.html` 相关的引用；不应再有 `' - ComfyUI'` 字符串。

---

## Task 4: 「关于」面板加衍生声明 + 对话框标题中性化

**Files:**
- Modify: `src/locales/en/main.json`（`g` 段）
- Modify: `src/components/dialog/content/setting/AboutPanel.vue`
- Modify: `src/services/dialogService.ts:243-246,251`

- [ ] **Step 1: 加 i18n 文案 key**

在 `src/locales/en/main.json` 的 `g` 段中（`"about": "About"` 所在行附近）加入一条：

```json
    "builtOnComfyUI": "基于 ComfyUI 构建 · GPL-3.0",
```

**为什么用 i18n 而非硬编码**：fork 的 ESLint 规则禁止模板中出现原始文本（`vue-i18n` 限制）。

- [ ] **Step 2: 在关于面板显示声明**

`AboutPanel.vue`，在模板的 `<Divider />` 与 `<SystemStatsPanel .../>` 之间插入：

```html
    <p class="my-0 text-sm text-muted">
      {{ $t('g.builtOnComfyUI') }}
    </p>
```

- [ ] **Step 3: 去掉对话框的装饰性标题组件**

`dialogService.ts:243-246`，把：

```ts
  async function showSignInDialog(): Promise<boolean> {
    const [{ default: SignInContent }, { default: ComfyOrgHeader }] =
      await Promise.all([lazySignInContent(), lazyComfyOrgHeader()])

    return new Promise<boolean>((resolve) => {
      dialogStore.showDialog({
        key: 'global-signin',
        component: SignInContent,
        headerComponent: ComfyOrgHeader,
```

改为：

```ts
  async function showSignInDialog(): Promise<boolean> {
    // 2026-09-25 ohos: 该对话框已仅承载 API Key 表单(ApiKeyForm 自带标题与说明),
    // 故去掉装饰性的 ComfyOrg logo 标题组件(详见 spec §3.2)。
    const { default: SignInContent } = await lazySignInContent()

    return new Promise<boolean>((resolve) => {
      dialogStore.showDialog({
        key: 'global-signin',
        component: SignInContent,
```

**注意**：`lazyComfyOrgHeader` 函数**保留不删**——`showUpdatePasswordDialog`（`:401-402`）仍在用。

- [ ] **Step 4: 确认没有残留引用**

```bash
cd /data/share/comfyui/thirdparty/comfyui-frontend && grep -n "ComfyOrgHeader" src/services/dialogService.ts
```

预期：只剩 `lazyComfyOrgHeader` 的定义（`:35-36`）与 `showUpdatePasswordDialog` 里的使用（`:401-402`）——`showSignInDialog` 内应无引用。

---

## Task 5: 关闭 release notes 自动拉取

**Files:**
- Modify: `src/platform/settings/constants/coreSettings.ts:479-484`

- [ ] **Step 1: 改默认值**

把 `Comfy.Notification.ShowVersionUpdates` 项的：

```ts
    defaultValue: true
```

改为：

```ts
    // 2026-09-25 ohos: 默认关闭 —— 该设置开启时前端会在启动后自动拉取
    // {api}/release-notes(带 current_version/form_factor/locale), 本应用版本锁定无更新语义。
    // 详见 spec §3.5
    defaultValue: false
```

**定位要点**：该文件里有**多个** `defaultValue: true`，必须改 `id: 'Comfy.Notification.ShowVersionUpdates'` 那一项（`:479-484`），不要改错。

- [ ] **Step 2: 确认改对了目标项**

```bash
cd /data/share/comfyui/thirdparty/comfyui-frontend && grep -n -A6 "Comfy.Notification.ShowVersionUpdates" src/platform/settings/constants/coreSettings.ts
```

预期：输出中 `defaultValue: false`。

---

## Task 6: 切生产配置

**Files:**
- Modify: `scripts/build_frontend.sh:51-52`（主仓库）

- [ ] **Step 1: 改构建命令**

把：

```bash
echo "[build] pnpm build(typecheck + vite)..."
pnpm build
```

改为：

```bash
# 2026-09-25: USE_PROD_CONFIG=true 使前端基址指向生产(api.comfy.org / dreamboothy)。
#   必须: API Key 保存时前端调 authStore.createCustomer() → buildApiUrl('/customers'),
#   基址由 comfyApi.ts:15-17 依此开关决定; 不切则打到 stagingapi.comfy.org, 用户的
#   生产 key 会被拒。详见 docs/superpowers/specs/2026-09-24-branding-cleanup-design.md §3.3
echo "[build] pnpm build(typecheck + vite) [USE_PROD_CONFIG=true]..."
USE_PROD_CONFIG=true pnpm build
```

- [ ] **Step 2: 加入产物断言（防"配置没生效"）**

紧跟该段之后（`pnpm build` 成功后的断言区，`:55-60`），加入：

```bash
grep -rq "api\.comfy\.org" dist/assets/*.js || { echo "FATAL: 产物未见 api.comfy.org — USE_PROD_CONFIG 未生效"; exit 1; }
if grep -rq "stagingapi\.comfy\.org" dist/assets/*.js; then echo "FATAL: 产物仍含 stagingapi.comfy.org"; exit 1; fi
echo "[OK] 生产配置已生效"
```

**注意**：`dist/` 若已存在且"新鲜"，脚本会在 `:40-44` 提前 `exit 0` 跳过重建——本次必须**先删 dist** 强制重建。

---

## Task 7: 生成应用图标

**Files:**
- Create: `scripts/gen_app_icons.py`（主仓库）
- Modify: `entry/src/main/resources/base/media/{startIcon,background,foreground}.png`
- Modify: `AppScope/resources/base/media/{background,foreground}.png`

- [ ] **Step 1: 写生成脚本**

新建 `/data/share/comfyui/scripts/gen_app_icons.py`：

```python
#!/usr/bin/env python3
"""生成应用图标 —— 复用首页视觉语言(Index.ets): 深空渐变底 + 紫蓝渐变圆 + 星光符号。

用法: python3 scripts/gen_app_icons.py
产物: entry/src/main/resources/base/media/{background,foreground,startIcon}.png
      AppScope/resources/base/media/{background,foreground}.png
断言: 尺寸与中心像素(失败非零退出)。
"""
import sys
from PIL import Image, ImageDraw

BG_TOP, BG_BOTTOM = (0x0A, 0x0E, 0x1E), (0x3B, 0x2A, 0x78)   # 首页背景渐变起止
CIRCLE_IN, CIRCLE_OUT = (0x8B, 0x5C, 0xF6), (0x4F, 0x46, 0xE5)  # 首页 logo 渐变圆
SIZE = 1024

def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))

def vertical_gradient(size, top, bottom):
    img = Image.new("RGBA", (size, size))
    d = ImageDraw.Draw(img)
    for y in range(size):
        d.line([(0, y), (size, y)], fill=lerp(top, bottom, y / (size - 1)) + (255,))
    return img

def make_background():
    return vertical_gradient(SIZE, BG_TOP, BG_BOTTOM)

def make_foreground():
    """前景层: 居中的渐变圆 + 四角星光。分层图标要求前景留出安全边距。"""
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = int(SIZE * 0.30)
    c = SIZE // 2
    for i in range(r, 0, -1):
        t = 1 - i / r
        d.ellipse([c - i, c - i, c + i, c + i], fill=lerp(CIRCLE_IN, CIRCLE_OUT, t) + (255,))
    # 四角星光(简洁几何, 与首页 sys.symbol.wand_and_stars 的星形呼应)
    star = int(SIZE * 0.085)
    for dx, dy, s in ((-1, -1, star), (1, -1, int(star * 0.7)),
                      (-1, 1, int(star * 0.7)), (1, 1, int(star * 0.55))):
        cx, cy = c + dx * int(SIZE * 0.26), c + dy * int(SIZE * 0.26)
        d.polygon([(cx, cy - s), (cx + s // 4, cy - s // 4), (cx + s, cy),
                   (cx + s // 4, cy + s // 4), (cx, cy + s),
                   (cx - s // 4, cy + s // 4), (cx - s, cy),
                   (cx - s // 4, cy - s // 4)], fill=(255, 255, 255, 235))
    return img

def main():
    bg, fg = make_background(), make_foreground()
    targets = [
        ("entry/src/main/resources/base/media/background.png", bg),
        ("entry/src/main/resources/base/media/foreground.png", fg),
        ("AppScope/resources/base/media/background.png", bg),
        ("AppScope/resources/base/media/foreground.png", fg),
    ]
    for path, im in targets:
        im.save(path)
        print(f"[write] {path} {im.size}")
    # 启动窗口图标: 背景 + 前景合成, 缩到 144x144
    start = Image.alpha_composite(bg, fg).resize((144, 144), Image.LANCZOS)
    start.save("entry/src/main/resources/base/media/startIcon.png")
    print("[write] entry/src/main/resources/base/media/startIcon.png (144, 144)")

    # ── 验收断言(失败非零退出) ──
    for path, expect_size in [(t[0], (SIZE, SIZE)) for t in targets] + \
                             [("entry/src/main/resources/base/media/startIcon.png", (144, 144))]:
        im = Image.open(path)
        assert im.size == expect_size, f"FATAL: {path} 尺寸 {im.size} != {expect_size}"
    assert Image.open("entry/src/main/resources/base/media/foreground.png").getpixel((SIZE // 2, SIZE // 2))[3] > 200, \
        "FATAL: 前景层中心应不透明(渐变圆)"
    assert Image.open("entry/src/main/resources/base/media/foreground.png").getpixel((4, 4))[3] < 10, \
        "FATAL: 前景层边角应透明(分层图标安全边距)"
    print("[OK] 图标生成通过验收")

if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: 运行生成**

```bash
cd /data/share/comfyui && python3 scripts/gen_app_icons.py
```

预期：逐行 `[write] ...`，最后 `[OK] 图标生成通过验收`；退出码 0。

- [ ] **Step 3: 目视确认产物**

```bash
cd /data/share/comfyui && python3 -c "
from PIL import Image
Image.open('entry/src/main/resources/base/media/startIcon.png').resize((288,288)).save('/tmp/icon_preview.png')
print('preview: /tmp/icon_preview.png')
"
```

用 Read 工具查看 `/tmp/icon_preview.png`，确认是「深空底 + 紫蓝圆 + 星光」而非模板方块。

---

## Task 8: 构建前端 dist

**Files:**
- 产物：`thirdparty/comfyui-frontend/dist/`（未跟踪，被 .gitignore 忽略）

- [ ] **Step 1: 强制重建（必须删 dist，否则脚本会跳过）**

```bash
rm -rf /data/share/comfyui/thirdparty/comfyui-frontend/dist
bash /data/share/comfyui/scripts/build_frontend.sh 2>&1 | tail -25
```

预期：`[build] pnpm build(typecheck + vite) [USE_PROD_CONFIG=true]...` → `[OK] 生产配置已生效` → `[OK] dist 就绪`。
**若 typecheck 报错**：按报错修（多半是 Task 2D 的 `computed` 导入或未使用变量），修完重新跑。

- [ ] **Step 2: 记录新锚**

```bash
md5sum /data/share/comfyui/thirdparty/comfyui-frontend/dist/index.html
```

把这个值记下——Task 9 Step 3 要用。

---

## Task 9: fork 提交 + pin/锚同步

**Files:**
- Modify: `config/externals.pins.tsv:10-11`（主仓库）

- [ ] **Step 1: 提交 fork 改动并推送**

```bash
git -C /data/share/comfyui/thirdparty/comfyui-frontend add -A src/
git -C /data/share/comfyui/thirdparty/comfyui-frontend commit -m "feat(ohos): 账号登录面门控 + 品牌正名; 切生产配置

- SignInContent 初始即 API Key 模式, 账号登录/注册/SSO 分支不可达; back 按钮门控
- 设置面板不注册 credits/user/workspace 面板(同一开关)
- 用户菜单订阅/充值区块门控
- 窗口标题与首屏大字改为「梦幻之流」; 关于面板加 GPL 衍生声明
- 去掉登录对话框的装饰性 ComfyOrg logo 标题
- 默认关闭 release notes 自动拉取"
git -C /data/share/comfyui/thirdparty/comfyui-frontend push origin ohos
NEW_COMMIT=$(git -C /data/share/comfyui/thirdparty/comfyui-frontend rev-parse HEAD)
echo "fork 新 commit = $NEW_COMMIT"
```

- [ ] **Step 2: 更新两处锚（缺一即链断）**

⚠ **md5 锚有两处，必须同值**：`config/externals.pins.tsv` 与 `scripts/make_comfyui_stage.py`
的 `FE_INDEX_MD5`。后者在 `:147-148` 有**强制断言**，不符会让 `make stage` / `make hap`
直接 FATAL。两行文件都不相邻，**用 grep 定位，不要按行号猜**。

```bash
grep -n "comfyui-frontend-src\|comfyui-frontend-index.html" /data/share/comfyui/config/externals.pins.tsv | cut -c1-140
grep -n "FE_INDEX_MD5" /data/share/comfyui/scripts/make_comfyui_stage.py
```

预期旧值：
- `comfyui-frontend-src` 行：commit = `bef1b1d1b2a8bbf100a51f20c7028bb334d000fe`
- `comfyui-frontend-index.html` 行：md5 = `641e0fec1e0c4f77676c1aead7fb210e`
- `make_comfyui_stage.py:25`：`FE_INDEX_MD5 = "641e0fec1e0c4f77676c1aead7fb210e"`，注释含 `ohos bef1b1d`

用 Edit 工具改五处：
1. `pins.tsv` 的 commit：`bef1b1d1b2a8bbf100a51f20c7028bb334d000fe` → `$NEW_COMMIT`
2. `pins.tsv` 的 md5：`641e0fec1e0c4f77676c1aead7fb210e` → 新 md5（Task 8 Step 2 记录）
3. `pins.tsv` index.html 行描述里的 `bef1b1d` → 新 commit 前 7 位
4. `make_comfyui_stage.py` 的 `FE_INDEX_MD5` 值 → 新 md5（与 2 同值）
5. `make_comfyui_stage.py` 注释里的 `ohos bef1b1d` → `ohos <新 commit 前 7 位>`

- [ ] **Step 2b: 同步 `python312.zip` 的锚（**前端 dist 一变就必然轮转**）**

`entry/src/main/resources/rawfile/python312.zip` 内嵌 `comfyui/frontend_static/*`（约 1000 条），
它的锚是 `sorted_namelist_sha256`，**前端 dist 的任何变化都会让它轮转**
（本次实测：`f4041590…` → `0792b869…`，size `213665992` → `213667907`）。

修复二选一：
```bash
python3 scripts/make_py312_zip.py --bless   # 官方机制: 实测值自动写回 pins.tsv 第 4/5 列
```
或手工把 `make hap` 失败输出里 `[MANIFEST] … sorted_namelist_sha256=<新值> … zip_size=<新值>`
填进 `config/externals.pins.tsv` 的 `python312.zip` 行。

**漏了会怎样**：`make hap` 的 `zip` 目标以 `Error 2` 失败（脚本主动 `exit 2`，并打印
`[FAIL] pins.tsv 锚未更新` 与修复指引）。

⚠ **同时注意**：`docs/manifests/python312.zip.manifest.gz` 会被脚本自动重写，需一并提交。

⚠ **本次踩过的坑**：`make hap 2>&1 | tail -40` 会让**外层退出码变成 `tail` 的 0**，
掩盖 make 的真实失败。务必用 `make hap > log 2>&1; rc=$?; tail log` 或 `set -o pipefail`。

⚠ **改 `pins.tsv` 这类制表符分隔文件时**：Edit 的 `new_string` 必须包含**完整的分隔符结构**。
本次 size 那处改动就因为 `new_string` 少写一个 tab，把第 5、6 列粘成了
`213667907锚=sorted-namelist…`，脚本读到 `row[4]` 是个带描述的巨串 ⇒ **继续 FAIL，
且报错信息把它显示成 `size=213667907锚=…` 才露馅**。
**自检手法**：`awk -F'\t' '{print NF}'` 对每行都应为 6 列。

- [ ] **Step 3: 验证三方一致**

```bash
cd /data/share/comfyui && \
  REAL=$(md5sum thirdparty/comfyui-frontend/dist/index.html | cut -d' ' -f1) && \
  PIN=$(grep "comfyui-frontend-index.html" config/externals.pins.tsv | cut -f4) && \
  STAGE=$(grep -oP 'FE_INDEX_MD5 = "\K[0-9a-f]+' scripts/make_comfyui_stage.py) && \
  echo "real=$REAL pin=$PIN stage=$STAGE" && \
  { [ "$REAL" = "$PIN" ] && [ "$REAL" = "$STAGE" ] && echo "[OK] 三方一致"; } || echo "[FAIL] 锚不一致"
```

预期：`[OK] 三方一致`。

- [ ] **Step 4: 提交主仓库的 pin 与脚本改动**

```bash
git -C /data/share/comfyui add config/externals.pins.tsv scripts/build_frontend.sh scripts/gen_app_icons.py entry/src/main/resources/base/media AppScope/resources/base/media
git -C /data/share/comfyui commit -m "feat(ohos): 前端 pin 升级(账号门控+品牌正名+生产配置) + 应用图标替换"
```

---

## Task 10: 打包

- [ ] **Step 1: 构建 HAP**

```bash
cd /data/share/comfyui && make hap 2>&1 | tail -20
```

预期：`BUILD SUCCESSFUL`，且 prebuilt 325 文件闭环 `PASS`。

- [ ] **Step 2: 装机**

```bash
hdc -t 192.168.1.5:44959 install -r entry/build/default/outputs/default/entry-default-signed.hap
```

预期：`install bundle successfully`。（只使用 1.5 这一台设备。）

---

## Task 11: 真机验证

- [ ] **Step 1: 启动并截图（确认品牌与门控）**

```bash
hdc -t 192.168.1.5:44959 shell aa force-stop app.fuqidian.magicflow
hdc -t 192.168.1.5:44959 shell aa start -a EntryAbility -b app.fuqidian.magicflow
sleep 8
hdc -t 192.168.1.5:44959 shell snapshot_display -f /data/local/tmp/s1.jpeg
hdc -t 192.168.1.5:44959 file recv /data/local/tmp/s1.jpeg /tmp/s1.jpeg
```

用 Read 查看 `/tmp/s1.jpeg`。**判据**：桌面图标为深空紫蓝圆（非蓝色四方格）。
**若截图是橙色大时钟 + 柯基壁纸 = 锁屏**，请用户解锁后重试（`power-shell wakeup` 只点亮不解锁）。

- [ ] **Step 2: 打开设置 → 关于**

在设备上点「启动 梦幻之流」，待后端就绪后：侧栏菜单 → Settings → About。
截图确认：面板中有「基于 ComfyUI 构建 · GPL-3.0」，且设置列表**无 User / Credits 项**。

- [ ] **Step 3: 穷举点击账号相关入口（**不要抽查**）**

spec §3.1 的「门控完整性教训」记录了本次两处**测试全绿但功能坏**的漏网（设置导航树、
账号设置入口）。故本步**逐一点击每个可能入口**，每点一次记录落点：

| 入口 | 期望 |
|---|---|
| 顶栏用户图标 | 只有 `API Key` 表单：无 Google/GitHub 按钮、无邮箱密码、无「注册」链接、无 back 按钮、顶部无 ComfyOrg logo |
| 设置对话框左侧导航 | 只有 `General` 一个分组；**无** User / Workspace / Plan & Credits / Members |
| 设置 → 搜索 `user` / `credits` / `plan` | 命中为空 |
| 用户菜单（已配 key 后展开） | **无** "Account settings"、无 "Add Credits"、无 "Partner Nodes Credits"；**保留** Logout（清除 key 的唯一入口） |
| Help 菜单 | **无** "Update ComfyUI"；文档 / GitHub / Discord 等外链**保留** |
| 工作流含官方 API 节点且未配 key 时的 Run 按钮 | 仍为 "Sign in to run"（**这是保留项**，点击能打开 API Key 表单；文案不匹配是已知且接受的，见 §3.2.1） |

- [ ] **Step 4: 验证 API Key 链路（需用户提供真实 key）**

粘贴一个可用的 comfy.org API Key → 保存 → 顶栏应变为已登录态（显示用户名）。
再用 `hdc ... shell curl` 或既有 smoke 脚本确认 `/object_info` 中官方 API 节点在列：

```bash
curl -s http://127.0.0.1:8191/object_info | python3 -c "
import json,sys
d=json.load(sys.stdin)
print('官方 API 节点数 =', sum(1 for v in d.values() if v.get('api_node')))
"
```

（走宿主映射端口 8191，不要试图在设备 shell 里 curl。）

预期：节点数 > 0（门控前基线为 259 左右；此处只需确认非 0，证明 API Key 能力保留）。

- [ ] **Step 5: 零回归 smoke**

```bash
cd /data/share/comfyui && bash scripts/verify_smoke.sh --port 8191 2>&1 | tail -20
```

预期：`ALL PASS (12 项)`。端口必须是 **8191**（8189 被另一台设备占用）。

- [ ] **Step 6: 确认 release notes 不再外发**

```bash
hdc -t 192.168.1.5:44959 shell hilog -x | grep -i "release-notes" | head -5
```

预期：**无输出**。

---

## Task 12: 文档同步

**Files:**
- Modify: `docs/comfyui-branding-audit.md`
- Modify: `docs/status-and-next.md`

- [ ] **Step 1: 审计清单清账**

按 spec §7 更新 `docs/comfyui-branding-audit.md`：

1. **D1 Datadog RUM** 标为已消解 —— 证据：`bootstrap.ts` 的 `if (__DISTRIBUTION__ === 'cloud')` 是编译期死分支；`dist/assets/*.js` 中搜不到 `prod-v2` / `resolveDeployEnv` / clientToken
2. **D2 遥测 providers** 标为已消解 —— 证据：9 个 provider 由 `initTelemetry.ts:9,19` 的 `if (!IS_CLOUD_BUILD) return` 统一门控
3. **E1 云组件** 标为已消解 —— 证据：`router.ts:25-28,61` 非 cloud 下不注册 `/cloud/*` 路由
4. **补录漏记项**：`GraphCanvas.vue:594` → `releaseStore.initialize()` 的 release-notes 自动外发（本次已关闭）
5. **B 类按实测改写**：原文"需产品决定"，实际顶栏登录按钮无门控、点击即开官方登录对话框
6. **新增一节「保留 API Key 能力是有意决策」** —— 记录本次结论：259 个官方 API 节点 + `api_key_comfy_org` 路径**有意保留**（`SignInContent.vue:101` 的 `v-if="!isCloud"` 本就是官方为非云构建留的入口）。防止后来者照旧清单把登录面当成待清理项再切一次。

- [ ] **Step 2: 状态页更新**

`docs/status-and-next.md` §3 推进方向表的 ⑦ 行，状态由「待做」改为：

```
**已交付(2026-09-25)** —— 账号登录面门控 + 品牌正名 + 保留 API Key 能力(259 节点);
审计清单 D1/D2/E1 复核为已消解, 详见 docs/superpowers/specs/2026-09-24-branding-cleanup-design.md
```

- [ ] **Step 3: 提交**

```bash
git -C /data/share/comfyui add docs/comfyui-branding-audit.md docs/status-and-next.md
git -C /data/share/comfyui commit -m "docs: 品牌审计清单清账 + 状态页更新"
```

---

## 完成判据

全部满足才算完成：

1. `pnpm build` 通过，且产物含 `api.comfy.org`、不含 `stagingapi.comfy.org`
2. `make hap` BUILD SUCCESSFUL
3. 真机：桌面图标 + 启动闪屏为新图标；窗口标题与首屏为「梦幻之流」
4. 真机：设置里无 User/Credits 面板，关于面板有 GPL 声明
5. 真机：登录对话框只有 API Key 表单（无 OAuth/密码/注册/logo/back）
6. 真机：粘贴 API Key 能保存并生效，官方 API 节点仍在
7. `verify_smoke.sh --port 8191` 12 项 ALL PASS
8. fork 已推送，pins 的 commit 与 md5 锚均与产物一致
9. `docs/comfyui-branding-audit.md` 与 `docs/status-and-next.md` 已按 Task 12 清账
