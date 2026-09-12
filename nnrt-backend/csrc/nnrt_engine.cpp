// nnrt_engine.cpp — NNRt 在线执行引擎实现。
//
// 三条实测纪律(全部来自 poc/npu 在 9030 上的验证, 见 nnrt_probe.cpp / npuchild.cpp):
//   1. Build 前必须 SetCache(否则 build 失败); 设备须选真实硬件(虚拟口无法编译)。
//   2. 参数张量 dtype 按算子而异且被 builder 严格校验 —— MATMUL 的 transpose 是
//      OH_NN_BOOL(非 INT64), SOFTMAX 的 axis 是 INT64; 张量 shape 为 rank=1、
//      元素数 = data 元素数(SetTensorData 按 shape 元素数校验)。
//   3. 执行时张量数按执行器实际输入数(GetInputCount), 不能用构图时的索引想当然。
#include "nnrt_engine.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "neural_network_runtime/neural_network_runtime.h"

namespace nnrt {

namespace {

void *g_rt = nullptr;
void *g_core = nullptr;

void *need(const char *n)
{
    void *p = g_rt ? dlsym(g_rt, n) : nullptr;
    if (!p && g_core) {
        p = dlsym(g_core, n);
    }
    return p;
}

// 执行链函数集(签名与 poc/npu npuchild.cpp 已验证版一致)
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
    // 诊断用(2026-09-12): 实测 mm 数值不符(4×4 全 1 相乘得 2.0 而非 4.0) ——
    //   需要看**执行器侧**理解的 shape/元素数, 才能区分"数据没写全"与"NNRt 按别的形状算"。
    OH_NN_ReturnCode (*descGetShape)(const void *, int32_t **, size_t *);
    OH_NN_ReturnCode (*devAll)(const size_t **, uint32_t *);
    OH_NN_ReturnCode (*devName)(size_t, const char **);
};

Fns &fns()
{
    static Fns f;
    return f;
}

bool g_collected = false;

bool collect()
{
    Fns &f = fns();
    bool ok = true;
#define GET(field, sym)                        \
    do {                                       \
        f.field = (decltype(f.field))need(sym); \
        if (!f.field) ok = false;              \
    } while (0)
    GET(modelNew, "OH_NNModel_Construct");
    GET(modelDel, "OH_NNModel_Destroy");
    GET(addTensor, "OH_NNModel_AddTensorToModel");
    GET(setTensorType, "OH_NNModel_SetTensorType");
    GET(setTensorData, "OH_NNModel_SetTensorData");
    GET(addOp, "OH_NNModel_AddOperation");
    GET(specify, "OH_NNModel_SpecifyInputsAndOutputs");
    GET(finish, "OH_NNModel_Finish");
    GET(descNew, "OH_NNTensorDesc_Create");
    GET(descSetShape, "OH_NNTensorDesc_SetShape");
    GET(descSetDtype, "OH_NNTensorDesc_SetDataType");
    GET(descSetFormat, "OH_NNTensorDesc_SetFormat");
    GET(descDel, "OH_NNTensorDesc_Destroy");
    GET(compNew, "OH_NNCompilation_Construct");
    GET(compSetCache, "OH_NNCompilation_SetCache");
    GET(compSetDev, "OH_NNCompilation_SetDevice");
    GET(compPerf, "OH_NNCompilation_SetPerformanceMode");
    GET(compBuild, "OH_NNCompilation_Build");
    GET(compDel, "OH_NNCompilation_Destroy");
    GET(execNew, "OH_NNExecutor_Construct");
    GET(execOutCount, "OH_NNExecutor_GetOutputCount");
    GET(execInCount, "OH_NNExecutor_GetInputCount");
    GET(execInDesc, "OH_NNExecutor_CreateInputTensorDesc");
    GET(execOutDesc, "OH_NNExecutor_CreateOutputTensorDesc");
    GET(execRun, "OH_NNExecutor_RunSync");
    GET(execDel, "OH_NNExecutor_Destroy");
    GET(tensorNew, "OH_NNTensor_Create");
    GET(tensorBuf, "OH_NNTensor_GetDataBuffer");
    GET(tensorDesc, "OH_NNTensor_GetTensorDesc");
    GET(tensorDel, "OH_NNTensor_Destroy");
    GET(descGetDataCount, "OH_NNTensorDesc_GetElementCount");
    GET(descGetShape, "OH_NNTensorDesc_GetShape");
    GET(devAll, "OH_NNDevice_GetAllDevicesID");
    GET(devName, "OH_NNDevice_GetName");
#undef GET
    g_collected = ok;
    return ok;
}

size_t dtypeSize(OH_NN_DataType t)
{
    switch (t) {
        case OH_NN_FLOAT32: return 4;
        case OH_NN_FLOAT16: return 2;
        case OH_NN_INT64:   return 8;
        case OH_NN_INT32:   return 4;
        case OH_NN_INT8:    return 1;
        case OH_NN_UINT8:   return 1;
        case OH_NN_BOOL:    return 1;   // 实测探针按 1 字节传
        default:            return 1;
    }
}

// 参数规格(照抄 nnrt_probe.cpp 的 TSpec 用法)
struct ParamSpec {
    OH_NN_DataType dtype;
    OH_NN_TensorType type;
    const void *data;
    size_t bytes;
};

// 单图执行: 输入按构造顺序, 输出为构图登记的 outputs。
//   onBuild 回调让调用方在图 Build 完成后、执行前做定制(当前未用, 保留)。
struct Graph {
    void *model = nullptr;
    std::vector<const float *> inData;   // 与 addInput 顺序一致
    std::vector<size_t> inBytes;
    uint32_t nTensor = 0;
    Fns *f = nullptr;
    bool ok = true;
    const char *why = "";
    // 图签名(张量登记顺序 × dtype/rank/shape/类型) —— 用作编译缓存的子目录键。
    //   2026-09-12 定谳: 同一 cacheDir 下不同图会互相污染, 详见 Runner::run 里的 SetCache 注释。
    std::string sig;
    // 构图期每个输入的**形状**(按 addInput 顺序) —— 用于把执行器暴露的输入与构图期输入对上号。
    //   NNRt 会把部分输入折叠成常量: 实测 conv2d 构图 3 个输入(x/w/bias), 执行器只暴露 2 个。
    //   数量不等时只能按形状匹配, 见 Runner::run 的 srcOf 逻辑。
    std::vector<std::vector<int32_t>> inShapes;

    bool addDescTensor(const int32_t *shape, size_t rank, OH_NN_DataType dtype,
                       OH_NN_TensorType ttype, const void *data, size_t bytes)
    {
        void *td = f->descNew();
        if (!td) { ok = false; why = "descNew"; return false; }
        bool r = f->descSetShape(td, shape, rank) == OH_NN_SUCCESS &&
                 f->descSetDtype(td, dtype) == OH_NN_SUCCESS &&
                 f->descSetFormat(td, OH_NN_FORMAT_NONE) == OH_NN_SUCCESS &&
                 f->addTensor(model, td) == OH_NN_SUCCESS;
        f->descDel(&td);
        if (!r) { ok = false; why = "desc/addTensor"; return false; }
        uint32_t idx = nTensor++;
        if (f->setTensorType(model, idx, ttype) != OH_NN_SUCCESS) {
            ok = false; why = "setTensorType"; return false;
        }
        if (data && bytes) {
            if (f->setTensorData(model, idx, data, bytes) != OH_NN_SUCCESS) {
                ok = false; why = "setTensorData"; return false;
            }
        }
        // 累积图签名: 索引顺序 + dtype + rank + 各维 + 张量类型(数据内容不计 —— 同形状不同数据
        //   应共享编译结果, 这正是缓存的意义)。
        char seg[96];
        int off = snprintf(seg, sizeof(seg), "%u|%d|%zu|", idx, (int)dtype, rank);
        for (size_t k = 0; k < rank && off > 0 && off < (int)sizeof(seg); k++) {
            off += snprintf(seg + off, sizeof(seg) - (size_t)off, "%d,", shape ? shape[k] : -999);
        }
        snprintf(seg + (off > 0 ? off : 0), sizeof(seg) - (size_t)(off > 0 ? off : 0),
                 "|%d;", (int)ttype);
        sig += seg;
        return true;
    }

    // 动态输入(执行时喂数据): 构图阶段只登记, 数据在 exec() 里写入
    bool addInput(const int32_t *shape, size_t rank)
    {
        if (!addDescTensor(shape, rank, OH_NN_FLOAT32, OH_NN_TENSOR, nullptr, 0)) { return false; }
        inShapes.emplace_back(shape, shape + rank);
        return true;
    }

    // 构图期就带数据的输入 —— 供**会被 NNRt 折叠为常量**的张量使用(conv2d 的 weight 实测如此:
    //   执行器只为 3 个构图输入暴露 2 个)。这类张量若不在这里给数据, 编译器读到的是未初始化
    //   内存。见 Engine::conv2d 的注释。
    bool addInputWithData(const int32_t *shape, size_t rank, const float *data, size_t bytes)
    {
        if (!addDescTensor(shape, rank, OH_NN_FLOAT32, OH_NN_TENSOR, data, bytes)) { return false; }
        inShapes.emplace_back(shape, shape + rank);
        return true;
    }

    bool addParam(const ParamSpec &p)
    {
        int32_t elems[1] = {(int32_t)(p.bytes / dtypeSize(p.dtype))};
        return addDescTensor(elems, 1, p.dtype, p.type, p.data, p.bytes);
    }

    bool addOutput(const int32_t *shape, size_t rank)
    {
        return addDescTensor(shape, rank, OH_NN_FLOAT32, OH_NN_TENSOR, nullptr, 0);
    }
};

class Runner {
public:
    Runner(Engine &e, Graph &g) : eng_(e), g_(g) {}
    ~Runner() { releaseExecComp(); }

    // 显式释放 exec/comp。**必须在销毁 model 之前调用** —— NNRt 里存在依赖倒置:
    //   exec 依赖 comp、comp 依赖 model。2026-09-12 实测: conv2d 走 run 的失败路径时
    //   (执行器输入数与构图期不符), 日志停在失败点前最后一条、无任何异常、comfy_child 直接从
    //   进程表消失 —— 定位到"先 modelDel 再析构 Runner"这个顺序上。成功后正常执行的路径
    //   (mm/softmax)未复现, 推测 execRun 成功已把内部状态绑定完整。稳妥起见统一成
    //   run → releaseExecComp → modelDel。
    //   ⚠ 仍然不碰 model_: 它归创建它的算子函数独占销毁(曾因二次销毁让 comfy_child 静默死亡,
    //   并且中途改成"run 里 g_.model=nullptr 转移所有权"更糟 —— addDescTensor 内部正是用成员
    //   model 调 AddTensor)。del 后指针置空, 故本方法可重复调用。
    void releaseExecComp()
    {
        Fns &f = fns();
        if (exec_) f.execDel(&exec_);
        if (comp_) f.compDel(&comp_);
    }

    // 构图收口 + 编译 + 执行; outputs 按构图顺序接收数据。
    bool run(const OH_NN_OperationType op, const std::vector<ParamSpec> &params,
             const std::vector<uint32_t> &ins, const std::vector<uint32_t> &outs,
             const std::vector<const float *> &execInputs,
             std::vector<float *> &execOutputs)
    {
        Fns &f = fns();
        elog("NNRT-ENG e0 run-enter op=%d nparam=%zu nins=%zu nouts=%zu",
             (int)op, params.size(), ins.size(), outs.size());
        // ⚠ model 的所有权: **由创建它的算子函数独占销毁**(run 结束后 f.modelDel(&g.model)),
        //   Runner 只在 run 期间借用指针(model_), 析构时**不**碰 model。
        //   2026-09-12 走过两个弯路, 记下来免得再犯:
        //     ① 原实现里算子函数与 ~Runner 都 modelDel → 同一模型二次销毁, comfy_child 在
        //        execRun 之后立即死亡, 日志停在 kernel 最后一行, 伪装成"卡死"(存活判据又只看
        //        主应用进程, 连掩盖三轮误判);
        //     ② 改成"run 里 g_.model=nullptr 转移所有权"→ 更糟: Graph::addDescTensor 内部正是
        //        用成员 model 调 OH_NNModel_AddTensor, 置空后下面的 addParam 立刻以 nullptr
        //        调用 → 崩在 e1 之前。**转移所有权必须保证原持有者此后再不被使用。**
        model_ = g_.model;
        OH_NN_UInt32Array aP = {nullptr, 0};
        std::vector<uint32_t> pIdx;
        uint32_t base = g_.nTensor;
        for (size_t i = 0; i < params.size(); i++) {
            if (!g_.addParam(params[i])) { return fail(g_.why); }
            pIdx.push_back(base + (uint32_t)i);
        }
        if (!pIdx.empty()) { aP = {pIdx.data(), (uint32_t)pIdx.size()}; }
        // ⚠ 张量登记顺序保持"输入 → 输出 → 参数"(由算子函数登记 out, run 登记 params)。
        //   2026-09-12 曾按 POC(nnrt_probe.cpp: ins → params → out)对齐改成"参数先于输出",
        //   真机结果: 进程在 m1 与 e1 之间即失联(e1 都没打印)。**事后查明那次改动的动机是
        //   误读自己的日志** —— e8 打的 execRun-rc 其实是 int(ran)(成功=1), 旧顺序下执行本就
        //   成功。故回退。若将来再动顺序, 先确认动机有真实数据支撑(现成结论: 本顺序可跑通)。
        OH_NN_UInt32Array aI = {const_cast<uint32_t *>(ins.data()), (uint32_t)ins.size()};
        OH_NN_UInt32Array aO = {const_cast<uint32_t *>(outs.data()), (uint32_t)outs.size()};
        // 分段日志(见 nnrt_engine.h): 真机上 Build/execRun 都是已知高风险点(P0 实测 Build rc=2),
        //   引擎内零日志会让"卡住"落在黑盒里。前缀 NNRT-ENG, 与 kernel 侧 NNRT-MM 对齐。
        elog("NNRT-ENG e1 graph-start op=%d", (int)op);
        if (f.addOp(model_, op, &aP, &aI, &aO) != OH_NN_SUCCESS) { return fail("addOp"); }
        if (f.specify(model_, &aI, &aO) != OH_NN_SUCCESS) { return fail("specify"); }
        if (f.finish(model_) != OH_NN_SUCCESS) { return fail("finish"); }
        elog("NNRT-ENG e2 graph-done");

        comp_ = f.compNew(model_);
        if (!comp_) { return fail("compNew"); }
        // ⚠ 编译缓存必须按"图"隔离(2026-09-12 真机定谳, 此前 POC 从未察觉):
        //   同一个 cacheDir 下, NNRt 会把**上一张图**的编译结果交给当前图 —— 实测 P1-a 探针的
        //   ADD 图(shape [1,2,2,3])污染了 Engine 的 MATMUL: 执行器报出的输入形状竟是 [1,2,2,3],
        //   cnt=12, 输出 2.0(= 1.0+1.0, 即 ADD 的结果)而非 4.0。POC 之所以"33 项零回归"仍成立,
        //   是因为它只断言 rcRun==SUCCESS、从不校验数值 —— **执行成功 ≠ 算得对**。
        //   故按图签名取子目录, 各图各缓存, 互不串味。
        // ⚠ 2026-09-12 实验: CONV2D 的 compBuild 在"**空**子目录 + HIGH(3)"下崩溃 —— 日志停在
        //   e3 之后毫无输出、comfy_child 从进程表消失(非挂起而是崩溃; 探针有 catch(...) 兜底却
        //   没打出 EX 行 ⇒ 异常不在我们的 try 块内)。同等参数在 poc/npu(顶层有效缓存目录 +
        //   EXTREME)下 3~4ms 编译成功。本实验让 CONV2D 改用**更接近 POC 的配置**:
        //     ① 顶层 cacheDir —— 里面有 NNRt 自己写的 cache_info.nncache, 是"有效缓存目录";
        //        我们的 <hash>_p3 子目录实测**全为空**(NNRt 从不往里写), 即"无效缓存目录"。
        //     ② perfMode=4(EXTREME) —— 注意 setPerfMode 原先把上限定为 3, 故 EXTREME 从未被
        //        真正测过(与 POC 的核心差异之一)。
        //   其他算子保持原样 ⇒ 零回归。若通过, 再单变量二分(缓存目录 / perfMode)。
        // ⚠⚠ 不要用顶层 cacheDir! 2026-09-12 踩过: conv2d 曾临时改用 eng_.cacheDir()(顶层),
        //   理由是"POC 用有效缓存目录而我们用空子目录" —— 结果 e4 build-ok 如期出现, 但那是
        //   **命中 09-11 遗留的旧缓存**的假象: 执行器报出的输入形状是 [1,2,2,3](ADD 图的),
        //   与本图 1x3x8x8 毫无关系(e5e/e5f 两行日志一对照即现原形)。顶层目录里的
        //   0.nncache/cache_info.nncache 是改造子目录机制之前留下的, **不可再被指向**。
        // ⚠ 2026-09-12 实验收尾: 曾让 CONV2D 单走 EXTREME(4)+顶层 cacheDir(为了贴近 POC),
        //   结论 = **两者都不是关键变量** —— 顶层目录那轮是命中遗留缓存的假象(见上), EXTREME
        //   那轮 conv 仍 build-rc=1。故回退为**所有算子统一 perfMode**(单变量原则: 探针矩阵里
        //   只留 rank/layout 一个变量)。conv 的真实差异已定位在 STRIDES/DILATION 的 rank 与
        //   形状布局上, 见 csrc/backend.cpp 与 bootstrap.cpp 的 CvTry 注释。
        const bool isConv = (op == OH_NN_OPS_CONV2D);   // 仅用于日志标记, 不再影响行为
        const int pm = eng_.perfMode();
        char subDir[1024];
        snprintf(subDir, sizeof(subDir), "%s/%016zx_p%d", eng_.cacheDir(),
                 (size_t)std::hash<std::string>{}(g_.sig), pm);
        mkdir(subDir, 0755);   // 已存在=正常
        elog("NNRT-ENG e2b cache-dir %s (sigLen=%zu perf=%d conv=%d)",
             subDir, g_.sig.size(), pm, int(isConv));
        if (f.compSetCache(comp_, subDir, 1) != OH_NN_SUCCESS) { return fail("setCache"); }
        if (f.compSetDev(comp_, eng_.deviceId()) != OH_NN_SUCCESS) { return fail("setDevice"); }
        f.compPerf(comp_, (OH_NN_PerformanceMode)pm);
        elog("NNRT-ENG e3 before-build dev=%zu", eng_.deviceId());
        const OH_NN_ReturnCode rcBuild = f.compBuild(comp_);
        // ⚠ 必须打返回码: "compBuild 失败"与"compBuild 挂起/崩溃"是两种完全不同的故障,
        //   而两者的日志表现(没有 e4)一模一样。2026-09-12 就因此绕了远路。
        elog("NNRT-ENG e3b build-rc=%d", (int)rcBuild);
        if (rcBuild != OH_NN_SUCCESS) { return fail("build"); }
        elog("NNRT-ENG e4 build-ok");

        exec_ = f.execNew(comp_);
        if (!exec_) { return fail("execNew"); }
        elog("NNRT-ENG e5 execNew-ok");
        size_t nIn = 0, nOut = 0;
        f.execInCount(exec_, &nIn);
        f.execOutCount(exec_, &nOut);
        // ⚠ 分段日志(e5a/b/c): conv2d 实测崩在 e5 与 e6 之间且**无 EX 行**(探针有 catch(...) 兜底
        //   ⇒ 是信号崩溃而非 C++ 异常), 必须定位到具体是哪一个 NNRt 调用。另外 e5a 会打出执行器
        //   侧的真实张量数 —— poc/npu 记载过"执行器输入数可能与构图期不同"(可能含激活标志)。
        elog("NNRT-ENG e5a exec-counts in=%zu out=%zu want-in=%zu want-out=%zu",
             nIn, nOut, execInputs.size(), execOutputs.size());
        if (execOutputs.size() != nOut) { return fail("exec-out-arity"); }
        std::vector<void *> tIn(nIn, nullptr), tOut(nOut, nullptr);
        for (size_t i = 0; i < nIn; i++) {
            void *d = f.execInDesc(exec_, (uint32_t)i);
            elog("NNRT-ENG e5b-%zu in-desc %s", i, d ? "ok" : "null");
            tIn[i] = d ? f.tensorNew(eng_.deviceId(), d) : nullptr;
            elog("NNRT-ENG e5b-%zu in-tensor %s", i, tIn[i] ? "ok" : "null");
        }
        for (size_t i = 0; i < nOut; i++) {
            void *d = f.execOutDesc(exec_, (uint32_t)i);
            tOut[i] = d ? f.tensorNew(eng_.deviceId(), d) : nullptr;
        }
        elog("NNRT-ENG e5c exec-tensors-ok");
        // ⚠ 执行器暴露的输入数可能**少于**构图期输入数 —— NNRt 会把部分输入折叠成常量
        //   (实测 conv2d: 构图 3 个 x/w/bias, 执行器只暴露 2 个)。所以**不能**按下标直接对应,
        //   数量不等时按**形状**匹配(取第一个形状相同且未占用的构图输入)。
        const size_t NONE = (size_t)-1;
        std::vector<size_t> srcOf(nIn, NONE);
        if (execInputs.size() == nIn) {
            for (size_t i = 0; i < nIn; i++) { srcOf[i] = i; }
        } else if (g_.inShapes.size() == execInputs.size()) {
            std::vector<bool> used(g_.inShapes.size(), false);
            for (size_t i = 0; i < nIn; i++) {
                if (!tIn[i]) { continue; }
                int32_t *shp = nullptr;
                size_t rl = 0;
                f.descGetShape(f.tensorDesc(tIn[i]), &shp, &rl);
                size_t m = NONE;
                for (size_t j = 0; j < g_.inShapes.size(); j++) {
                    if (used[j] || g_.inShapes[j].size() != rl) { continue; }
                    bool same = true;
                    for (size_t k = 0; k < rl; k++) {
                        if (g_.inShapes[j][k] != (shp ? shp[k] : -1)) { same = false; break; }
                    }
                    if (same) { m = j; break; }
                }
                if (m == NONE) {
                    // ⚠ 执行器可能多出**非数据输入**(poc/npu 记载过"执行器输入数可能是
                    //   2 = 输入 + 激活标志", 与构图期定义的算子输入数不同)。这类输入的形状不在
                    //   构图期输入里, **不喂数据**(保持 NNRt 默认), 仅留痕。
                    //   风险: 若某个真数据输入也被判为"无匹配", 它就不会被喂 —— 这种情况由
                    //   c28/c29 的数值对拍兜住(数值错会立刻暴露)。
                    char ed[48] = "";
                    for (size_t k = 0; k < rl && k < 4; k++) {
                        snprintf(ed + strlen(ed), sizeof(ed) - strlen(ed), "%s%d",
                                 k ? "," : "", shp ? shp[k] : -1);
                    }
                    elog("NNRT-ENG e5e exec-in%zu rank=%zu shape=[%s] 无匹配 -> 跳过", i, rl, ed);
                    continue;
                }
                used[m] = true;
                srcOf[i] = m;
            }
        } else {
            return fail("exec-arity-mismatch");
        }
        elog("NNRT-ENG e5d input-map exec=%zu want=%zu", nIn, execInputs.size());
        // 数量不等时把构图期输入形状也打出来 —— e5e(执行器侧) 与 e5f(构图期) 对照即可看出
        //   究竟是哪一个输入被 NNRt 折叠成了常量。
        if (execInputs.size() != nIn) {
            for (size_t j = 0; j < g_.inShapes.size(); j++) {
                char gd[48] = "";
                for (size_t k = 0; k < g_.inShapes[j].size() && k < 4; k++) {
                    snprintf(gd + strlen(gd), sizeof(gd) - strlen(gd), "%s%d",
                             k ? "," : "", g_.inShapes[j][k]);
                }
                elog("NNRT-ENG e5f graph-in%zu shape=[%s]", j, gd);
            }
        }
        bool allOk = true;
        for (size_t i = 0; i < nIn; i++) {
            if (!tIn[i]) { allOk = false; break; }
            size_t cnt = 0;
            f.descGetDataCount(f.tensorDesc(tIn[i]), &cnt);
            void *buf = f.tensorBuf(tIn[i]);
            // 执行器侧视图: 若这里的 shape/cnt 与构图登记的不一致(实测 mm 得 2.0 而非 4.0
            //   时高度怀疑此点), 那么 memcpy 只写进一部分数据, 数值必然错 —— 先看它。
            int32_t *shp = nullptr;
            size_t rl = 0;
            f.descGetShape(f.tensorDesc(tIn[i]), &shp, &rl);
            char dims[48] = "";
            for (size_t k = 0; k < rl && k < 4; k++) {
                snprintf(dims + strlen(dims), sizeof(dims) - strlen(dims), "%s%d",
                         k ? "," : "", shp ? shp[k] : -1);
            }
            elog("NNRT-ENG e6-in%zu cnt=%zu rank=%zu shape=[%s] src=%zu copyBytes=%zu",
                 i, cnt, rl, dims, srcOf[i], cnt * sizeof(float));
            if (srcOf[i] == NONE) { continue; }   // 非数据输入(如激活标志): 不喂数据
            if (!buf || !execInputs[srcOf[i]]) { allOk = false; break; }
            memcpy(buf, execInputs[srcOf[i]], cnt * sizeof(float));
        }
        // 保险: 一个输入都没喂上 ⇒ 输出必然错, 直接失败(免得把错误数据当成结果用)。
        size_t fed = 0;
        for (size_t i = 0; i < nIn; i++) { if (srcOf[i] != NONE) { fed++; } }
        elog("NNRT-ENG e6 tensors-ready in=%zu out=%zu allOk=%d fed=%zu",
             nIn, nOut, int(allOk), fed);
        if (allOk && fed == 0 && nIn > 0) { return fail("exec-no-input-fed"); }
        bool ran = false;
        if (allOk) {
            elog("NNRT-ENG e7 before-execRun");
            const OH_NN_ReturnCode rcRun = f.execRun(exec_, tIn.data(), nIn, tOut.data(), nOut);
            ran = (rcRun == OH_NN_SUCCESS);
            // ⚠ 标签别写 "rc": 曾经写成 execRun-rc=%d 而值是 int(ran)(成功=1), 与"返回码"语义
            //   恰好相反 —— 排查时被自己的日志误导了一轮。这里把原始返回码与判定分开打。
            elog("NNRT-ENG e8 execRun ret=%d ok=%d", (int)rcRun, int(ran));
        }
        if (ran) {
            for (size_t i = 0; i < nOut; i++) {
                size_t cnt = 0;
                f.descGetDataCount(f.tensorDesc(tOut[i]), &cnt);
                int32_t *shp = nullptr;
                size_t rl = 0;
                f.descGetShape(f.tensorDesc(tOut[i]), &shp, &rl);
                char dims[48] = "";
                for (size_t k = 0; k < rl && k < 4; k++) {
                    snprintf(dims + strlen(dims), sizeof(dims) - strlen(dims), "%s%d",
                             k ? "," : "", shp ? shp[k] : -1);
                }
                elog("NNRT-ENG e9-out%zu cnt=%zu rank=%zu shape=[%s]", i, cnt, rl, dims);
                void *buf = f.tensorBuf(tOut[i]);
                if (buf && execOutputs[i]) {
                    memcpy(execOutputs[i], buf, cnt * sizeof(float));
                }
            }
        }
        for (size_t i = 0; i < nIn; i++) { if (tIn[i]) f.tensorDel(&tIn[i]); }
        for (size_t i = 0; i < nOut; i++) { if (tOut[i]) f.tensorDel(&tOut[i]); }
        if (!ran) {
            return fail(allOk ? "runSync" : "exec-buffer");
        }
        return true;
    }

    const char *why() const { return why_ ? why_ : "?"; }

private:
    // 只记录原因, 由 Engine 侧统一 setErr —— 避免 Runner(匿名 namespace)访问 Engine 私有成员
    bool fail(const char *why)
    {
        why_ = why;
        return false;
    }
    Engine &eng_;
    Graph &g_;
    const char *why_ = nullptr;
    void *model_ = nullptr;
    void *comp_ = nullptr;
    void *exec_ = nullptr;
};

} // namespace

Engine &Engine::inst()
{
    static Engine e;
    return e;
}

void Engine::setErr(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_, sizeof(err_), fmt, ap);
    va_end(ap);
}

bool Engine::init(const char *cacheDir)
{
    if (ready_) {
        return true;
    }
    if (cacheDir && *cacheDir) {
        snprintf(cacheDir_, sizeof(cacheDir_), "%s", cacheDir);
    }
    if (!g_rt) {
        g_rt = dlopen("libneural_network_runtime.so", RTLD_NOW | RTLD_GLOBAL);
        g_core = dlopen("libneural_network_core.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!g_rt) {
        setErr("nnrt-engine: dlopen libneural_network_runtime.so failed");
        return false;
    }
    if (!collect()) {
        setErr("nnrt-engine: dlsym 缺失(设备库与 SDK 版本不匹配?)");
        return false;
    }
    Fns &f = fns();
    const size_t *ids = nullptr;
    uint32_t n = 0;
    f.devAll(&ids, &n);
    for (int i = 0; i < 6 && n == 0; i++) {   // HDI 异步注册: 首查可能为空
        usleep(500 * 1000);
        f.devAll(&ids, &n);
    }
    if (n == 0) {
        setErr("nnrt-engine: 无可见 NNRt 设备");
        return false;
    }
    bool found = false;
    for (uint32_t i = 0; i < n && i < 4; i++) {
        const char *nm = nullptr;
        f.devName(ids[i], &nm);
        if (nm && (strstr(nm, "NPU_") || strstr(nm, "Kirin"))) {
            device_ = ids[i];
            found = true;
        }
    }
    if (!found) {
        setErr("nnrt-engine: 无真实硬件口(仅虚拟设备, 无法编译)");
        return false;
    }
    ready_ = true;
    return true;
}

bool Engine::matmul(const float *a, const float *b, float *y,
                    int64_t M, int64_t K, int64_t N)
{
    // ready_=0 时直接返回 false(不抛) —— 上层 TORCH_CHECK 转成异常。此处必须留痕:
    //   否则"引擎未 init"与"引擎执行失败"在日志上长得一模一样, 都是 kernel 无后续输出。
    elog("NNRT-ENG m1 matmul-enter ready=%d M=%lld K=%lld N=%lld",
         int(ready_), (long long)M, (long long)K, (long long)N);
    if (!ready_) { return false; }
    Fns &f = fns();
    int32_t sA[2] = {(int32_t)M, (int32_t)K};
    int32_t sB[2] = {(int32_t)K, (int32_t)N};
    int32_t sY[2] = {(int32_t)M, (int32_t)N};
    Graph g;
    g.f = &f;
    g.model = f.modelNew();
    if (!g.model) { setErr("matmul: modelNew"); return false; }
    if (!g.addInput(sA, 2) || !g.addInput(sB, 2) || !g.addOutput(sY, 2)) {
        f.modelDel(&g.model);
        setErr("matmul: %s", g.why);
        return false;
    }
    const bool kFalse = false;   // 实测: transpose 标志须 OH_NN_BOOL
    std::vector<ParamSpec> params = {
        {OH_NN_BOOL, OH_NN_MATMUL_TRANSPOSE_A, &kFalse, sizeof(kFalse)},
        {OH_NN_BOOL, OH_NN_MATMUL_TRANSPOSE_B, &kFalse, sizeof(kFalse)},
    };
    std::vector<uint32_t> ins = {0, 1}, outs = {2};
    std::vector<const float *> in = {a, b};
    std::vector<float *> out = {y};
    Runner r(*this, g);
    bool ok = r.run(OH_NN_OPS_MATMUL, params, ins, outs, in, out);
    // ⚠ 顺序不可换: 先释放 exec/comp, 再销毁 model(依赖倒置, 见 releaseExecComp 注释)。
    //   model 归本函数(创建者)独占销毁; ~Runner 不碰它。
    r.releaseExecComp();
    f.modelDel(&g.model);
    if (!ok) { setErr("matmul: %s", r.why()); }
    return ok;
}

bool Engine::softmax(const float *x, float *y, int64_t rows, int64_t cols)
{
    if (!ready_) { return false; }
    Fns &f = fns();
    int32_t sx[2] = {(int32_t)rows, (int32_t)cols};
    Graph g;
    g.f = &f;
    g.model = f.modelNew();
    if (!g.model) { setErr("softmax: modelNew"); return false; }
    if (!g.addInput(sx, 2) || !g.addOutput(sx, 2)) {
        f.modelDel(&g.model);
        setErr("softmax: %s", g.why);
        return false;
    }
    const int64_t axis = 1;   // 沿最后一维(列)做 softmax
    std::vector<ParamSpec> params = {
        {OH_NN_INT64, OH_NN_SOFTMAX_AXIS, &axis, sizeof(axis)},
    };
    std::vector<uint32_t> ins = {0}, outs = {1};
    std::vector<const float *> in = {x};
    std::vector<float *> out = {y};
    Runner r(*this, g);
    bool ok = r.run(OH_NN_OPS_SOFTMAX, params, ins, outs, in, out);
    // ⚠ 顺序不可换: 先释放 exec/comp, 再销毁 model(依赖倒置, 见 releaseExecComp 注释)。
    r.releaseExecComp();
    f.modelDel(&g.model);
    if (!ok) { setErr("softmax: %s", r.why()); }
    return ok;
}

bool Engine::conv2d(const float *x, const float *w, const float *bias, float *y,
                    int64_t n, int64_t c, int64_t ih, int64_t iw,
                    int64_t oc, int64_t kh, int64_t kw,
                    const int64_t *strides, const int64_t *pads)
{
    if (!ready_) { return false; }
    Fns &f = fns();
    // ⚠⚠ 形状按 **NHWC** 声明: NNRt 走 MindIR Conv2DFusion(NHWC 语义), 传 NCHW 的 [1,C,H,W]
    //   会被读成 N=1,H=C,W=H,C=W, 与权重反推的 inChannel=C 冲突 ⇒ compBuild rc=1。
    //   依据: (1) POC 样本 [1,2,2,3] 在 NHWC 下 C=3 与权重 OHWI [*,*,*,3] 自洽;
    //         (2) 2026-09-12 实测矩阵(s2/s4 × nchw/nhwc)见 bootstrap.cpp 的 CvTry 注释。
    //   调用方(backend.cpp)已把数据重排成 NHWC。
    int32_t sx[4] = {(int32_t)n, (int32_t)ih, (int32_t)iw, (int32_t)c};
    // ⚠ 权重布局 = NNRt 约定的 **[outC, kH, kW, inC]**(OHWI), 不是 PyTorch 的 OIHW。
    //   权威依据: build/nnrt-src/.../ops/conv2d_builder.cpp —— SetChannel 取
    //   m_inChannel = weightShape[3]、m_outChannel = weightShape[0]; SetKernelSize 取
    //   m_kernelSize = [weightShape[1], weightShape[2]]。传错布局(如直接给 OIHW)时
    //   编译器会拿到 "kernel=[IC,KH] 且 inChannel=KW" 的自相矛盾图, 后果**不是报错而是
    //   compBuild 挂起不返回** —— 2026-09-12 实测: N=1 C=2 H=W=6, OIHW [3,2,3,3] 直接传,
    //   声明 inChannel(=KW=3) 与输入通道(=IC=2) 不符, e3 before-build 后 110s 无 e4。
    //   (poc/npu 的 CONV2D 样本 kH=kW=inC=3 三数恰好相等, 无法区分布局, 所以从未暴露。)
    int32_t sw[4] = {(int32_t)oc, (int32_t)kh, (int32_t)kw, (int32_t)c};
    int32_t sb[1] = {(int32_t)oc};
    // ⚠ strides 是 **rank=2** 的 [sH, sW](不是 4 元素) —— 见 backend.cpp 同名注释与
    //   conv2d_builder.cpp 的 SetStrides(不校验 rank, 整段拷给 MindIR; 单测为 2 元素)。
    //   pads 仍是 4 元素 [top,bottom,left,right](SetPad 按 elementCount==4 判定 padList)。
    int32_t oh = (int32_t)((ih + pads[0] + pads[1] - kh) / strides[0] + 1);
    int32_t ow = (int32_t)((iw + pads[2] + pads[3] - kw) / strides[1] + 1);
    int32_t sy[4] = {(int32_t)n, oh, ow, (int32_t)oc};   // NHWC 输出
    Graph g;
    g.f = &f;
    g.model = f.modelNew();
    if (!g.model) { setErr("conv2d: modelNew"); return false; }
    // 顺序须与 execInDesc 的索引一致: 输入 x/w/bias, 输出 y
    // ⚠⚠ weight 与 bias 用 **addInputWithData**(构图期即交付数据): NNRt 会把它们折叠成常量 ——
    //   实测证据: 同一张图构图期 3 个输入, 执行器只暴露 **2** 个(e5a 日志), 即其中一个不是运行时
    //   输入而是编译器常量。若像 x 那样只登记形状不给数据, 编译器读到的就是未初始化内存。
    //   ⚠ poc/npu 从未暴露这一点: 它对**所有**执行器输入统一填 1.0, 从不需要知道谁对应谁。
    //   数据量: weight = oc*kh*kw*c 个 float(OHWI 布局, 调用方已重排), bias = oc 个 float。
    if (!g.addInput(sx, 4) ||
        !g.addInputWithData(sw, 4, w, (size_t)oc * kh * kw * c * sizeof(float)) ||
        !g.addInputWithData(sb, 1, bias, (size_t)oc * sizeof(float)) ||
        !g.addOutput(sy, 4)) {
        f.modelDel(&g.model);
        setErr("conv2d: %s", g.why);
        return false;
    }
    // ⚠ dilation **必须显式给**: Conv2DBuilder 的 m_dilation 默认是**空 vector**, 而
    //   GetPrimitive() 会把它交给 MindIR_Conv2DFusion_CreatePrimitive —— 空 dilation 很可能造出
    //   非法算子, 表现为 compBuild 返回非 SUCCESS(不是崩溃; 两者日志都"没有 e4", 得靠 e3b 的
    //   返回码区分)。POC 同样没传 dilation, 但它的"3~4ms 编译成功"极可能是**缓存命中**,
    //   从未真正验证过 CONV2D 的首次编译 —— 别拿它当反证。
    const int64_t dil[2] = {1, 1};   // rank=2, 同 strides
    std::vector<ParamSpec> params = {
        {OH_NN_INT64, OH_NN_CONV2D_STRIDES, strides, sizeof(int64_t) * 2},
        {OH_NN_INT64, OH_NN_CONV2D_PAD, pads, sizeof(int64_t) * 4},
        {OH_NN_INT64, OH_NN_CONV2D_DILATION, dil, sizeof(dil)},
    };
    std::vector<uint32_t> ins = {0, 1, 2}, outs = {3};
    std::vector<const float *> in = {x, w, bias};
    std::vector<float *> out = {y};
    Runner r(*this, g);
    bool ok = r.run(OH_NN_OPS_CONV2D, params, ins, outs, in, out);
    // 分段日志: conv2d 是**唯一走 run 失败路径**的算子(执行器输入数与构图期不符), 而失败路径
    //   恰是崩溃发生处 —— 三段日志用来区分"run 返回" / "释放 exec/comp" / "销毁 model"。
    elog("NNRT-ENG c2a conv-run-returned ok=%d", int(ok));
    r.releaseExecComp();
    elog("NNRT-ENG c2b conv-exec-comp-released");
    f.modelDel(&g.model);
    elog("NNRT-ENG c2c conv-model-released");
    if (!ok) { setErr("conv2d: %s", r.why()); }
    return ok;
}

} // namespace nnrt
