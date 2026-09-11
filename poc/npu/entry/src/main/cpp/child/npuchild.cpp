// npuchild.cpp — poc/npu CH-06: 官方 NCP native 子进程内的 NNRt 全链验证。
//
// 回答:「原生子进程(无 Ability 上下文)能否使用 NNRt?」—— 分两级, 缺一不可:
//   ① 可见   OH_NNDevice_GetAllDevicesID     (2026-09-07 起 9030 = count=1 带真实硬件名)
//   ② 可执行 ADD 构图 → SetCache/SetDevice → Build → RunSync
//            (2026-09-11 补: 此前 CH-06 只验了①, README 里「子进程形态无需搬家」属推断 ——
//             本版把②补上, 结论以执行链结果为准)
//
// 背景: 主项目 comfyui NCP 子进程(与主项目推理子进程同一形态)曾在 9020 上 count=0;
//        本探针在独立 demo 里复现同一形态(官方 StartNativeChildProcess 拉起的纯 native 进程)。
// 结果: 写父进程传入的 resultPath(NCP NORMAL isolation 与 App 同 uid, 可写 App sandbox);
//        cacheDir 由 resultPath 推导为同目录 nncache(Build 必须先 SetCache, 目录父进程已建)。
// 铁律: 全部 dlopen+dlsym, 不硬链 NNRt 库(与 libentry.so 侧同构)。
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cstdint>
#include <dlfcn.h>
#include <unistd.h>
#include <AbilityKit/native_child_process.h>
#include "neural_network_runtime/neural_network_runtime.h"
#include "neural_network_runtime/neural_network_core.h"

namespace {

void *g_rt = nullptr;
void *g_core = nullptr;
char g_miss[768] = "";

void *need(const char *n)
{
    void *p = g_rt ? dlsym(g_rt, n) : nullptr;
    if (!p && g_core) p = dlsym(g_core, n);
    if (!p) {
        strncat(g_miss, n, sizeof(g_miss) - strlen(g_miss) - 1);
        strncat(g_miss, " ", sizeof(g_miss) - strlen(g_miss) - 1);
    }
    return p;
}

void app(char *out, size_t outsz, const char *s)
{
    strncat(out, s, outsz - strlen(out) - 1);
}

void appf(char *out, size_t outsz, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    app(out, outsz, line);
}

// 执行链所需函数集(与 nnrt_probe.cpp 的 RtFns 同源, 此处只取 CH-06 用到的子集)
struct Fns {
    void *(*modelNew)();
    void (*modelDel)(void **);
    OH_NN_ReturnCode (*addTensor)(void *, const void *);
    OH_NN_ReturnCode (*setTensorType)(void *, uint32_t, OH_NN_TensorType);
    OH_NN_ReturnCode (*setTensorData)(void *, uint32_t, const void *, size_t);
    OH_NN_ReturnCode (*addOp)(void *, OH_NN_OperationType, const void *, const void *, const void *);
    OH_NN_ReturnCode (*specify)(void *, const void *, const void *);
    OH_NN_ReturnCode (*finish)(void *);
    void *(*descNew)();
    OH_NN_ReturnCode (*descSetShape)(void *, const int32_t *, size_t);
    OH_NN_ReturnCode (*descSetDtype)(void *, OH_NN_DataType);
    OH_NN_ReturnCode (*descSetFormat)(void *, OH_NN_Format);
    OH_NN_ReturnCode (*descDel)(void **);
    void *(*compNew)(const void *);
    OH_NN_ReturnCode (*compSetCache)(void *, const char *, uint32_t);
    OH_NN_ReturnCode (*compSetDev)(void *, size_t);
    OH_NN_ReturnCode (*compPerf)(void *, OH_NN_PerformanceMode);
    OH_NN_ReturnCode (*compBuild)(void *);
    OH_NN_ReturnCode (*compDel)(void **);
    void *(*execNew)(void *);
    OH_NN_ReturnCode (*execOutCount)(const void *, size_t *);
    OH_NN_ReturnCode (*execInCount)(const void *, size_t *);
    void *(*execInDesc)(const void *, size_t);
    void *(*execOutDesc)(const void *, size_t);
    OH_NN_ReturnCode (*execRun)(void *, void **, size_t, void **, size_t);
    void (*execDel)(void **);
    void *(*tensorNew)(size_t, void *);
    void *(*tensorBuf)(const void *);
    void *(*tensorDesc)(const void *);
    OH_NN_ReturnCode (*tensorDel)(void **);
    OH_NN_ReturnCode (*descGetDataCount)(const void *, size_t *);
};

bool collect(Fns &f)
{
    f.modelNew = (decltype(f.modelNew))need("OH_NNModel_Construct");
    f.modelDel = (decltype(f.modelDel))need("OH_NNModel_Destroy");
    f.addTensor = (decltype(f.addTensor))need("OH_NNModel_AddTensorToModel");
    f.setTensorType = (decltype(f.setTensorType))need("OH_NNModel_SetTensorType");
    f.setTensorData = (decltype(f.setTensorData))need("OH_NNModel_SetTensorData");
    f.addOp = (decltype(f.addOp))need("OH_NNModel_AddOperation");
    f.specify = (decltype(f.specify))need("OH_NNModel_SpecifyInputsAndOutputs");
    f.finish = (decltype(f.finish))need("OH_NNModel_Finish");
    f.descNew = (decltype(f.descNew))need("OH_NNTensorDesc_Create");
    f.descSetShape = (decltype(f.descSetShape))need("OH_NNTensorDesc_SetShape");
    f.descSetDtype = (decltype(f.descSetDtype))need("OH_NNTensorDesc_SetDataType");
    f.descSetFormat = (decltype(f.descSetFormat))need("OH_NNTensorDesc_SetFormat");
    f.descDel = (decltype(f.descDel))need("OH_NNTensorDesc_Destroy");
    f.compNew = (decltype(f.compNew))need("OH_NNCompilation_Construct");
    f.compSetCache = (decltype(f.compSetCache))need("OH_NNCompilation_SetCache");
    f.compSetDev = (decltype(f.compSetDev))need("OH_NNCompilation_SetDevice");
    f.compPerf = (decltype(f.compPerf))need("OH_NNCompilation_SetPerformanceMode");
    f.compBuild = (decltype(f.compBuild))need("OH_NNCompilation_Build");
    f.compDel = (decltype(f.compDel))need("OH_NNCompilation_Destroy");
    f.execNew = (decltype(f.execNew))need("OH_NNExecutor_Construct");
    f.execOutCount = (decltype(f.execOutCount))need("OH_NNExecutor_GetOutputCount");
    f.execInCount = (decltype(f.execInCount))need("OH_NNExecutor_GetInputCount");
    f.execInDesc = (decltype(f.execInDesc))need("OH_NNExecutor_CreateInputTensorDesc");
    f.execOutDesc = (decltype(f.execOutDesc))need("OH_NNExecutor_CreateOutputTensorDesc");
    f.execRun = (decltype(f.execRun))need("OH_NNExecutor_RunSync");
    f.execDel = (decltype(f.execDel))need("OH_NNExecutor_Destroy");
    f.tensorNew = (decltype(f.tensorNew))need("OH_NNTensor_Create");
    f.tensorBuf = (decltype(f.tensorBuf))need("OH_NNTensor_GetDataBuffer");
    f.tensorDesc = (decltype(f.tensorDesc))need("OH_NNTensor_GetTensorDesc");
    f.tensorDel = (decltype(f.tensorDel))need("OH_NNTensor_Destroy");
    f.descGetDataCount = (decltype(f.descGetDataCount))need("OH_NNTensorDesc_GetElementCount");
    return g_miss[0] == '\0';
}

// ADD 单算子全链(构图 → 编译 → 执行); 输出摘要到 out
void run_add_chain(char *out, size_t outsz, const char *cacheDir)
{
    using FDev = OH_NN_ReturnCode (*)(const size_t **, uint32_t *);
    using FName = OH_NN_ReturnCode (*)(size_t, const char **);
    using FType = OH_NN_ReturnCode (*)(size_t, OH_NN_DeviceType *);
    FDev fDev = (FDev)need("OH_NNDevice_GetAllDevicesID");
    FName fName = (FName)need("OH_NNDevice_GetName");
    FType fType = (FType)need("OH_NNDevice_GetType");
    if (!fDev) { app(out, outsz, "npuchild: dlsym GetAllDevicesID fail\n"); return; }

    // ① 枚举(9030 上 HDI 异步注册, 首查可能为空 —— 重试)
    const size_t *ids = nullptr;
    uint32_t n = 0;
    int rcDev = (int)fDev(&ids, &n);
    for (int i = 0; i < 6 && n == 0; i++) {
        usleep(500 * 1000);
        rcDev = (int)fDev(&ids, &n);
    }
    appf(out, outsz, "npuchild: dev-rc=%d count=%u\n", rcDev, (unsigned)n);
    size_t target = 0;
    bool found = false;
    for (uint32_t i = 0; i < n && i < 4; i++) {
        const char *nm = nullptr;
        OH_NN_DeviceType tp = (OH_NN_DeviceType)-1;
        if (fName && fType) { fName(ids[i], &nm); fType(ids[i], &tp); }
        appf(out, outsz, "  [%u] name=%s type=%d\n", i, nm ? nm : "?", (int)tp);
        if (nm && (strstr(nm, "NPU_") || strstr(nm, "Kirin"))) { target = ids[i]; found = true; }
    }
    if (n == 0) {
        app(out, outsz, "npuchild: RESULT: NO DEVICE VISIBLE(子进程不可见 NNRt)\n");
        return;
    }
    if (!found) {
        app(out, outsz, "npuchild: RESULT: no real-hw device(仅虚拟口, 无法编译)\n");
        return;
    }
    appf(out, outsz, ">> target id=%zu\n", target);

    Fns f;
    if (!collect(f)) { appf(out, outsz, "npuchild: missing symbols: %s\n", g_miss); return; }

    // ② 构图: ADD, 2×[1,2,2,3] fp32 输入(填 1.0) → 1 输出
    const int32_t S[4] = {1, 2, 2, 3};
    float inData[12];
    for (int i = 0; i < 12; i++) inData[i] = 1.0f;

    void *model = f.modelNew();
    if (!model) { app(out, outsz, "npuchild: model null\n"); return; }
    auto addOne = [&](uint32_t idx, bool isIn) -> bool {
        void *td = f.descNew();
        if (!td) return false;
        bool ok = f.descSetShape(td, S, 4) == OH_NN_SUCCESS &&
                  f.descSetDtype(td, OH_NN_FLOAT32) == OH_NN_SUCCESS &&
                  f.descSetFormat(td, OH_NN_FORMAT_NONE) == OH_NN_SUCCESS &&
                  f.addTensor(model, td) == OH_NN_SUCCESS &&
                  f.setTensorType(model, idx, OH_NN_TENSOR) == OH_NN_SUCCESS;
        f.descDel(&td);
        if (ok && isIn) ok = f.setTensorData(model, idx, inData, sizeof(inData)) == OH_NN_SUCCESS;
        return ok;
    };
    if (!addOne(0, true) || !addOne(1, true) || !addOne(2, false)) {
        app(out, outsz, "npuchild: addTensor fail\n");
        f.modelDel(&model);
        return;
    }
    uint32_t ins[2] = {0, 1}, outs[1] = {2};
    OH_NN_UInt32Array arrP = {nullptr, 0};
    OH_NN_UInt32Array arrI = {ins, 2};
    OH_NN_UInt32Array arrO = {outs, 1};
    int rcAdd = (int)f.addOp(model, OH_NN_OPS_ADD, &arrP, &arrI, &arrO);
    int rcSpec = (int)f.specify(model, &arrI, &arrO);
    int rcFin = rcAdd == 0 ? (int)f.finish(model) : -1;
    appf(out, outsz, "[g:%d/%d/%d]", rcAdd, rcSpec, rcFin);
    if (rcAdd != 0 || rcSpec != 0 || rcFin != 0) {
        app(out, outsz, " graph-fail\n");
        f.modelDel(&model);
        return;
    }

    // ③ 编译(Build 前必须 SetCache; 设备选真实硬件)
    void *comp = f.compNew(model);
    int rcCache = -1, rcSetDev = -1, rcBuild = -1;
    if (comp) {
        rcCache = (int)f.compSetCache(comp, cacheDir, 1);
        rcSetDev = (int)f.compSetDev(comp, target);
        f.compPerf(comp, OH_NN_PERFORMANCE_EXTREME);
        rcBuild = (int)f.compBuild(comp);
    }
    appf(out, outsz, " setCache=%d setDevice=%d build=%d", rcCache, rcSetDev, rcBuild);

    // ④ 执行 —— 张量数按执行器实际输入数(GetInputCount 法则: 单输入 op 也要按此)
    int rcRun = -1;
    float o0 = -1.0f;
    size_t cnt = 0;
    if (comp && rcBuild == 0) {
        void *exec = f.execNew(comp);
        if (exec) {
            size_t nOut = 0, nIn = 0;
            f.execOutCount(exec, &nOut);
            f.execInCount(exec, &nIn);
            void *insDesc[4] = {nullptr};
            void *outsDesc[1] = {nullptr};
            for (size_t i = 0; i < nIn && i < 4; i++) insDesc[i] = f.execInDesc(exec, i);
            outsDesc[0] = f.execOutDesc(exec, 0);
            void *tin[4] = {nullptr};
            void *tout = nullptr;
            for (size_t i = 0; i < nIn && i < 4; i++)
                if (insDesc[i]) tin[i] = f.tensorNew(target, insDesc[i]);
            if (outsDesc[0]) tout = f.tensorNew(target, outsDesc[0]);
            if (tin[0] && tout) {
                for (size_t i = 0; i < nIn && i < 4; i++) {
                    if (!tin[i]) continue;
                    size_t c = 0;
                    f.descGetDataCount(f.tensorDesc(tin[i]), &c);
                    float *p = (float *)f.tensorBuf(tin[i]);
                    for (size_t k = 0; k < c; k++) p[k] = 1.0f;
                }
                rcRun = (int)f.execRun(exec, tin, nIn, &tout, 1);
                if (rcRun == 0 && tout) {
                    f.descGetDataCount(f.tensorDesc(tout), &cnt);
                    float *p = (float *)f.tensorBuf(tout);
                    if (cnt > 0) o0 = p[0];
                }
            }
            appf(out, outsz, " [exec:%zu/%zu run=%d out[0]=%.2f n=%zu]",
                 nOut, nIn, rcRun, o0, cnt);
            for (size_t i = 0; i < nIn && i < 4; i++) if (tin[i]) f.tensorDel(&tin[i]);
            if (tout) f.tensorDel(&tout);
            f.execDel(&exec);
        }
    }
    if (comp) f.compDel(&comp);
    f.modelDel(&model);

    bool pass = (rcAdd == 0 && rcSpec == 0 && rcFin == 0 && rcBuild == 0 && rcRun == 0 && o0 == 2.0f);
    appf(out, outsz, "\nnpuchild: RESULT: NCP 全链 %s\n",
         pass ? "PASS(构图→编译→执行 可用)" : "FAIL(见上)");
}

} // namespace

extern "C" void Main(NativeChildProcess_Args args)
{
    const char *resultPath = (args.entryParams && args.entryParams[0]) ? args.entryParams : "/data/local/tmp/npuchild-result.txt";
    char out[4096] = {0};
    appf(out, sizeof(out), "npuchild: pid=%d start\n", (int)getpid());

    // cacheDir = resultPath 同目录下 nncache(Build 必须先 SetCache; 目录父进程已建)
    char cacheDir[512] = {0};
    snprintf(cacheDir, sizeof(cacheDir), "%s", resultPath);
    char *slash = strrchr(cacheDir, '/');
    if (slash) {
        *slash = '\0';
        strncat(cacheDir, "/nncache", sizeof(cacheDir) - strlen(cacheDir) - 1);
    } else {
        snprintf(cacheDir, sizeof(cacheDir), "/data/local/tmp/nncache");
    }

    g_rt = dlopen("libneural_network_runtime.so", RTLD_NOW | RTLD_GLOBAL);
    g_core = dlopen("libneural_network_core.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_rt || !g_core) {
        appf(out, sizeof(out), "npuchild: dlopen fail rt=%p core=%p\n", g_rt, g_core);
    } else {
        run_add_chain(out, sizeof(out), cacheDir);
    }

    // 写结果文件(尽力; 失败则把结果也写 stderr, 由 hilog 通道抓捕)
    FILE *f = fopen(resultPath, "w");
    if (f) {
        fwrite(out, 1, strlen(out), f);
        fclose(f);
    } else {
        fprintf(stderr, "npuchild result file open failed(%s); inline:</op>\n%.*s\n",
                resultPath, (int)strlen(out), out);
    }
    _exit(0);
}
