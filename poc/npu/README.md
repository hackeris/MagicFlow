# NPU 能力验证(poc/npu)

> ## ⚠️ 2026-09-12 修正:核心结论已证伪,NPU 路线已终止
>
> **「SD 链 33/34 算子完整通过」是假通过。** 原因是 NNRt 的**编译缓存串图**:同一个
> `cacheDir` 下,`OH_NNCompilation_SetCache` 把**别的图**的编译结果交给了当前图 —— 探针
> 34 项的输出全部等于第 0 项(ADD)的结果 `2.00`,而 SUB 本该是 0、DIV/POW 本该是 1,数值面
> 自相矛盾;耗时上也只有 ADD 是真实编译(`build=0`,24ms),其余全是 3~4ms 缓存命中。
> 实际得到真实验证的只有 **ADD 一个算子**。
>
> **随后用自建 torch Engine 复测:CONV2D 在 9030 上 `OH_NNCompilation_Build` 直接失败**
> (`rc=1`)。20 余组单变量实验穷尽排除了权重布局 / STRIDES rank / NCHW-NHWC / PAD 形式 /
> 规模 / 卷积核 / 性能模式 / 缓存 / 数据供给等因素,失败点定位到设备侧
> `IsSupportedModel()` → `GetSupportedOperation()`。**设备 NNRt 不支持卷积算子** ⇒ SD 出图
> 的算力主体无法下沉,端到端加速不成立 —— **用户已拍板放弃 NPU 计划**。
>
> NPU 的全部实现与完整修正记录归档在 **`npu-poc` 分支**(`docs/npu-backend-roadmap.md`
> 与修正后的 known-limitations)。**本文其余内容仍然有效**:三条替代路线(离线模型 / 单算子
> 直调 / 部分设备子进程不可见)的否定结论、子进程形态结论、NNRt 构图参数规格、工具链细节。

本文档的正文是 2026-09-07 一轮 NPU 验证的记录(其中算子覆盖结论已被上注修正)。
其他 NPU 文档(p0-nnrt-verdict、p0-nnrt-offline-verdict、torch-backend-cann、
ascendc-npu-acceleration、npu-poc-verdict)均为过程记录,已按用户决定删除。

## 三句话结论

1. ~~**方案可行,且比预期顺**:做一个 PyTorch 后端插件,用 NNRt"在线构图+内置算子"执行每个算子,
   内置算子库没有的算子回到 CPU(OpenBLAS)。验证结果:SD 链 34 个算子中 **33 个在真机上完整通过**
   (构图、编译、执行、数值全部正确),剩余 1 个(LAYER_NORM)的失败是参数格式细节,不是算子缺失。~~
   **⚠️ 已证伪(2026-09-12):这是编译缓存串图造成的假通过,实际只有 ADD 一个算子真实可用;
   CONV2D 在设备上直接编译失败,路线已终止。见顶部修正。**
2. **执行开销可接受**:单个算子编译约 3 到 4 毫秒、执行约 1 毫秒。按 SD-Turbo 每步约一百个算子算,
   每张图的调度开销约零点几秒,远低于当前 CPU 出图时间(一台 256×256 图约 88 秒)。
3. **形态障碍比预想小**:9030 设备上,原生子进程也能看到 NPU 设备(数量为 1,带真实硬件名)。
   主项目当前"子进程跑推理"的产品形态在 9030 目标机上不需要搬家。

## 为什么要做这个 demo

主项目把 ComfyUI 移植到了鸿蒙设备,推理目前用 CPU。本 demo 是一个独立工程,专门在真机上
回答"PyTorch 能不能在设备 NPU 上执行、用哪种方式接入",并保留可重复运行的程序。
此前结论散落在多篇文档里,混有调研过程、推测和已被推翻的说法,现全部统一到此文。

## 验证内容与结果(9030 设备,192.168.1.4:44959)

demo 安装后页面自动运行,报告写在应用的 `filesDir/npu-poc-report.md`。共 7 个通道;
两个补充说明:①应用刚启动的 1 到 2 秒内查设备可能查不到(异步注册,探针已加等待);
②下面所有执行类结果都来自带真实硬件名的设备口 NPU_ohos.boot.hardware.KirinXE90_v2_0。

| 通道 | 验证什么 | 结果 |
|---|---|---|
| 设备枚举(C API) | 应用进程里能不能枚举到 NNRt 设备 | 可以。9030 上有一个真实硬件名加一个虚拟口 HIAI_F |
| 设备枚举(ArkTS) | MindSporeLite Kit 侧能不能看到 | 能看到,两个设备都在。但该 Kit 只能 ArkTS 用,子进程和 Python 使用不了 |
| 算子覆盖矩阵 | SD 链 34 个算子能否完整跑通 | ~~**33/34 完整通过、输出数值全部正确**~~ **⚠️ 已证伪(2026-09-12):编译缓存串图造成的假通过 —— 34 项输出全等于第 0 项 ADD 的结果 2.00,实际只有 ADD 一个算子真实可用**(当时的验证范围:ADD/SUB/MUL/DIV/POW/MINIMUM/MAXIMUM/BIAS_ADD/SQUARED_DIFFERENCE/MOD/RELU/SIGMOID/TANH/SQRT/EXP/LOG/ERF/NEG/ABS/SWISH(SILU)/HSWISH/GELU/LEAKY_RELU/SOFTMAX/L2_NORMALIZE/MATMUL/FULL_CONNECTION/CONV2D/CONV2D_TRANSPOSE/DEPTHWISE_CONV2D_NATIVE/AVG_POOL/MAX_POOL/CONCAT) |
| 逐算子耗时 | 单算子的编译与执行各花多少时间 | 编译 3 到 4 毫秒,执行 0 到 1 毫秒 |
| 离线模型 | 自定义算子编译成 .omc 离线模型后能否加载执行 | 不能。系统库明确拒绝(编译返回错误 1)。9030 与另一台设备上结果一致 |
| 单算子直调 | 能否不经过 NNRt 直接调用 NPU 单算子接口 | 不能。开发包没有对应头文件,11 个候选库全部无法加载 |
| 子进程 | 官方原生子进程(与主项目推理子进程同一形态)能否看到设备 | **9030 上可以**(数量 1,真实硬件名)。另一台设备(9020)上不可见——子进程可见性与设备有关,不是"所有子进程都不行" |

## 可行的 PyTorch 后端方案(支持完整 ComfyUI)

### 怎么做

- 用 torch 的标准机制(PrivateUse1)注册一个名为 cann 的设备类型;
- 每个算子交给 NNRt 执行:在线构图、编译、执行;张量内存与 NPU 内存池共用(ION,零拷贝);
  **执行前必须用 OH_NNExecutor_GetInputCount/GetOutputCount 查询执行器实际输入输出数并按其创建张量**
  (本次验证发现:执行器期望的输入数恒为 2,与构图期算子定义张量数不同;按构图期定义传参会导致
  单输入类算子返回参数错误——修掉之后 21 项假失败全部转 PASS);
- 内置算子库覆盖不了的算子按查表回到 CPU(OpenBLAS)——这是"完整 ComfyUI"的兜底:任何工作流可运行;
- 权重一次性转 FP16 常驻 NPU;
- 推理进程:9030 上现有子进程形态可用,不必搬 UIAbility 进程(NAPI 桥不再是必须;9020 类设备仍需)。

### 为什么这样可行

- ~~覆盖:33/34 算子全通过,SD 链主力(卷积、矩阵乘法、线性、归一化、激活、池化、注意力相关)
  全部在内,自写算子工作量非常小;~~ **⚠️ 此条支撑已消失(2026-09-12):算子覆盖结论不成立,
  且 CONV2D —— 卷积正是 SD 链主力 —— 在设备上 `Build` 直接失败。**
- 开销:单算子 5 毫秒以内,缓存复用路径没有额外的更慢成本(Build 一直在 3-4ms);
- 替代路线三选一都确认不通:离线模型被拒、单算子直调无接口、部分设备子进程不可见——
  在线构图+内置算子就是唯一路径,而它恰好可行。

### 后续实施顺序

> **⚠️ 本节计划已作废(2026-09-12)**:R1/R2/R3 的前提是算子覆盖结论成立;实际设备不支持卷积,
> 用户已拍板放弃 NPU 计划。列出仅为保留当时的判断依据。

1. 补 LAYER_NORM 参数(见技术备注;预计半天);
2. R1(1 到 2 周):后端插件骨架 + ION 内存池 + 三个核心算子(卷积、矩阵乘法、softmax)实机与
   CPU 结果对比(数值差小于千分之一);
3. R2(3 到 5 周):按实际算子清单补齐,SD-Turbo 256×256 真机出图,时间小于 30 秒、
   出图与 CPU 版本相似度(SSIM)大于 0.85;
4. R3(1 周):512 级试跑,回归收编。

## 技术备注

- **算子参数规格的权威来源**:OpenHarmony NNRt 源码
  (本机 `build/nnrt-src/frameworks/native/neural_network_runtime/ops/` 各算子 builder)。
  实测:形状、轴、补偿等参数是 INT64;epsilon 是 FLOAT32;激活参数是 INT8;
  transpose/has_bias/use_axis 是 BOOL;GELU 的 approximate 是 BOOL;
  每个算子的参数张量用各自命名空间(CONV2D_、CONV2D_TRANSPOSE_、DEPTHWISE_CONV2D_NATIVE_ 等)。
- **LAYER_NORM 独一项未过**:构图返回无效参数,其一步骤(AddOperation)就失败。
  该算子的 gamma/beta 输入形状校验与参数排列仍需对照源码(ops/layernorm_builder.cpp)核对;
  已确认它不属于"算子不支持",属于"参数组合不合规范"。
- **执行器张量数法则**:per-op 执行时张量数量以 `OH_NNExecutor_GetInputCount/GetOutputCount`
  返回为准;执行器输入数可能是 2(输入+激活标志),与构图期定义的算子输入数不同。
  这是本次验证中"单输入算子全部失败、双输入算子全部通过"现象的根本原因,现已修正。
  ⚠️ 本条的验证背景已证伪(见顶部修正):当时那轮"修正后 21 项假失败全部转 PASS"正是缓存
  串图的典型表现,故此"根本原因"的归因**未经独立验证**,引用需谨慎。
- **设备选择**:枚举出的设备名里以 NPU_ 或 Kirin 开头的才是真实硬件;只选第一个或选 HIAI_F 会编译失败。
- 数据输入统一填 1.0、输出统一 FP32,便于人工核对数值。

## 工程说明

### 构建与运行

```bash
./poc/npu/build_and_run.sh            # 只构建(输出 poc/npu/entry/build/.../entry-default-signed.hap)
./poc/npu/build_and_run.sh run        # 构建+安装+启动+等待报告并打印
HDC_TARGET=192.168.1.x:5555 ./poc/npu/build_and_run.sh run   # 指定设备
```

依赖与主项目一致:hvigorw、鸿蒙 SDK、签名材料(/data/share/hap/.ohos/config)。
报告轮询上限 300 秒;脚本会在启动前删除设备上的旧报告(否则会读到上一次的结果)。

### 注意事项

- 本工程与主项目同一包名(app.fuqidian.magicflow)、同一签名。**两者不能同时装在同一台设备,
  装哪个哪个生效**。验证完回主项目时重装主项目 HAP 即可。
- 构建脚本里的工程根变量名是 POC 而不是 ROOT:主项目的环境脚本 env.sh 会覆盖 ROOT 变量,
  用 ROOT 会把产物路径指到主项目构建目录(发生过把主项目 482MB 的 HAP 发到设备的失误)。

### 目录结构

```
poc/npu/
├── README.md                    # 本文档(唯一结论)
├── build_and_run.sh             # 一键构建/部署/运行
├── build-profile.json5          # 签名配置(与应用主项目相同)
├── AppScope/ entry/             # 标准工程
│   └── entry/src/main/
│       ├── cpp/nnrt_probe.cpp   # 7 个通道主体(C++,NNRt 用 dlopen 方式调用)
│       ├── cpp/napi_init.cpp    # 接口注册
│       ├── cpp/child/npuchild.cpp # 子进程通道的子进程程序
│       └── ets/pages/Index.ets  # 页面,自动运行
└── entry/src/main/resources/rawfile/custom_graph.omc   # 离线通道输入(6405 字节)
```

## 结论依据

每次完整报告:设备 `filesDir/npu-poc-report.md`(与页面同内容)。最近一次报告
(当时记为 33/34 PASS,**⚠️ 已证伪,见顶部修正**)全文见 `/tmp/npupoc-9030-v5.txt`。
