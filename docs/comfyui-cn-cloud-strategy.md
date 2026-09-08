# 端云一体模型策略(国内网络 · 鸿蒙 Pad/PC) — comfyui-cn-cloud-strategy.md

> 2026-09-08 定稿。决策链: 用户拍板「外部化两层 → P0 模型获取国产化(本次)+ P1 远程算力 API」。
> 本文是 `patches/18-ohos-catalog-cn.patch` 与后续 W4 的依据; P1 设计方案见
> [external-api-node-design.md](external-api-node-design.md)(2026-09-07 已定稿)。

## 0. 一句话结论

本地内存 12-32G + NPU(未翻转)物理锁死大模型 → **产品体验上限 = 云端**。「外部模型」拆两层:

| 层 | 含义 | 定位 |
|---|---|---|
| ① 模型文件外部化 | 权重从国内可达源进入设备(HF 主站被墙) | **先决**(本次 P0 已交付) |
| ② 推理算力外部化 | 生成任务交给云端图像 API 执行 | **体验上限**(P1, 方案已定稿) |

量化**不做端上转换**(载入全量必 OOM/慢/无硬件加速/画质损), 改为 catalog 直接提供**预量化成品**变体(下载即用)。

## 1. 判断依据(本地/网络现实)

| 档位 | 本地能跑(实测/推算) |
|---|---|
| 设备 5.2G F32 上限(实测) | 256 级小图(SD-Turbo 2 步 ~87s/图) |
| 12G | SD1.5 512 级; SDXL 小图边缘 |
| 32G | SDXL 正常; fp8 FLUX 边缘(17.2G 权重) |
| NPU | NNRt P0 未翻转(热控 + 离线模型拒), **不计入近期路线**(见 comfyui-p0-nnrt-progress) |

结论: 本地保底小图 + 测试;**12-32G 设备想撑住体验, 外部算力是唯一解**; 对鸿蒙生态尤其成立(连模型获取都被墙)。

## 2. 网络实测(2026-09-08, 宿主)

| 源 | 结果 | 带宽(5MB 段实测) | 裁决 |
|---|---|---|---|
| huggingface.co 直链 | 不可达 | — | ✗ 被墙 |
| **modelscope.cn** resolve(sd-turbo / vae / sd15 / comfy-org flux1-dev 全 200, 206 分片正常) | 可达 | **1.34 MB/s** | **✓ 主源**(阿里 OSS) |
| hf-mirror.com resolve(302→cas-bridge.xethub.hf.co, 签名 **1h 有效期**) | 可达 | 0.84 MB/s(1h×0.84≈3GB ≥ 2.6GB, **贴边**) | 备选 |
| github.com **release**(RealESRGAN) | 可达 | 0.78 MB/s | ✓ 保留(仅 raw 不通) |
| api.siliconflow.cn / dashscope.aliyuncs.com | TLS 可达(404=根路径空) | — | P1 用正式路径再验 |

**结论修正**: 宿主并非无外网 —— 是 HF 主站 / gh-raw 被墙、国内 CDN 与镜像正常; W3 时"Q2 走 rport 隧道"的假设(设备无外网)仍待真机判据(#87 首项: 设备直连镜像)。

## 3. P0 交付(2026-09-08) — 模型获取国产化

| 文件 | 改动 |
|---|---|
| `patches/18-ohos-catalog-cn.patch`(新) | hosts 白名单 +`modelscope.cn`/`hf-mirror.com`(实测标准见 §2 注释); catalog 3 条主 url 换 ModelScope resolve(留 `url_alt` 原生源兜底, 前端只读 url 无感); 新增 `flux1-dev-fp8` 预量化变体(17.2GB/仅 32G/落 diffusion_models) |
| `scripts/fetch_externals.sh` | `ohos_patch_applied` 证据串 +`OHOS_DL_REGION v1`; 新增 ③d patch 18 apply 段(逐缺补齐); 修 `COMFYUI_SRC_DIR` 旧树分支(证据不全 → 走补齐链而非提前 return) |
| `entry/.../utils/ModelsDir.ets` | SUBDIRS +`diffusion_models`(extra_model_paths.yaml 自动同步), README 补行 |

- `OHOS_CATALOG_MIRROR` env 机制保留(手工覆盖; rport 隧道 smoke 不受影响——smoke 直 POST 不过 catalog)。
- **安全注记**: urllib 跟随 302 不校验 redirect host —— 白名单只约束提交的初始 URL。镜像域名本身受控(MS/HF-mirror 不会跳到任意内网), 风险有限; `127.0.0.1` 白名单仍是设计内唯一回环口(有网环境 smoke 除外)。
- sd-turbo 的 `sha256` **未填**: 宿主缓存(HF 版, sha=6b33199d…)仅作参考锚; 等真机从 MS 源下载后对设备文件校验一致再补(防"MS 版≠HF 版"时误杀)。

## 4. P1 远程算力 API(W4 立项)

已定稿 [external-api-node-design.md](external-api-node-design.md): 自研 `OHOS_API_Image`/`Text`/`HTTP` 节点组, 输出**标准 IMAGE**(可直接接 PreviewImage/SaveImage/原生下游), 零新依赖(requests/PIL 均在栈内), 社区节点只借鉴设计不背依赖。服务商适配层: 硅基流动(FLUX/SDXL)、阿里百炼(万相)。配套: API 密钥设置页(系统 keystore)+ 本地/云双模自动路由(本地能跑→本地; 否则提示一键转云)。**前置: P0 真机验收**。

## 5. 量化立场

- 端上不做"转换"功能: ①转换需全量载入权重→12-16G 设备 OOM; ②CPU 无 int8/fp8 硬件加速, 收益仅体积/带宽; ③画质损耗一档; ④镜上已有成品(fp8/GGUF), 下载即用。
- 落地形态 = catalog 条目级变体(已有 flux1-dev-fp8 示例), 标注体积/内存需求/推荐设备档位。
- "量化流水线"仅当 NPU 翻转后(格式必须贴合 NNRt 量化规则, 与通用 GGUF/fp8 不同), 形态为发布侧预量化包, 非端上交互。

## 6. 分级体验形态

- **Pad(12G)** = 创作入口 + 轻预览 + 云执行端; **PC(32G)** = 本地重载执行(SDXL/fp8)+ 同等云能力。
- 共享 catalog/工作流; 缺模型 → 镜像一键下载; 跑不动 → 一键转云。
- 后期差异化候选: 鸿蒙分布式协同(PC 部署后端做家庭算力, Pad 当遥控器)。

## 7. 验收 / 回退

- **真机 Q 判据**(#87): ①设备直连镜像下载 SD-Turbo 2.6GB → 入库 → 选择器出现 → 出图 256 成功(UI 一键路径, 不经 rport); ②记录镜像下载速度(与宿主带宽比对); ③模型库侧栏可见 diffusion_models 目录与 flux 变体条目。
- **回退**: catalog 主 url 还原(`url_alt` 即原样, 换回一行); patch 18 闭 = fetch 3 处注释(两处可逆)。
- **风险**: 设备无外网 → 保持 rport 隧道(白名单 127.0.0.1 已备)并把镜像下载列为"有网环境可用"; xet 1h 签名 → hf-mirror 仅备选, 失败重下(v1 无续传, 已在模型下载文档注明)。
