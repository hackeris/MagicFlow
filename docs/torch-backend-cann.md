# torch 端侧 NPU 后端方案分析(接入 Ascend C / CANN Kit / MindSpore Lite Kit)

> 2026-09-06。背景:设备 = Kirin 9020(MatePad,已在端侧 CANN Kit 支持芯片名单)。
> 需求:ComfyUI(Python torch 2.10 内嵌)在端侧 NPU 上出图,取代纯 CPU(87s/图、F32 5.2G
> 致 256 级内存上限)。本文回答「能否给 torch 做一个端侧后端,把 Ascend C / CANN /
> MindSpore Lite Kit 适配进去」—— 结论:**能,主路径 = CANN Kit 的 NNRt 单算子执行
> 接口(per-op),做成 torch PrivateUse1 后端插件;MindSpore Lite Kit 不可行(接口
> ArkTS-only);Ascend C 自写算子 = 内置算子库的补差手段。**

## 0. 决策摘要

| 接入面 | 接口形态 | 能否从 NCP 子进程(Python/C++)调用 | 与 torch 适配形态 | 判定 |
|---|---|---|---|---|
| **CANN Kit / NNRt(单算子执行)** | Native C/C++(`libneural_network_core.so`,OH_NN_* 系列) | ✅ dlopen/链接系统库 | per-op 执行器,正对 torch dispatcher | **主路径** |
| CANN Kit(整模型.om 推理) | 同上 | ✅ | 整图一次(NPU 上跑全图) | 备选:图级加速(保生态难) |
| MindSpore Lite Kit | ArkTS API(`@kit.MindSporeLiteKit`) | ❌ 纯 ArkTS,子进程无法调用 | — | **排除**(除非推理移到父进程过 IPC,代价过大) |
| Ascend C 自写算子 | 算子工程(msopgen/op_host/op_kernel),集成进 NNRt | ✅(编译进 .so) | 补差值(编写难度大) | 补差手段 |

一句话:**做一个 torch PrivateUse1 后端插件(`cann` device),每个 ATen op 经 NNRt
单算子执行器落到 NPU;平台内置算子库(system 插件 kirin9020 平台包)覆盖不了的 op
再用 Ascend C 补写;ComfyUI 侧只需小改 device 判定。**

## 1. 端侧 CANN Kit / NNRt 事实(已实证,勿重查)

- NNRt 三对象:`OH_NNModel`(图描述)、`OH_NNCompilation`(编译到指定硬件)、
  `OH_NNExecutor`(持有编译结果执行推理)。
- 单算子执行场景(官方语):「第三方框架加载原始模型阶段,根据算子的输入/输出/权重
  等信息创建单算子执行器对象并完成加载;框架执行模型推理时,在算子计算过程中调用
  单算子推理接口完成运算」—— **这就是巧妙的形状匹配:torch 的 dispatcher → 每个
  ATen op 回调 → 一个单算子执行器**。
- 完整调用流程:设备枚举 `OH_NNDevice_GetAllDevicesID`(name == `HIAI_F` = 高性能
  NPU)→ 加载模型 `OH_NNCompilation_ConstructWithOfflineModelBuffer` → `SetDevice`
  → `Build`(host 侧做 tiling/shape 推导)→ 创建执行器 `OH_NNExecutor_Construct`
  → 部分 OTensors `OH_NNTensor_Create` / `GetDataBuffer`(**共享张量 = 零拷贝
  ION 内存**)→ `OH_NNExecutor_RunSync` 同步推理 → 释放。
- 链接:`target_link_libraries(… neural_network_runtime neural_network_core)`
  —— NDK 提供头文件与库,设备侧系统库存在(OpenHarmony NNRt 开源,部件随系统发布)。
- 模型文件:离线转换(**运行时不能现场编译**);AddCustom 样例是 ONNX 单算子模型;
  端侧 .om 模型为 CANN 库离线产物。→ 我们的做法:**离线预生成 per-op 模型库**
  (每个 ATen op 一个固定 shape 的单算子模型),运行时按 op+shape 查表加载。
- 自写算子链:原型 JSON → `msopgen gen -i AddCustom.json -c ai_core-kirin9020 -f ONNX
  -out ./AddCustom` → op_host(op 原型注册/shape 推导/tiling)+ op_kernel(Ascend C 核函数)
  → `./build.sh` 部署(产物导入 NNRt)→ `ascendebug kernel --chip-version kirin9020`
  调试。平台包(DDK 的 `tools/platform/ kirin9020`)含内置算子支撑,样例仅覆盖
  Mate 70 系/X6/MateBook,平板 = **PoC 第一验证点**。
- 芯片支持:Kirin 9020 / 9030 / X90;AI Core 分 Cube(矩阵)/Vector/Scalar +
  层次化存储, 浮点以 fp16/bf16/int8 为主(样例即 fp16/ND 格式)。

## 2. MindSpore Lite Kit 为何排除(本次调研关键发现)

- 接口 = ArkTS(`import { mindSporeLite } from '@kit.MindSporeLiteKit'`;
  `loadModelFromBuffer` / `model.predict`),**无 C/C++ 子进程入口**。我们的推理在
  NCP 子进程(内嵌 CPython),跨进程调 ArkTS = 与后台交互,且张量数据过 IPC 会
  毁掉吞吐;把推理移到父进程 = 重组产品进程模型。排除。
- 即便可用:白名单算子、官方明确不支持自定义算子、不支持控制流(采样循环得拆图)、
  动态 shape 部分算子回退 CPU、NPU 全量推理约束多 —— 与「torch 生态自适应」目的不符。
- 结论:MS Lite 只对"换 .ms 图、丢 ComfyUI"路线有意义,与本文目标相悖。

## 3. torch 后端总体架构

```
┌─ 产品侧(已有, 小改)────────────────────────────────────────────┐
│ ComfyUI 0.34 + patch(device 判定: 'cann' 当第一硬件加速设备)      │
│   comfy.model_management.get_torch_device() 等几处硬编码            │
└──────────────────────────────┬──────────────────────────────────┘
                               │ 单条链: 模型 .to('cann') / torch.zeros(..., device='cann')
┌─ torch (skh 树内 2.10, 不改树)────────────────────────────────┐
│ Dispatcher: PrivateUse1 关键 → 本后端注册的 TORCH_LIBRARY_IMPL    │
│ Tensor/Storage/Allocator: at::DeviceType::PrivateUse1 + ION 池   │
└──────────────────────────────┬──────────────────────────────────┘
                               │ C++ 插件 cann_backend.so
┌─ 本后端(新代码)────────────────────────────────────────────────┐
│ ① Python 注册层:                                                   │
│    torch.utils.rename_privateuse1_backend('cann')                 │
│    torch._register_device_module('cann', cann_module)             │
│    torch.utils.generate_methods_for_privateuse1_backend(...)      │
│ ② C++ dispatch 层(TORCH_LIBRARY_IMPL(aten|自定义, PrivateUse1)): │
│    per-op 实现: input/weights → ION 共享张量 → 单算子执行器 → 回写   │
│    内置算子表(平台库) | 自写算子表(.om 库) | CPU fallback 表        │
│ ③ 接入层(NNRt + 本后端预生成算子库):                                │
│    op_lib: <op>.om 单算子模型(离线烘焙, 静态 shape × N 常用张量)    │
│    device 枚举 HIAI_F / compilation cache / executor 复用池          │
│ ④ 内存层: ION 分配器(torch empty → GetDataBuffer 共享指针可达)      │
│    + 模型权重常驻 NPU(fp16 一次性转换)                              │
└──────────────────────────────────────────────────────────────────┘
```

要点:
- **不改 torch 树**:插件 .so 通过 `torch.ops.load_library` 载入,与树内 libtorch
  ABI 一致编译(我们的构建链已有 torch 树,加独立 CMake target 即可)。
- **同步执行**(RunSync)起步:SD 链计算密集,每 op 同步往返的开销可接受;无需 stream/
  event 语义,大幅简化 v1(后续异步+event 再优化)。
- **op 库烘焙策略**:256 图 latents 固定 32×32×4,SD-Turbo 全链 shape 静态 → 每个
  op 只烘焙 1-2 种 shape 的单算子模型(权重为常量的 op 烘焙成权重内置参数)。
  动态部分(batch/dimension 变体)走 CPU fallback 表(数据小,代价可控)。
- **CPU fallback 表设计**:每个 op 决策 `ALLOW_CPU` 链表——搬运一次小张量的开销
  ~0.1ms 级,对 2 步 256 链的慢环节(单步计算集中)而言,把 `index/arange/cat/
  copy_/view` 等轻操作留给 CPU 合理;DP/dtype 元信息 op 全套 CPU。
- **精度**:模型 fp16(已有转换手段),AI Core fp16 原生;euler 数学 ops 在 CPU
  (数据量少),UNet/VAE 主计算全 NPU。先 fp16 后试 bf16/int8。

## 4. op 覆盖策略与 SD-Turbo 链清单(草案)

**提取方法(工程上):** 在真机/本地跑一轮 `torch.profiler`(profiler 记录 CPU 图的
ATen op 名),与 dispatch 错误驱动迭代结合:第一次跑把所有 op 都当 unsupported 报
错,报错列表 = 最小实现集。

**经验清单(按图结构估算,~40-50 distinct ATen op):**

| 段 | 主要 op | 难度 / 备注 |
|---|---|---|
| UNet(SD-Turbo 12 层) | conv2d, add, mul, relu/silu, layer_norm, group_norm, gelu, softmax, matmul/bmm, transpose/permute, reshape/view, cat/split, arange, scatter_add, linear | 卷积/归一化/attention 是重活=全 NPU;reshape 类元操作=CPU 免费 |
| CLIP 两分支 | text-encoder 同上(无 conv, 有 attention/embedding) | 少量, NLP 精度敏感点 |
| VAE 编解码 | conv2d, conv_transpose2d(+interpolate/nearest), add, relu | convT 是 VAE 特有 op,内置库缺失则必须自写(难点候选) |
| KSampler(euler, 2 步) | mul/add/div/exp/neg/cat/scatter/broadcast_to/randn… | 全轻量数学,CPU fallback 亦可接受 |
| 权重加载/搬运 | copy_, empty, zeros, ones, dtype 转换 | CPU 侧常规 |

**内置算子库覆盖评估 = 最大未知(P0/P1 要实测):** 平台插件里 conv2d/matmul/softmax
/group_norm 这类"主力"大概率在(官方样例与框架集成产物都依赖它),但「conv_transpose2d」
「fused attention」等不确定 → 每缺失一个自写 kernel ≈ 熟悉 Ascend C 后 3-7 天。
**据此止损:** 全部主力 op 缺失占比超 1/3 ⇒ 重估(见 §7)。

## 5. 与现有构建链集成

- 新增工程:`entry/src/main/cpp/cann_backend/`(CMake target `cann_backend.so`,
  链接 `neural_network_runtime` + `neural_network_core`)——与 cpp/ 现有 NCP 宿主
  同构(子进程直接 dlopen 即可,系统库全进程可访问)。
- 预生成算子库:开发机脚本(maketools 时代)输出 `cann_op_lib/<op>.om → rawfile
  或 pyroot 资源`,随 HAP 打包;运行时 NNRt `ConstructWithOfflineModelBuffer` 加载。
- Python 侧:`cann_backend` 插件的 `register.py`(rename/device_module/generate_methods)
  + ComfyUI patch(几行 device 判定:存在 cann device 且可用 → 当作首选;不存在 →
  原 CPU 路径不变,完全可逆)。
- 模型:复用现有 W3 下载链,模型在 `ModelRoot`;后端启动时校验 fp16 权重(离线转换
  一次缓存 `~-fp16.safetensors`)。
- verify_smoke 演进:新增 K 判据「NPU 出图」:指定 device=cann 跑吞吐采样,时间
  <30s 且图与 CPU 图相似度在阈值(SSIM>0.85),原 CPU 判据全保留(回归不破坏)。

## 6. 里程碑与判据(每步失败即止损)

| 阶段 | 内容 | 判据 | 失败止损 |
|---|---|---|---|
| P0(1-2 天) | ① DDK/平台包安装② 官方 AddCustom(或 Sobel Codelab)真机跑通——**第一款非官方列表设备验证** | 真机上 HIAI_F 枚举/单算子执行成功 | 平板 runtime 不可用 ⇒ 全盘放弃(cpu 继续榨) |
| P1(1-2 周) | cann_backend 骨架:device/ION 内存池 + add/matmul/conv2d 3 个核心 op;真实权重跑通 | torch 前端构造 cann tensor 加法正确;conv 与 CPU 结果 diff<1e-3 | 适配器不可行(每 op 包装成本超预期) ⇒ 转整图 .om 路线评估 |
| P2(3-5 周) | op 集补齐(以 profiler 清单驱动)→ SD-Turbo 256 全链出图 + 精度校准 + 内置/自写比例确认 | 真机 256 出图 ≤ 30s(基准 87s);图 SSIM>0.85;无 OOM | 覆盖缺失>1/3(自写成本爆炸)⇒ 降级 P2' 混合(重算 NPU 轻算 CPU)继续 |
| P3(1 周) | 512 级试跑(fp16 2.6G 内存判据)+ 异步/流优化 + verify 收编 | 512 链段可行或明确失败原因;全量回归 PASS | — |

**总预算**:P1-P2 为主导 → 单人多周;所有阶段可回退(ComfyUI patch 可删、后端插件
可卸载)。

## 7. 风险表

| # | 风险 | 缓解 / 证据 |
|---|---|---|
| 1 | 平板 9020 固件/NDK 对 NNRt 的支持(DK 样例未列平板) | P0 第一判据;OpenHarmony NNRt 开源部件内,系统库存在概率高 |
| 2 | NNRt 内置算子库对 SD 链覆盖(尤其 convT/attention 融合) | P1 实测;缺失库写 Ascend C(3-7 天/算子),超 1/3 止损 |
| 3 | ION 大块分配上限(fp16 模型 ~2.6G + 中间张量) | P1 内存压力测试;超限 ⇒ 只做 256 + stagewise 卸载 |
| 4 | fp16 精度/算子差异致出图崩或偏色 | P2 精度校准(SSIM 阈值);可用 bf16/int8 变体 |
| 5 | per-op 调度开销吃掉收益(每 op host 往返 ~ms 级×每步上千 op 极端) | 2 步 SD-Turbo 每步 op 数 ≤ 数百,计算密度高;若仍差 ⇒ 图融合(整 .om 子图)兜底 |
| 6 | torch 2.10 PrivateUse1 与 skh 构建树 ABI | 树内编译插件,同 ABI;参考实现:torch_openreg 库, torch_npu(云侧,结构参考) |
| 7 | ComfyUI 对非 cuda 设备的硬编码 | 几处 patch(已知家底:model_management 等),可逆 |

## 8. 参考资料

- CANN Kit 基本架构 / AscendC 算子开发(设备端文档,device.harmonyos.com apiref)
- [cannkit_samplecode_add_custom_cpp(官方样例, 含 msopgen/ascendebug 流程)](https://gitee.com/harmonyos_samples/cannkit_samplecode_add_custom_cpp/blob/e1cf1b854dcd733bb4b7b96cd9af8682ba9811fe/README.md)
- [基于 AscendC 算子实现图像边缘检测(Codelab)](https://developer.huawei.com/consumer/cn/codelabsPortal/carddetails/tutorials_CANNKit-AscendC-sobel)
- CANN Kit 模型推理 / 集成模型(developer.huawei.com doccenter-capabilities)
- NNRt(Native)接口:OH_NNDevice_GetAllDevicesID / OH_NNCompilation_ConstructWithOfflineModelBuffer /
  OH_NNExecutor_Construct / OH_NNExecutor_RunSync(opengym 等源码与文档)
- MindSpore Lite Kit:`@kit.MindSporeLiteKit`(ArkTS-only 实证)、converter_lite 转换链、
  不支持自定义算子(华为开发者论坛官方答复)、算子白名单(mindspore-lite-supported-operators)
- torch 自定义后端:PrivateUse1(torch 2.1+ 官方加速器集成机制)、
  [torch_npu 注册结构参考](https://gitcode.com/Ascend/pytorch/blob/6ccc63b598a92f0251280f141f3ddd1a95079c0b/torch_npu/_init/registry/backend.py)、
  [Accelerator Integration 官方文档](https://docs.pytorch.org/docs/2.13/accelerator/index.html)
