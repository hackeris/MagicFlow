# 状态与后续(Status & Next)

> 2026-09-12 更新(初稿 2026-09-05)。本文是**项目级状态页**:已交付什么、还剩什么、为什么。
> 各专项的设计、数据与验证细节在对应文档里,本文只做索引与决策记录。

## 1. 已完成(勿重复投入)

| 目标 | 证据/产物 |
|---|---|
| CPU 多线程推理(OpenBLAS 12 核) | MM4x=0.71s(0.61s 复现),`BLAS_INFO=open`;`docs/local-inference-torch-parallel.md` §6 |
| 装载全链修复(mbind/60 LAPACK/gfortran stub) | 未定义符号差=0;同上 §6.1 |
| 真机出图 | 256×256 SD-Turbo ≈88s;字节级复现 103,411B |
| **全链字节级复现** | `docs/BUILD.md` + `thirdparty/ohos-torch/run_repro_chain.sh` |
| **smoke 黑盒验证(10 判据)** | `scripts/verify_smoke.sh` + `make verify`;设计见 `docs/smoke-design.md` |
| **W1 环境门户 / W1.5 单入口 / W1.6 官方同构启动页** | `docs/workspace-design.md` §3(零自动启动,用户点击才能起后端) |
| **前端 fork 化**(官方 v1.54.4 + 自维护 ohos 定制线) | `docs/frontend-fork-plan.md`;pin 见 `config/externals.pins.tsv` |
| **W3 模型管理**(下载端点 + 用户可见模型目录 + 轻量模板) | `docs/model-download.md`;patch 16/17 |
| **P0 模型获取国产化**(catalog 12 条,国内镜像直连) | `docs/comfyui-cn-cloud-strategy.md` §2-3;patch 18-22 |
| 品牌更名(梦幻之流 / MagicFlow) | bundleName=`app.fuqidian.magicflow`(2026-09-07) |
| 图片保存(ArkWeb 桥 + picker 另存) | commit 6cfa2f0 |
| **NPU 可行性验证 → 结论:不可行,路线终止** | `poc/npu/README.md` **顶部的修正说明**(原"33/34 算子 PASS"已证伪) |

## 2. 设备能力上限定谳

MatePad 11.5 S / 11.8GB RAM:**稳态规格 = 256×256 级**(fp16 模型)。

| 规格 | 结果 |
|---|---|
| 256×256(fp32 模型) | ✅ 87-89s |
| 256×256(fp16 模型 2.6G) | ✅ 100s(含冷启动+载入),更稳 |
| 448×448 / 512×512(含 fp16) | ❌ SIG9(峰值 5.0-6.3G) |

- **fp16 收益已确认**:模型常驻 5.2G→2.6G,存储减半;但**不能解锁 512** ——
  全局内存仍空 5.4G 时照样 SIG9 ⇒ 属**平台级限制**(cgroup/调度器),非全局 OOM。
- 瓶颈链:pyroot+torch 运行时基线 ≈2.3G + fp16 模型 2.6G = **4.9G 常驻**,大张量即触顶。
- 数据详见 §3 与本文件 git 历史(2026-09-05 版 §2.5)。

## 3. 推进方向(按就绪度)

| # | 方向 | 价值 | 成本/风险 | 状态 |
|---|---|---|---|---|
| ① | ~~workspace 门 + 延迟启动~~ | 中(产品观感) | — | **已交付**(W1/W1.5/W1.6) |
| ② | ~~一键 smoke(`make verify`)~~ | 中(防回归) | — | **已交付**(10 判据) |
| ③ | ~~fp16 模型试点~~ | — | — | **已完成**,结论见 §2 |
| ④ | ~~CANN/AscendC 试点~~ | — | — | **已被 NNRt 路线取代** |
| ⑤ | ~~NPU 后端(NNRt / PrivateUse1)~~ | — | — | **已终止**(设备不支持卷积,见下) |
| ⑥ | **W4:远程生图 API 节点组** | **高(体验上限)** | 中(见设计文档) | **待立项** —— 设计已定稿,前置(P0 真机验收)已完成 |
| ⑦ | 品牌/遥测清理 | 中(发布合规底线) | 低-中 | 待做 —— `docs/comfyui-branding-audit.md`(遥测项优先) |
| ⑧ | 运行时基线优化(减小 2.3G 常驻) | 中(解锁 512 的唯一路径) | 高(动 torch/运行时) | 研究项,非紧急 |
| ⑨ | workspace 规格选择 UI | 低(512 已放弃,只剩 256 提示) | 低 | 待重新定义范围 |

**W4 说明**(端云策略 §4):自研 `OHOS_API_Text/Image/HTTP` 三节点,让工作流直接调第三方生图
API,输出标准 IMAGE 张量可接原生下游;零新依赖(custom_nodes 目录机制,与 patch 15 同构)。
设计见 `docs/external-api-node-design.md` —— ⚠ **实施前需修正该文档中的 patch 编号**
(文中写 18,已被 `18-ohos-catalog-cn.patch` 占用,应改用 23)。

**NPU 终止原因**(2026-09-12 用户拍板):设备侧 NNRt **不支持卷积算子** ⇒ SD 出图的算力主体
无法下沉,端到端加速不成立。全部实现与完整证据归档在 `npu-poc` 分支。

## 4. 决策记录

- 2026-09-05:用户拍板"能跑就行,先把版本固定" → 512×4 判据**放弃**,不追;
- 2026-09-05:复现链验证目标达成 → 归档为 `docs/BUILD.md`;
- 2026-09-07:包名改 `app.fuqidian.magicflow`,签名改用共享 `default_MagicFlow_*`;
- 2026-09-08:用户拍板「外部化两层」→ P0 模型获取国产化(已交付)+ P1 远程算力 API(W4);
- 2026-09-12:用户拍板**放弃 NPU 计划** → 全部 NPU 工作归档 `npu-poc` 分支(8 提交),
  master 回到与 `origin/master` 一致;
- 2026-09-12:文档清账 —— 修正被证伪的 NPU 结论、过期包名/路径/破引用,并清除
  `prebuilt/nnrt/` 的 NPU 残留(该残留会使 `make hap` 的 prebuilt 死文件断言 FATAL)。
