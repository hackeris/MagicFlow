// P0 端侧 NNRt 探针(2026-09-06): 在 NCP 子进程内验证 NNRt 可用性 +
//   动态构图执行内置 ADD 算子(AiCore)。触发: entryParams 含 "npu-probe"
//   (诊断模式, 正常启动路径零影响; P0 验证后建议从 ArkTS 侧摘除触发)。
//
//   已知事实(本文件依据, 勿重查):
//   - NNRt 系统库: libneural_network_runtime.so + libneural_network_core.so
//     (SDK native 提供头文件, 设备系统自带; 子进程 dlopen 即可, App 域可加载)
//   - hdc shell 域无法执行 push 的自编译 ELF(SELinux), 故探针必须由 App
//     进程(本子进程)执行 —— 这正是它与最终 torch 后端同形的地方
//   - 构图路径: OH_NNModel_AddTensor/AddOperation/Finish →
//     OH_NNCompilation(SetDevice/Build) → Executor(AllocateInputMemory/RunSync)
//     —— 不需要 omg 离线 .omc 模型(绕开 DDK 版本代差)
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <climits>
#include <unistd.h>

#include "neural_network_runtime/neural_network_runtime.h"
#include "neural_network_runtime/neural_network_core.h"

namespace {

class Probe {
public:
    explicit Probe(const char *pyroot)
    {
        if (pyroot && pyroot[0]) {
            char path[PATH_MAX * 2];
            snprintf(path, sizeof(path), "%s/npu-probe.log", pyroot);
            fp = fopen(path, "a");
        }
    }
    ~Probe() { if (fp) fclose(fp); }
    void out(const char *fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        if (fp) vfprintf(fp, fmt, ap);
        vfprintf(stdout, fmt, ap);   // stdout 已 freopen 到 diag.log
        va_end(ap);
    }
private:
    FILE *fp = nullptr;
};

bool sym(void *h, const char *name, void **fn)
{
    *fn = dlsym(h, name);
    if (!*fn) { fprintf(stderr, "dlsym(%s) fail: %s\n", name, dlerror()); return false; }
    return true;
}

// 非致命符号(不在系统库即跳过, 用于探测接口裁剪面)
bool sym_opt(void *h, const char *name, void **fn)
{
    *fn = dlsym(h, name);
    return *fn != nullptr;
}

}  // namespace

// 返回 0=成功(NPU 上 ADD 执行通过)
int npu_probe_run(const char *pyroot)
{
    Probe probe(pyroot);
    void *hRt = dlopen("libneural_network_runtime.so", RTLD_NOW | RTLD_GLOBAL);
    void *hCore = dlopen("libneural_network_core.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hRt) { probe.out("NPU-PROBE FAIL: dlopen runtime: %s\n", dlerror()); return 1; }
    if (!hCore) { probe.out("NPU-PROBE FAIL: dlopen core: %s\n", dlerror()); return 1; }

    void *fn = nullptr;
    using F0 = OH_NN_ReturnCode (*)(const size_t **, uint32_t *);
    using F1 = OH_NN_ReturnCode (*)(size_t, const char **);
    using F2 = OH_NN_ReturnCode (*)(size_t, OH_NN_DeviceType *);
    using F3 = OH_NNModel *(*)();
    using F4 = void (*)(OH_NNModel **);
    using F5 = NN_TensorDesc *(*)();
    using F6 = OH_NN_ReturnCode (*)(NN_TensorDesc *, OH_NN_DataType);
    using F7 = OH_NN_ReturnCode (*)(NN_TensorDesc *, const int32_t *, size_t);
    using F8 = OH_NN_ReturnCode (*)(NN_TensorDesc *, OH_NN_Format);
    using F9 = OH_NN_ReturnCode (*)(NN_TensorDesc **);
    using F10 = OH_NN_ReturnCode (*)(OH_NNModel *, const NN_TensorDesc *);
    using F11 = OH_NN_ReturnCode (*)(OH_NNModel *, uint32_t, const void *, size_t);
    using F12 = OH_NN_ReturnCode (*)(OH_NNModel *, OH_NN_OperationType, const OH_NN_UInt32Array *,
                                     const OH_NN_UInt32Array *, const OH_NN_UInt32Array *);
    using F13 = OH_NN_ReturnCode (*)(OH_NNModel *, const OH_NN_UInt32Array *, const OH_NN_UInt32Array *);
    using F14 = OH_NN_ReturnCode (*)(OH_NNModel *);
    using F15 = OH_NNCompilation *(*)(const OH_NNModel *);
    using F16 = OH_NN_ReturnCode (*)(OH_NNCompilation *, size_t);
    using F17 = OH_NN_ReturnCode (*)(OH_NNCompilation *);
    using F18 = void (*)(OH_NNCompilation **);
    using F19 = OH_NNExecutor *(*)(OH_NNCompilation *);
    using F20 = void (*)(OH_NNExecutor **);
    using F21 = OH_NN_Memory *(*)(OH_NNExecutor *, uint32_t, size_t);
    using F22 = OH_NN_ReturnCode (*)(OH_NNExecutor *, uint32_t, const OH_NN_Memory *);
    using F23 = OH_NN_ReturnCode (*)(OH_NNExecutor *);
    using F24 = void (*)(OH_NN_Memory **);
    using F25 = OH_NN_ReturnCode (*)(const OH_NN_Memory *, void **, size_t *);

    // 设备枚举: 必须有的符号(runtime 库)
    bool ok = true;
    ok &= sym(hRt, "OH_NNDevice_GetAllDevicesID", &fn); F0 f0 = (F0)fn;
    ok &= sym(hCore, "OH_NNDevice_GetName", &fn); F1 f1 = (F1)fn;
    ok &= sym(hCore, "OH_NNDevice_GetType", &fn); F2 f2 = (F2)fn;
    if (!ok) { probe.out("NPU-PROBE FAIL: device enum symbols missing\n"); return 1; }
    // 构图/执行: 系统库可能裁剪(商用端侧只允许离线 .omc 模型) —— 非致命
    bool okC = true;
    okC &= sym_opt(hCore, "OH_NNModel_Construct", &fn); F3 f3 = (F3)fn;
    okC &= sym_opt(hCore, "OH_NNModel_Destroy", &fn); F4 f4 = (F4)fn;
    okC &= sym_opt(hCore, "OH_NNTensorDesc_Create", &fn); F5 f5 = (F5)fn;
    okC &= sym_opt(hCore, "OH_NNTensorDesc_SetDataType", &fn); F6 f6 = (F6)fn;
    okC &= sym_opt(hCore, "OH_NNTensorDesc_SetShape", &fn); F7 f7 = (F7)fn;
    okC &= sym_opt(hCore, "OH_NNTensorDesc_SetFormat", &fn); F8 f8 = (F8)fn;
    okC &= sym_opt(hCore, "OH_NNTensorDesc_Destroy", &fn); F9 f9 = (F9)fn;
    okC &= sym_opt(hCore, "OH_NNModel_AddTensor", &fn); F10 f10 = (F10)fn;
    okC &= sym_opt(hCore, "OH_NNModel_SetTensorData", &fn); F11 f11 = (F11)fn;
    okC &= sym_opt(hCore, "OH_NNModel_AddOperation", &fn); F12 f12 = (F12)fn;
    okC &= sym_opt(hCore, "OH_NNModel_SpecifyInputsAndOutputs", &fn); F13 f13 = (F13)fn;
    okC &= sym_opt(hCore, "OH_NNModel_Finish", &fn); F14 f14 = (F14)fn;
    okC &= sym_opt(hCore, "OH_NNCompilation_Construct", &fn); F15 f15 = (F15)fn;
    okC &= sym_opt(hCore, "OH_NNCompilation_SetDevice", &fn); F16 f16 = (F16)fn;
    okC &= sym_opt(hCore, "OH_NNCompilation_Build", &fn); F17 f17 = (F17)fn;
    okC &= sym_opt(hCore, "OH_NNCompilation_Destroy", &fn); F18 f18 = (F18)fn;
    okC &= sym_opt(hCore, "OH_NNExecutor_Construct", &fn); F19 f19 = (F19)fn;
    okC &= sym_opt(hCore, "OH_NNExecutor_Destroy", &fn); F20 f20 = (F20)fn;
    okC &= sym_opt(hCore, "OH_NNExecutor_AllocateInputMemory", &fn); F21 f21 = (F21)fn;
    okC &= sym_opt(hCore, "OH_NNExecutor_SetInputWithMemory", &fn); F22 f22 = (F22)fn;
    okC &= sym_opt(hCore, "OH_NNExecutor_RunSync", &fn); F23 f23 = (F23)fn;

    probe.out("NPU-PROBE begin (NNRt, pid=%d)\n", (int)getpid());
    const size_t *ids = nullptr;
    uint32_t n = 0;
    // 探针在 NCP 进程极早期执行, 系统 AI 服务(HIAI)可能尚未就绪(观察 2026-09-06:
    //   首次 rc=0 count=0) → 6×5s 重试探测
    OH_NN_ReturnCode rc = (OH_NN_ReturnCode)-1;
    for (int i = 0; i < 6; i++) {
        rc = f0(&ids, &n);
        probe.out("GetAllDevicesID[%d] rc=%d count=%u\n", i, (int)rc, n);
        if (rc == OH_NN_SUCCESS && n > 0) break;
        if (i < 5) sleep(5);
    }
    if (rc != OH_NN_SUCCESS || n == 0) {
        probe.out("NPU-PROBE FAIL: no NNRt device visible from this process\n");
        return 2;
    }
    size_t target = ids[0];
    for (uint32_t i = 0; i < n; i++) {
        const char *nm = nullptr;
        OH_NN_DeviceType tp = (OH_NN_DeviceType)-1;
        f1(ids[i], &nm);
        f2(ids[i], &tp);
        probe.out("  device[%u] id=%zu name=%s type=%d\n", i, ids[i], nm ? nm : "?", (int)tp);
        if (nm && strstr(nm, "HIAI_F")) { target = ids[i]; }
    }
    probe.out(">> target device id=%zu\n", target);

    if (!okC) {
        probe.out("NPU-PROBE INFO: MODEL/EXEC symbols absent in /system lib (硬件裁剪面: 仅离线 .omc 模型路径)\n");
        probe.out("NPU-PROBE RESULT (partial): DEVICE-VISIBLE, offline-model-only\n");
        return 3;
    }

    // 构图: 2 输入 + ADD + 1 输出(fp32; Build 不支持则降 fp16)
    int result = 5;
    for (int dti = 0; dti < 2 && result != 0; dti++) {
        OH_NN_DataType dt = (dti == 0) ? OH_NN_FLOAT32 : OH_NN_FLOAT16;
        probe.out("== try %s ==\n", dti == 0 ? "FP32" : "FP16");
        OH_NNModel *model = f3();
        NN_TensorDesc *desc = f5();
        int32_t shape[1] = { 2 };
        f6(desc, dt);
        f7(desc, shape, 1);
        f8(desc, OH_NN_FORMAT_NCHW);
        f10(model, desc);
        f10(model, desc);
        f10(model, desc);
        float xs[2] = { 1.0f, 2.0f };
        f11(model, 0, xs, sizeof(xs));
        f11(model, 1, xs, sizeof(xs));
        uint32_t in_idx[2] = { 0, 1 };
        uint32_t out_idx[1] = { 2 };
        OH_NN_UInt32Array inputs = { in_idx, 2 };
        OH_NN_UInt32Array outputs = { out_idx, 1 };
        OH_NN_UInt32Array params = { nullptr, 0 };
        OH_NN_ReturnCode r = f12(model, OH_NN_OPS_ADD, &params, &inputs, &outputs);
        if (r == OH_NN_SUCCESS) r = f13(model, &inputs, &outputs);
        OH_NNCompilation *comp = nullptr;
        if (r == OH_NN_SUCCESS) r = f14(model);
        if (r == OH_NN_SUCCESS) comp = f15(model);
        if (comp && r == OH_NN_SUCCESS) r = f16(comp, target);
        if (comp && r == OH_NN_SUCCESS) r = f17(comp);
        if (r == OH_NN_SUCCESS) {
            OH_NNExecutor *exec = f19(comp);
            if (!exec) { r = (OH_NN_ReturnCode)-1; }
            else {
                OH_NN_Memory *mIn = f21(exec, 0, sizeof(xs));
                OH_NN_Memory *mIn2 = f21(exec, 1, sizeof(xs));
                OH_NN_Memory *mOut = f21(exec, 0, sizeof(xs));
                if (!mIn || !mIn2 || !mOut) { r = (OH_NN_ReturnCode)-1; }
                else {
                    // OH_NN_Memory{data,length} —— struct 直读(老库的 MemoryDestroy/GetData
                    // 符号不存在, 也不需要; 探针不释放(短命进程))
                    void *pIn = (void *)mIn->data;
                    size_t nIn = mIn->length;
                    if (pIn && nIn == sizeof(xs)) memcpy(pIn, xs, sizeof(xs));
                    f22(exec, 0, mIn);
                    f22(exec, 1, mIn2);
                    f22(exec, 0, mOut);
                    r = f23(exec);
                    void *pOut = (void *)mOut->data;
                    size_t nOut = mOut->length;
                    float g0 = -1.0f, g1 = -1.0f;
                    if (dt == OH_NN_FLOAT32 && pOut && nOut >= 8) {
                        g0 = ((float *)pOut)[0]; g1 = ((float *)pOut)[1];
                    } else if (pOut && nOut >= 4) {
                        // fp16 半精: 打印原始位(诊断)
                        probe.out("  fp16 out raw: %u %u\n", ((uint16_t *)pOut)[0], ((uint16_t *)pOut)[1]);
                    }
                    probe.out("  out[0]=%f out[1]=%f (expect 3.0 3.0)\n", g0, g1);
                    probe.out("NPU-PROBE RESULT: %s\n",
                        (r == OH_NN_SUCCESS && g0 == 3.0f && g1 == 3.0f) ? "PASS" : "CHECK-FAIL");
                    if (g0 == 3.0f && g1 == 3.0f && r == OH_NN_SUCCESS) result = 0;
                    // 不释放: 系统库无 MemoryDestroy 符号; 探针进程短命, 泄漏可忽略
                }
                f20(&exec);
            }
        } else {
            probe.out("  FAIL: build chain rc=%d\n", (int)r);
        }
        if (comp) f18(&comp);
        f4(&model);
        f9(&desc);
    }
    return result;
}
