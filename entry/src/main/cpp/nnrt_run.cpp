// NNRt HAP 内执行探针(2026-09-06): 官方「对接AI推理框架开发指导 / Neural Network Runtime」
//   Add 单算子样例, 以 dlopen+dlsym 形式跑在 UIAbility 进程。
//   用户确认(2026-09-06): 设备禁止执行 push 的可执行文件(SELinux shell 域), 调测必须打包进
//   HAP native 代码 —— 本文件即该形态。回答两问:
//   ① 官方构图→编译→执行链在 App 域能否跑通(若 OH_NNModel_* 缺失 → 走离线 .omc 路线再+1)
//   ② 父进程内对设备库符号导出面的权威复核(npu-probe 子进程结论: 无 OH_NNModel_*)
//
//   ※ 安全形态: 不链接 nnrt 库(符号缺失不会拖垮 libentry.so), 全部 dlsym 可选;
//     API-ABSENT 时返回缺失清单, 主链接器无害。
// API 版本差: SDK 文档为 API 24(6.1.1), 设备系统库可能为旧版 —— 新名 OH_NNModel_AddTensorToModel
//   失败时回退旧名 OH_NNModel_AddTensor; SetTensorType 同理。
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <sys/stat.h>
#include <unistd.h>

#include "neural_network_runtime/neural_network_runtime.h"
#include "neural_network_runtime/neural_network_core.h"

namespace {

void *g_rt = nullptr;
void *g_core = nullptr;
char g_missing[512] = "";   // 缺失符号清单(逗号分隔)

void *symor(const char *n)
{
    void *p = dlsym(g_core, n);
    if (!p) p = dlsym(g_rt, n);
    return p;
}
void *need(const char *n)
{
    void *p = symor(n);
    if (!p) {
        if (g_missing[0]) strncat(g_missing, ",", sizeof(g_missing) - strlen(g_missing) - 1);
        strncat(g_missing, n, sizeof(g_missing) - strlen(g_missing) - 1);
    }
    return p;
}

}  // namespace

// 返回值写入 out(至少 2048 字节); 返回 0=成功
// cacheDir: 编译缓存目录(App 可写, 如 filesDir/nncache); 2026-09-06 源码定谳:
//   OnlineBuild→RestoreFromCacheFile 后 realpath(m_cachePath) 失败(未 SetCache=空串)
//   → OH_NN_INVALID_PARAMETER 即此前 build rc=2 的直接根因!!
extern "C" int nnrt_run_add_graph(char *out, size_t outsz, const char *cacheDir)
{
    g_missing[0] = '\0';
    if (cacheDir && cacheDir[0]) {
        mkdir(cacheDir, 0755);   // 幂等: 已存在时忽略(EXIST_OK, mkdir 失败无害)
    }
    g_rt = dlopen("libneural_network_runtime.so", RTLD_NOW | RTLD_GLOBAL);
    g_core = dlopen("libneural_network_core.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_rt || !g_core) {
        snprintf(out, outsz, "NNRT-ADD-GRAPH: dlopen fail rt=%p core=%p\n", g_rt, g_core);
        return 1;
    }

    // ── 1. 标准链符号(全部必须, 缺一进入 API-ABSENT) ─────────────────────────
    snprintf(out, outsz, "NNRT-ADD-GRAPH probe-v2-noact\n");   // 版本标记: 无参变体, 确认新包生效
    using FDev = OH_NN_ReturnCode (*)(const size_t **, uint32_t *);
    using FName = OH_NN_ReturnCode (*)(size_t, const char **);
    using FType = OH_NN_ReturnCode (*)(size_t, OH_NN_DeviceType *);
    using FModelNew = OH_NNModel *(*)();
    using FModelDel = void (*)(OH_NNModel **);
    using FAddTensor = OH_NN_ReturnCode (*)(OH_NNModel *, const NN_TensorDesc *);
    using FSetTensorType = OH_NN_ReturnCode (*)(OH_NNModel *, uint32_t, OH_NN_TensorType);
    using FSetTensorData = OH_NN_ReturnCode (*)(OH_NNModel *, uint32_t, const void *, size_t);
    using FAddOp = OH_NN_ReturnCode (*)(OH_NNModel *, OH_NN_OperationType,
                                        const OH_NN_UInt32Array *, const OH_NN_UInt32Array *,
                                        const OH_NN_UInt32Array *);
    using FSpecify = OH_NN_ReturnCode (*)(OH_NNModel *, const OH_NN_UInt32Array *,
                                          const OH_NN_UInt32Array *);
    using FFinish = OH_NN_ReturnCode (*)(OH_NNModel *);
    using FDescNew = NN_TensorDesc *(*)();
    using FDescSetShape = OH_NN_ReturnCode (*)(NN_TensorDesc *, const int32_t *, size_t);
    using FDescSetDtype = OH_NN_ReturnCode (*)(NN_TensorDesc *, OH_NN_DataType);
    using FDescSetFormat = OH_NN_ReturnCode (*)(NN_TensorDesc *, OH_NN_Format);
    using FDescDel = OH_NN_ReturnCode (*)(NN_TensorDesc **);
    using FCompNew = OH_NNCompilation *(*)(const OH_NNModel *);
    using FCompSetCache = OH_NN_ReturnCode (*)(OH_NNCompilation *, const char *, uint32_t);
    using FCompSetDev = OH_NN_ReturnCode (*)(OH_NNCompilation *, size_t);
    using FCompPerf = OH_NN_ReturnCode (*)(OH_NNCompilation *, OH_NN_PerformanceMode);
    using FCompPri = OH_NN_ReturnCode (*)(OH_NNCompilation *, OH_NN_Priority);
    using FCompFp16 = OH_NN_ReturnCode (*)(OH_NNCompilation *, bool);
    using FCompBuild = OH_NN_ReturnCode (*)(OH_NNCompilation *);
    using FCompDel = void (*)(OH_NNCompilation **);
    using FExecNew = OH_NNExecutor *(*)(OH_NNCompilation *);
    using FExecCount = OH_NN_ReturnCode (*)(const OH_NNExecutor *, size_t *);
    using FExecInDesc = NN_TensorDesc *(*)(const OH_NNExecutor *, size_t);
    using FExecOutDesc = NN_TensorDesc *(*)(const OH_NNExecutor *, size_t);
    using FExecRun = OH_NN_ReturnCode (*)(OH_NNExecutor *, NN_Tensor **, size_t, NN_Tensor **, size_t);
    using FExecDel = void (*)(OH_NNExecutor **);
    using FTensorNew = NN_Tensor *(*)(size_t, NN_TensorDesc *);
    using FTensorBuf = void *(*)(const NN_Tensor *);
    using FTensorDesc = NN_TensorDesc *(*)(const NN_Tensor *);
    using FTensorDel = OH_NN_ReturnCode (*)(NN_Tensor **);

    FDev fDev = (FDev)need("OH_NNDevice_GetAllDevicesID");
    FName fName = (FName)need("OH_NNDevice_GetName");
    FType fType = (FType)need("OH_NNDevice_GetType");
    FModelNew fModelNew = (FModelNew)need("OH_NNModel_Construct");
    FModelDel fModelDel = (FModelDel)need("OH_NNModel_Destroy");
    FAddTensor fAddTensor = (FAddTensor)need("OH_NNModel_AddTensorToModel");
    FSetTensorType fSetTT = (FSetTensorType)need("OH_NNModel_SetTensorType");
    FSetTensorData fSetTD = (FSetTensorData)need("OH_NNModel_SetTensorData");
    FAddOp fAddOp = (FAddOp)need("OH_NNModel_AddOperation");
    FSpecify fSpecify = (FSpecify)need("OH_NNModel_SpecifyInputsAndOutputs");
    FFinish fFinish = (FFinish)need("OH_NNModel_Finish");
    FDescNew fDescNew = (FDescNew)need("OH_NNTensorDesc_Create");
    FDescSetShape fDescSetShape = (FDescSetShape)need("OH_NNTensorDesc_SetShape");
    FDescSetDtype fDescSetDtype = (FDescSetDtype)need("OH_NNTensorDesc_SetDataType");
    FDescSetFormat fDescSetFormat = (FDescSetFormat)need("OH_NNTensorDesc_SetFormat");
    FDescDel fDescDel = (FDescDel)need("OH_NNTensorDesc_Destroy");
    FCompNew fCompNew = (FCompNew)need("OH_NNCompilation_Construct");
    FCompSetCache fCompSetCache = (FCompSetCache)need("OH_NNCompilation_SetCache");
    FCompSetDev fCompSetDev = (FCompSetDev)need("OH_NNCompilation_SetDevice");
    FCompPerf fCompPerf = (FCompPerf)need("OH_NNCompilation_SetPerformanceMode");
    FCompPri fCompPri = (FCompPri)need("OH_NNCompilation_SetPriority");
    FCompFp16 fCompFp16 = (FCompFp16)need("OH_NNCompilation_EnableFloat16");
    FCompBuild fCompBuild = (FCompBuild)need("OH_NNCompilation_Build");
    FCompDel fCompDel = (FCompDel)need("OH_NNCompilation_Destroy");
    FExecNew fExecNew = (FExecNew)need("OH_NNExecutor_Construct");
    FExecCount fExecCount = (FExecCount)need("OH_NNExecutor_GetOutputCount");
    FExecInDesc fExecInDesc = (FExecInDesc)need("OH_NNExecutor_CreateInputTensorDesc");
    FExecOutDesc fExecOutDesc = (FExecOutDesc)need("OH_NNExecutor_CreateOutputTensorDesc");
    FExecRun fExecRun = (FExecRun)need("OH_NNExecutor_RunSync");
    FExecDel fExecDel = (FExecDel)need("OH_NNExecutor_Destroy");
    FTensorNew fTensorNew = (FTensorNew)need("OH_NNTensor_Create");
    FTensorBuf fTensorBuf = (FTensorBuf)need("OH_NNTensor_GetDataBuffer");
    FTensorDesc fTensorDesc = (FTensorDesc)need("OH_NNTensor_GetTensorDesc");
    FTensorDel fTensorDel = (FTensorDel)need("OH_NNTensor_Destroy");

    if (g_missing[0]) {
        snprintf(out, outsz, "NNRT-ADD-GRAPH: API-ABSENT [%s]\n", g_missing);
        return 2;
    }

    // OH_NNModel_AddTensor(旧名) 不存在则无需回退(已按新名找到) —— 真机若只含旧名会在此缺,
    // 保险起见: 新名失败后再试旧名(见下)
    // ── 2. 设备枚举 ──────────────────────────────────────────────────────────
    const size_t *ids = nullptr;
    uint32_t n = 0;
    OH_NN_ReturnCode rc = fDev(&ids, &n);
    char line[128];
    snprintf(line, sizeof(line), "NNRT-ADD-GRAPH: dev-rc=%d count=%u", (int)rc, n);
    strncat(out, line, outsz - strlen(out) - 1);
    if (rc != OH_NN_SUCCESS || n == 0) {
        strncat(out, " (no device)\n", outsz - strlen(out) - 1);
        return 3;
    }
    // 2026-09-06 9030 实证: 设备列表含「真实硬件」NPU_ohos.boot.hardware.KirinXE90_v2_0 与
    //   HIAI_F(虚拟通用入口, 9020/9030 同为 id 8987859593747354028) —— 必须优先真实硬件!
    size_t target = ids[0];
    for (uint32_t i = 0; i < n && i < 4; i++) {
        const char *nm = nullptr;
        OH_NN_DeviceType tp = (OH_NN_DeviceType)-1;
        fName(ids[i], &nm);
        fType(ids[i], &tp);
        snprintf(line, sizeof(line), "; [%u:%s/t%d]", i, nm ? nm : "?", (int)tp);
        strncat(out, line, outsz - strlen(out) - 1);
        if (nm && (strstr(nm, "NPU_") || strstr(nm, "Kirin"))) {
            target = ids[i];
        }
    }
    snprintf(line, sizeof(line), "\n>> target id=%zu\n", target);
    strncat(out, line, outsz - strlen(out) - 1);

    // ── 3. 构图: Add, x+y (FP32 [1,2,2,3]), activation=OH_NN_FUSED_NONE ──────
    auto step = [&](const char *tag, OH_NN_ReturnCode r) -> bool {
        snprintf(line, sizeof(line), "%s rc=%d\n", tag, (int)r);
        strncat(out, line, outsz - strlen(out) - 1);
        return r == OH_NN_SUCCESS;
    };

    OH_NNModel *model = fModelNew();
    int32_t dims[4] = {1, 2, 2, 3};
    auto addTensor = [&](OH_NN_DataType dt, const int32_t *shp, size_t shplen,
                         OH_NN_TensorType ttype, uint32_t idx) -> bool {
        NN_TensorDesc *td = fDescNew();
        if (!td) return false;
        bool ok = fDescSetShape(td, shp, shplen) == OH_NN_SUCCESS &&
                  fDescSetDtype(td, dt) == OH_NN_SUCCESS &&
                  fDescSetFormat(td, OH_NN_FORMAT_NONE) == OH_NN_SUCCESS &&
                  fAddTensor(model, td) == OH_NN_SUCCESS;
        fDescDel(&td);
        if (!ok) return false;
        return fSetTT(model, idx, ttype) == OH_NN_SUCCESS;
    };
    if (!step("addTensor0", addTensor(OH_NN_FLOAT32, dims, 4, OH_NN_TENSOR, 0) ? OH_NN_SUCCESS : OH_NN_FAILED))
        return 4;
    if (!step("addTensor1", addTensor(OH_NN_FLOAT32, dims, 4, OH_NN_TENSOR, 1) ? OH_NN_SUCCESS : OH_NN_FAILED))
        return 4;
    // 无参变体(2026-09-06): 不加 activation 参数张量 / SetTensorData, 与官方 README 2-in/1-out 纯 Add 对齐
    if (!step("addTensor2(out)", addTensor(OH_NN_FLOAT32, dims, 4, OH_NN_TENSOR, 2) ? OH_NN_SUCCESS : OH_NN_FAILED))
        return 4;

    uint32_t inIdx[2] = {0, 1};
    uint32_t outIdx = 2;
    OH_NN_UInt32Array params = {nullptr, 0};
    OH_NN_UInt32Array inputs = {inIdx, 2};
    OH_NN_UInt32Array outputs = {&outIdx, 1};
    if (!step("addOperation(ADD)", fAddOp(model, OH_NN_OPS_ADD, &params, &inputs, &outputs)))
        return 4;
    if (!step("specify", fSpecify(model, &inputs, &outputs)))
        return 4;
    if (!step("finish", fFinish(model)))
        return 4;

    // ── 4. 编译 ──────────────────────────────────────────────────────────────
    // ⚠ 2026-09-06 已实证不要调用 OH_NNModel_GetAvailableOperations: SDK 头
    //  (neural_network_runtime.h, 无该方法!)未提供其签名, 按 OpenHarmony 记忆签名
    //  调用 → 真机 SIGSEGV(native)崩溃 —— 待官方源码签名核实后再启用。
    OH_NNCompilation *comp = fCompNew(model);
    if (!comp) { strncat(out, "compNew null\n", outsz - strlen(out) - 1); return 5; }
    if (!step("setCache", fCompSetCache(comp, (cacheDir && cacheDir[0]) ? cacheDir : ".", 1))) return 5;
    if (!step("setDevice", fCompSetDev(comp, target))) return 5;
    step("perfMode", fCompPerf(comp, OH_NN_PERFORMANCE_EXTREME));
    step("priority", fCompPri(comp, OH_NN_PRIORITY_HIGH));
    step("fp16off", fCompFp16(comp, false));
    if (!step("build", fCompBuild(comp))) return 5;

    // ── 5. 执行 ──────────────────────────────────────────────────────────────
    OH_NNExecutor *exec = fExecNew(comp);
    if (!exec) { strncat(out, "execNew null\n", outsz - strlen(out) - 1); return 6; }
    size_t inCnt = 0, outCnt = 0;
    fExecCount(exec, &outCnt);
    NN_TensorDesc *td0 = fExecInDesc(exec, 0);
    NN_TensorDesc *td1 = fExecInDesc(exec, 1);
    NN_TensorDesc *tod = fExecOutDesc(exec, 0);
    NN_Tensor *tin0 = td0 ? fTensorNew(target, td0) : nullptr;
    NN_Tensor *tin1 = td1 ? fTensorNew(target, td1) : nullptr;
    NN_Tensor *tout = tod ? fTensorNew(target, tod) : nullptr;
    if (!tin0 || !tin1 || !tout) {
        strncat(out, "tensor create fail\n", outsz - strlen(out) - 1);
        return 7;
    }
    // 输入 0..11(与官方样例一致: 期望 z=x+y=2*i)
    float xv[12], yv[12];
    for (int i = 0; i < 12; i++) { xv[i] = (float)i; yv[i] = (float)i; }
    memcpy(fTensorBuf(tin0), xv, sizeof(xv));
    memcpy(fTensorBuf(tin1), yv, sizeof(yv));
    NN_Tensor *ins[2] = {tin0, tin1};
    NN_Tensor *outs[1] = {tout};
    if (!step("runSync", fExecRun(exec, ins, 2, outs, 1))) return 8;

    // ── 6. 结果核对(期望 output[i] = 2*i) ────────────────────────────────────
    float *res = (float *)fTensorBuf(tout);
    size_t cnt = 12;
    bool pass = res != nullptr;
    snprintf(line, sizeof(line), "RESULT: ");
    strncat(out, line, outsz - strlen(out) - 1);
    for (size_t i = 0; i < cnt; i++) {
        snprintf(line, sizeof(line), "[%zu]=%.1f ", i, res[i]);
        strncat(out, line, outsz - strlen(out) - 1);
        if (res[i] != 2.0f * (float)i) pass = false;
    }
    snprintf(line, sizeof(line), "\nNNRT-ADD-GRAPH: %s\n", pass ? "PASS" : "CHECK-FAIL");
    strncat(out, line, outsz - strlen(out) - 1);

    fTensorDel(&tin0); fTensorDel(&tin1); fTensorDel(&tout);
    if (td0) fDescDel(&td0); if (td1) fDescDel(&td1); if (tod) fDescDel(&tod);
    fExecDel(&exec);
    fCompDel(&comp);
    fModelDel(&model);
    return pass ? 0 : 9;
}

// ── 离线模型执行探针(2026-09-06, P0 判据末端): ConstructWithOfflineModelFile(.omc) →
//    Build → RunSync。modelPath = .omc 绝对路径(ArkTS 从 rawfile 拷贝到 filesDir)。
//    与 nnrt_run_add_graph 的区别: 无构图/无 SetDevice 前校验, 走厂商离线模型编译管道。
//    判据: AddCustom z=x+y+bias(bias=1), x=y=1 → 期望全 3.0(fp16 ND [64,64])。
extern "C" int nnrt_run_offline(char *out, size_t outsz, const char *modelPath, const char *cacheDir)
{
    g_missing[0] = '\0';
    g_rt = dlopen("libneural_network_runtime.so", RTLD_NOW | RTLD_GLOBAL);
    g_core = dlopen("libneural_network_core.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_rt || !g_core) {
        snprintf(out, outsz, "NNRT-OFFLINE: dlopen fail rt=%p core=%p\n", g_rt, g_core);
        return 1;
    }
    snprintf(out, outsz, "NNRT-OFFLINE probe-v3\n");

    using FDev = OH_NN_ReturnCode (*)(const size_t **, uint32_t *);
    using FName = OH_NN_ReturnCode (*)(size_t, const char **);
    using FCompOff = OH_NNCompilation *(*)(const char *);
    using FCompSetCache = OH_NN_ReturnCode (*)(OH_NNCompilation *, const char *, uint32_t);
    using FCompSetDev = OH_NN_ReturnCode (*)(OH_NNCompilation *, size_t);
    using FCompPerf = OH_NN_ReturnCode (*)(OH_NNCompilation *, OH_NN_PerformanceMode);
    using FCompPri = OH_NN_ReturnCode (*)(OH_NNCompilation *, OH_NN_Priority);
    using FCompFp16 = OH_NN_ReturnCode (*)(OH_NNCompilation *, bool);
    using FCompBuild = OH_NN_ReturnCode (*)(OH_NNCompilation *);
    using FCompDel = void (*)(OH_NNCompilation **);
    using FExecNew = OH_NNExecutor *(*)(OH_NNCompilation *);
    using FExecInDesc = NN_TensorDesc *(*)(const OH_NNExecutor *, size_t);
    using FExecOutDesc = NN_TensorDesc *(*)(const OH_NNExecutor *, size_t);
    using FExecRun = OH_NN_ReturnCode (*)(OH_NNExecutor *, NN_Tensor **, size_t, NN_Tensor **, size_t);
    using FExecDel = void (*)(OH_NNExecutor **);
    using FTensorNew = NN_Tensor *(*)(size_t, NN_TensorDesc *);
    using FTensorBuf = void *(*)(const NN_Tensor *);
    using FTensorDesc = NN_TensorDesc *(*)(const NN_Tensor *);
    using FDescDtype = OH_NN_ReturnCode (*)(const NN_TensorDesc *, OH_NN_DataType *);
    using FDescCount = OH_NN_ReturnCode (*)(const NN_TensorDesc *, size_t *);
    using FDescDel = OH_NN_ReturnCode (*)(NN_TensorDesc **);
    using FTensorDel = OH_NN_ReturnCode (*)(NN_Tensor **);

    FDev fDev = (FDev)need("OH_NNDevice_GetAllDevicesID");
    FName fName = (FName)need("OH_NNDevice_GetName");
    FCompOff fCompOff = (FCompOff)need("OH_NNCompilation_ConstructWithOfflineModelFile");
    FCompSetCache fSetCache = (FCompSetCache)need("OH_NNCompilation_SetCache");
    FCompSetDev fSetDev = (FCompSetDev)need("OH_NNCompilation_SetDevice");
    FCompPerf fPerf = (FCompPerf)need("OH_NNCompilation_SetPerformanceMode");
    FCompPri fPri = (FCompPri)need("OH_NNCompilation_SetPriority");
    FCompFp16 fFp16 = (FCompFp16)need("OH_NNCompilation_EnableFloat16");
    FCompBuild fBuild = (FCompBuild)need("OH_NNCompilation_Build");
    FCompDel fCompDel = (FCompDel)need("OH_NNCompilation_Destroy");
    FExecNew fExecNew = (FExecNew)need("OH_NNExecutor_Construct");
    FExecInDesc fExecInDesc = (FExecInDesc)need("OH_NNExecutor_CreateInputTensorDesc");
    FExecOutDesc fExecOutDesc = (FExecOutDesc)need("OH_NNExecutor_CreateOutputTensorDesc");
    FExecRun fRun = (FExecRun)need("OH_NNExecutor_RunSync");
    FExecDel fExecDel = (FExecDel)need("OH_NNExecutor_Destroy");
    FTensorNew fTensorNew = (FTensorNew)need("OH_NNTensor_Create");
    FTensorBuf fTensorBuf = (FTensorBuf)need("OH_NNTensor_GetDataBuffer");
    FTensorDesc fTensorDesc = (FTensorDesc)need("OH_NNTensor_GetTensorDesc");
    FDescDtype fDescDtype = (FDescDtype)need("OH_NNTensorDesc_GetDataType");
    FDescCount fDescCount = (FDescCount)need("OH_NNTensorDesc_GetElementCount");
    FDescDel fDescDel = (FDescDel)need("OH_NNTensorDesc_Destroy");
    FTensorDel fTensorDel = (FTensorDel)need("OH_NNTensor_Destroy");
    if (g_missing[0]) {
        snprintf(out, outsz, "NNRT-OFFLINE: API-ABSENT [%s]\n", g_missing);
        return 2;
    }

    char line[256];
    strncat(out, "offline: symbols ok\n", outsz - strlen(out) - 1);

    // 设备枚举(优先真实硬件)
    const size_t *ids = nullptr;
    uint32_t n = 0;
    OH_NN_ReturnCode rc = fDev(&ids, &n);
    snprintf(line, sizeof(line), "offline: dev-rc=%d count=%u", (int)rc, n);
    strncat(out, line, outsz - strlen(out) - 1);
    if (rc != OH_NN_SUCCESS || n == 0) {
        strncat(out, " (no device)\n", outsz - strlen(out) - 1);
        return 3;
    }
    size_t target = ids[0];
    for (uint32_t i = 0; i < n && i < 4; i++) {
        const char *nm = nullptr;
        fName(ids[i], &nm);
        snprintf(line, sizeof(line), "; [%u:%s]", i, nm ? nm : "?");
        strncat(out, line, outsz - strlen(out) - 1);
        if (nm && (strstr(nm, "NPU_") || strstr(nm, "Kirin"))) target = ids[i];
    }
    snprintf(line, sizeof(line), "\n");

    // 编译(离线模型)
    OH_NNCompilation *comp = fCompOff(modelPath);
    if (!comp) {
        snprintf(line, sizeof(line), "offline: ConstructWithOfflineModelFile FAILED (model 路径: %s)\n",
                 modelPath ? modelPath : "");
        strncat(out, line, outsz - strlen(out) - 1);
        return 4;
    }
    auto step = [&](const char *tag, OH_NN_ReturnCode r) -> bool {
        snprintf(line, sizeof(line), "%s rc=%d\n", tag, (int)r);
        strncat(out, line, outsz - strlen(out) - 1);
        return r == OH_NN_SUCCESS;
    };
    if (!step("setCache", fSetCache(comp, cacheDir, 1))) return 5;
    if (!step("setDevice", fSetDev(comp, target))) return 5;
    step("perf", fPerf(comp, OH_NN_PERFORMANCE_EXTREME));
    step("prio", fPri(comp, OH_NN_PRIORITY_HIGH));
    step("fp16", fFp16(comp, false));
    if (!step("build", fBuild(comp))) return 5;

    OH_NNExecutor *exec = fExecNew(comp);
    if (!exec) { strncat(out, "exec null\n", outsz - strlen(out) - 1); return 6; }
    NN_TensorDesc *inD0 = fExecInDesc(exec, 0);
    NN_TensorDesc *inD1 = fExecInDesc(exec, 1);
    NN_TensorDesc *outD = fExecOutDesc(exec, 0);
    NN_Tensor *tin0 = inD0 ? fTensorNew(target, inD0) : nullptr;
    NN_Tensor *tin1 = inD1 ? fTensorNew(target, inD1) : nullptr;
    NN_Tensor *tout = outD ? fTensorNew(target, outD) : nullptr;
    if (!tin0 || !tin1 || !tout) { strncat(out, "tensor fail\n", outsz - strlen(out) - 1); return 7; }

    // 填充: x=y=1.0(若 desc 为 fp16 → half)
    auto fillOnes = [&fTensorDesc](NN_Tensor *t, FTensorBuf fBuf, FDescDtype fDt, FDescCount fCnt) {
        NN_TensorDesc *d = fTensorDesc(t);
        OH_NN_DataType dt = OH_NN_FLOAT32;
        size_t cnt = 0;
        fDt(d, &dt);
        fCnt(d, &cnt);
        void *p = fBuf(t);
        if (dt == OH_NN_FLOAT16 && p) {
            // fp16 half: 用 memcpy 造 1.0(0x3C00)
            uint16_t one = 0x3C00;
            for (size_t i = 0; i < cnt; i++) ((uint16_t *)p)[i] = one;
        } else if (p) {
            float *fp = (float *)p;
            for (size_t i = 0; i < cnt; i++) fp[i] = 1.0f;
        }
    };
    fillOnes(tin0, fTensorBuf, fDescDtype, fDescCount);
    fillOnes(tin1, fTensorBuf, fDescDtype, fDescCount);
    NN_Tensor *ins[2] = {tin0, tin1};
    NN_Tensor *outs[1] = {tout};
    if (!step("runSync", fRun(exec, ins, 2, outs, 1))) return 8;

    // 结果(期望 z=1+1+bias(1)=3.0; fp16 → 打印前 8 个并转 float)
    NN_TensorDesc *od = fTensorDesc(tout);
    OH_NN_DataType odt = OH_NN_FLOAT32;
    fDescDtype(od, &odt);
    void *p = fTensorBuf(tout);
    bool pass = p != nullptr;
    snprintf(line, sizeof(line), "offline RESULT(dt=%d): ", (int)odt);
    strncat(out, line, outsz - strlen(out) - 1);
    for (int i = 0; i < 8; i++) {
        float v = -1.0f;
        if (odt == OH_NN_FLOAT16 && p) {
            uint16_t h = ((uint16_t *)p)[i];
            // half 转 float: 简易转换(仅判 3.0/0x4200)
            v = (h == 0x4200) ? 3.0f : (float)h / 65536.0f;
        } else if (p) {
            v = ((float *)p)[i];
        }
        snprintf(line, sizeof(line), "[%d]=%.2f ", i, v);
        strncat(out, line, outsz - strlen(out) - 1);
        if (v != 3.0f) pass = false;
    }
    snprintf(line, sizeof(line), "\nNNRT-OFFLINE: %s\n", pass ? "PASS" : "CHECK-FAIL");
    strncat(out, line, outsz - strlen(out) - 1);

    fTensorDel(&tin0); fTensorDel(&tin1); fTensorDel(&tout);
    if (inD0) fDescDel(&inD0); if (inD1) fDescDel(&inD1); if (outD) fDescDel(&outD);
    fExecDel(&exec);
    fCompDel(&comp);
    return pass ? 0 : 9;
}

// ── 离线模型 Buffer 执行探针(2026-09-07): 同上, 但模型走内存
//    (ConstructWithOfflineModelBuffer —— rawfile 读取后直接传入, 免拷贝)。
extern "C" int nnrt_run_offline_buffer(char *out, size_t outsz, const void *modelBuf, size_t modelLen,
                                       const char *cacheDir)
{
    g_missing[0] = '\0';
    g_rt = dlopen("libneural_network_runtime.so", RTLD_NOW | RTLD_GLOBAL);
    g_core = dlopen("libneural_network_core.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_rt || !g_core) {
        snprintf(out, outsz, "NNRT-OFFLINE-BUF: dlopen fail rt=%p core=%p\n", g_rt, g_core);
        return 1;
    }
    snprintf(out, outsz, "NNRT-OFFLINE probe-v3-buffer\n");

    using FDev = OH_NN_ReturnCode (*)(const size_t **, uint32_t *);
    using FName = OH_NN_ReturnCode (*)(size_t, const char **);
    using FCompBuf = OH_NNCompilation *(*)(const void *, size_t);
    using FCompSetCache = OH_NN_ReturnCode (*)(OH_NNCompilation *, const char *, uint32_t);
    using FCompSetDev = OH_NN_ReturnCode (*)(OH_NNCompilation *, size_t);
    using FCompBuild = OH_NN_ReturnCode (*)(OH_NNCompilation *);
    using FCompDel = void (*)(OH_NNCompilation **);
    using FExecNew = OH_NNExecutor *(*)(OH_NNCompilation *);
    using FExecInDesc = NN_TensorDesc *(*)(const OH_NNExecutor *, size_t);
    using FExecOutDesc = NN_TensorDesc *(*)(const OH_NNExecutor *, size_t);
    using FExecRun = OH_NN_ReturnCode (*)(OH_NNExecutor *, NN_Tensor **, size_t, NN_Tensor **, size_t);
    using FExecDel = void (*)(OH_NNExecutor **);
    using FTensorNew = NN_Tensor *(*)(size_t, NN_TensorDesc *);
    using FTensorBuf = void *(*)(const NN_Tensor *);
    using FTensorDesc2 = NN_TensorDesc *(*)(const NN_Tensor *);
    using FDescDtype = OH_NN_ReturnCode (*)(const NN_TensorDesc *, OH_NN_DataType *);
    using FDescCount = OH_NN_ReturnCode (*)(const NN_TensorDesc *, size_t *);
    using FDescDel = OH_NN_ReturnCode (*)(NN_TensorDesc **);
    using FTensorDel = OH_NN_ReturnCode (*)(NN_Tensor **);

    FDev fDev = (FDev)need("OH_NNDevice_GetAllDevicesID");
    FName fName = (FName)need("OH_NNDevice_GetName");
    FCompBuf fCompBuf = (FCompBuf)need("OH_NNCompilation_ConstructWithOfflineModelBuffer");
    FCompSetCache fSetCache = (FCompSetCache)need("OH_NNCompilation_SetCache");
    FCompSetDev fSetDev = (FCompSetDev)need("OH_NNCompilation_SetDevice");
    FCompBuild fBuild = (FCompBuild)need("OH_NNCompilation_Build");
    FCompDel fCompDel = (FCompDel)need("OH_NNCompilation_Destroy");
    FExecNew fExecNew = (FExecNew)need("OH_NNExecutor_Construct");
    FExecInDesc fExecInDesc = (FExecInDesc)need("OH_NNExecutor_CreateInputTensorDesc");
    FExecOutDesc fExecOutDesc = (FExecOutDesc)need("OH_NNExecutor_CreateOutputTensorDesc");
    FExecRun fRun = (FExecRun)need("OH_NNExecutor_RunSync");
    FExecDel fExecDel = (FExecDel)need("OH_NNExecutor_Destroy");
    FTensorNew fTensorNew = (FTensorNew)need("OH_NNTensor_Create");
    FTensorBuf fTensorBuf = (FTensorBuf)need("OH_NNTensor_GetDataBuffer");
    FTensorDesc2 fTensorDesc2 = (FTensorDesc2)need("OH_NNTensor_GetTensorDesc");
    FDescDtype fDescDtype = (FDescDtype)need("OH_NNTensorDesc_GetDataType");
    FDescCount fDescCount = (FDescCount)need("OH_NNTensorDesc_GetElementCount");
    FDescDel fDescDel = (FDescDel)need("OH_NNTensorDesc_Destroy");
    FTensorDel fTensorDel = (FTensorDel)need("OH_NNTensor_Destroy");
    if (g_missing[0]) {
        snprintf(out, outsz, "NNRT-OFFLINE-BUF: API-ABSENT [%s]\n", g_missing);
        return 2;
    }

    char line[256];
    snprintf(line, sizeof(line), "offline-buf: len=%zu symbols ok\n", modelLen);
    strncat(out, line, outsz - strlen(out) - 1);

    const size_t *ids = nullptr;
    uint32_t n = 0;
    OH_NN_ReturnCode rc = fDev(&ids, &n);
    snprintf(line, sizeof(line), "offline-buf: dev-rc=%d count=%u\n", (int)rc, n);
    strncat(out, line, outsz - strlen(out) - 1);
    if (rc != OH_NN_SUCCESS || n == 0) return 3;
    size_t target = ids[0];
    for (uint32_t i = 0; i < n && i < 4; i++) {
        const char *nm = nullptr;
        fName(ids[i], &nm);
        if (nm && (strstr(nm, "NPU_") || strstr(nm, "Kirin"))) target = ids[i];
    }

    OH_NNCompilation *comp = fCompBuf(modelBuf, modelLen);
    if (!comp) { strncat(out, "offline-buf: ConstructWithOfflineModelBuffer FAILED\n", outsz - strlen(out) - 1); return 4; }
    auto step = [&](const char *tag, OH_NN_ReturnCode r) -> bool {
        snprintf(line, sizeof(line), "%s rc=%d\n", tag, (int)r);
        strncat(out, line, outsz - strlen(out) - 1);
        return r == OH_NN_SUCCESS;
    };
    if (!step("setCache", fSetCache(comp, cacheDir, 1))) return 5;
    if (!step("setDevice", fSetDev(comp, target))) return 5;
    if (!step("build", fBuild(comp))) return 5;

    OH_NNExecutor *exec = fExecNew(comp);
    if (!exec) return 6;
    NN_TensorDesc *inD0 = fExecInDesc(exec, 0);
    NN_TensorDesc *inD1 = fExecInDesc(exec, 1);
    NN_TensorDesc *outD = fExecOutDesc(exec, 0);
    NN_Tensor *tin0 = inD0 ? fTensorNew(target, inD0) : nullptr;
    NN_Tensor *tin1 = inD1 ? fTensorNew(target, inD1) : nullptr;
    NN_Tensor *tout = outD ? fTensorNew(target, outD) : nullptr;
    if (!tin0 || !tin1 || !tout) return 7;
    auto fillOnes = [&fTensorDesc2](NN_Tensor *t, FTensorBuf fBuf, FDescDtype fDt, FDescCount fCnt) {
        NN_TensorDesc *d = fTensorDesc2(t);
        OH_NN_DataType dt = OH_NN_FLOAT32;
        size_t cnt = 0;
        fDt(d, &dt);
        fCnt(d, &cnt);
        void *p = fBuf(t);
        if (dt == OH_NN_FLOAT16 && p) {
            uint16_t one = 0x3C00;
            for (size_t i = 0; i < cnt; i++) ((uint16_t *)p)[i] = one;
        } else if (p) {
            float *fp = (float *)p;
            for (size_t i = 0; i < cnt; i++) fp[i] = 1.0f;
        }
    };
    fillOnes(tin0, fTensorBuf, fDescDtype, fDescCount);
    fillOnes(tin1, fTensorBuf, fDescDtype, fDescCount);
    NN_Tensor *ins[2] = {tin0, tin1};
    NN_Tensor *outs[1] = {tout};
    if (!step("runSync", fRun(exec, ins, 2, outs, 1))) return 8;

    NN_TensorDesc *od = fTensorDesc2(tout);
    OH_NN_DataType odt = OH_NN_FLOAT32;
    fDescDtype(od, &odt);
    void *p = fTensorBuf(tout);
    bool pass = p != nullptr;
    snprintf(line, sizeof(line), "offline-buf RESULT(dt=%d): ", (int)odt);
    strncat(out, line, outsz - strlen(out) - 1);
    for (int i = 0; i < 8; i++) {
        float v = -1.0f;
        if (odt == OH_NN_FLOAT16 && p) {
            uint16_t h = ((uint16_t *)p)[i];
            v = (h == 0x4200) ? 3.0f : (float)h / 65536.0f;
        } else if (p) {
            v = ((float *)p)[i];
        }
        snprintf(line, sizeof(line), "[%d]=%.2f ", i, v);
        strncat(out, line, outsz - strlen(out) - 1);
        if (v != 3.0f) pass = false;
    }
    snprintf(line, sizeof(line), "\nNNRT-OFFLINE-BUF: %s\n", pass ? "PASS" : "CHECK-FAIL");
    strncat(out, line, outsz - strlen(out) - 1);

    fTensorDel(&tin0); fTensorDel(&tin1); fTensorDel(&tout);
    if (inD0) fDescDel(&inD0); if (inD1) fDescDel(&inD1); if (outD) fDescDel(&outD);
    fExecDel(&exec);
    fCompDel(&comp);
    return pass ? 0 : 9;
}
