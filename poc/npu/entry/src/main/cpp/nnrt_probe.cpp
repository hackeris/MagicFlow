// nnrt_probe.cpp — poc/npu 独立工程核心探针(2026-09-07)
//
// 目的: 一个独立 demo 工程内, 以多通道实证回答「PyTorch 接入 NPU 后端 POC」的
// 全部关键问题。每个通道输出结构化文本, 由 ArkTS 汇总为 filesDir/npu-poc-report.md。
//
// 通道清单:
//   CH-01 npuEnum       设备枚举(UIAbility 进程, NNRt C API)              → 设备可见性
//   CH-02 npuOpsMatrix  SD 链 33 算子覆盖矩阵(构图→编译→执行三 rc+耗时)   → 内置算子覆盖(P0.1)
//   CH-03 npuPerfRepeat Add 同一图 5 轮 build+run 时序                     → per-op 开销/缓存摊销(P0.2)
//   CH-04 npuOffline    离线 .omc 模型 buffer(预期 Build rc=1 系系统库拒绝)→ 自定义算子离线链
//   CH-05 npuSingleOp   单算子直调(HMS_HiAISingleOp*)候选库/符号存在性探测 → 直调通道 F
//   CH-06 npuStartChild 官方 NCP 子进程内同库枚举(结果文件回传)            → 子进程形态(P1a 对照)
//
// ★ 安全铁律(沿袭主项目教训):
//   1. NNRt 全部 dlopen+dlsym(不链接), 符号缺失 → API-ABSENT 清单, 不拖垮 libentry.so;
//   2. 未知签名函数一律不调用(dlsym 只验证存在性) —— 2026-09-06 GetAvailableOperations
//      按猜测签名调用致真机 SIGSEGV 的教训;
//   3. 每个通道输出固定缓冲(char[]), 与长度边界全部 strncat/snprintf 约束。
//
// 参数规格权威来源: build/nnrt-src/frameworks/native/neural_network_runtime/ops/*_builder.{h,cpp}
//   (OpenHarmony NNRt 源码): 参数张量按 OH_NN_TensorType 区分语义;
//   shape/axis/stride/pad 类参数 = OH_NN_INT64; epsilon/slope = OH_NN_FLOAT32;
//   GELU approximate = OH_NN_BOOL; round/pad mode = OH_NN_INT32; 未传入的参数用 builder 默认值。
//
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include "neural_network_runtime/neural_network_runtime.h"
#include "neural_network_runtime/neural_network_core.h"

namespace {

void *g_rt = nullptr;
void *g_core = nullptr;
char g_missing[1024] = "";

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

// 单调时钟 ms(用于 per-op 计时; POC 精度足够)
long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void write_n(const char *s, char *out, size_t outsz)
{
    size_t l = strlen(out);
    if (l < outsz - 1) strncat(out, s, outsz - l - 1);
}

}  // namespace

// ── CH-01 设备枚举 ─────────────────────────────────────────────────────────
// 判据: count>0 且存在 NPU_/Kirin 真实硬件名 → 设备对三方 App(UIAbility 进程)开放
extern "C" int nnrt_ch_enum(char *out, size_t outsz)
{
    g_missing[0] = '\0';
    g_rt = dlopen("libneural_network_runtime.so", RTLD_NOW | RTLD_GLOBAL);
    g_core = dlopen("libneural_network_core.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_rt || !g_core) {
        snprintf(out, outsz, "[CH-01] dlopen fail rt=%p core=%p\n", g_rt, g_core);
        return 1;
    }
    using FDev = OH_NN_ReturnCode (*)(const size_t **, uint32_t *);
    using FName = OH_NN_ReturnCode (*)(size_t, const char **);
    using FType = OH_NN_ReturnCode (*)(size_t, OH_NN_DeviceType *);
    FDev fDev = (FDev)need("OH_NNDevice_GetAllDevicesID");
    FName fName = (FName)need("OH_NNDevice_GetName");
    FType fType = (FType)need("OH_NNDevice_GetType");
    if (g_missing[0]) { snprintf(out, outsz, "[CH-01] API-ABSENT [%s]\n", g_missing); return 2; }

    // 启动早期竞态: 首次 dlopen 后 backend 注册(HDI proxy 获取)为异步, 立即查询可能 count=0;
    // 实测(2026-09-07 v2): 同一进程稍后(CH-02)再查即有设备 —— CH-01 补 3×500ms 重试
    const size_t *ids = nullptr;
    uint32_t n = 0;
    OH_NN_ReturnCode rc = OH_NN_FAILED;
    for (int attempt = 0; attempt < 3; attempt++) {
        rc = fDev(&ids, &n);
        if (rc == OH_NN_SUCCESS && n > 0) break;
        usleep(500 * 1000);
    }
    snprintf(out, outsz, "[CH-01] devices: rc=%d count=%u\n", (int)rc, n);
    char line[256];
    for (uint32_t i = 0; i < n && i < 8; i++) {
        const char *nm = nullptr;
        OH_NN_DeviceType tp = (OH_NN_DeviceType)-1;
        fName(ids[i], &nm);
        fType(ids[i], &tp);
        snprintf(line, sizeof(line), "  [%u] id=%zu name=%s type=%d\n", i, ids[i],
                 nm ? nm : "?", (int)tp);
        write_n(line, out, outsz);
    }
    if (n == 0) write_n("  RESULT: NO DEVICE VISIBLE(进程无法访问 NNRt)\n", out, outsz);
    else write_n("  RESULT: device visible\n", out, outsz);
    return 0;
}

// ── CH-02/04 通用机(下一节) ───────────────────────────────────────────────
// 单算子执行器所需函数集(一次收集, 缺一进入 missing 清单)
struct RtFns {
    // 模型
    void *(*fModelNew)();
    void (*fModelDel)(void **);
    OH_NN_ReturnCode (*fAddTensor)(void *, const void *);
    OH_NN_ReturnCode (*fSetTensorType)(void *, uint32_t, OH_NN_TensorType);
    OH_NN_ReturnCode (*fSetTensorData)(void *, uint32_t, const void *, size_t);
    OH_NN_ReturnCode (*fAddOp)(void *, OH_NN_OperationType, const void *, const void *, const void *);
    OH_NN_ReturnCode (*fSpecify)(void *, const void *, const void *);
    OH_NN_ReturnCode (*fFinish)(void *);
    // desc
    void *(*fDescNew)();
    OH_NN_ReturnCode (*fDescSetShape)(void *, const int32_t *, size_t);
    OH_NN_ReturnCode (*fDescSetDtype)(void *, OH_NN_DataType);
    OH_NN_ReturnCode (*fDescSetFormat)(void *, OH_NN_Format);
    OH_NN_ReturnCode (*fDescDel)(void **);
    // 编译
    void *(*fCompNew)(const void *);
    OH_NN_ReturnCode (*fCompSetCache)(void *, const char *, uint32_t);
    OH_NN_ReturnCode (*fCompSetDev)(void *, size_t);
    OH_NN_ReturnCode (*fCompPerf)(void *, OH_NN_PerformanceMode);
    OH_NN_ReturnCode (*fCompBuild)(void *);
    OH_NN_ReturnCode (*fCompDel)(void **);
    // 执行器
    void *(*fExecNew)(void *);
    OH_NN_ReturnCode (*fExecCount)(const void *, size_t *);
    OH_NN_ReturnCode (*fExecInCount)(const void *, size_t *);   // 执行器实际输入数(2026-09-07: 单输入 op 必须用它)
    void *(*fExecInDesc)(const void *, size_t);
    void *(*fExecOutDesc)(const void *, size_t);
    OH_NN_ReturnCode (*fExecRun)(void *, void **, size_t, void **, size_t);
    void (*fExecDel)(void **);
    // tensor
    void *(*fTensorNew)(size_t, void *);
    void *(*fTensorBuf)(const void *);
    void *(*fTensorDesc)(const void *);
    OH_NN_ReturnCode (*fTensorDel)(void **);
    // desc 读
    OH_NN_ReturnCode (*fDescGetDataCount)(const void *, size_t *);
    OH_NN_ReturnCode (*fDescGetDataType)(const void *, OH_NN_DataType *);
    OH_NN_ReturnCode (*fDescGetShape)(const void *, int32_t **, size_t *);   // 3 参(头文件实数, 非 4 参!)
};

// 收集全部函数指针; 缺失时返回缺失符号列表到 miss[]; 0=全部就绪
static int collect_rt(RtFns &f, char *miss, size_t missSz)
{
    f.fModelNew = (decltype(f.fModelNew))need("OH_NNModel_Construct");
    f.fModelDel = (decltype(f.fModelDel))need("OH_NNModel_Destroy");
    f.fAddTensor = (decltype(f.fAddTensor))need("OH_NNModel_AddTensorToModel");
    f.fSetTensorType = (decltype(f.fSetTensorType))need("OH_NNModel_SetTensorType");
    f.fSetTensorData = (decltype(f.fSetTensorData))need("OH_NNModel_SetTensorData");
    f.fAddOp = (decltype(f.fAddOp))need("OH_NNModel_AddOperation");
    f.fSpecify = (decltype(f.fSpecify))need("OH_NNModel_SpecifyInputsAndOutputs");
    f.fFinish = (decltype(f.fFinish))need("OH_NNModel_Finish");
    f.fDescNew = (decltype(f.fDescNew))need("OH_NNTensorDesc_Create");
    f.fDescSetShape = (decltype(f.fDescSetShape))need("OH_NNTensorDesc_SetShape");
    f.fDescSetDtype = (decltype(f.fDescSetDtype))need("OH_NNTensorDesc_SetDataType");
    f.fDescSetFormat = (decltype(f.fDescSetFormat))need("OH_NNTensorDesc_SetFormat");
    f.fDescDel = (decltype(f.fDescDel))need("OH_NNTensorDesc_Destroy");
    f.fCompNew = (decltype(f.fCompNew))need("OH_NNCompilation_Construct");
    f.fCompSetCache = (decltype(f.fCompSetCache))need("OH_NNCompilation_SetCache");
    f.fCompSetDev = (decltype(f.fCompSetDev))need("OH_NNCompilation_SetDevice");
    f.fCompPerf = (decltype(f.fCompPerf))need("OH_NNCompilation_SetPerformanceMode");
    f.fCompBuild = (decltype(f.fCompBuild))need("OH_NNCompilation_Build");
    f.fCompDel = (decltype(f.fCompDel))need("OH_NNCompilation_Destroy");
    f.fExecNew = (decltype(f.fExecNew))need("OH_NNExecutor_Construct");
    f.fExecCount = (decltype(f.fExecCount))need("OH_NNExecutor_GetOutputCount");
    f.fExecInCount = (decltype(f.fExecInCount))need("OH_NNExecutor_GetInputCount");
    f.fExecInDesc = (decltype(f.fExecInDesc))need("OH_NNExecutor_CreateInputTensorDesc");
    f.fExecOutDesc = (decltype(f.fExecOutDesc))need("OH_NNExecutor_CreateOutputTensorDesc");
    f.fExecRun = (decltype(f.fExecRun))need("OH_NNExecutor_RunSync");
    f.fExecDel = (decltype(f.fExecDel))need("OH_NNExecutor_Destroy");
    f.fTensorNew = (decltype(f.fTensorNew))need("OH_NNTensor_Create");
    f.fTensorBuf = (decltype(f.fTensorBuf))need("OH_NNTensor_GetDataBuffer");
    f.fTensorDesc = (decltype(f.fTensorDesc))need("OH_NNTensor_GetTensorDesc");
    f.fTensorDel = (decltype(f.fTensorDel))need("OH_NNTensor_Destroy");
    f.fDescGetDataCount = (decltype(f.fDescGetDataCount))need("OH_NNTensorDesc_GetElementCount");
    f.fDescGetDataType = (decltype(f.fDescGetDataType))need("OH_NNTensorDesc_GetDataType");
    f.fDescGetShape = (decltype(f.fDescGetShape))need("OH_NNTensorDesc_GetShape");
    if (g_missing[0] && missSz > 0) {
        snprintf(miss, missSz, "%s", g_missing);
        return 1;
    }
    return 0;
}

// ── 算子描述表 ─────────────────────────────────────────────────────────────
struct TSpec {
    OH_NN_DataType dt;
    OH_NN_TensorType tt;       // OH_NN_TENSOR = 数据/权重; 其他 = 参数型
    const int32_t *dims;
    uint32_t rank;
    const void *data;          // 仅参数张量使用(构图期填参)
    size_t dataLen;
};
struct OpSpec {
    const char *name;
    OH_NN_OperationType op;
    const TSpec *ins; int nIn;       // 数据输入(构图期无数据, 运行期填 1.0)
    const TSpec *params; int nParams;
    const TSpec *out;
};

// 静态参数数据(按 builder 要求 dtype)
static const int64_t I64_0[1] = {0};
static const int64_t I64_1[1] = {1};
static const int64_t I64_3[1] = {3};
static const int64_t I64_1_1[2] = {1, 1};
static const int64_t I64_1_1_2_2[4] = {1, 1, 2, 2};
static const int64_t I64_0_0_0_0[4] = {0, 0, 0, 0};
static const int64_t I64_0_2_3_1[4] = {0, 2, 3, 1};
static const float  F32_1E5[1] = {1e-5f};
static const float  F32_SLOPE[1] = {0.1f};
static const bool   BOOL_TRUE[1] = {true};
static const bool   BOOL_FALSE[1] = {false};
static const int8_t I8_0[1] = {0};

// 形状表
static const int32_t S_1123[4] = {1, 2, 2, 3};
static const int32_t S_1121[4] = {1, 2, 2, 1};
static const int32_t S_1122[4] = {1, 2, 2, 2};
static const int32_t S_3[1] = {3};
static const int32_t S_23[2] = {2, 3};
static const int32_t S_34[2] = {3, 4};
static const int32_t S_24[2] = {2, 4};
static const int32_t S_1_8[2] = {1, 8};
static const int32_t S_8_4[2] = {8, 4};
static const int32_t S_1_4[2] = {1, 4};
static const int32_t S_4[1] = {4};
static const int32_t S_1_4_8[3] = {1, 4, 8};
static const int32_t S_1_1_8[3] = {1, 1, 8};
static const int32_t S_1388[4] = {1, 3, 8, 8};
static const int32_t S_4333[4] = {4, 3, 3, 3};
static const int32_t S_3133[4] = {3, 1, 3, 3};
static const int32_t S_1433[4] = {1, 4, 3, 3};
static const int32_t S_1444[4] = {1, 4, 4, 4};
static const int32_t S_141717[4] = {1, 4, 17, 17};

#define T_F32(dims, rank) {OH_NN_FLOAT32, OH_NN_TENSOR, (dims), (rank), nullptr, 0}
#define T_TENSOR(dims, rank) T_F32(dims, rank)   // 数据/权重: 统一 float32

// 参数张量的 shape: 同一维(rank=1), 元素数 = data 元素数(实测 SetTensorData 会按 shape 元素数校验,
//  2026-09-07 用 reinterpret int64→int32 视口当 dims 曾致 shape 元素数错 → addTensor(pX) fail)
static const int32_t D1[1] = {1};
static const int32_t D2[1] = {2};
static const int32_t D4[1] = {4};

// SD 链 33 项(来源 docs/torch-backend-cann.md §4: UNet/CLIP/VAE/KSampler 主算子)
static const TSpec P_STRIDES_1122 = {OH_NN_INT64, OH_NN_CONV2D_STRIDES, D4, 1, I64_1_1_2_2, sizeof(I64_1_1_2_2)};
static const TSpec P_PAD_0000     = {OH_NN_INT64, OH_NN_CONV2D_PAD, D4, 1, I64_0_0_0_0, sizeof(I64_0_0_0_0)};
static const TSpec P_AXIS1        = {OH_NN_INT64, OH_NN_SOFTMAX_AXIS, D1, 1, I64_1, sizeof(I64_1)};

static const TSpec D_1123[2] = {T_F32(S_1123, 4), T_F32(S_1123, 4)};
static const TSpec O_1123 = T_F32(S_1123, 4);
static const TSpec O_1444 = T_F32(S_1444, 4);

struct OpsTable { const OpSpec *specs; int n; };
#define OP_SPEC(name, op, ins, nin, params, np, out) {name, op, ins, nin, params, np, out}

static const TSpec BIAS_INS[2] = {T_F32(S_1123, 4), T_F32(S_3, 1)};
static const TSpec LN_INS[3] = {T_F32(S_1_4_8, 3), T_F32(S_1_1_8, 3), T_F32(S_1_1_8, 3)};
static const TSpec LN_PARAMS[3] = {
    {OH_NN_INT64, OH_NN_LAYER_NORM_BEGIN_NORM_AXIS, D1, 1, I64_1, sizeof(I64_1)},
    {OH_NN_FLOAT32, OH_NN_LAYER_NORM_EPSILON, D1, 1, F32_1E5, sizeof(F32_1E5)},
    {OH_NN_INT64, OH_NN_LAYER_NORM_BEGIN_PARAM_AXIS, D1, 1, I64_1, sizeof(I64_1)},
};
static const TSpec L2_PARAMS[2] = {
    {OH_NN_INT64, OH_NN_L2_NORMALIZE_AXIS, D1, 1, I64_1, sizeof(I64_1)},
    {OH_NN_FLOAT32, OH_NN_L2_NORMALIZE_EPSILON, D1, 1, F32_1E5, sizeof(F32_1E5)},
};
// matmul 的 transpose 标志 = OH_NN_BOOL(实测 builder 校验, 非 INT64)
static const TSpec MM_PARAMS[2] = {
    {OH_NN_BOOL, OH_NN_MATMUL_TRANSPOSE_A, D1, 1, BOOL_FALSE, sizeof(BOOL_FALSE)},
    {OH_NN_BOOL, OH_NN_MATMUL_TRANSPOSE_B, D1, 1, BOOL_FALSE, sizeof(BOOL_FALSE)},
};
static const TSpec MM_INS[2] = {T_F32(S_23, 2), T_F32(S_34, 2)};
// full_connection: has_bias/use_axis = OH_NN_BOOL, axis = OH_NN_INT64, activation = OH_NN_INT8
// FC: 单轴标准场景(use_axis=true, 无 bias → 2 输入)
static const TSpec FC_PARAMS[3] = {
    {OH_NN_BOOL, OH_NN_FULL_CONNECTION_USE_AXIS, D1, 1, BOOL_TRUE, sizeof(BOOL_TRUE)},
    {OH_NN_INT64, OH_NN_FULL_CONNECTION_AXIS, D1, 1, I64_1, sizeof(I64_1)},
    {OH_NN_INT8, OH_NN_FULL_CONNECTION_ACTIVATIONTYPE, D1, 1, I8_0, sizeof(I8_0)},
};
static const TSpec FC_INS[2] = {T_F32(S_1_8, 2), T_F32(S_8_4, 2)};
static const TSpec CONV_PARAMS[2] = {P_STRIDES_1122, P_PAD_0000};
static const TSpec CONV_INS[3] = {T_F32(S_1388, 4), T_F32(S_4333, 4), T_F32(S_4, 1)};
static const TSpec DW_INS[3] = {T_F32(S_1388, 4), T_F32(S_3133, 4), T_F32(S_3, 1)};
// conv_transpose/depthwise 均用各自专用参数命名空间(非 CONV2D_*)
static const TSpec CONVT_PARAMS[2] = {
    {OH_NN_INT64, OH_NN_CONV2D_TRANSPOSE_STRIDES, D4, 1, I64_1_1_2_2, sizeof(I64_1_1_2_2)},
    {OH_NN_INT64, OH_NN_CONV2D_TRANSPOSE_PAD, D4, 1, I64_0_0_0_0, sizeof(I64_0_0_0_0)},
};
static const TSpec DW_PARAMS[2] = {
    {OH_NN_INT64, OH_NN_DEPTHWISE_CONV2D_NATIVE_STRIDES, D4, 1, I64_1_1_2_2, sizeof(I64_1_1_2_2)},
    {OH_NN_INT64, OH_NN_DEPTHWISE_CONV2D_NATIVE_PAD, D4, 1, I64_0_0_0_0, sizeof(I64_0_0_0_0)},
};
static const TSpec POOL_PARAMS[2] = {
    {OH_NN_INT64, OH_NN_AVG_POOL_KERNEL_SIZE, D2, 1, I64_1_1, sizeof(I64_1_1)},
    {OH_NN_INT64, OH_NN_AVG_POOL_STRIDE, D2, 1, I64_1_1, sizeof(I64_1_1)},
};
static const TSpec MAXP_PARAMS[2] = {
    {OH_NN_INT64, OH_NN_MAX_POOL_KERNEL_SIZE, D2, 1, I64_1_1, sizeof(I64_1_1)},
    {OH_NN_INT64, OH_NN_MAX_POOL_STRIDE, D2, 1, I64_1_1, sizeof(I64_1_1)},
};
static const TSpec CAT_INS[2] = {T_F32(S_1121, 4), T_F32(S_1121, 4)};
static const TSpec CAT_PARAM[1] = {
    {OH_NN_INT64, OH_NN_CONCAT_AXIS, D1, 1, I64_3, sizeof(I64_3)},
};
static const TSpec UN_IN[1] = {T_F32(S_1123, 4)};
static const TSpec GELU_P[1] = {
    {OH_NN_BOOL, OH_NN_GELU_APPROXIMATE, D1, 1, BOOL_TRUE, sizeof(BOOL_TRUE)},
};
// 各输出的命名静态(避免 C++ 不允许的 compound literal)
static const TSpec O_1448 = T_F32(S_1_4_8, 3);
static const TSpec O_24 = T_F32(S_24, 2);
static const TSpec O_14 = T_F32(S_1_4, 2);
static const TSpec O_1433 = T_F32(S_1433, 4);
static const TSpec O_141717 = T_F32(S_141717, 4);
static const TSpec O_1444b = T_F32(S_1444, 4);
static const TSpec O_1122b = T_F32(S_1122, 4);

static const OpSpec OP_ADD = OP_SPEC("ADD", OH_NN_OPS_ADD, D_1123, 2, nullptr, 0, &O_1123);
static const OpSpec OP_SUB = OP_SPEC("SUB", OH_NN_OPS_SUB, D_1123, 2, nullptr, 0, &O_1123);
static const OpSpec OP_MUL = OP_SPEC("MUL", OH_NN_OPS_MUL, D_1123, 2, nullptr, 0, &O_1123);
static const OpSpec OP_DIV = OP_SPEC("DIV", OH_NN_OPS_DIV, D_1123, 2, nullptr, 0, &O_1123);
static const OpSpec OP_POW = OP_SPEC("POW", OH_NN_OPS_POW, D_1123, 2, nullptr, 0, &O_1123);
static const OpSpec OP_MIN = OP_SPEC("MINIMUM", OH_NN_OPS_MINIMUM, D_1123, 2, nullptr, 0, &O_1123);
static const OpSpec OP_MAX = OP_SPEC("MAXIMUM", OH_NN_OPS_MAXIMUM, D_1123, 2, nullptr, 0, &O_1123);
static const OpSpec OP_BIAS = OP_SPEC("BIAS_ADD", OH_NN_OPS_BIAS_ADD, BIAS_INS, 2, nullptr, 0, &O_1123);
static const OpSpec OP_SQD = OP_SPEC("SQUARED_DIFFERENCE", OH_NN_OPS_SQUARED_DIFFERENCE, D_1123, 2, nullptr, 0, &O_1123);
static const OpSpec OP_MOD = OP_SPEC("MOD", OH_NN_OPS_MOD, D_1123, 2, nullptr, 0, &O_1123);
static const OpSpec OP_RELU = OP_SPEC("RELU", OH_NN_OPS_RELU, UN_IN, 1, nullptr, 0, &O_1123);
static const OpSpec OP_SIGM = OP_SPEC("SIGMOID", OH_NN_OPS_SIGMOID, UN_IN, 1, nullptr, 0, &O_1123);
static const OpSpec OP_TANH = OP_SPEC("TANH", OH_NN_OPS_TANH, UN_IN, 1, nullptr, 0, &O_1123);
static const OpSpec OP_SQRT = OP_SPEC("SQRT", OH_NN_OPS_SQRT, UN_IN, 1, nullptr, 0, &O_1123);
static const OpSpec OP_EXP = OP_SPEC("EXP", OH_NN_OPS_EXP, UN_IN, 1, nullptr, 0, &O_1123);
static const OpSpec OP_LOG = OP_SPEC("LOG", OH_NN_OPS_LOG, UN_IN, 1, nullptr, 0, &O_1123);
static const OpSpec OP_ERF = OP_SPEC("ERF", OH_NN_OPS_ERF, UN_IN, 1, nullptr, 0, &O_1123);
static const OpSpec OP_NEG = OP_SPEC("NEG", OH_NN_OPS_NEG, UN_IN, 1, nullptr, 0, &O_1123);
static const OpSpec OP_ABS = OP_SPEC("ABS", OH_NN_OPS_ABS, UN_IN, 1, nullptr, 0, &O_1123);
static const OpSpec OP_SWISH = OP_SPEC("SWISH(SILU)", OH_NN_OPS_SWISH, UN_IN, 1, nullptr, 0, &O_1123);
static const OpSpec OP_HSWISH = OP_SPEC("HSWISH", OH_NN_OPS_HSWISH, UN_IN, 1, nullptr, 0, &O_1123);
static const OpSpec OP_GELU = OP_SPEC("GELU", OH_NN_OPS_GELU, UN_IN, 1, GELU_P, 1, &O_1123);
static const OpSpec OP_LEAKY = OP_SPEC("LEAKY_RELU", OH_NN_OPS_LEAKY_RELU, UN_IN, 1, nullptr, 0, &O_1123);
static const TSpec LEAKY_P[1] = {
    {OH_NN_FLOAT32, OH_NN_LEAKY_RELU_NEGATIVE_SLOPE, reinterpret_cast<const int32_t *>(F32_SLOPE), 1, F32_SLOPE, sizeof(F32_SLOPE)},
};
static const TSpec SOFTMAX_P[1] = {
    {OH_NN_INT64, OH_NN_SOFTMAX_AXIS, D1, 1, I64_1, sizeof(I64_1)},
};
static const OpSpec OP_SOFTMAX = OP_SPEC("SOFTMAX", OH_NN_OPS_SOFTMAX, UN_IN, 1, SOFTMAX_P, 1, &O_1123);
static const OpSpec OP_L2NORM = OP_SPEC("L2_NORMALIZE", OH_NN_OPS_L2_NORMALIZE, UN_IN, 1, L2_PARAMS, 2, &O_1123);
static const OpSpec OP_LAYERNORM = OP_SPEC("LAYER_NORM", OH_NN_OPS_LAYER_NORM, LN_INS, 3, LN_PARAMS, 3, &O_1448);
static const OpSpec OP_MATMUL = OP_SPEC("MATMUL", OH_NN_OPS_MATMUL, MM_INS, 2, MM_PARAMS, 2, &O_24);
static const OpSpec OP_FC = OP_SPEC("FULL_CONNECTION", OH_NN_OPS_FULL_CONNECTION, FC_INS, 2, FC_PARAMS, 3, &O_14);
static const OpSpec OP_CONV = OP_SPEC("CONV2D", OH_NN_OPS_CONV2D, CONV_INS, 3, CONV_PARAMS, 2, &O_1433);
static const OpSpec OP_CONVT = OP_SPEC("CONV2D_TRANSPOSE", OH_NN_OPS_CONV2D_TRANSPOSE, CONV_INS, 3, CONVT_PARAMS, 2, &O_141717);
static const OpSpec OP_DW = OP_SPEC("DEPTHWISE_CONV2D_NATIVE", OH_NN_OPS_DEPTHWISE_CONV2D_NATIVE, DW_INS, 3, DW_PARAMS, 2, &O_1433);
static const TSpec AVG_IN[1] = {T_F32(S_1388, 4)};
static const OpSpec OP_AVG = OP_SPEC("AVG_POOL", OH_NN_OPS_AVG_POOL, AVG_IN, 1, POOL_PARAMS, 2, &O_1444b);
static const OpSpec OP_MAXP = OP_SPEC("MAX_POOL", OH_NN_OPS_MAX_POOL, AVG_IN, 1, MAXP_PARAMS, 2, &O_1444b);
static const OpSpec OP_CAT = OP_SPEC("CONCAT", OH_NN_OPS_CONCAT, CAT_INS, 2, CAT_PARAM, 1, &O_1122b);

static const OpSpec *const OP_ALL[] = {
    &OP_ADD, &OP_SUB, &OP_MUL, &OP_DIV, &OP_POW, &OP_MIN, &OP_MAX, &OP_BIAS, &OP_SQD, &OP_MOD,
    &OP_RELU, &OP_SIGM, &OP_TANH, &OP_SQRT, &OP_EXP, &OP_LOG, &OP_ERF, &OP_NEG, &OP_ABS,
    &OP_SWISH, &OP_HSWISH, &OP_GELU, &OP_LEAKY,
    &OP_SOFTMAX, &OP_L2NORM, &OP_LAYERNORM, &OP_MATMUL, &OP_FC,
    &OP_CONV, &OP_CONVT, &OP_DW, &OP_AVG, &OP_MAXP, &OP_CAT,
};

// ── 单算子执行器 ───────────────────────────────────────────────────────────
// 返回 0=构图/编译/执行全链成功(OUT: 写该算子一行摘要); <0 = 通道级失败(符号缺失)
static int op_run(RtFns &f, size_t target, const OpSpec &s, const char *cacheDir,
                  char *out, size_t outsz, char *errline, size_t errsz)
{
    errline[0] = '\0';
    OH_NN_ReturnCode rc;
    char line[256];

    void *model = f.fModelNew();
    if (!model) { snprintf(errline, errsz, "model null"); return -1; }

    auto addTensor = [&](const TSpec &t, uint32_t idx) -> bool {
        void *td = f.fDescNew();
        if (!td) return false;
        bool ok = f.fDescSetShape(td, t.dims, t.rank) == OH_NN_SUCCESS &&
                  f.fDescSetDtype(td, t.dt) == OH_NN_SUCCESS &&
                  f.fDescSetFormat(td, OH_NN_FORMAT_NONE) == OH_NN_SUCCESS &&
                  f.fAddTensor(model, td) == OH_NN_SUCCESS &&
                  f.fSetTensorType(model, idx, t.tt) == OH_NN_SUCCESS;
        f.fDescDel(&td);
        if (ok && t.tt != OH_NN_TENSOR && t.data && t.dataLen > 0) {
            ok = f.fSetTensorData(model, idx, t.data, t.dataLen) == OH_NN_SUCCESS;
        }
        return ok;
    };

    uint32_t idx = 0;
    for (int i = 0; i < s.nIn; i++) {
        if (!addTensor(s.ins[i], idx)) { snprintf(errline, errsz, "addTensor(in%d) fail", i); f.fModelDel(&model); return -1; }
        idx++;
    }
    for (int i = 0; i < s.nParams; i++) {
        if (!addTensor(s.params[i], idx)) { snprintf(errline, errsz, "addTensor(p%d) fail", i); f.fModelDel(&model); return -1; }
        idx++;
    }
    if (!addTensor(*s.out, idx)) { snprintf(errline, errsz, "addTensor(out) fail"); f.fModelDel(&model); return -1; }

    // AddOperation: params 索引区间 + inputs/outouts
    uint32_t params[4], ins[3], outs[1];
    for (int i = 0; i < s.nParams; i++) params[i] = (uint32_t)s.nIn + i;
    for (int i = 0; i < s.nIn; i++) ins[i] = (uint32_t)i;
    outs[0] = (uint32_t)(s.nIn + s.nParams);
    OH_NN_UInt32Array arrP = {s.nParams ? params : nullptr, (uint32_t)s.nParams};
    OH_NN_UInt32Array arrI = {ins, (uint32_t)s.nIn};
    OH_NN_UInt32Array arrO = {outs, 1};

    // graph 链分段记录(2026-09-07 9030 复跑: LAYER_NORM graph=2 需区分 AddOp/Specify/Finish 环节)
    int rcAddOp = (int)f.fAddOp(model, s.op, &arrP, &arrI, &arrO);
    int rcSpecify = (int)f.fSpecify(model, &arrI, &arrO);
    int rcFinish = rcAddOp == 0 ? (int)f.fFinish(model) : -1;
    int rcGraph = rcAddOp != 0 ? rcAddOp : (rcSpecify != 0 ? rcSpecify : rcFinish);
    {
        char g[96];
        snprintf(g, sizeof(g), "[g:%d/%d/%d]", rcAddOp, rcSpecify, rcFinish);
        write_n(g, out, outsz);
    }

    // 编译
    void *comp = rcGraph == OH_NN_SUCCESS ? f.fCompNew(model) : nullptr;
    OH_NN_ReturnCode rcBuild = OH_NN_FAILED;
    long long tBuild = 0, tRun = 0;
    if (comp) {
        f.fCompSetCache(comp, cacheDir, 1);
        f.fCompSetDev(comp, target);
        f.fCompPerf(comp, OH_NN_PERFORMANCE_EXTREME);
        long long t0 = now_ms();
        rcBuild = f.fCompBuild(comp);
        tBuild = now_ms() - t0;
    }

    // 执行 —— 按执行器实际输入数 nInAct 动态创建/填充(2026-09-07: GetInputCount 修正单输入 op run=2)
    OH_NN_ReturnCode rcRun = OH_NN_FAILED;
    if (comp && rcBuild == OH_NN_SUCCESS) {
        void *exec = f.fExecNew(comp);
        if (exec) {
            size_t nOut = 0, nInAct = 0;
            f.fExecCount(exec, &nOut);
            f.fExecInCount(exec, &nInAct);
            void *insDesc[4] = {nullptr};
            void *outsDesc[1] = {nullptr};
            for (size_t i = 0; i < nInAct && i < 4; i++) insDesc[i] = f.fExecInDesc(exec, i);
            outsDesc[0] = f.fExecOutDesc(exec, 0);
            // 执行器侧 desc 摘要(形状/dtype 核对)
            {
                char d[128];
                snprintf(d, sizeof(d), "[ex:%zu/%zu", nOut, nInAct);
                write_n(d, out, outsz);
                for (size_t i = 0; i < nInAct && i < 4; i++) {
                    if (!insDesc[i]) { write_n("|in=null", out, outsz); continue; }
                    OH_NN_DataType dt = OH_NN_FLOAT32;
                    size_t rl = 0;
                    int32_t *shp = nullptr;
                    f.fDescGetDataType(insDesc[i], &dt);
                    OH_NN_ReturnCode rs = f.fDescGetShape(insDesc[i], &shp, &rl);
                    char seg[64];
                    snprintf(seg, sizeof(seg), "|in%zu:dt%d:s%d[", i, (int)dt, (int)rs);
                    write_n(seg, out, outsz);
                    for (size_t k = 0; k < rl && k < 8; k++) {
                        char v[16];
                        snprintf(v, sizeof(v), "%s%d", k ? "," : "", shp ? shp[k] : 0);
                        write_n(v, out, outsz);
                    }
                    write_n("]", out, outsz);
                }
                if (outsDesc[0]) {
                    OH_NN_DataType dt = OH_NN_FLOAT32;
                    size_t rl = 0;
                    int32_t *shp = nullptr;
                    f.fDescGetDataType(outsDesc[0], &dt);
                    OH_NN_ReturnCode rs = f.fDescGetShape(outsDesc[0], &shp, &rl);
                    char seg[64];
                    snprintf(seg, sizeof(seg), "|out:dt%d:s%d[", (int)dt, (int)rs);
                    write_n(seg, out, outsz);
                    for (size_t k = 0; k < rl && k < 8; k++) {
                        char v[16];
                        snprintf(v, sizeof(v), "%s%d", k ? "," : "", shp ? shp[k] : 0);
                        write_n(v, out, outsz);
                    }
                    write_n("]]", out, outsz);
                }
            }
            void *tin[4] = {nullptr}, *tout = nullptr;
            for (size_t i = 0; i < nInAct && i < 4; i++)
                if (insDesc[i]) tin[i] = f.fTensorNew(target, insDesc[i]);
            if (outsDesc[0]) tout = f.fTensorNew(target, outsDesc[0]);
            if (tin[0] && tout) {
                // 输入填 1.0(数据张量统一 float32)
                for (size_t i = 0; i < nInAct && i < 4; i++) {
                    if (!tin[i]) continue;
                    size_t cnt = 0;
                    f.fDescGetDataCount(f.fTensorDesc(tin[i]), &cnt);
                    float *p = (float *)f.fTensorBuf(tin[i]);
                    for (size_t k = 0; k < cnt; k++) p[k] = 1.0f;
                }
                long long t1 = now_ms();
                rcRun = f.fExecRun(exec, tin, nInAct, &tout, 1);
                tRun = now_ms() - t1;
            }
            // 输出首 4 元素(预留: 浮点)
            snprintf(line, sizeof(line), " out=");
            write_n(line, out, outsz);
            if (tout && rcRun == OH_NN_SUCCESS) {
                size_t cnt = 0;
                f.fDescGetDataCount(f.fTensorDesc(tout), &cnt);
                float *p = (float *)f.fTensorBuf(tout);
                for (size_t k = 0; k < cnt && k < 4; k++) {
                    snprintf(line, sizeof(line), "%s%.2f", k ? "," : "", p[k]);
                    write_n(line, out, outsz);
                }
            }
            if (tin[0]) f.fTensorDel(&tin[0]);
            if (tin[1]) f.fTensorDel(&tin[1]);
            if (tin[2]) f.fTensorDel(&tin[2]);
            if (tin[3]) f.fTensorDel(&tin[3]);
            if (tout) f.fTensorDel(&tout);
            f.fExecDel(&exec);
        }
    }
    if (comp) f.fCompDel(&comp);
    f.fModelDel(&model);

    // 摘要行: [id] name graph=rc build=rc(ms) run=rc(ms)
    char summary[256];
    snprintf(summary, sizeof(summary), " graph=%d build=%d(%lldms) run=%d(%lldms)",
             (int)rcGraph, (int)rcBuild, tBuild, (int)rcRun, tRun);
    if (rcGraph == OH_NN_SUCCESS && rcBuild == OH_NN_SUCCESS && rcRun == OH_NN_SUCCESS) {
        write_n(" ✓PASS", out, outsz);
    }
    write_n(summary, out, outsz);
    write_n("\n", out, outsz);
    return (rcGraph == OH_NN_SUCCESS && rcBuild == OH_NN_SUCCESS && rcRun == OH_NN_SUCCESS) ? 0 : 1;
}

// ── CH-02 算子覆盖矩阵 ─────────────────────────────────────────────────────
extern "C" int nnrt_ch_ops(char *out, size_t outsz, const char *cacheDir)
{
    g_missing[0] = '\0';
    g_rt = dlopen("libneural_network_runtime.so", RTLD_NOW | RTLD_GLOBAL);
    g_core = dlopen("libneural_network_core.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_rt || !g_core) { snprintf(out, outsz, "[CH-02] dlopen fail rt=%p core=%p\n", g_rt, g_core); return 1; }
    if (cacheDir && cacheDir[0]) mkdir(cacheDir, 0755);

    RtFns f{};
    char miss[1024];
    if (collect_rt(f, miss, sizeof(miss)) != 0) {
        snprintf(out, outsz, "[CH-02] API-ABSENT [%s]\n", miss);
        return 2;
    }

    // 设备选择(优先真实硬件名)
    std::memset(out, 0, outsz);
    const size_t *ids = nullptr;
    uint32_t n = 0;
    using FDev = OH_NN_ReturnCode (*)(const size_t **, uint32_t *);
    using FName = OH_NN_ReturnCode (*)(size_t, const char **);
    FDev fDev = (FDev)symor("OH_NNDevice_GetAllDevicesID");
    FName fName = (FName)symor("OH_NNDevice_GetName");
    if (!fDev || !fName) { snprintf(out, outsz, "[CH-02] enum dlsym fail\n"); return 3; }
    fDev(&ids, &n);
    if (n == 0) { snprintf(out, outsz, "[CH-02] no device\n"); return 4; }
    size_t target = ids[0];
    for (uint32_t i = 0; i < n && i < 4; i++) {
        const char *nm = nullptr;
        fName(ids[i], &nm);
        if (nm && (strstr(nm, "NPU_") || strstr(nm, "Kirin"))) target = ids[i];
    }
    snprintf(out, outsz, "[CH-02] op matrix (target id=%zu, %zu ops, fp32, 1.0-filled inputs)\n", target, sizeof(OP_ALL) / sizeof(OP_ALL[0]));

    int ok = 0, fail = 0;
    char errline[128];
    for (size_t i = 0; i < sizeof(OP_ALL) / sizeof(OP_ALL[0]); i++) {
        char line[256];
        snprintf(line, sizeof(line), "  %02zu %-24s", i, OP_ALL[i]->name);
        write_n(line, out, outsz);
        int r = op_run(f, target, *OP_ALL[i], cacheDir, out, outsz, errline, sizeof(errline));
        if (r == 0) ok++;
        else {
            fail++;
            if (errline[0]) {
                char e[96];
                snprintf(e, sizeof(e), "  # %s", errline);
                write_n(e, out, outsz);
                write_n("\n", out, outsz);
            }
        }
    }
    char tail[128];
    snprintf(tail, sizeof(tail), "[CH-02] RESULT: %d/%zu PASS\n", ok, sizeof(OP_ALL) / sizeof(OP_ALL[0]));
    write_n(tail, out, outsz);
    return 0;
}

// ── CH-03 Add 五轮时序(缓存复用) ───────────────────────────────────────────
extern "C" int nnrt_ch_perf(char *out, size_t outsz, const char *cacheDir)
{
    g_missing[0] = '\0';
    g_rt = dlopen("libneural_network_runtime.so", RTLD_NOW | RTLD_GLOBAL);
    g_core = dlopen("libneural_network_core.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_rt || !g_core) { snprintf(out, outsz, "[CH-03] dlopen fail\n"); return 1; }
    if (cacheDir && cacheDir[0]) mkdir(cacheDir, 0755);
    std::memset(out, 0, outsz);
    snprintf(out, outsz, "[CH-03] ADD 5-round timing (per-op overhead; same graph each round)\n");

    RtFns f{};
    char miss[1024];
    if (collect_rt(f, miss, sizeof(miss)) != 0) {
        snprintf(out, outsz, "[CH-03] API-ABSENT [%s]\n", miss);
        return 2;
    }
    using FDev = OH_NN_ReturnCode (*)(const size_t **, uint32_t *);
    using FName = OH_NN_ReturnCode (*)(size_t, const char **);
    FDev fDev = (FDev)symor("OH_NNDevice_GetAllDevicesID");
    FName fName = (FName)symor("OH_NNDevice_GetName");
    const size_t *ids = nullptr; uint32_t n = 0;
    fDev(&ids, &n);
    if (n == 0) { write_n("[CH-03] no device\n", out, outsz); return 3; }
    size_t target = ids[0];
    for (uint32_t i = 0; i < n && i < 4; i++) {
        const char *nm = nullptr; fName(ids[i], &nm);
        if (nm && (strstr(nm, "NPU_") || strstr(nm, "Kirin"))) target = ids[i];
    }

    char errline[128];
    char pfx[128];
    for (int rnd = 0; rnd < 5; rnd++) {
        snprintf(pfx, sizeof(pfx), "  round %d:", rnd);
        write_n(pfx, out, outsz);
        int r = op_run(f, target, OP_ADD, cacheDir, out, outsz, errline, sizeof(errline));
        char aux[64];
        snprintf(aux, sizeof(aux), "    -> %s\n", r == 0 ? "OK" : "FAIL");
        write_n(aux, out, outsz);
    }
    write_n("[CH-03] note: round>=2 exercises compiler cache (SetCache) reuse path\n", out, outsz);
    return 0;
}

// ── CH-04 离线 .omc(buffer) ────────────────────────────────────────────────
extern "C" int nnrt_ch_offline(char *out, size_t outsz, const void *modelBuf, size_t modelLen,
                               const char *cacheDir)
{
    g_missing[0] = '\0';
    g_rt = dlopen("libneural_network_runtime.so", RTLD_NOW | RTLD_GLOBAL);
    g_core = dlopen("libneural_network_core.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_rt || !g_core) { snprintf(out, outsz, "[CH-04] dlopen fail\n"); return 1; }
    std::memset(out, 0, outsz);
    snprintf(out, outsz, "[CH-04] offline .omc buffer (%zu bytes, AddCustom z=x+y+bias)\n", modelLen);
    using FDev = OH_NN_ReturnCode (*)(const size_t **, uint32_t *);
    using FName = OH_NN_ReturnCode (*)(size_t, const char **);
    using FCompBuf = void *(*)(const void *, size_t);
    using FSetCache = OH_NN_ReturnCode (*)(void *, const char *, uint32_t);
    using FSetDev = OH_NN_ReturnCode (*)(void *, size_t);
    using FBuild = OH_NN_ReturnCode (*)(void *);
    using FDel = OH_NN_ReturnCode (*)(void **);
    using FExecNew = void *(*)(void *);
    using FExecRun = OH_NN_ReturnCode (*)(void *, void **, size_t, void **, size_t);
    using FExecDel = void (*)(void **);
    using FTensorNew = void *(*)(size_t, void *);
    using FDescNew = void *(*)();
    using FDescSetDtype = OH_NN_ReturnCode (*)(void *, OH_NN_DataType);
    using FDescSetShape = OH_NN_ReturnCode (*)(void *, const int32_t *, size_t);
    using FDescSetFormat = OH_NN_ReturnCode (*)(void *, OH_NN_Format);
    using FDescDel = void (*)(void **);
    using FTensorBuf = void *(*)(const void *);
    using FTensorDel = OH_NN_ReturnCode (*)(void **);
    using FExecInDesc = void *(*)(const void *, size_t);
    using FExecOutDesc = void *(*)(const void *, size_t);
    using FDDtype = OH_NN_ReturnCode (*)(const void *, OH_NN_DataType *);
    using FDCnt = OH_NN_ReturnCode (*)(const void *, size_t *);
    using FTDt = void *(*)(const void *);   // OH_NNTensor_GetTensorDesc: 1 参返回指针

    FDev fDev = (FDev)need("OH_NNDevice_GetAllDevicesID");
    FName fName = (FName)need("OH_NNDevice_GetName");
    FCompBuf fCompBuf = (FCompBuf)need("OH_NNCompilation_ConstructWithOfflineModelBuffer");
    FSetCache fSetCache = (FSetCache)need("OH_NNCompilation_SetCache");
    FSetDev fSetDev = (FSetDev)need("OH_NNCompilation_SetDevice");
    FBuild fBuild = (FBuild)need("OH_NNCompilation_Build");
    FDel fCompDel = (FDel)need("OH_NNCompilation_Destroy");
    FExecNew fExecNew = (FExecNew)need("OH_NNExecutor_Construct");
    FExecInDesc fExecInDesc = (FExecInDesc)need("OH_NNExecutor_CreateInputTensorDesc");
    FExecOutDesc fExecOutDesc = (FExecOutDesc)need("OH_NNExecutor_CreateOutputTensorDesc");
    FExecRun fRun = (FExecRun)need("OH_NNExecutor_RunSync");
    FExecDel fExecDel = (FExecDel)need("OH_NNExecutor_Destroy");
    FTensorNew fTensorNew = (FTensorNew)need("OH_NNTensor_Create");
    FTensorBuf fTensorBuf = (FTensorBuf)need("OH_NNTensor_GetDataBuffer");
    FDescNew fDescNew = (FDescNew)need("OH_NNTensorDesc_Create");
    FDescSetDtype fDescSetDtype = (FDescSetDtype)need("OH_NNTensorDesc_SetDataType");
    FDescSetShape fDescSetShape = (FDescSetShape)need("OH_NNTensorDesc_SetShape");
    FDescSetFormat fDescSetFormat = (FDescSetFormat)need("OH_NNTensorDesc_SetFormat");
    FDescDel fDescDel = (FDescDel)need("OH_NNTensorDesc_Destroy");
    FTensorDel fTensorDel = (FTensorDel)need("OH_NNTensor_Destroy");
    FDDtype fDDtype = (FDDtype)need("OH_NNTensorDesc_GetDataType");
    FDCnt fDCnt = (FDCnt)need("OH_NNTensorDesc_GetElementCount");
    FTDt fTDt = (FTDt)need("OH_NNTensor_GetTensorDesc");
    (void)fDescNew; (void)fDescSetDtype; (void)fDescSetShape; (void)fDescSetFormat; (void)fDescDel;
    (void)fDDtype; (void)fDCnt; (void)fTDt;
    if (g_missing[0]) {
        char m[512]; snprintf(m, sizeof(m), ", %s", g_missing);
        write_n(m, out, outsz);
        write_n("\n", out, outsz);
        return 2;
    }

    const size_t *ids = nullptr; uint32_t n = 0;
    OH_NN_ReturnCode rc = fDev(&ids, &n);
    size_t target = 0;
    char line[128];
    snprintf(line, sizeof(line), "  dev rc=%d count=%u", (int)rc, n);
    write_n(line, out, outsz);
    if (rc != OH_NN_SUCCESS || n == 0) { write_n(" (no device)\n", out, outsz); return 3; }
    for (uint32_t i = 0; i < n && i < 4; i++) {
        const char *nm = nullptr; fName(ids[i], &nm);
        snprintf(line, sizeof(line), "; [%u:%s]", i, nm ? nm : "?");
        write_n(line, out, outsz);
        if (nm && (strstr(nm, "NPU_") || strstr(nm, "Kirin"))) target = ids[i];
    }
    write_n("\n", out, outsz);

    void *comp = fCompBuf(modelBuf, modelLen);
    if (!comp) { write_n("  ConstructWithOfflineModelBuffer FAILED\n", out, outsz); return 4; }
    write_n("  construct=OK; setCache=", out, outsz);
    rc = fSetCache(comp, cacheDir, 1);
    snprintf(line, sizeof(line), "rc=%d; setDevice=", (int)rc);
    write_n(line, out, outsz);
    rc = fSetDev(comp, target);
    snprintf(line, sizeof(line), "rc=%d; build=", (int)rc);
    write_n(line, out, outsz);
    rc = fBuild(comp);
    snprintf(line, sizeof(line), "rc=%d (expect rc=1 = system lib rejects offline path)\n", (int)rc);
    write_n(line, out, outsz);

    // 若 build 意外成功则继续执行并验证数值
    if (rc == OH_NN_SUCCESS) {
        void *exec = fExecNew(comp);
        if (exec) {
            void *in0 = fExecInDesc(exec, 0) ? fTensorNew(target, fExecInDesc(exec, 0)) : nullptr;
            void *in1 = fExecInDesc(exec, 1) ? fTensorNew(target, fExecInDesc(exec, 1)) : nullptr;
            void *tout = fExecOutDesc(exec, 0) ? fTensorNew(target, fExecOutDesc(exec, 0)) : nullptr;
            if (in0 && in1 && tout) {
                size_t cnt = 0;
                fDCnt(fTDt(in0), &cnt);
                OH_NN_DataType dt; fDDtype(fTDt(in0), &dt);
                for (size_t k = 0; k < cnt; k++) {
                    if (dt == OH_NN_FLOAT16) ((uint16_t *)fTensorBuf(in0))[k] = 0x3C00;
                    else ((float *)fTensorBuf(in0))[k] = 1.0f;
                }
                fDCnt(fTDt(in1), &cnt);
                for (size_t k = 0; k < cnt; k++) {
                    if (dt == OH_NN_FLOAT16) ((uint16_t *)fTensorBuf(in1))[k] = 0x3C00;
                    else ((float *)fTensorBuf(in1))[k] = 1.0f;
                }
                void *ins[2] = {in0, in1}; void *outs[1] = {tout};
                rc = fRun(exec, ins, 2, outs, 1);
                snprintf(line, sizeof(line), "  UNEXPECTED: run rc=%d; out[0..3]=", (int)rc);
                write_n(line, out, outsz);
                void *od = fTDt(tout);
                OH_NN_DataType odt = OH_NN_FLOAT32; fDDtype(od, &odt);
                fDCnt(od, &cnt);
                for (size_t k = 0; k < cnt && k < 4; k++) {
                    float v = odt == OH_NN_FLOAT16
                        ? (((uint16_t *)fTensorBuf(tout))[k] == 0x4200 ? 3.0f : -2.0f)
                        : ((float *)fTensorBuf(tout))[k];
                    snprintf(line, sizeof(line), "%s%.2f", k ? "," : "", v);
                    write_n(line, out, outsz);
                }
            }
            if (in0) fTensorDel(&in0);
            if (in1) fTensorDel(&in1);
            if (tout) fTensorDel(&tout);
            fExecDel(&exec);
        }
    }
    fCompDel(&comp);
    if (rc == OH_NN_SUCCESS) write_n("\n[CH-04] RESULT: offline path EXECUTED (unexpected!)\n", out, outsz);
    else write_n("\n[CH-04] RESULT: offline rejected (Build rc!=0) — custom ops unreachable\n", out, outsz);
    return 0;
}

// ── CH-05 单算子直调(HMS_HiAISingleOp*)候选库/符号存在性探测 ────────────────
// 只做 dlopen/dlsym, 绝不调用未知签名函数(见文件头顶部安全铁律 2)。
extern "C" int nnrt_ch_singleop(char *out, size_t outsz)
{
    std::memset(out, 0, outsz);
    snprintf(out, outsz,
        "[CH-05] single-op direct-call probe (HMS_HiAISingleOp*) — existence only, no invocation\n"
        "  note: SDK sysroot has NO hiai_single_op.h (checked); HiAI Foundation Kit native header\n"
        "  not present in dev SDK => direct-call API needs separate Kit package or is unimplemented.\n");
    static const char *kLibs[] = {
        "libhiai.so", "libhiai.so.1", "libhiai_client.so", "libhiai_hand_api_provider.so",
        "libhiai_hand_api_impl.so", "libhiai_dynamic_plugin.so", "libhiai_single_op.so",
        "libhiai_ffi.so", "libnpu_client.so", "libnpu_engine.so", "librl_search.so",
    };
    char line[192];
    for (size_t i = 0; i < sizeof(kLibs) / sizeof(kLibs[0]); i++) {
        void *h = dlopen(kLibs[i], RTLD_NOW | RTLD_GLOBAL);
        if (!h) {
            snprintf(line, sizeof(line), "  dlopen %-36s -> FAIL (%s)\n", kLibs[i], dlerror() ? dlerror() : "?");
        } else {
            // 库存在, 再试 HMS_HiAISingleOp 前缀符号
            char sym[64];
            int found = 0;
            for (int k = 0; k < 8; k++) {
                snprintf(sym, sizeof(sym), "HMS_HiAISingleOp_%d", k);
                void *p = dlsym(h, sym);
                if (p) { found = 1; break; }
            }
            snprintf(line, sizeof(line), "  dlopen %-36s -> OK, HMS_HiAISingleOp_* %s\n",
                     kLibs[i], found ? "FOUND" : "not found");
        }
        write_n(line, out, outsz);
    }
    write_n("[CH-05] RESULT: direct-call channel not materialized (see note)\n", out, outsz);
    return 0;
}
