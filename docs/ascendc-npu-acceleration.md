# AscendC 算子加速 PyTorch 推理——鸿蒙端侧调研(2026-09-05)

## 结论速览

**部分可行,但本项目当前真机不在支持名单。** 华为已于 2025 年底前后在 HarmonyOS 端侧正式开放
**CANN Kit(异构计算框架服务)**,开发者可用 AscendC(类 CUDA C 的算子语言)编写自定义算子,
在 PC 上交叉编译后随应用在真机运行。但支持芯片**仅限麒麟 9020 / 9030 / X90 系列**
(Mate 70 及更新旗舰);**本机 MatePad 11.5 S(麒麟 9000 系)不在此列**。

- 此前的"端侧不能为 torch 写 NPU 算子"结论已过时;"端侧无 torch_npu"依然成立(没有官方 NPU 后端)。
- 接入 PyTorch 只能自研桥接:`torch::library` 自定义 op → 端侧单算子 API。
- 完整 NPU ATen 后端不可行(人月级、端侧无 dispatch 注册点、无图编译/图执行)。

## 1. 端侧 AscendC 运行环境的真实情况

| 项 | 事实 |
|---|---|
| 开放姿态 | HarmonyOS 6 官方 Kit(非 OEM/签名白名单);DOC 域 `hiai-*`(HiAI Foundation Kit 承载),开发接口(hiai_single_op.h / OH_NN* / NNRt)公开,官方 Codelab 与 Gitee 样例公开 |
| 支持芯片 | **Kirin 9020 / 9030 / X90 系列**(2025-12 新增 X90/9030);麒麟 9000s/9010 等旧旗舰不在支持列表 |
| 系统要求 | HarmonyOS 5.0.5 Release 及以上 + DevEco Studio 6.1.0 + HarmonyOS 6.1.0 SDK |
| 开发形态 | 工具链在 **Ubuntu 22.04(x86)PC**:DDK Tools 5.1.1.0 + 芯片插件包(如 `kirin9020-plugin`)+ `tools_ascendc`;`msopgen gen -i x.json -c ai_core-kirin9020` 生成工程,`build.sh` 交叉编译 → 算子包(`aclnn_xx.h`+`libcust_opapi.so`)或 `.omc` 单算子模型,进 `resources/rawfile` |
| 调试 | `ascendebug kernel --backend cpu/simulator/npu --chip-version kirin9020` |
| Host runtime | 端侧单算子层:`HMS_HiAISingleOp*` C API(ION 内存零拷贝);`AiModelMngerClient`(<.omc>);NNRt `OH_NNDevice*/OH_NNExecutor*`(设备名 HIAI_F)。**⚠ 端侧暂不支持图编译与图执行**——算子只能按"AI 框架算子适配"集成(单算子或 ONNX 子图转 .om 后 NNRt 执行),没有云侧 `aclrtLaunchKernel` 直调闭环 |
| 性能模型 | 端侧 SoC 统一内存(NPU/CPU 共享 LPDDR,无独立显存账户);DaVinci 矩阵指令 **FP16/BF16/INT8 为主,无原生 FP32**;麒麟 9020 等效 LLM 有效算力约 2.5 TOPS(INT8 口径),标称稀疏 30~38 TOPS |
| 精度 | PyTorch CPU 版是 FP32 → 下沉需 fp16 化或 Cast,否则收益打折 |
| 已知限制 | NPU 调度器并发在 5.0.0.138+ 支持 2~4 路;高频小算子被调度/启动开销淹没 → 必须做**融合算子** |
| 先例 | QQ 音乐用 CANN 端侧自定义算子(声伴分离核心算子)实现移动端实时推理(2025-12 官方口径) |
| 开源侧 | OpenHarmony/开源无 CANN Kit(其 AI 子系统是 NNRt/MindSpore Lite),是商业 HarmonyOS 专有 |

## 2. PyTorch 接入路径对比

| 路径 | 可行性 | 工程量 | 说明 |
|---|---|---|---|
| ① torch::library 自定义 op + C++ 调端侧算子 API | **可行(推荐)** | 单算子约 2~4 周(含工具链) | wrapper .so:`torch tensor ↔ ION Buffer`(零拷贝 `at::from_blob` 可引用单片映射)→ `HMS_HiAISingleOp*`/NNRt;算子本体:ONNX 原型 → msopgen → CopyIn/Compute/CopyOut → 交叉编译 |
| ② 完整 NPU ATen 后端 | **不可行** | 人月级×N | 无端侧 torch_npu/无 dispatch 注册点;云侧 OpPlugin 与云侧 CANN 深度耦合;叠加图执行限制 |
| ③ 算子级替换(热点下沉) | 可行(①的应用形态) | 每算子 1~4 周 | Linear/MatMul、Attention QKV、Conv、LayerNorm/Softmax 下沉,其余留 OpenBLAS;收益取决于融合粒度与 fp16 化 |

## 3. 本项目的可执行结论

- **当前真机 MatePad 11.5 S(麒麟 9000 系,SLG-W30)不在支持名单** → 此路线临时搁置。
- 若未来换 麒麟 9020+ 真机,最低成本试点:
  1. Linux 配齐 DDK Tools 5.1.1.0 + kirin9020-plugin + tools_ascendc(官方 Sobel Codelab 流程);
  2. 选**单个 2048×2048 Linear/MatMul** 探针(与现有基准对比:OpenBLAS 12 线程 0.24s/次);
  3. ONNX 原型 → msopgen → kernel(Vector 先跑通,再上 Cube 双缓冲)→ ascendebug simulator 验精度;
  4. 真机集成决策点:(a) 能直接调单算子 API → torch::library op 零拷贝接入(理想路径);(b) 只走框架适配 → ONNX 子图 OMG 转 .om + NNRt 执行;
  5. 验收:端到端(含数据进出)显著低于 0.24s 且 ≥2× 才留;2 周无 2× 即止损(CPU 多线程已是可用基线)。
- **红线**:不做完整 NPU ATen 后端;无 9020+ 真机不立项;不做依赖 GE 图/图编译的融合方案。

## 4. 来源

- CANN Kit 文档(华为开发者联盟):AscendC 简介 / 基本架构 / Native C API 参考(含 hiai_single_op.h)
- 快速入门 / 算子工程编译 / 图编译与图执行(明确"暂不支持图编译与图执行,仅支持框架算子适配")
- 官方示例:cannkit_samplecode_add_custom_cpp(Gitee)/ CANNKit-AscendC-sobel Codelab
- 生态:IT之家"CANN 开放端侧 NPU 自定义算子编程,QQ 音乐首创移动端实时声伴分离"
- 端侧算力/调度并发参阅:麒麟 9020 本地大模型部署实测(等效吞吐、fp16/w4a16、调度并发)
- 云侧对照:huawei ascend OpPlugin 概述 / aclrtMalloc / Kernel 直调概述
