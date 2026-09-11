// bootstrap.cpp — torch C++ 扩展: 设备加载(P0) + NNRt 共存验证(P1-a)。
//
// 加载路径 = torch 官方 out-of-tree 设备后端机制, 零侵入 torch 源码:
//   torch/__init__.py 末尾 _import_device_backends()
//     → importlib.metadata.entry_points(group="torch.backends")
//     → 调用 [_nnrt_bootstrap:_register]
//
// ⚠ _register() 绝不抛异常: torch 的 _import_device_backends 会让异常冒泡成
//   RuntimeError → 整个 torch import 失败(后端起不来)。所有逻辑吞异常, 只落日志。
//
// 2026-09-11 P0 定谳(部署链见 nnrt-backend/scripts/build_ext.sh 文件头):
//   本扩展必须落 HAP libs/<abi>/(文件名带 cpython tag) → 由 comfy_child 的 _LibsFinder
//   短名重定向加载; 且须显式进 comfy_child 的 DT_NEEDED(否则 relocating 缺 libpython 符号)。
//
// 2026-09-11 P1-a 共存验证(本文件新增段):
//   本扩展的执行点在 torch/__init__.py 末尾 —— 此刻整条 torch 栈(libc++ std::__1)已在
//   本进程内。在此 dlopen NNRt 并跑一次完整 ADD 构图, 即可判定两者能否同进程共存。
//   这是 NPU 路线的可行性命门: poc/npu 的 npuchild 虽"全链 PASS", 但那是独立进程(无 torch),
//   两者从未共存过。调用序列照抄已 PASS 的 CH-06 链(纯 dlopen+dlsym, 不链 NNRt 库)。
#include <ATen/ATen.h>
#include <torch/version.h>
#include <pybind11/pybind11.h>

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "neural_network_runtime/neural_network_runtime.h"

namespace py = pybind11;

// ─────────────────────────────────────────────────────────────────────────────
// 纯 C 分步落盘: PYTHONHOME 即设备 pyroot; 不依赖 Python API(崩溃时不可靠)
// ─────────────────────────────────────────────────────────────────────────────
static void write_log_raw(const char *msg)
{
    const char *home = getenv("PYTHONHOME");
    if (!home || !*home) {
        return;
    }
    std::string path = std::string(home) + "/nnrt-p0a.log";
    FILE *f = fopen(path.c_str(), "a");
    if (!f) {
        return;
    }
    fputs(msg, f);
    fputc('\n', f);
    fclose(f);
}

static void logf(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    write_log_raw(line);
}

// ─────────────────────────────────────────────────────────────────────────────
// P1-a: NNRt 共存探针
//   铁律(沿袭 poc/npu): 全部 dlopen+dlsym, 不链接 NNRt 库 —— 符号缺失不会拖垮本进程。
// ─────────────────────────────────────────────────────────────────────────────
namespace {

void *g_rt = nullptr;
void *g_core = nullptr;
char g_miss[512] = "";

void *need(const char *n)
{
    void *p = g_rt ? dlsym(g_rt, n) : nullptr;
    if (!p && g_core) {
        p = dlsym(g_core, n);
    }
    if (!p) {
        strncat(g_miss, n, sizeof(g_miss) - strlen(g_miss) - 1);
        strncat(g_miss, " ", sizeof(g_miss) - strlen(g_miss) - 1);
    }
    return p;
}

// CH-06 用到的函数子集(签名照抄 poc/npu/entry/src/main/cpp/child/npuchild.cpp 已验证版)
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

// ADD 单算子全链(构图→编译→执行): 2×[1,2,2,3] fp32 全 1.0 → 期望全 2.0。
//   与 CH-06 完全同构; 每步落盘, 任一步失败即停并标注 —— 用于区分"共存失败(加载/符号)"
//   与"NPU 本身失败(编译/执行)"。
void run_add_chain(const char *cacheDir)
{
    using FDev = OH_NN_ReturnCode (*)(const size_t **, uint32_t *);
    using FName = OH_NN_ReturnCode (*)(size_t, const char **);
    using FType = OH_NN_ReturnCode (*)(size_t, OH_NN_DeviceType *);
    FDev fDev = (FDev)need("OH_NNDevice_GetAllDevicesID");
    FName fName = (FName)need("OH_NNDevice_GetName");
    FType fType = (FType)need("OH_NNDevice_GetType");
    if (!fDev) {
        logf("NNRT-P1A s5c dlsym-GetAllDevicesID-FAILED");
        return;
    }

    // ① 枚举(9030 上 HDI 异步注册, 首查可能为空 —— 重试)
    const size_t *ids = nullptr;
    uint32_t n = 0;
    int rcDev = (int)fDev(&ids, &n);
    for (int i = 0; i < 6 && n == 0; i++) {
        usleep(500 * 1000);
        rcDev = (int)fDev(&ids, &n);
    }
    logf("NNRT-P1A s5c dev-rc=%d count=%u", rcDev, (unsigned)n);
    size_t target = 0;
    bool found = false;
    for (uint32_t i = 0; i < n && i < 4; i++) {
        const char *nm = nullptr;
        OH_NN_DeviceType tp = (OH_NN_DeviceType)-1;
        if (fName && fType) {
            fName(ids[i], &nm);
            fType(ids[i], &tp);
        }
        logf("NNRT-P1A s5c   [%u] name=%s type=%d", i, nm ? nm : "?", (int)tp);
        if (nm && (strstr(nm, "NPU_") || strstr(nm, "Kirin"))) {
            target = ids[i];
            found = true;
        }
    }
    if (n == 0) {
        logf("NNRT-P1A s5c RESULT: NO DEVICE VISIBLE(torch 进程内不可见 NNRt)");
        return;
    }
    if (!found) {
        logf("NNRT-P1A s5c RESULT: no real-hw device(仅虚拟口, 无法编译)");
        return;
    }
    logf("NNRT-P1A s5c target-id=%zu", target);

    Fns f;
    if (!collect(f)) {
        logf("NNRT-P1A s5c missing-symbols: %s", g_miss);
        return;
    }
    logf("NNRT-P1A s5d symbols-ok(31 fns)");

    // ② 构图
    const int32_t S[4] = {1, 2, 2, 3};
    float inData[12];
    for (int i = 0; i < 12; i++) {
        inData[i] = 1.0f;
    }
    void *model = f.modelNew();
    if (!model) {
        logf("NNRT-P1A s5d model-null");
        return;
    }
    auto addOne = [&](uint32_t idx, bool isIn) -> bool {
        void *td = f.descNew();
        if (!td) {
            return false;
        }
        bool ok = f.descSetShape(td, S, 4) == OH_NN_SUCCESS &&
                  f.descSetDtype(td, OH_NN_FLOAT32) == OH_NN_SUCCESS &&
                  f.descSetFormat(td, OH_NN_FORMAT_NONE) == OH_NN_SUCCESS &&
                  f.addTensor(model, td) == OH_NN_SUCCESS &&
                  f.setTensorType(model, idx, OH_NN_TENSOR) == OH_NN_SUCCESS;
        f.descDel(&td);
        if (ok && isIn) {
            ok = f.setTensorData(model, idx, inData, sizeof(inData)) == OH_NN_SUCCESS;
        }
        return ok;
    };
    if (!addOne(0, true) || !addOne(1, true) || !addOne(2, false)) {
        logf("NNRT-P1A s5d addTensor-fail");
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
    logf("NNRT-P1A s5d graph add=%d spec=%d finish=%d", rcAdd, rcSpec, rcFin);
    if (rcAdd != 0 || rcSpec != 0 || rcFin != 0) {
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
    logf("NNRT-P1A s5e setCache=%d setDevice=%d build=%d", rcCache, rcSetDev, rcBuild);

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
            for (size_t i = 0; i < nIn && i < 4; i++) {
                insDesc[i] = f.execInDesc(exec, i);
            }
            outsDesc[0] = f.execOutDesc(exec, 0);
            void *tin[4] = {nullptr};
            void *tout = nullptr;
            for (size_t i = 0; i < nIn && i < 4; i++) {
                if (insDesc[i]) {
                    tin[i] = f.tensorNew(target, insDesc[i]);
                }
            }
            if (outsDesc[0]) {
                tout = f.tensorNew(target, outsDesc[0]);
            }
            if (tin[0] && tout) {
                for (size_t i = 0; i < nIn && i < 4; i++) {
                    if (!tin[i]) {
                        continue;
                    }
                    size_t c = 0;
                    f.descGetDataCount(f.tensorDesc(tin[i]), &c);
                    float *p = (float *)f.tensorBuf(tin[i]);
                    for (size_t k = 0; k < c; k++) {
                        p[k] = 1.0f;
                    }
                }
                rcRun = (int)f.execRun(exec, tin, nIn, &tout, 1);
                if (rcRun == 0 && tout) {
                    f.descGetDataCount(f.tensorDesc(tout), &cnt);
                    float *p = (float *)f.tensorBuf(tout);
                    if (cnt > 0) {
                        o0 = p[0];
                    }
                }
            }
            logf("NNRT-P1A s5e exec out=%zu in=%zu run=%d out[0]=%.2f n=%zu",
                 nOut, nIn, rcRun, o0, cnt);
            for (size_t i = 0; i < nIn && i < 4; i++) {
                if (tin[i]) {
                    f.tensorDel(&tin[i]);
                }
            }
            if (tout) {
                f.tensorDel(&tout);
            }
            f.execDel(&exec);
        }
    }
    if (comp) {
        f.compDel(&comp);
    }
    f.modelDel(&model);

    bool pass = (rcAdd == 0 && rcSpec == 0 && rcFin == 0 && rcBuild == 0 && rcRun == 0 && o0 == 2.0f);
    logf("NNRT-P1A RESULT: NNRt×torch 同进程 %s",
         pass ? "PASS(共存可行, 构图→编译→执行 全通)" : "FAIL(见上各步)");
}

void probe_nnrt(const char *cacheDir)
{
    logf("NNRT-P1A s5a probe-entered (torch 已加载, 同进程 dlopen NNRt)");
    g_rt = dlopen("libneural_network_runtime.so", RTLD_NOW | RTLD_GLOBAL);
    g_core = dlopen("libneural_network_core.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_rt) {
        logf("NNRT-P1A s5b dlopen-rt-FAILED core=%p", g_core);
        return;
    }
    logf("NNRT-P1A s5b dlopen-rt-ok core=%p", g_core);
    run_add_chain(cacheDir);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// pybind11 模块(设备加载验证 P0)
// ─────────────────────────────────────────────────────────────────────────────
static std::string describe()
{
    auto t = at::ones({2, 3}, at::kFloat);
    float s = t.sum().item<float>();
    return std::string("torch=") + TORCH_VERSION + " aten=ok sum=" + std::to_string(s);
}

static void _register()
{
    write_log_raw("NNRT-P0A v1 s1 register-entered");

    try {
        py::module_ sys = py::module_::import("sys");
        std::string p = py::cast<std::string>(sys.attr("prefix"));
        write_log_raw(("NNRT-P0A v1 s2 import-sys-ok prefix=" + p).c_str());
    } catch (...) {
        write_log_raw("NNRT-P0A v1 s2 import-sys-FAILED");
    }

    try {
        auto t = at::ones({2, 3}, at::kFloat);
        float s = t.sum().item<float>();
        write_log_raw(("NNRT-P0A v1 s3 aten-ok sum=" + std::to_string(s)).c_str());
    } catch (const std::exception &e) {
        write_log_raw((std::string("NNRT-P0A v1 s3 aten-EX ") + e.what()).c_str());
    } catch (...) {
        write_log_raw("NNRT-P0A v1 s3 aten-EX unknown");
    }

    write_log_raw("NNRT-P0A v1 s4 register-done");

    // ── P1-a: NNRt 共存验证(此时 torch 全栈已在进程内) ──
    //   任何异常都不许逃逸(否则 torch import 炸), 故整段包 try。
    try {
        const char *home = getenv("PYTHONHOME");
        if (home && *home) {
            std::string cache = std::string(home) + "/nncache";
            mkdir(cache.c_str(), 0755);   // 已存在=正常
            probe_nnrt(cache.c_str());
        } else {
            write_log_raw("NNRT-P1A s5a no-PYTHONHOME-skip");
        }
    } catch (const std::exception &e) {
        write_log_raw((std::string("NNRT-P1A s5x EX ") + e.what()).c_str());
    } catch (...) {
        write_log_raw("NNRT-P1A s5x EX unknown");
    }
}

PYBIND11_MODULE(_nnrt_bootstrap, m)
{
    m.doc() = "torch_nnrt bootstrap (P0: 加载与 ATen 调用; P1-a: NNRt 同进程共存)";
    m.def("describe", &describe, "返回 torch 版本与一次真实 ATen 调用结果");
    m.def("_register", &_register, "torch.backends entry point(自动调用, 不抛异常)");
}
