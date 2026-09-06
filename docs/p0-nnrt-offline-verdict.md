# P0 定谳报告:offline Build rc=1 = 系统库显式拒绝离线模型(2026-09-07)

> 本文档归档 2026-09-07「回归 HAP 探针路径,定谳 Build rc=1」调研全过程。
> 结论先行:**设备系统 NNRt 库为 OpenHarmony 开源版能力面,显式拒绝离线模型入口,
> App 进程内经 NNRt 跑自定义算子(AddCustom .omc)在当前系统不可达。**
> 对应 probe 阶段任务 #71 的当前状态 = 系统级阻塞(非代码问题)。

## 0. 一句话

`OH_NNCompilation_ConstructWithOfflineModel*` 只存指针(Build 前全部 rc=0),
`OH_NNCompilation_Build` 时 **NNBackend::CreateCompiler 检测到 offline 模型输入 →
拒绝 → 返回 OH_NN_FAILED(rc=1)**,从未到达 HDI/设备服务端。

## 1. 证据链(全直接实证)

| # | 证据 | 获取方式 |
|---|---|---|
| 1 | App 实际 dlopen 的库:`/system/lib64/ndk/libneural_network_runtime.so`(1.5MB)+ `libneural_network_core.so`(107KB) | HAP 探针 `/proc/self/maps` 提取路径(dladdr 失效;shell 无权限读 /vendor /system) |
| 2 | rt 库内字符串逐字存在 `[NNBackend] CreateCompiler failed, only support build NN model and NN model cache.` | 库 dump 至 App filesDir → hdc recv → 本地 `strings -a`(dump 副本留存 `/tmp/devlib/libnnrt_{core,rt}.dump`) |
| 3 | 行为全吻合:Construct 非 null / setCache rc=0 / setDevice rc=0 / Build rc=1 / 无服务端(hiai/HDI)日志 / clean 缓存目录变体 rc 仍 1 | probe-v5 输出(见 §3) |
| 4 | 系统库符号含 `HDIDeviceV2_0/V2_1::PrepareOfflineModel(...)`(开源实现样),但 **NNBackend 层入口被拒** | strings + 与 nnrt-src 源码对照 |
| 5 | 在线构图(单算子 ADD)全链 PASS 不受影响(对照:Build 走同 CreateCompiler 但 nnModel 输入被允许) | 9030 真机 `npuRunAddGraph` 输出 0..22 全对 |

**对照源码**(`build/nnrt-src/frameworks/native/neural_network_runtime/nnbackend.cpp:107-137`):

```cpp
Compiler* NNBackend::CreateCompiler(Compilation* compilation)
{
    ...
    // 仅支持从 nnmodel 和 nnmodel-cache构建编译器
    if ((compilation->offlineModelPath != nullptr) ||
        ((compilation->offlineModelBuffer.first != nullptr) ||
         (compilation->offlineModelBuffer.second != static_cast<size_t>(0)))) {
        LOGE("[NNBackend] CreateCompiler failed, only support build NN model and NN model cache.");
        return nullptr;   // ← OH_NNCompilation_Build 链 (neural_network_core.cpp:795-799)
    }                     //    → "fail to create compiler" → return OH_NN_FAILED = 1
    ...
}
```

`Build()` 后置动作一概不发生:SetCompilationOptions / Authentication / PullUpDlliteService /
compiler->Build(NNCompiler::Build / IsOfflineModel / BuildOfflineModel / HDI PrepareOfflineModel)
**执行优先级均低于 CreateCompiler 拒绝** —— 与「无服务端日志」现象自洽。

## 2. 由此推翻/修正的旧认知

- 「Build rc=1 可能=服务端/热控/uid 白名单」→ **全部撤销**(此前已撤 uid 归因,本次从根上定因)。
- 「App 域可经离线模型跑 Custom」→ **系统级不可达**(API 文档面有接口,设备库无实现)。
- add_graph 探针写入的 `nncache/0.nncache`(26KB)+ cache_info 与 offline 无关
  (=在线构图缓存),clean-dir 对照实验已排除其干扰。

## 3. probe-v5 探针(重建配方,代码已按用户指示从项目恢复)

结论性输出摘录(设备 192.168.1.4:44959, 9030/MOR-M1):

```
NNRT-OFFLINE probe-v5-buffer
core: /system/lib64/ndk/libneural_network_core.so   [dump 107144B 成功]
rt:   /system/lib64/ndk/libneural_network_runtime.so [dump 1531864B 成功]
offline-buf: len=6405 symbols ok / dev-rc=0 count=2
setCache rc=0 / setDevice rc=0 / build rc=1
setCache2 rc=0 / setDevice2 rc=0 / build2-clean rc=1   ← 干净缓存对照,仍 rc=1
```

如未来需要重建该探针(本次调研结束后已从源码恢复):

1. `nnrt_run.cpp` `nnrt_run_offline_buffer()`:
   - 用 `/proc/self/maps` 扫描 `libneural_network_core.so` / `libneural_network_runtime.so`
     提取 ELF 路径 → fopen 整文件拷贝到 `<cacheDir>/libnnrt_<tag>.dump`(**注意 dladdr
     在该库上不返回 dli_fname,必须 maps 法**;mkdir cacheDir 幂等);
   - 主线 Build 失败后追加:同 buffer 新建 compilation → `SetCache(<cacheDir>/clean-cache,1)`
     → SetDevice → Build 记第二组 rc(隔离 nncache 旧缓存干扰)。
2. `Index.ets` aboutToAppear 探针链:rawfile 读 `custom_graph.omc`(6405B, IMOD 魔数,
   ir_model_compile 产物,由 601 链产出)→ `npuRunOfflineModelBuffer(buf, nnCacheDir)` →
   写 `filesDir/npu-offline.log`。file 版对照(`npuRunOfflineModel(omcPath,..)`)当时
   在 ArkTS 侧报 13900020(@ohos.fs 写文件路径问题,非 NNRt 问题,未深究)。

## 4. 「自定义算子开发」文档板块可行性总表(2026-09-07 调研)

| 板块 | 内容 | 结论 | 实证 |
|---|---|---|---|
| 环境准备 | 装 ascendc 工具链 | ✅ | 601 安装成功 |
| 快速入门 | msopgen gen→核函数→Host→build.sh→部署(全在本机) | ✅ **全部可行已走通** | gen9030 / Build and install success / platform 目录同构部署 |
| 算子实现 | 写内核+Tiling+原型注册 | ✅ 本机 | 编译成功(gcc15 两修: cstdint/-include) |
| 算子部署 | ir_model_compile → .omc | ✅ **已走通** | SaveCompiledModelToFile SUCCESS,6405B IMOD(601+kirin9030 链) |
| 调试调优-CPU孪生/Simulator | 本机跑+golen 比对 | 🟡 应可用(未复验,5.0.2 曾遇 gcc15 GET_TILING_DATA) | — |
| 调试调优-NPU 上板 | ascendebug kernel -b npu:本地编译 kernel_run_tool(aarch64 musl)→ hdc shell 推 /data/local/tmp 执行 | 🟡 工具内建(源码实证 LocalOperator 支持本机 hdc 直连;no device_info.ip_address 即不 SSH);**前提=设备允许 shell 执行 ELF,未实测** | 工具源码: package/npu_kernel_launch(x86-64 编排器) + package/kernel_run_tool(aarch64 ELF 设备侧) + remote_operator.py(Local/Remote 双模式) |
| **端侧运行(算子部署第 2 环)** | NNRt offline .omc | ❌ **系统库拒绝(本次定谳)** | 见 §1 |

## 5. 下一步候选(用户拍板)

1. 换/升级系统镜像(带商业版 NNRt 库的 ROM);
2. 官方调测工具路径(kernel_run_tool 上板,先做「设备能否跑 ELF」五分钟实测);
3. P0 判据重构:以「官方 Add 内置算子真机全链 PASS(09-06 已达成)」+「DDK 全链(编译→
   .omc)完成」为达成;Custom 真机执行标记为系统库能力面受限,待升级复测。

## 6. 调研期间目录改动与恢复说明

- `entry/src/main/cpp/nnrt_run.cpp`:本次新增 dumpLib(v4/v5)与 build2-clean 变体
  —— **已恢复**至调研前(probe-v3-buffer 形态)。
- `entry/src/main/ets/pages/Index.ets`:新增 `npuRunOfflineModel` import 与 file 版调用块
  —— **已恢复**(保留 buf 版探针,与调研前一致)。
- 设备(192.168.1.4)上运行的是 probe-v5 构建的 HAP;目录恢复后需重新 `make hap` 才会
  回到 v3 形态(下次真机验证前顺手重编即可,不影响归档结论)。
- memory(comfyui-p0-nnrt-progress.md / MEMORY.md)已随定谳更新。
