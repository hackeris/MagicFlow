# 外部 API 节点(自研)设计方案 —— 第三方图像/文本/通用 API 接入

> 2026-09-07 定稿。目标:在 OHOS 移植的 ComfyUI(后端=NCP 内嵌 CPython+torch,前端=自建 fork dist)内,
> 通过**自研一组通用 Python 节点**,让工作流直接调用**任意第三方 HTTP API**(图像生成、文本/LLM、以及
> "不限于这些"的通用端点),节点输出可以继续接 ComfyUI 原生节点(预览/保存/再处理)。
> 用户拍板方向:自研,不是官方云、不是薄客户端。关键词:通用性(任意 URL/任意请求体模板)、
> 零新编译依赖(复用栈内既有 requests/httpx/aiohttp)、极简前端(无前端改动)。

---

## 0. 为什么自研 —— 与已有机制的分界(实证)

| 已有机制 | 现状 | 为什么不直接用它 |
|---|---|---|
| **官方 `comfy_api_nodes/`**(core 03468f4 内置,37 组:OpenAI/Anthropic/Gemini/Qwen/Kling/Sora/语音/3D…) | 已经随 `python312.zip` 上设备(86 文件在包内),由 `nodes.py:2546 init_builtin_api_nodes()` 默认加载 | **全部走 Comfy 官方云网关** `default_base_url()=args.comfy_api_base= https://api.comfy.org`,认证=`auth_token_comfy_org`/`api_key_comfy_org`(由前端只有 isCloud 分支才填,见 `api.ts:1063`)+ `X-Comfy-Credits-Used` 计费头——**不是"调用第三方 API",是"调用 Comfy 官方中转"**;要求 comfy.org 云账号+付费 credits,与"任意第三方 API/自建网关"诉求不符。**但其 util/client.py(ApiEndpoint+sync_op/poll_op+重试/限速/计费解析+上传下载助手)是极佳现成参考**,自研节点可复用其设计范式 |
| **官方 `--comfy-api-base`**(cli_args.py:262,默认 api.comfy.org) | 可覆盖 base URL | 覆盖后所有官方节点改为打到自定义网关——但网关必须实现 Comfy 云协议(不含 API 密钥/计费体系,`get_auth_header` 只认 comfy_org 二字样头);且前端 isCloud 分支全关,`api_key_comfy_org` 根本传不进来。不是"我的 API key"通道 |
| **社区自定义节点**(NimbusPro 等 OpenAI 桥接节点) | 生态成熟,但需 pip 依赖链+前端 web 扩展 | 设备栈**没有** requests/httpx SDK 依赖之外的扩展库(openai/anthropic SDK 等均不在栈);社区节点常带 pip 安装依赖+web 前端扩展,以 pyc/zip 方式逐库适配成本不对等 |
| **设备内已有网络库**(在 zip 内 ) | `requests 2.34.2`、`urllib3 2.7.0`、`httpx 0.28.1`、`aiohttp 3.14.3`、`PIL 11.1.0`、`pydantic 2.13.5`、`numpy`、`torch`、`multidict/propcache/yarl/frozenlist` | → **自研节点以 requests+aiohttp+仅 PyYAML(PyYAML 已在栈)零新依赖落地**,直接用原生 imagem 类型输出(IMAGE=tensor),无需 PIL 之外的东西(PIL 在栈内) |

**结论**:自研节点 = custom node 目录机制(`custom_nodes/ohos_external_api/`),与既有 `custom_nodes/ohos_smoke/`(patch 15)完全同构——**该路径已被验证**:patch 15 即通过"目录 + 2 个 py 文件 + zip 打包 + main.py init_custom_nodes 自动加载"上了真机(ohos_smoke 判据 A/B 达成)。复用该模式,零新依赖、零前端改动。

## 0.1 参考社区节点:借设计、不背依赖(2026-09-08 定稿)

**策略**:社区节点只做"设计参考",不做"直接移植"。借鉴它们的成熟部分,其余一律自写:

| 借鉴(抄设计) | 不抄(绕开) |
|---|---|
| OpenAI 兼容请求体构造(消息数组、参数映射、超时重试) | openai/anthropic SDK 依赖——它们只是 HTTP 请求封装,用栈内 requests 写等价功能,零新包 |
| 响应解析:base64 图片 → 图片张量(bytesio_to_image_tensor 一类) | 前端 web 扩展(节点声明后界面自动可见) |
| 错误处理模式:报错组织、非 200/超时处理、重试 | 云计费/官方云鉴权(comfy.org credits 体系,正是要绕开的) |
| 节点声明结构(INPUT_TYPES/RETURN_TYPES,属于插件规范) | — |

**参考对象优先级**:
1. 官方 `comfy_api_nodes/util/client.py`——同源码树、已在设备包内、无许可证问题;poll/retry/错误解析范式;
2. 社区主流节点(GitHub 如 ComfyUI-NimbusPro 等):OpenAI 兼容请求模板与图片转换写法;参考处注释保留出处(多数为 GPL/MIT,注明来源既礼貌也合规)。

**落地点**:patch 18 编写前,先临时拉选中的社区节点源码到临时目录(不进仓库)阅读,再动笔实现。

---

## 1. 节点清单

包名 `ohos_external_api`,CATEGORY 统一 `ohos/api`。全部纯 Python、无 .so、无 pip 依赖。

| 节点类 | 用途 | 输入 | 输出 |
|---|---|---|---|
| `OHOS_API_Text` | 文本/LLM API(OpenAI 兼容 chat completions 及其他 JSON 文本端点) | `url`,`method`,`headers`(json str),`body`(json str 模板),`xpath`(可选,响应字段提取路径,空=整串),`timeout` | `TEXT`(STRING) |
| `OHOS_API_Image` | 图像生成/编辑 API(OpenAI 兼容 images/generations、及任意返回 base64/json 的端点) | 同上 + `field`(响应中 base64 数据字段路径,默认 `data[0].b64_json`),可选 `prompt` 混合模板 | `IMAGE`(torch 张量,兼容 PreviewImage/SaveImage/GetImageSize 全部原生下游) |
| `OHOS_API_HTTP` | 通用 HTTP 请求(任意 JSON API:第三方网关/自建 hub/代理服务;GET/POST/PUT) | 同上通用,`response_format`(`text`/`json`/`first_json_value`) | `TEXT` + `JSON`(STRING 双输出) |
| `OHOS_API_Upscale`?(可选,二期) | 远程超分/修复 API → 输入 IMAGE 上传 base64 → 返回 IMAGE | 同 Image + `input_image` | IMAGE |

### BASE64/tensor 转换
- 出图:`requests.get/response.json() → field 提取 base64 → PIL.Image.open(BytesIO) → np → torch tensor (h,w,c) float32/255 → batch 维度补 1`。与官方 `util/conversions.py` 中 `bytesio_to_image_tensor` 同构(参考其实现,不用引入其 comfy_org 云依赖)。
- 入图上送(二期 Upscale 用):`tensor_to_base64_string` 同构。

### 请求体模板
- `body` 支持占位符 `{{prompt}}`、`{{seed}}`、`{{width}}`、`{{height}}`、`{{temperature}}` 等:**由节点生成的纯 dict 模板`(write 模式)或由上游 TEXT 输入拼入`(read 模式)** . 简化:v1 只做「静态 json str + 标准化 `{{...}}` 替换 + 参数 widget」,不做上游 TEXT 输入二次拼串(上游 TEXT 可先经官方 `nodes_text.py` 的 WriteText/ConcatText 节点拼接成模板?——不,v1 采用「上游 TEXT → body 整体作为 prompt field」,即 `body` 模板中 `{{prompt}}` 由 widget 或上游 TEXT 填充,二者择一)。
- 参数化 widget:顶部 `params` json str(默认 `{}`),与 `body` 模板双替。简化到:模板顶层字段 `meta`(任意 json)+ `prompt` 可选专列。

**实际上 v1 收敛**:任意 API 差异太大,所以
- `OHOS_API_Text`/`OHOS_API_Image` 提供 **OpenAI 兼容模式的甜蜜路径**:内建 `chat completions`/`images/generations` 模板,外加 `custom json 模板`模式(完整 JSON str,widget 填写,模板支持 `{{prompt}}` 等 4 个占位符)。
- `OHOS_API_HTTP` 纯通用:method/url/headers/body 全显式,无甜蜜路径,响应按 `format` 处理。

---

## 2. 加载与打包链(补丁复用 patch 15/16/17 模式)

1. 新 patch `patches/18-ohos-external-api.patch`:新增 `custom_nodes/ohos_external_api/__init__.py`(暴露 NODE_CLASS_MAPPINGS/DISPLAY 名)+ `custom_nodes/ohos_external_api/nodes.py` + `custom_nodes/ohos_external_api/api_client.py`(HTTP 客户端封装:requests 同步 + 可选 aiohttp async,重试/超时/JSON 解析/占位符替换)。
   - **证据串**:`# OHOS_EXTERNAL_API v1`(nodes.py 文件内)+ `NODE_CLASS_MAPPINGS` dict 校验。
   - patch 幂等:同 15(文件存在即 skip;部分存在则逐文件 apply)。
2. `scripts/fetch_externals.sh` ③d 段:新 patch 18 应用(逐 patch 幂等,失败必死)逻辑——与 ③c/③b 完全同构。
3. `scripts/make_py312_zip.py`/`make_comfyui_stage.py`:**无需改**——`custom_nodes/` 目录已在 SRC_ROOT_ONLY 之外(copytree 全部),stage 已含(现有 ohos_smoke 验证),zip 自动收。
4. zip 重新构建 → `python312.zip` **内容锚必变**(新增 3 文件/6 条 pyc + mtime)→ 更新 `pins.tsv` 的 `python312.zip` 行锚(三步走:重建 → make_py312_zip.py 校验 → 更新 tsv 行;漂移字段 zip_sha256 同现行为,`sorted_namelist`+`per_entry` 双锚也需同步)。
5. `scripts/verify_smoke.sh` 新增判据(见 §4 验证矩阵)。

**前端**:无需任何改动——后端 `/object_info` 自动提供节点定义,前端 canvas 通用渲染。与前端 fork 的差异仅:节点名/描述用中文放 `locales`?——不,v1 不做前端 i18n,节点 DISPLAY_NAME 用中文(后端侧 `NODE_DISPLAY_NAME_MAPPINGS`),canvas 直接显示。

---

## 3. 兼容性清单(为什么设备能跑)

- **Python 3.12.7 aarch64-ohos**(栈):requests(纯 py)+ urllib3(纯 py)+ PIL(纯 py,C 加速件走 .so 剔除自动降级)+ torch/numpy(已载)。
- **network**:INTERNET 权限已有(module.json5);HTTPS 使用 `libssl.so.3`(已在 prebuilt_manifest)+ `_ssl` dynload ✓。
- **DNS/出网**:与 W3 下载同款约束——设备 nns 网关可达则由节点直连;不可达时经 rport 隧道(已有隧道先例:模型下载 Q2 用 `127.0.0.1:18080→宿主 18001`;**节点 URL 同样支持 127.0.0.1 + rport 反向隧道**,验证矩阵 Q2e 用)。
- **内存**:单请求 dict/json 解析,数十 KB~MB;响应 base64 图像 decode 有瞬时放大(4x),tiny 设备注意 `max_response_bytes` 守卫(widget,默认 20MB,超限报错)。

---

## 4. 验证矩阵

| 轮 | 验证点 | 判据 |
|---|---|---|
| H0(宿主) | 单测:api_client 解析/模板替换/格式提取(bencode 单元,无 torch) | script 退出码 0;四模式 x 三响应样例 |
| H1(宿主) | local http.server 模拟器:`POST /v1/chat/completions`、`POST /v1/images/generations`(base64 1pixel Png) | 标准 api_client 调用两样全绿 |
| Q0(真机) | 节点注册 | `/object_info` 含 `OHOS_API_Text|OHOS_API_Image|OHOS_API_HTTP`,NODE_DISPLAY 中文名可见 |
| Q1(真机) | smoke workflow:Text 节点 → 模拟器(rport 隧道) | 出 TEXT 正确;`history` outputs |
| Q2(真机) | Image 节点 → 模拟器 base64 图 | SMOKE 出图;按 API 大小/类型正确;下游 PreviewImage 可见 |
| Q2e(真机) | rport 隧道路径 | `OHOS_API_*` 直接调 `http://127.0.0.1:18080/...`(同模型下载)成功 |
| Q3(真机) | HTTP 通用节点:GET 任意 json(带 header) | 双输出 TEXT+JSON;响应头/错误处理正确 |
| Q4(真机) | 错误路径:404/超时/非 json 响应 | 节点报错信息可读(HTTP 状态码 + body 前 200 字),不死机 |
| R(全量) | `make verify`(原判据全过 + 新判据) | 原 28 判据 + new 4 判据 |

---

## 5. 风险与回退

| 风险 | 对策 |
|---|---|
| a) 官方 comfy_api_nodes 初始化早于自定义节点且缺依赖即 WARN(does not break) | 已实证:zip 有 api_nodes 86 文件 + nodes.py 已有 import_failed 容错 WARN;**无需处理**,但 Q0 检查 WARN 只涉全官方节点件(不涉我们) |
| b) 任一第三方 API 键在设备存储 | 节点 widget 常规(与 ComfyUI 本地 key/widget 无特殊封装);如需,二期加 `settings/api-key 下拉`(前端) |
| c) 外网不可达 | rport 隧道 + 模拟器判据兜底;真实 API 验证在连接可达环境验收,smoke 不接外网(延续 W3 铁律) |
| d) 模板/字段输入错误 | 所有参数 json str 解析失败 → 明确报错(节点级 VALIDATION_MSG),不 raise 裸异常 |
| e) zip 锚更新遗漏 | 重建脚本强制:内容变化(per_entry 变化)但 pins.tsv 未更新 → make 报错。见 §2.4 |

---

## 6. 一图流

```
[前端 canvas: 完整工作流]
   OHOS_API_Text ──TEXT──▶ (可选官方 prompt 节点) ──▶ OHOS_API_Image ──IMAGE──▶ PreviewImage/SaveImage
   OHOS_API_HTTP ──TEXT/JSON(通用链路)
                    │
                    ▼
          api_client.py (requests, timeout/retry/JSON 提取/{{prompt}} 替换)
                    │
        ┌───────────┴────────────┐
        │ 设备外网可直连          │ 不可达
        ▼                        ▼
  https://api.xxx.com     http://127.0.0.1:18080(rport)→宿主模拟器
```

---

## 7. 实施清单(按序)

1. patch 18 编写(3 文件,含证据串)+ 文档本文件自查(每节真实)。
2. fetch_externals.sh ③d:patch 18 幂等段(逐文件删除补齐同构)+ 死签。
3. 重建 zip(for real,stage→make_py312_zip)→ tsv 锚更新。
4. H0/H1 宿主冒烟。
5. Q0-Q4 真机;修。
6. verify_smoke.sh 判据扩展 + 重编 `OHOS_API_*` 判据(R 轮全量)。
7. memory:V2 前景文档落格(本设计要点)到 `comfyui-stack-build-decision.md`?不,memory 已有 P0 位,新建 `comfyui-external-api-node.md` one-liner hook。
