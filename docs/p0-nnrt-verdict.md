# P0 实证报告:端侧 NNRt / Ascend C 链路(2026-09-06)

> 目标:装 DDK → 真机跑通官方 AddCustom 样例(9030/9020 平板, 192.168.1.8:33363)。
> **判决:目标未完全达成,但结论性证据全部拿到 —— 三条独立事实把「端侧 NPU 加速」
> 的可行性面精确勾勒出来了。本页是证据记录 + 下一步决策点。**

## 0. 一句话结论

- **硬件侧:OPEN** —— 平板 9020 的 NPU(`HIAI_F`)对**普通三方应用(UIAbility 进程)开放**,
  且 NNRt C API(与我们将来 torch 后端同构)在应用进程内**完整可用**(双通道验证);
- **工具侧:半开** —— DDK 5.0.2(旧版)在本机完成了 msopgen 生成 + Ascend C 算子工程
  **编译安装全链**(能出 libcustom_op.so 算子包),但 **5.0.2 的 omg 是云侧版本**,端侧
  `.omc` 离线模型需要 5.1.1+ 的 DDK + kirin9020 平台插件(官方下载页需登录,公开渠道
  未见直链;平台插件包(含 librl_search.so 等)缺失是工具侧唯一阻塞);
- **形态侧:RED FLAG** —— **NCP 子进程(`comfy_child`)对 NNRt 设备不可见(count=0,30s 重试
  稳定)**;同为同 uid 的 UIAbility 父进程 count=1。**这意味着:若走 NNRt 加速,推理必须搬进
  UIAbility 进程(NAPI/torch-in-parent),当前「子进程内 Python 推理」形态与 NPU 之间有鸿沟。**

## 1. 判据矩阵(全部真机实测)

| 判据 | 结果 | 证据 |
|---|---|---|
| DDK 5.0.2 安装/msopgen | ✅ | install.sh 成功;`msopgen gen -c ai_core-kirin9020 -f ONNX` 生成算子工程 |
| Ascend C 算子编译 | ✅ | `./build.sh` → "Build and install success"; 产物 `tools_ascendc/custom_op/kirin9020/lib/libcustom_op.so` + `aic-kirin9020-ops-info.json`(同步进 tools_omg/master 配置) |
| CPU/仿真调试 ascendebug | ❌(兼容问题) | gcc15 下 `GET_TILING_DATA` 未声明(依赖工具链内部模板宏, 非必要路径, 跳过) |
| 端侧模型转换 omg→.omc | ⚠ 本地链通至平台库缺口 | **ir_model_compile 路线全通**(python3.10 环境 + cp1251 修复 + gcc15 兼容三修后, kernel 编译通过), 最终仅差 **平台内核库(FMK_CL/librl_search.so/libai_npucore_generated.so)**, 归属 kirin9020 平台插件包(官方登录下载, 3 轮直链搜索无果) |
| NNRt 设备可见性(UIAbility 进程, C API) | ✅ **count=1 HIAI_F type=3** | NAPI `npuEnumerate`(父进程同库同调用方式)回显 |
| NNRt 设备可见性(UIAbility, ArkTS MindSporeLite Kit) | ✅ **count=1 HIAI_F** | `mindSporeLite.getAllNNRTDeviceDescriptions()` |
| NNRt 设备可见性(**NCP 子进程**, C API) | ❌ **count=0** | 子进程内置 npu_probe(dlopen 同库)6×5s 重试恒 0 |
| 系统库导出面 | 已固定 | nm -D(设备库): 仅**离线模型路径**(OH_NNCompilation_ConstructWithOfflineModel{File,Buffer}/Build/RunSync + NN_Tensor 体系);**无 OH_NNModel_*(动态构图)、无 AllocateInputMemory(Memory 老接口)** |
| 系统内 .omc 模型 | 未找到 | find /system /vendor /etc(无输出, 模型在私有分区) |
| 零自动启动 | ✅ 未破坏 | 恢复产品态后 ps=0 |

## 2. 三个决定性事实(对未来方案的含义)

1. **设备对三方开**
   `GetAllDevicesID=1, HIAI_F, type=3(NPU)/Performance` —— 9020 平板系统**已装载端侧
   CANN/NNRt 且对三方 App(签名调试版)开放**,不是"系统独占"。P1 可行性第一前提成立。

2. **端侧接口 = 纯离线模型 + NN_Tensor**(无动态构图/无老 Memory API)
   - torch 后端 per-op 必须拿到 per-op 的 `.omc` 模型库**提前生成**(op 库烘焙, 与
     docs/torch-backend-cann.md §3 的「op 库烘焙策略」一致);
   - 输入输出绑定走 `NN_Tensor`(`OH_NNTensor_Create` + driver 零拷贝);
   - **CPU 侧数据/元操作**仍需 in-host。

3. **NCP 子进程无设备可见性 —— 形态障碍(最大 BLOCKER 候选)**
   同 uid 父进程可见、子进程不可见。原因属性: NNRt 客户端注册/服务通道绑定
   application 进程上下文(app so、token), NCP 是纯 native 进程无能力集。
   **候选解法(待选):**
   - A. NNRt 是否提供"初始化"API(设备库导出只有上述; 未见); 或 OpenHarmony NNRt 源码
     服务端注册规则可按 uid+appId 匹配 —— 需要读 openharmony NNRt 源码确认;
   - B. 换形态: 推理在 **UIAbility 进程**(NAPI 接入 torch + NPU 后端), 子进程仅做
     HTTP/API 服务。架构收益: NNRt 可用化; 代价: Python/torch 已在子进程(HAP内 pyroot),
     搬父进程 = 大改。
   - C. 中间: 子进程获得设备后(**尝试在子进程构造时携带 app 上下文模拟?** 不可行,
     NCP 无 App 环境)。

## 3. 完成的工具侧全链(可复现命令)

```bash
# 1. DDK 5.0.2(公开直链, 已验证 CDN 可拉)
curl -L -H "User-Agent: Mozilla/5.0" -H "Referer: https://developer.huawei.com/" \
  -o DDK_tools_5.0.2.0.zip "https://contentcenter-vali-drcn.dbankcdn.cn/pvt_2/DeveloperAlliance_package_901_9/24/v3/-oSN_kh6Tba4GDB0EMlkMg/DDK_tools_5.0.2.0.zip?HW-CC-KV=V1&HW-CC-Date=20241226T031910Z&HW-CC-Expire=315360000&HW-CC-Sign=B06D348B1E3B988F9259B7EAEAFF56E3618890A10EC4FF2C2DF4A69139E51EA3"
# 2. 解压+install
unzip -q DDK_tools_5.0.2.0.zip -d ddk-root
cd ddk-root/tools/tools_ascendc && chmod +x install.sh && source ./install.sh
source set_ascendc_env.sh   # 每次 shell 需要
# 3. 样例
git clone --depth 1 https://gitee.com/harmonyos_samples/cannkit_samplecode_add_custom_cpp.git
msopgen gen -i AddCustom.json -c ai_core-kirin9020 -f ONNX -out ./AddCustom-gen
#   （生成后需两处 gcc15 兼容修复, 见 §4）
bash ./build.sh   # -> Build and install success
# 4. 真机可运行探针(进 HAP, 见 §5)
```

## 4. gcc15 × 2024 工具链的两个兼容修复(归档)

1. 生成工程 `op_host/add_custom_tiling.h` 顶部补 `#include <cstdint>`(uint32_t 未声明);
2. 生成工程 CMakeLists `add_compile_options(-include cstdint)`(DDK 自带头
   graph/buffer.h 的 std::uint8_t 未声明)。
(均为构建期构造问题, 后续拿到新 DDK 时复验。)

## 5. 已入库的诊断探针(保留, 默认关闭)

- `entry/src/main/cpp/child/npu_probe.cpp` — 子进程 NNRt 探针(entryParams 含
  `npu-probe` 才执行; 含 6×5s 重试, 平时禁用);
- `enter` NAPI `npuEnumerate`(napi_init.cpp) + `Index.ets` startApp 内 ArkTS
  对照探针(MindSporeLite Kit 设备列表)。均在用户点「启动」后才执行, 零自动启动不受影响。

## 6. 下一步决策点(用户拍板)

| 选项 | 内容 | 条件 |
|---|---|---|
| **P0'** | 拿到 **kirin9020 平台插件**(用户手工从官网下载, 已公开直链通道下载 5.0.2 成功+插件名/SHA 已知)→ 补最后缺口 → 出 AddCustom .omc → **UIAbility 进程内** NAPI 验证(**判据达成路径唯一**) | 用户 1 次登录态下载 |
| P1a | ✅ **已完成(源码级判定, 见 §7)** | — |
| 挂起 | 结论归档, 等生态 | — |

## 7. P1a 判定:NCP 子进程为什么"无设备"(源码级, 2026-09-06)

OpenHarmony `ai_neural_network_runtime` 源码(`frameworks/native/.../register_hdi_device_v2_1.cpp`,
`backend_registrar.h`):

- 设备后端注册 = `REGISTER_BACKEND(HDIDeviceV2_1, ...)` 静态构造器;执行时回调
  `HDIDeviceV2_1Creator()` → `V2_1::INnrtDevice::Get()`(**HDF IPC 客户端**)→ 拿不到服务对象
  即不注册 → `BackendManager::GetAllBackendsID()` 恒 0;
- `GetAllDevicesID` 本身是**进程内查表**(无 IPC), 父进程 1 设备/子进程 0 设备的全部差异
  在 **HDI proxy 获取资格**(系统服务按进程身份/上下文授予);
- **判定:NCP 原生子进程(无 Ability 上下文)拿不到该 HDF 设备对象, 代码层无绕过**;
  官方上板场景(Optimize Codelab)本身也在 **App(UIAbility)进程**内 —— 与产品"子进程
  推理"形态的鸿沟是**系统行为**, 不是工具/工程障碍。

**推论(与 docs/torch-backend-cann.md §3 的关系)**:若 NPU 加速成行, torch 后端必须挂
**UIAbility 进程**(通过 NAPI), NCP 子进程只做 HTTP/IO 服务;或维持纯 CPU。

## 7. 归档链接

- torch 后端方案: docs/torch-backend-cann.md
- 探针源码: entry/src/main/cpp/child/npu_probe.cpp; napi_init.cpp NpuEnumerate;
  Index.ets 探针块(注释标记 P0 临时)
