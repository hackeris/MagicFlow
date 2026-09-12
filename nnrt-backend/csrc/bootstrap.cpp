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
#include <c10/core/GradMode.h>   // c10::AutoGradMode(诊断用)
#include <torch/version.h>
#include <pybind11/pybind11.h>
#include <pybind11/eval.h>   // py::exec(门控自检脚本)

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "neural_network_runtime/neural_network_runtime.h"

#include "nnrt_engine.h"   // P1: 执行引擎(与 torch 解耦, backend.cpp 同用)

namespace py = pybind11;

// backend.cpp 提供的 PrivateUse1 注册入口(两文件同链进本扩展, 故意不引 pybind11)
namespace nnrt_backend {
void registerBackend();
}

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
    OH_NN_ReturnCode (*compFp16)(void *, bool);
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
    // ⚠ 可选符号: 2026-09-12 实测 9030 的 libneural_network_runtime.so **没有**这个符号
    //   (P1A s5c missing-symbols)。若按 need() 处理会拖垮整个 collect() → 所有探针 SKIP,
    //   **一轮真机就这么白跑了**。诊断用/增强用接口一律走可选 dlsym, 调用点判空。
    f.compFp16 = (decltype(f.compFp16))dlsym(g_rt, "OH_NNCompilation_SetEnableFp16");
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

// ─────────────────────────────────────────────────────────────────────────────
// P1-c 端到端自检(**无条件跑**, P1 阶段刻意如此)
//   为何不用门控文件: 应用沙箱 el2 不可外部写, 设备上推不进任何开关文件, 改了开关就得
//   改 ArkTS 重打包一轮 —— 而本段只花一次 64×128 matmul(NNRt 侧 ~百 ms), 代价可忽略。
//   副作用反成收益: 每步都落盘, 崩溃时 log 停在某一步 = 自带定位。
//   P2 起应改为开关(届时走 AppStorage/配置项, 而非文件)。
//   ⚠ 本函数在 torch import 中途执行, Python 异常状态绝不许逃逸 —— 否则 torch import
//     炸掉后端起不来。故脚本内层层 try, 且 C++ 侧兜底清 PyErr。
// ─────────────────────────────────────────────────────────────────────────────
void run_selftest(const char *home)
{
    // 覆盖: 设备名/设备数 → 搬运 → matmul 下沉 → 数值对拍 → add 回落 → empty(device=)
    static const char *kScript = R"PY(
import os, torch
P = os.environ.get("PYTHONHOME", "/tmp")
LOG = P + "/nnrt-p1.log"
def w(m):
    with open(LOG, "a") as f:
        f.write(m + "\n")
# 构建标记: 每次改脚本必改此串 —— 否则无法区分"修复没生效"与"新包没部署上"
w("=== P1 selftest build=p1c4 ===")
# faulthandler: SIGSEGV 时打 C/Python 栈到独立文件; 25s 未跑完则视为死锁, dump 后退出。
#   为什么必须有: 2026-09-12 两轮实测都"日志停在 s2 之后、既无 EX 行也无后续行",
#   Python 层 except 抓不到 —— 只能靠 C 层 dump 区分 段错误 / 死锁 / 卡死。
try:
    import faulthandler
    faulthandler.enable(open(P + "/nnrt-fault.log", "a"))
    faulthandler.dump_traceback_later(25, exit=True)
except Exception as ex:
    w("faulthandler EX %r" % ex)
try:
    w("s1 torch=%s backend-name=%s" % (torch.__version__, torch._C._get_privateuse1_backend_name()))
    w("s2 device_count=%d current=%d" % (torch.nnnrt.device_count(), torch.nnnrt.current_device()))
except Exception as ex:
    w("s2 EX %r" % ex)
try:
    torch.manual_seed(0)
    w("s2b manual_seed ok")
    a = torch.randn(64, 128)
    w("s2c randn-a ok")
    b = torch.randn(128, 32)
    w("s2d randn-b ok")
    ref = a @ b
    w("s2e cpu-matmul ok ref00=%.4f" % ref[0, 0].item())
except Exception as ex:
    w("s2f EX %r" % ex)
# 二分定位(p1c4): 工厂路径 / 反向搬运 / 直接算子 / Python 包装, 逐条隔离崩点
try:
    w("s3a1 before torch.device(nnrt)")
    dv = torch.device("nnrt")
    w("s3a2 device ok type=%s index=%s" % (dv.type, dv.index))
except Exception as ex:
    w("s3a EX %r" % ex)
try:
    w("s3b1 before empty(device=nnrt)")
    e1 = torch.empty(2, 2, device=dv)
    w("s3b2 empty ok dev=%s shape=%s" % (e1.device, tuple(e1.shape)))
except Exception as ex:
    w("s3b EX %r" % ex)
try:
    w("s3c1 before to(cpu)")
    ac = a.to("cpu")
    w("s3c2 to(cpu) ok dev=%s same_ptr=%s" % (ac.device, ac.data_ptr() == a.data_ptr()))
except Exception as ex:
    w("s3c EX %r" % ex)
try:
    w("s3d1 before aten._to_copy(device=nnrt)")
    ad = torch.ops.aten._to_copy.default(a, device=dv)
    w("s3d2 _to_copy ok dev=%s" % ad.device)
except Exception as ex:
    w("s3d EX %r" % ex)
try:
    w("s3e1 before Tensor.to(nnrt)")
    an = a.to(dv)
    w("s3e2 first to ok dev=%s" % an.device)
    bn = b.to(dv)
    w("s3e3 second to ok dev=%s" % bn.device)
except Exception as ex:
    w("s3e EX %r" % ex)
try:
    w("s4a before matmul")
    out = torch.matmul(an, bn)
    w("s4b matmul ok dev=%s shape=%s" % (out.device, tuple(out.shape)))
except Exception as ex:
    w("s4 EX %r" % ex)
    out = None
try:
    w("s5a before to(cpu)")
    got = out.to("cpu")
    d = (got - ref).abs().max().item()
    sc = ref.abs().max().item()
    w("s5b to(cpu) dev=%s maxabs=%.6e scale=%.6e rel=%.3e" % (got.device, d, sc, (d / sc) if sc else 0.0))
    w("s5 verdict=%s" % ("PASS" if (sc and d / sc < 1e-3) else "FAIL"))
except Exception as ex:
    w("s5 EX %r" % ex)
try:
    w("s6a before add")
    c = an + 1.0
    w("s6b add dev=%s" % c.device)
    d2 = ((c.to("cpu")) - (a + 1.0)).abs().max().item()
    w("s6c fallback-add maxabs=%.6e" % d2)
except Exception as ex:
    w("s6 EX %r" % ex)
try:
    w("s7a before empty(nnrt)")
    e_dev = torch.empty(4, 4, device="nnrt")
    w("s7b empty dev=%s shape=%s" % (e_dev.device, tuple(e_dev.shape)))
except Exception as ex:
    w("s7 EX %r" % ex)
try:
    s = an.sum()
    w("s8 sum dev=%s val=%.4f cpu=%.4f" % (s.device, s.item(), a.sum().item()))
except Exception as ex:
    w("s8 EX %r" % ex)
try:
    faulthandler.cancel_dump_traceback_later()
except Exception:
    pass
w("=== done ===")
)PY";
    try {
        py::exec(kScript);
        logf("NNRT-P1C selftest-ran (详见 %s/nnrt-p1.log)", home);
    } catch (py::error_already_set &e) {
        logf("NNRT-P1C pyEX %s", e.what());
        e.restore();
        PyErr_Clear();   // 关键: 不留挂起异常给 torch import 后续代码
    } catch (const std::exception &e) {
        logf("NNRT-P1C EX %s", e.what());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// P1-x: 调度表诊断
//   背景(2026-09-12): aten::mm(本设备,本设备) 真机卡死, 而 kernel 侧(NNRT-MM)与
//   fallback 侧(NNRT-FB)都零日志 —— 无法区分下面两种完全不同的病因:
//     (a) TORCH_LIBRARY_IMPL 的 m.impl("mm", ...) 没生效 → 该 key 上根本没 kernel;
//     (b) 注册生效了, 但调用路径在 kernel 入口之前卡住。
//   本段在**任何探针之前**直接问 dispatcher: 这些算子在 PrivateUse1 上到底有没有 kernel。
//   有 = 病因 (b); 无 = 病因 (a)。(顺带对照 mm.out / matmul / sum —— 后两者性质不同。)
// ─────────────────────────────────────────────────────────────────────────────
void probeDispatchTable()
{
    // ⚠ 只查**有默认重载**的算子名。2026-09-12 实测: 列表里带 "aten::empty" 时, 进程在打印完
    //   前 4 行后失联(P1-a 一行都没跑, fault 日志空 —— 崩在 C++ 层, 非 Python 异常)。
    //   "aten::empty" 是多重载算子(真名 empty.memory_format / empty.out / ...), 不存在默认
    //   重载的 schema; hasKernelForOperator 对未知名会走 findOrRegisterName_, 不是纯查表。
    //   经此一坑: 诊断代码本身也会改变被诊断系统的状态, 列表只留 mm/mm.out/matmul/sum 这类
    //   单重载名。每轮仍跑 —— "mm 的 kernel 注册是否生效"是最值得复验的不变量。
    const char *ops[] = {"aten::mm", "aten::mm.out", "aten::matmul", "aten::sum"};
    const size_t nOps = sizeof(ops) / sizeof(ops[0]);
    try {
        py::object C = py::module_::import("torch").attr("_C");
        py::object hasKernel = C.attr("_dispatch_has_kernel_for_dispatch_key");
        logf("NNRT-DIAG begin n=%zu", nOps);
        for (size_t i = 0; i < nOps; i++) {
            logf("NNRT-DIAG q%zu-enter %s", i, ops[i]);   // 逐次留痕: 崩在哪一次一目了然
            try {
                bool has = hasKernel(ops[i], "PrivateUse1").cast<bool>();
                logf("NNRT-DIAG q%zu %s pu1-kernel=%d", i, ops[i], int(has));
            } catch (const std::exception &e) {
                logf("NNRT-DIAG q%zu %s query-EX %s", i, ops[i], e.what());
            }
        }
        logf("NNRT-DIAG done");
    } catch (const std::exception &e) {
        logf("NNRT-DIAG setup-EX %s", e.what());
    } catch (...) {
        logf("NNRT-DIAG setup-EX unknown");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// P1-g: 手工复刻 poc/npu 的 CONV2D 构图(**绕过 Engine**) —— 切分"我们的实现"与"设备侧"
//   背景: Engine 编译 conv2d 稳定返回 OH_NN_FAILED(e3b build-rc=1), 而 poc/npu 的 CONV2D
//   是 PASS 的。差异已收敛到实现细节, 只能逐项复刻来切分。
//   本函数**严格照抄** poc/npu/entry/src/main/cpp/nnrt_probe.cpp 的 op_run:
//     ① 张量登记顺序 ins → params → out;
//     ② weight / bias **不给数据**(只 addTensor, 等价于 POC 的 T_F32 规格);
//     ③ 参数只给 STRIDES + PAD(不传 dilation/group/activation);
//     ④ 性能模式 EXTREME;  ⑤ 缓存目录独立(mkdir 后即用)。
//   判读: 它成功 ⇒ 问题在我们的 Engine(逐项对比即可定位); 它也失败 ⇒ 设备/环境侧, 与实现无关。
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// 一次 CONV 类编译尝试的配置。tag 进日志；其余是图参数。
//   ⚠ 2026-09-12 查源码(ops/conv2d_builder.cpp)纠正了两处**规格**错误, 均属"旧写法":
//   (1) **STRIDES/DILATION 的 rank 必须是 2**, 不是 4 —— SetStrides/SetDilation **不校验 rank**,
//       按 GetElementCount() 整段拷进 m_strides/m_dilation, 再由 MindIR_Conv2DFusion 吃下;
//       而单测(m_stride_dim{2}/m_dilation_dim{2})与 GetPrimitive 断言({1,1})都是 **2 元素**。
//       传 4 元素 ⇒ 设备侧拿到畸形 stride ⇒ compBuild 拒绝。**PAD 相反, padList 必须是 4 元素**。
//   (2) **形状是 NHWC 语义** —— 走 MindIR Conv2DFusion, 同 POC 的 [1,2,2,3] 读法(C=3);
//       传 NCHW 的 [1,3,8,8] 会被读成 N=1,H=3,W=8,C=8, 与权重反推的 inChannel=3 冲突。
//   两者都保留开关做成矩阵, 免得又用"猜"代替"测"。
struct CvTry {
    const char *tag;
    OH_NN_OperationType op;
    int32_t c, h, w, oc, kh, kw;   // 逻辑尺寸(NCHW 语义); 权重恒 OHWI [oc,kh,kw,c]
    int32_t stride;
    int padKind;      // 0=不传 PAD, 1=padList(INT64×4), 2=padMode(INT8×1)
    int nhwc;         // 形状声明为 NHWC [1,h,w,c]/[1,oh,ow,oc]?  (0=NCHW)
    int strideRank;   // STRIDES/DILATION 的 rank: 2(单测规格) 或 4(旧写法)
    int fp16;         // 调 OH_NNCompilation_SetEnableFp16(true)? (默认 false)
    int noCache;      // 跳过 SetCache? (隔离"缓存机制本身"是否是失败源)
};

} // namespace

// ── P1-h: 直接问设备"这张图的算子你支持吗" ──────────────────────────────────────
// 权威依据: nncompiler.cpp:364 的 NormalBuild() —— 它在真正 PrepareModel **之前**先调
//   IsSupportedModel() → device->GetSupportedOperation(→ HDI 服务); 只要有算子判 false 就
//   返回 **OH_NN_FAILED(=1)**。这正是 conv2d 的 e3b build-rc=1 的来源。
//   `OH_NNModel_GetAvailableOperations` 是同一判定的**公开入口**(neural_network_runtime.h
//   since 9), 且**不需要 build** —— 用它能把"设备侧不支持"与"我们构图/参数有误"彻底分开:
//   前者 → isSupported[k]=false, 后者 → 构图阶段(AddOperation/Finish)就非 0。
//   ⚠ 该符号同样走**可选 dlsym**(设备可能裁剪; 2026-09-12 已证 9030 缺 SetEnableFp16)。
static void probe_op_support()
{
    const char *home = getenv("PYTHONHOME");
    if (!home || !*home) { logf("NNRT-P1H SKIP no-pythonhome"); return; }
    if (!g_rt) { g_rt = dlopen("libneural_network_runtime.so", RTLD_NOW | RTLD_GLOBAL); }
    if (!g_core) { g_core = dlopen("libneural_network_core.so", RTLD_NOW | RTLD_GLOBAL); }
    if (!g_rt || !g_core) { logf("NNRT-P1H SKIP no-rt"); return; }
    Fns f{};
    if (!collect(f)) { logf("NNRT-P1H SKIP no-syms"); return; }
    using FDev = OH_NN_ReturnCode (*)(const size_t **, uint32_t *);
    using FName = OH_NN_ReturnCode (*)(size_t, const char **);
    using FAvail = OH_NN_ReturnCode (*)(void *, size_t, const bool **, uint32_t *);
    FDev fDev = (FDev)need("OH_NNDevice_GetAllDevicesID");
    FName fName = (FName)need("OH_NNDevice_GetName");
    FAvail fAvail = (FAvail)dlsym(g_rt, "OH_NNModel_GetAvailableOperations");
    if (!fDev || !fName) { logf("NNRT-P1H SKIP no-dev-api"); return; }
    if (!fAvail) { logf("NNRT-P1H SKIP no-GetAvailableOperations(符号缺失)"); return; }
    const size_t *ids = nullptr;
    uint32_t n = 0;
    fDev(&ids, &n);
    size_t target = 0;
    bool found = false;
    for (uint32_t i = 0; i < n && i < 4; i++) {
        const char *nm = nullptr;
        fName(ids[i], &nm);
        if (nm && (strstr(nm, "NPU_") || strstr(nm, "Kirin"))) { target = ids[i]; found = true; }
    }
    if (!found) { logf("NNRT-P1H SKIP no-real-dev"); return; }

    struct Spec {
        OH_NN_DataType dt; OH_NN_TensorType tt; const int32_t *dims;
        uint32_t rank; const void *data; size_t bytes;
    };
    // 逐张图问设备。返回码/支持位逐位打印 —— 这是"设备能力"的**直接证据**, 不再是推断。
    auto query = [&](const char *tag, void *model) {
        const bool *sup = nullptr;
        uint32_t cnt = 0;
        const int rc = (int)fAvail(model, target, &sup, &cnt);
        // ⚠ 2026-09-12 实测 9030: ADD/CONV2D 两张图都返回 **rc=0 且 sup=空指针** ——
        //   即该 API 在此设备上是**假成功**(裁剪版 NNRt, 同 SetEnableFp16 符号缺失)。
        //   故这里把 sup/cnt 一并打出来, 免得又被 rc=0 误导成"设备支持"。
        if (rc != 0 || !sup) {
            logf("NNRT-P1H[%s] GetAvailableOperations rc=%d sup=%s opCount=%u (rc=0 且 sup 非空才算可用)",
                 tag, rc, sup ? "非空" : "**空指针 → 该 API 在设备上未真正实现**", cnt);
            return;
        }
        char buf[160] = "";
        int nFalse = 0;
        for (uint32_t i = 0; i < cnt && i < 8; i++) {
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%d",
                     i ? "," : "", sup[i] ? 1 : 0);
            if (!sup[i]) { nFalse++; }
        }
        logf("NNRT-P1H[%s] rc=0 opCount=%u supported=[%s] -> %s",
             tag, cnt, buf, nFalse ? "**设备不支持**" : "设备支持");
    };
    auto makeTensors = [&](void *model, const Spec *sp, int nsp) {
        for (int i = 0; i < nsp; i++) {
            void *td = f.descNew();
            if (!td) { return false; }
            const bool ok = f.descSetShape(td, sp[i].dims, sp[i].rank) == OH_NN_SUCCESS &&
                            f.descSetDtype(td, sp[i].dt) == OH_NN_SUCCESS &&
                            f.descSetFormat(td, OH_NN_FORMAT_NONE) == OH_NN_SUCCESS &&
                            f.addTensor(model, td) == OH_NN_SUCCESS;
            f.descDel(&td);
            if (!ok) { return false; }
            if (f.setTensorType(model, (uint32_t)i, sp[i].tt) != OH_NN_SUCCESS) { return false; }
            if (sp[i].data && sp[i].bytes &&
                f.setTensorData(model, (uint32_t)i, sp[i].data, sp[i].bytes) != OH_NN_SUCCESS) {
                return false;
            }
        }
        return true;
    };

    // 图 A: ADD(对照) —— 已知在 9030 上能真实编译(POC 唯一真实编译过的算子)。若它这里也报
    //   不支持, 说明本探针本身有问题, 而不是设备的问题。
    {
        static const int32_t SA[4] = {1, 2, 2, 3};
        const Spec sp[3] = {
            {OH_NN_FLOAT32, OH_NN_TENSOR, SA, 4, nullptr, 0},
            {OH_NN_FLOAT32, OH_NN_TENSOR, SA, 4, nullptr, 0},
            {OH_NN_FLOAT32, OH_NN_TENSOR, SA, 4, nullptr, 0},
        };
        void *model = f.modelNew();
        bool ok = model && makeTensors(model, sp, 3);
        uint32_t ins[2] = {0, 1}, outs[1] = {2};
        OH_NN_UInt32Array aP = {nullptr, 0}, aI = {ins, 2}, aO = {outs, 1};
        if (ok) { ok = f.addOp(model, OH_NN_OPS_ADD, &aP, &aI, &aO) == OH_NN_SUCCESS; }
        if (ok) { ok = f.specify(model, &aI, &aO) == OH_NN_SUCCESS; }
        ok = ok && f.finish(model) == OH_NN_SUCCESS;
        if (ok) { query("ADD", model); } else { logf("NNRT-P1H[ADD] 构图未过"); }
        if (model) { f.modelDel(&model); }
    }
    // 图 B: CONV2D —— 用与前几轮扫描完全相同的规格(NHWC + rank2 + padList), 只把"编译"
    //   换成"问支持性", 这样两边的结论可直接对照。
    {
        static const int32_t SX[4] = {1, 8, 8, 3};
        static const int32_t SW[4] = {4, 3, 3, 3};   // OHWI
        static const int32_t SB[1] = {4};
        static const int32_t SO[4] = {1, 4, 4, 4};   // (8+1+1-3)/2+1 = 4
        static const int32_t SD2[1] = {2};
        static const int32_t SD4[1] = {4};
        static const int64_t STR[2] = {2, 2};
        static const int64_t PADL[4] = {1, 1, 1, 1};
        static const int64_t DIL[2] = {1, 1};
        const Spec sp[7] = {
            {OH_NN_FLOAT32, OH_NN_TENSOR, SX, 4, nullptr, 0},
            {OH_NN_FLOAT32, OH_NN_TENSOR, SW, 4, nullptr, 0},
            {OH_NN_FLOAT32, OH_NN_TENSOR, SB, 1, nullptr, 0},
            {OH_NN_INT64, OH_NN_CONV2D_STRIDES, SD2, 1, STR, sizeof(STR)},
            {OH_NN_INT64, OH_NN_CONV2D_PAD, SD4, 1, PADL, sizeof(PADL)},
            {OH_NN_INT64, OH_NN_CONV2D_DILATION, SD2, 1, DIL, sizeof(DIL)},
            {OH_NN_FLOAT32, OH_NN_TENSOR, SO, 4, nullptr, 0},
        };
        void *model = f.modelNew();
        bool ok = model && makeTensors(model, sp, 7);
        uint32_t params[3] = {3, 4, 5}, ins[3] = {0, 1, 2}, outs[1] = {6};
        OH_NN_UInt32Array aP = {params, 3}, aI = {ins, 3}, aO = {outs, 1};
        if (ok) { ok = f.addOp(model, OH_NN_OPS_CONV2D, &aP, &aI, &aO) == OH_NN_SUCCESS; }
        if (ok) { ok = f.specify(model, &aI, &aO) == OH_NN_SUCCESS; }
        ok = ok && f.finish(model) == OH_NN_SUCCESS;
        if (ok) { query("CONV2D", model); } else { logf("NNRT-P1H[CONV2D] 构图未过"); }
        if (model) { f.modelDel(&model); }
    }
}

static void probe_conv_scan()
{
    const char *home = getenv("PYTHONHOME");
    if (!home || !*home) { logf("NNRT-P1G SKIP no-pythonhome"); return; }
    if (!g_rt) { g_rt = dlopen("libneural_network_runtime.so", RTLD_NOW | RTLD_GLOBAL); }
    if (!g_core) { g_core = dlopen("libneural_network_core.so", RTLD_NOW | RTLD_GLOBAL); }
    if (!g_rt || !g_core) { logf("NNRT-P1G SKIP no-rt"); return; }
    Fns f{};
    if (!collect(f)) { logf("NNRT-P1G SKIP no-syms"); return; }
    using FDev = OH_NN_ReturnCode (*)(const size_t **, uint32_t *);
    using FName = OH_NN_ReturnCode (*)(size_t, const char **);
    FDev fDev = (FDev)need("OH_NNDevice_GetAllDevicesID");
    FName fName = (FName)need("OH_NNDevice_GetName");
    if (!fDev || !fName) { logf("NNRT-P1G SKIP no-dev-api"); return; }
    const size_t *ids = nullptr;
    uint32_t n = 0;
    fDev(&ids, &n);
    size_t target = 0;
    bool found = false;
    for (uint32_t i = 0; i < n && i < 4; i++) {
        const char *nm = nullptr;
        fName(ids[i], &nm);
        if (nm && (strstr(nm, "NPU_") || strstr(nm, "Kirin"))) { target = ids[i]; found = true; }
    }
    if (!found) { logf("NNRT-P1G SKIP no-real-dev"); return; }

    // ⚠ 背景: 同一张 CONV2D 图, Engine 与"照抄 poc/npu 的手工构图"**都**得到 build=1;
    //   而 ADD/mm/softmax 都能真实编译成功。故做一轮参数扫描, 判定究竟是**设备侧不支持**
    //   还是**某种参数表达**的问题。每组: 独立 model + **独立缓存目录**(避免串图),
    //   只记录 build 返回码(0=成功)。
    // ⚠ 不收 CONV2D_TRANSPOSE: 其输出形状公式与普通卷积不同((H-1)*s-2p+KH), 用同一套
    //   so_h/so_w 算会给出错误形状, 失败时无法区分"算子不支持"与"形状给错" —— 污染结论。
    // 矩阵: 前两轮已把 **rank / layout / pad 形式 / 规模** 逐一证伪(全部 build=1, 见 commit
    //   历史与 memory)。本轮的 4 组针对**编译配置**三个尚未单独动过的开关。
    static const CvTry tries[] = {
        {"s2-nhwc",         OH_NN_OPS_CONV2D, 3, 8, 8, 4, 3, 3, 2, 1, 1, 2, 0, 0},  // 基线(复跑)
        {"s2-nhwc-fp16",    OH_NN_OPS_CONV2D, 3, 8, 8, 4, 3, 3, 2, 1, 1, 2, 1, 0},  // 开 fp16
        {"s2-nhwc-nocache", OH_NN_OPS_CONV2D, 3, 8, 8, 4, 3, 3, 2, 1, 1, 2, 0, 1},  // 不设缓存
        {"s2-nhwc-fp16-nc", OH_NN_OPS_CONV2D, 3, 8, 8, 4, 3, 3, 2, 1, 1, 2, 1, 1},  // 两者都
    };
    for (size_t t = 0; t < sizeof(tries) / sizeof(tries[0]); t++) {
        const CvTry &k = tries[t];
        const int padKind = k.padKind;
        const int pH = (padKind == 1) ? 1 : 0;   // padList 给 1; padMode 的 padding 由设备按 SAME 自算
        int32_t so_h = (k.h + 2 * pH - k.kh) / k.stride + 1;
        int32_t so_w = (k.w + 2 * pH - k.kw) / k.stride + 1;
        if (so_h <= 0 || so_w <= 0) { so_h = so_w = 1; }
        // 形状按声明布局排列(NCHW=[1,c,h,w] / NHWC=[1,h,w,c]); 权重恒 OHWI(与布局无关)
        const int32_t SX[4] = {1, k.nhwc ? k.h : k.c, k.nhwc ? k.w : k.h, k.nhwc ? k.c : k.w};
        const int32_t SW[4] = {k.oc, k.kh, k.kw, k.c};   // OHWI(见 conv2d_builder 的 SetChannel)
        const int32_t SB[1] = {k.oc};
        const int32_t SD1[1] = {1};   // 标量/单元素
        const int32_t SD2[1] = {2};   // rank=2 数组(单测规格)
        const int32_t SD4[1] = {4};   // rank=4(padList)
        int32_t SO[4] = {1, k.nhwc ? so_h : k.oc, k.nhwc ? so_w : so_h, k.nhwc ? k.oc : so_w};
        const int64_t STR2[2] = {k.stride, k.stride};
        const int64_t STR4[4] = {1, 1, k.stride, k.stride};
        const int64_t DIL2[2] = {1, 1};
        const int64_t DIL4[4] = {1, 1, 1, 1};
        const int64_t PADL[4] = {1, 1, 1, 1};
        const int8_t PADM[1] = {1};   // 1 = PAD_MODE_SAME(值域见 Validation::ValidatePadMode)
        const void *pStr = (k.strideRank == 2) ? (const void *)STR2 : (const void *)STR4;
        const size_t nStr = (k.strideRank == 2) ? sizeof(STR2) : sizeof(STR4);
        const void *pDil = (k.strideRank == 2) ? (const void *)DIL2 : (const void *)DIL4;
        const size_t nDil = (k.strideRank == 2) ? sizeof(DIL2) : sizeof(DIL4);
        const int32_t *pRd = (k.strideRank == 2) ? SD2 : SD4;
        void *model = f.modelNew();
        if (!model) { logf("NNRT-P1G[%s] model-null", k.tag); continue; }
        struct Spec {
            OH_NN_DataType dt; OH_NN_TensorType tt; const int32_t *dims;
            uint32_t rank; const void *data; size_t bytes;
        };
        Spec sp[8];
        int nsp = 0;
        sp[nsp++] = {OH_NN_FLOAT32, OH_NN_TENSOR, SX, 4, nullptr, 0};
        sp[nsp++] = {OH_NN_FLOAT32, OH_NN_TENSOR, SW, 4, nullptr, 0};
        sp[nsp++] = {OH_NN_FLOAT32, OH_NN_TENSOR, SB, 1, nullptr, 0};
        sp[nsp++] = {OH_NN_INT64, OH_NN_CONV2D_STRIDES, pRd, 1, pStr, nStr};
        if (padKind == 1) {
            sp[nsp++] = {OH_NN_INT64, OH_NN_CONV2D_PAD, SD4, 1, PADL, sizeof(PADL)};
        } else if (padKind == 2) {
            sp[nsp++] = {OH_NN_INT8, OH_NN_CONV2D_PAD, SD1, 1, PADM, sizeof(PADM)};
        }
        sp[nsp++] = {OH_NN_INT64, OH_NN_CONV2D_DILATION, pRd, 1, pDil, nDil};
        const int outIdx = nsp;
        sp[nsp++] = {OH_NN_FLOAT32, OH_NN_TENSOR, SO, 4, nullptr, 0};
        bool ok = true;
        for (int i = 0; i < nsp && ok; i++) {
            void *td = f.descNew();
            if (!td) { ok = false; break; }
            ok = f.descSetShape(td, sp[i].dims, sp[i].rank) == OH_NN_SUCCESS &&
                 f.descSetDtype(td, sp[i].dt) == OH_NN_SUCCESS &&
                 f.descSetFormat(td, OH_NN_FORMAT_NONE) == OH_NN_SUCCESS &&
                 f.addTensor(model, td) == OH_NN_SUCCESS;
            f.descDel(&td);
            if (ok && f.setTensorType(model, (uint32_t)i, sp[i].tt) != OH_NN_SUCCESS) { ok = false; }
            if (ok && sp[i].data && sp[i].bytes &&
                f.setTensorData(model, (uint32_t)i, sp[i].data, sp[i].bytes) != OH_NN_SUCCESS) {
                ok = false;
            }
        }
        if (!ok) { f.modelDel(&model); logf("NNRT-P1G[%s] graph-fail", k.tag); continue; }
        uint32_t params[4];
        int np = 0;
        params[np++] = 3;                                            // STRIDES
        if (padKind != 0) { params[np++] = 4; }
        params[np++] = (uint32_t)(4 + (padKind != 0 ? 1 : 0));       // DILATION(始终附带)
        uint32_t ins[3] = {0, 1, 2};
        uint32_t outs[1] = {(uint32_t)outIdx};
        OH_NN_UInt32Array aP = {params, (uint32_t)np};
        OH_NN_UInt32Array aI = {ins, 3};
        OH_NN_UInt32Array aO = {outs, 1};
        const int rcAdd = (int)f.addOp(model, k.op, &aP, &aI, &aO);
        const int rcSpec = (int)f.specify(model, &aI, &aO);
        const int rcFin = (rcAdd == 0) ? (int)f.finish(model) : -1;
        int rcB = -1, rcFp = -2, rcC = -2, rcD = -2, rcP = -2;
        if (rcAdd == 0 && rcSpec == 0 && rcFin == 0) {
            void *comp = f.compNew(model);
            if (comp) {
                // noCache 组跳过 SetCache: 隔离"缓存机制本身是否是失败源"
                if (!k.noCache) {
                    char dir[512];
                    snprintf(dir, sizeof(dir), "%s/nncache_scan", home);
                    mkdir(dir, 0755);   // 父目录必须先建: 目标是二级路径
                    snprintf(dir, sizeof(dir), "%s/nncache_scan/%s", home, k.tag);
                    mkdir(dir, 0755);
                    rcC = (int)f.compSetCache(comp, dir, 1);
                }
                rcD = (int)f.compSetDev(comp, target);
                rcP = (int)f.compPerf(comp, OH_NN_PERFORMANCE_EXTREME);
                // ⚠ SetEnableFp16 的返回码**本身就是诊断**: 它内部先问设备
                //   IsFloat16PrecisionSupported(), 设备不支持时直接返回非 0 —— 无需看图。
                //   rcFp = -3 表示符号在设备上不存在(9030 实测如此, 见 collect 内注释)。
                if (k.fp16) { rcFp = f.compFp16 ? (int)f.compFp16(comp, true) : -3; }
                rcB = (int)f.compBuild(comp);
                f.compDel(&comp);
            }
        }
        logf("NNRT-P1G[%s] op=%d nhwc=%d srank=%d pad=%d nparam=%d "
             "set={c:%d d:%d p:%d fp16:%d} graph=%d/%d/%d build=%d (0=成功)",
             k.tag, (int)k.op, k.nhwc, k.strideRank, padKind, np,
             rcC, rcD, rcP, rcFp, rcAdd, rcSpec, rcFin, rcB);
        f.modelDel(&model);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// P1-d: C++ 侧最底层探针(绕开 Python 层)
//   背景(2026-09-12 实判): selftest 里 `torch.empty(device="nnrt")` 必崩, 且 fallback
//   日志(NNRT-FB)一行都没有 —— 即崩点在**抵达 backend kernel 之前**。
//   本段用 C++ 逐层下行(allocator → allocator.allocate → at::empty → in-place op → sum),
//   与 Python 路径构成二分: 本段也崩 ⇒ dispatch/allocator 层; 本段全过 ⇒ Python 包装层。
// ─────────────────────────────────────────────────────────────────────────────
void probe_cpp_basic()
{
    // c0: 注册**后**的纯 CPU 基线(与 _register 里 P1-d0 的注册前基线对照, 见其注释)
    try {
        logf("NNRT-P1D c0a reg-after cpu-ones");
        auto cp = at::ones({4, 4}, at::kFloat);
        logf("NNRT-P1D c0b cpu-ones-ok");
        auto cps = cp.sum();
        logf("NNRT-P1D c0c cpu-sum-ok");
        logf("NNRT-P1D c0d cpu-sum=%.1f", (double)cps.item<float>());
    } catch (const std::exception &e) {
        logf("NNRT-P1D c0 EX %s", e.what());
    } catch (...) {
        logf("NNRT-P1D c0 EX unknown");
    }
    try {
        logf("NNRT-P1D c1 before GetAllocator(PrivateUse1)");
        c10::Allocator *al = c10::GetAllocator(c10::DeviceType::PrivateUse1);
        logf("NNRT-P1D c2 allocator=%s", al ? "nonnull" : "NULL");
        if (al) {
            c10::DataPtr dp = al->allocate(64);
            logf("NNRT-P1D c3 allocate=%s", dp.get() ? "ok" : "NULL");
        }
    } catch (const std::exception &e) {
        logf("NNRT-P1D c3 EX %s", e.what());
    } catch (...) {
        logf("NNRT-P1D c3 EX unknown");
    }
    try {
        logf("NNRT-P1D c4 before at::empty(PrivateUse1)");
        auto opts = at::TensorOptions().dtype(at::kFloat)
                        .device(c10::DeviceType::PrivateUse1);
        auto t = at::empty({4, 4}, opts);
        logf("NNRT-P1D c5 empty-ok dim=%lld numel=%lld",
             (long long)t.dim(), (long long)t.numel());

        logf("NNRT-P1D c5x t.device=%s pu1=%d contig=%d",
             t.device().str().c_str(),
             int(t.unsafeGetTensorImpl()->key_set().has(c10::DispatchKey::PrivateUse1)),
             int(t.is_contiguous()));

        // 读路径: 归约(out-of-place, 结果再 retag 回本设备)
        logf("NNRT-P1D c6 before sum()");
        auto s0 = t.sum().item<float>();
        logf("NNRT-P1D c7 sum=%.1f", (double)s0);

        // 纯 out-of-place 元素级
        logf("NNRT-P1D c8 before add-scalar(out-of-place)");
        auto t2 = t + 1.0f;
        logf("NNRT-P1D c9 add-ok numel=%lld", (long long)t2.numel());

        // 单张量 vs 双张量: mm/matmul 卡而 sum/add-scalar 通 —— 变量是"stack 里几个本设备张量"
        logf("NNRT-P1D c9b before add(PU1,PU1)");
        auto tadd = t + t;
        logf("NNRT-P1D c9c add-pu1-pu1-ok numel=%lld", (long long)tadd.numel());

        // in-place 写路径(输入经 shallowRetag 贴 CPU 标签后交 CPU)
        logf("NNRT-P1D c10 before fill_(in-place)");
        t.fill_(1.0f);
        logf("NNRT-P1D c11 fill-ok");

        logf("NNRT-P1D c12 before sum-after-fill");
        auto s1 = t.sum().item<float>();
        logf("NNRT-P1D c13 sum-after=%.1f (期望 16.0)", (double)s1);

        // ── 分离实验: 已下沉算子(matmul) —— 不经 fallback, 直接命中本后端 kernel ──
        //   放在 fallback 类算子之后: 若上面全通, 这里的结果才拿得到。
        //   填值走裸内存写(绕开 dispatch), 免得又被 fallback 卡住。
        for (int64_t i = 0; i < t.numel(); i++) {
            t.data_ptr<float>()[i] = 1.0f;
        }
        logf("NNRT-P1D c14 memory-filled");
        // mm = 真正的 backend 算子(已下沉); matmul = composite, 应 polyfill 到 mm
        logf("NNRT-P1D c15 before mm(PU1,PU1)");
        auto tm = at::mm(t, t);
        logf("NNRT-P1D c16 mm-ok dim=%lld val=%.4f (期望 4.0)",
             (long long)tm.dim(), (double)tm.data_ptr<float>()[0]);
        logf("NNRT-P1D c17 before matmul(PU1,PU1)");
        auto t3 = at::matmul(t, t);
        logf("NNRT-P1D c18 matmul-ok dim=%lld val=%.4f (期望 4.0)",
             (long long)t3.dim(), (double)t3.data_ptr<float>()[0]);

        // ── P1-3 判据: NPU × CPU 数值对拍 ──────────────────────────────────
        //   c16/c18 用"全 1 矩阵"只是特例(期望值可心算, 对不出精度)。这里用随机数据做真对拍:
        //   相对误差 < 1e-3 才算 NPU 算得对。
        //   ⚠ 全 1 特例曾经掩盖过一个真 bug: 那时 NNRt 拿的是别的图的编译缓存, 输出恒为
        //     2.0(=1.0+1.0), 而"期望 4.0"的断言恰好把它暴露出来 —— 所以特例也要断言。
        {
            // ⚠ 参考解用 fp64, 而不是 CPU 的 fp32: 否则分不清"NPU 精度差"与"CPU 参考自己就有
            //   累加误差"。同时给 CPU-fp32-vs-fp64 作对照 —— 若两者同量级, 问题在参考不在 NPU。
            //   K=64 作对照: 累加型精度损失会随 K 放大, 而结构性错误不会。
            auto &eng = nnrt::Engine::inst();
            // 造本设备张量: from_blob + 提升设备标签(与 backend.cpp 的搬运同法),
            //   避免依赖 .to(device) 的 fallback 语义(那条路另有测试覆盖)。
            auto mkDev2 = [](const at::Tensor &cpu) {
                auto holder = std::make_shared<at::Tensor>(cpu);
                return at::from_blob(cpu.data_ptr(), cpu.sizes(), cpu.strides(),
                                     [holder](void *) {},
                                     cpu.options().device(c10::DeviceType::PrivateUse1));
            };
            auto compare = [&](int64_t K, const char *tag) {
                auto aX = at::randn({8, K}, at::TensorOptions().dtype(at::kFloat));
                auto bX = at::randn({K, 8}, at::TensorOptions().dtype(at::kFloat));
                auto ref64 = at::mm(aX.to(at::kDouble), bX.to(at::kDouble));
                auto ref32 = at::mm(aX, bX);
                auto aDx = mkDev2(aX), bDx = mkDev2(bX);
                auto outX = at::mm(aDx, bDx);
                const float *p = outX.data_ptr<float>();
                const float *pc = ref32.data_ptr<float>();
                const double *q = ref64.data_ptr<double>();
                double mNpu = 0.0, sNpu = 0.0, mCpu = 0.0;
                const int64_t n = outX.numel();
                for (int64_t i = 0; i < n; i++) {
                    const double den = std::fabs(q[i]) > 1e-6 ? std::fabs(q[i]) : 1e-6;
                    const double rN = std::fabs((double)p[i] - q[i]) / den;
                    const double rC = std::fabs((double)pc[i] - q[i]) / den;
                    if (rN > mNpu) { mNpu = rN; }
                    if (rC > mCpu) { mCpu = rC; }
                    sNpu += rN;
                }
                logf("NNRT-P1D c21[%s] npu-vs-fp64 max=%.3e mean=%.3e | cpu-fp32-vs-fp64 max=%.3e",
                     tag, mNpu, sNpu / (double)n, mCpu);
            };
            compare(8, "K8");
            compare(64, "K64");
            // 精度归因(2026-09-12): 实测 NPU 相对误差 ~1e-3 且**与 K 无关**(K8 的 max 甚至比
            //   K64 大) ⇒ 不是累加损失; CPU fp32 参考自身只有 ~2e-6 ⇒ 参考干净。
            //   怀疑是"fp32 输入进 NPU 后被降为 fp16 再算"。验证: 用 2 的幂(0.5/0.25, fp16
            //   可精确表示)作输入, 期望输出恰为 0.5 —— 若结果精确, 则误差全部来自输入降精度的
            //   舍入, 而非算法/累加错误。
            {
                auto aP = at::full({8, 4}, 0.5f, at::TensorOptions().dtype(at::kFloat));
                auto bP = at::full({4, 8}, 0.25f, at::TensorOptions().dtype(at::kFloat));
                auto aPd = mkDev2(aP), bPd = mkDev2(bP);
                auto outP = at::mm(aPd, bPd);
                const float *pp = outP.data_ptr<float>();
                double maxAbs = 0.0;
                for (int64_t i = 0; i < outP.numel(); i++) {
                    const double d = std::fabs((double)pp[i] - 0.5);
                    if (d > maxAbs) { maxAbs = d; }
                }
                logf("NNRT-P1D c23 exact-inputs(0.5x0.25,K=4) maxAbsErr=%.3e 期望0 ⇒ 误差源自输入降精度",
                     maxAbs);
            }
            logf("NNRT-P1D c22 P1-3 判据 maxRel<1e-3(SD 级出图可放到 1e-2, 见结论行)");

            // ── P1-4: SOFTMAX 下沉 NNRt(对照 fp64 参考) ──────────────────────
            //   与 mm 同理: 下沉的是 backend 算子 `aten::_softmax`, composite 的 `aten::softmax`
            //   会 polyfill 到它 —— 所以这里调 at::softmax 即可覆盖两条路径。
            {
                auto xC = at::randn({4, 8}, at::TensorOptions().dtype(at::kFloat));
                auto refS = at::softmax(xC.to(at::kDouble), 1);
                auto xD = mkDev2(xC);
                logf("NNRT-P1D c24 before softmax(PU1,4x8)");
                auto yD = at::softmax(xD, 1);
                const float *py = yD.data_ptr<float>();
                const double *pr = refS.data_ptr<double>();
                double maxAbs = 0.0, sumAbs = 0.0;
                const int64_t n = yD.numel();
                for (int64_t i = 0; i < n; i++) {
                    const double d = std::fabs((double)py[i] - pr[i]);
                    if (d > maxAbs) { maxAbs = d; }
                    sumAbs += d;
                }
                // softmax 输出恒在 [0,1] 且每行和为 1, 故用绝对误差判定更直观。
                //   fp16 精度下 exp 的舍入比矩阵乘更明显, 阈值取 5e-3(与 mean 判据同量级)。
                logf("NNRT-P1D c25 softmax maxAbs=%.3e meanAbs=%.3e", maxAbs, sumAbs / (double)n);
                logf("NNRT-P1D c26 softmax %s (判据 maxAbs<5e-3)", maxAbs < 5e-3 ? "PASS" : "FAIL");
            }

            // ── P1-4: CONV2D 下沉 NNRt(对照 fp64 参考) ───────────────────────
            //   走 at::conv2d → composite → polyfill 到 aten::convolution(我们下沉的那个)。
            //   非转置 / groups=1 / dilation=1 / 带 bias, 小尺寸(1,2,6,6)@(3,2,3,3) 便于人工核对。
            //   ⚠ 2026-09-12 澄清: 之前把它读成"compBuild **挂起**", 实为 **compBuild 返回
            //     OH_NN_FAILED**(e3b build-rc=1), 随后在清理阶段因销毁顺序错误而崩溃(已修);
            //     两者日志都是"没有 e4", 极易混淆 —— 故 e3b 的返回码日志是必需的。
            //   补传 dilation 无效(仍 rc=1), 说明不是"参数漏传"。故增设 P1-g 对照(绕过 Engine
            //   手工复刻 POC 构图, 见上) + 下表逐组测试。
            //   ⚠ **每组独立 try/catch**: 某组的 kernel TORCH_CHECK 抛异常不再吃掉后面的组。
            //   2026-09-12 升级为**参数扫描**(6 组: 基线/无pad/padMode/极小图/深度卷积/转置卷积),
            //   目的是判定"设备侧不支持 conv"还是"某种参数表达"的问题 —— 单一基线已证伪不了。
            probe_conv_scan();    // P1-g: 结果见 NNRT-P1G[...] 行
            probe_op_support();   // P1-h: 结果见 NNRT-P1H[...] 行(设备支持性的直接证据)
            {
                struct CvCase { const char *tag; int64_t c, h, w, oc, kh, kw, stride; };
                const CvCase cs[] = {
                    {"A-poc-repl", 3, 8, 8, 4, 3, 3, 2},
                    {"B-inC2-s2",  2, 6, 6, 3, 3, 3, 2},
                    {"C-inC3-s1",  3, 6, 6, 3, 3, 3, 1},
                    {"D-original", 2, 6, 6, 3, 3, 3, 1},
                };
                for (const CvCase &k : cs) {
                    try {
                        auto xC = at::randn({1, k.c, k.h, k.w}, at::TensorOptions().dtype(at::kFloat));
                        auto wC = at::randn({k.oc, k.c, k.kh, k.kw}, at::TensorOptions().dtype(at::kFloat));
                        auto bC = at::randn({k.oc}, at::TensorOptions().dtype(at::kFloat));
                        auto refV = at::conv2d(xC.to(at::kDouble), wC.to(at::kDouble), bC.to(at::kDouble),
                                               {k.stride, k.stride});
                        auto xD = mkDev2(xC), wD = mkDev2(wC), bD = mkDev2(bC);
                        logf("NNRT-P1D c27[%s] before conv2d(PU1, 1x%lldx%lldx%lld * %lldx%lldx%lldx%lld s=%lld)",
                             k.tag, (long long)k.c, (long long)k.h, (long long)k.w,
                             (long long)k.oc, (long long)k.c, (long long)k.kh, (long long)k.kw,
                             (long long)k.stride);
                        auto yV = at::conv2d(xD, wD, bD, {k.stride, k.stride});
                        // 分段: 2026-09-12 曾在上一行**无异常、无日志**地让 comfy_child 消失
                        //   (信号级崩溃)。这行用来区分"崩在 conv2d 内"与"崩在之后的对拍"。
                        logf("NNRT-P1D c27b[%s] conv2d-returned numel=%lld",
                             k.tag, (long long)yV.numel());
                        const float *pv = yV.data_ptr<float>();
                        const double *pr = refV.data_ptr<double>();
                        double maxAbs = 0.0, sumAbs = 0.0;
                        const int64_t n = yV.numel();
                        for (int64_t i = 0; i < n; i++) {
                            const double d = std::fabs((double)pv[i] - pr[i]);
                            if (d > maxAbs) { maxAbs = d; }
                            sumAbs += d;
                        }
                        logf("NNRT-P1D c28[%s] conv2d outNumel=%lld (期望 %lld) maxAbs=%.3e meanAbs=%.3e",
                             k.tag, (long long)n, (long long)refV.numel(), maxAbs, sumAbs / (double)n);
                        logf("NNRT-P1D c29[%s] conv2d %s (判据 maxAbs<5e-3)",
                             k.tag, maxAbs < 5e-3 ? "PASS" : "FAIL");
                    } catch (const std::exception &e) {
                        logf("NNRT-P1D c29[%s] EX %s", k.tag, e.what());
                    } catch (...) {
                        logf("NNRT-P1D c29[%s] EX unknown", k.tag);
                    }
                }
            }
            (void)eng;
        }
    } catch (const std::exception &e) {
        logf("NNRT-P1D EX %s", e.what());
    } catch (...) {
        logf("NNRT-P1D EX unknown");
    }
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

    // ── P1-d0: 注册**前**的纯 CPU 算子基线 ──
    //   与注册后的 probe_cpp_basic 构成对照, 用于二分: 同一段 CPU 算子链若"注册前通过、
    //   注册后卡死" ⇒ 是本后端注册带来的副作用; 若两处都卡 ⇒ 是此刻环境/时机的问题
    //   (即 _register 执行点 = torch/__init__.py 导入中途, 某些算子尚不可用)。
    //   2026-09-12 背景: at::empty(本设备) 通过, 但 stack 里带张量的算子(sum/fill_)卡死。
    try {
        write_log_raw("NNRT-P1D0 a1 before-reg cpu-ones");
        auto cp = at::ones({4, 4}, at::kFloat);
        write_log_raw("NNRT-P1D0 a2 cpu-ones-ok");
        auto cps = cp.sum();
        write_log_raw("NNRT-P1D0 a3 cpu-sum-ok");
        write_log_raw(("NNRT-P1D0 a4 cpu-sum=" + std::to_string(cps.item<float>())).c_str());
    } catch (const std::exception &e) {
        write_log_raw((std::string("NNRT-P1D0 EX ") + e.what()).c_str());
    } catch (...) {
        write_log_raw("NNRT-P1D0 EX unknown");
    }

    const char *home = getenv("PYTHONHOME");

    // ── P1-b: PrivateUse1 设备后端注册 ──
    //   顺序有讲究: allocator/hooks/guard 先就位, 再做 Python 侧设备改名 —— 改名后
    //   任何 "nnrt" 用法会立刻走后端, 组件没齐就会崩。guard 是静态注册(见 backend.cpp)。
    try {
        nnrt_backend::registerBackend();
        write_log_raw("NNRT-P1B s1 backend-registered(allocator+hooks+guard)");
    } catch (const std::exception &e) {
        write_log_raw((std::string("NNRT-P1B s1 EX ") + e.what()).c_str());
    } catch (...) {
        write_log_raw("NNRT-P1B s1 EX unknown");
    }

    try {
        py::module_::import("torch.utils.backend_registration")
            .attr("rename_privateuse1_backend")("nnrt");
        write_log_raw("NNRT-P1B s2 renamed-to-nnrt");
    } catch (const std::exception &e) {
        write_log_raw((std::string("NNRT-P1B s2 EX ") + e.what()).c_str());
    } catch (...) {
        write_log_raw("NNRT-P1B s2 EX unknown");
    }

    // 调度表诊断: 必须早于一切探针 —— 探针可能卡死, 诊断要先生效(否则又白跑一轮)
    probeDispatchTable();

    if (!home || !*home) {
        write_log_raw("NNRT-P1 s3 no-PYTHONHOME-skip(engine/selftest 全跳过)");
        return;
    }
    const std::string cache = std::string(home) + "/nncache";
    mkdir(cache.c_str(), 0755);   // 已存在=正常

    // ── P1-a: NNRt 共存验证(此时 torch 全栈已在进程内) ──
    //   任何异常都不许逃逸(否则 torch import 炸), 故整段包 try。
    try {
        probe_nnrt(cache.c_str());
    } catch (const std::exception &e) {
        write_log_raw((std::string("NNRT-P1A s5x EX ") + e.what()).c_str());
    } catch (...) {
        write_log_raw("NNRT-P1A s5x EX unknown");
    }

    // ── P1-c: NNRt Engine 就绪 + 门控端到端自检 ──
    //   探针(P1-a)与 Engine 各自 dlopen 同一批库 —— 引用计数共享, 无冲突。
    try {
        auto &eng = nnrt::Engine::inst();
        if (eng.init(cache.c_str())) {
            write_log_raw(("NNRT-P1C s1 engine-ready dev=" + std::to_string(eng.deviceId())).c_str());
        } else {
            write_log_raw((std::string("NNRT-P1C s1 engine-FAILED: ") + eng.err()).c_str());
        }
        probe_cpp_basic();   // 必须先于 selftest: selftest 会崩, C++ 探针结果要先生效
        run_selftest(home);
    } catch (py::error_already_set &e) {
        write_log_raw((std::string("NNRT-P1C pEX ") + e.what()).c_str());
        e.restore();
        PyErr_Clear();
    } catch (const std::exception &e) {
        write_log_raw((std::string("NNRT-P1C EX ") + e.what()).c_str());
    } catch (...) {
        write_log_raw("NNRT-P1C EX unknown");
    }
}

PYBIND11_MODULE(_nnrt_bootstrap, m)
{
    m.doc() = "torch_nnrt bootstrap (P0: 加载与 ATen 调用; P1-a: NNRt 同进程共存)";
    m.def("describe", &describe, "返回 torch 版本与一次真实 ATen 调用结果");
    m.def("_register", &_register, "torch.backends entry point(自动调用, 不抛异常)");
}
