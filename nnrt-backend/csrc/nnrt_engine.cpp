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
        return addDescTensor(shape, rank, OH_NN_FLOAT32, OH_NN_TENSOR, nullptr, 0);
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
    ~Runner()
    {
        Fns &f = fns();
        if (exec_) f.execDel(&exec_);
        if (comp_) f.compDel(&comp_);
        // ⚠ 不碰 model_: 它归创建它的算子函数销毁(见 run 里的所有权说明)。
        //   曾经在这里 modelDel, 与算子函数的那次构成二次销毁 → comfy_child 静默死亡。
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
        char subDir[1024];
        snprintf(subDir, sizeof(subDir), "%s/%016zx_p%d", eng_.cacheDir(),
                 (size_t)std::hash<std::string>{}(g_.sig), eng_.perfMode());
        mkdir(subDir, 0755);   // 已存在=正常
        elog("NNRT-ENG e2b cache-dir %s (sigLen=%zu)", subDir, g_.sig.size());
        if (f.compSetCache(comp_, subDir, 1) != OH_NN_SUCCESS) { return fail("setCache"); }
        if (f.compSetDev(comp_, eng_.deviceId()) != OH_NN_SUCCESS) { return fail("setDevice"); }
        f.compPerf(comp_, (OH_NN_PerformanceMode)eng_.perfMode());
        elog("NNRT-ENG e3 before-build dev=%zu", eng_.deviceId());
        if (f.compBuild(comp_) != OH_NN_SUCCESS) { return fail("build"); }
        elog("NNRT-ENG e4 build-ok");

        exec_ = f.execNew(comp_);
        if (!exec_) { return fail("execNew"); }
        elog("NNRT-ENG e5 execNew-ok");
        size_t nIn = 0, nOut = 0;
        f.execInCount(exec_, &nIn);
        f.execOutCount(exec_, &nOut);
        if (execInputs.size() != nIn || execOutputs.size() != nOut) {
            return fail("exec-arity-mismatch");
        }
        std::vector<void *> tIn(nIn, nullptr), tOut(nOut, nullptr);
        for (size_t i = 0; i < nIn; i++) {
            void *d = f.execInDesc(exec_, (uint32_t)i);
            tIn[i] = d ? f.tensorNew(eng_.deviceId(), d) : nullptr;
        }
        for (size_t i = 0; i < nOut; i++) {
            void *d = f.execOutDesc(exec_, (uint32_t)i);
            tOut[i] = d ? f.tensorNew(eng_.deviceId(), d) : nullptr;
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
            elog("NNRT-ENG e6-in%zu cnt=%zu rank=%zu shape=[%s] copyBytes=%zu",
                 i, cnt, rl, dims, cnt * sizeof(float));
            if (!buf || !execInputs[i]) { allOk = false; break; }
            memcpy(buf, execInputs[i], cnt * sizeof(float));
        }
        elog("NNRT-ENG e6 tensors-ready in=%zu out=%zu allOk=%d", nIn, nOut, int(allOk));
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
    f.modelDel(&g.model);   // model 由本函数(创建者)独占销毁; ~Runner 不碰它(见其注释)
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
    f.modelDel(&g.model);   // model 由本函数(创建者)独占销毁; ~Runner 不碰它(见其注释)
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
    int32_t sx[4] = {(int32_t)n, (int32_t)c, (int32_t)ih, (int32_t)iw};
    int32_t sw[4] = {(int32_t)oc, (int32_t)c, (int32_t)kh, (int32_t)kw};
    int32_t sb[1] = {(int32_t)oc};
    int32_t oh = (int32_t)((ih + 2 * pads[1] - kh) / strides[1] + 1);
    int32_t ow = (int32_t)((iw + 2 * pads[3] - kw) / strides[3] + 1);
    int32_t sy[4] = {(int32_t)n, (int32_t)oc, oh, ow};
    Graph g;
    g.f = &f;
    g.model = f.modelNew();
    if (!g.model) { setErr("conv2d: modelNew"); return false; }
    // 顺序须与 execInDesc 的索引一致: 输入 x/w/bias, 输出 y
    if (!g.addInput(sx, 4) || !g.addInput(sw, 4) || !g.addInput(sb, 1) ||
        !g.addOutput(sy, 4)) {
        f.modelDel(&g.model);
        setErr("conv2d: %s", g.why);
        return false;
    }
    std::vector<ParamSpec> params = {
        {OH_NN_INT64, OH_NN_CONV2D_STRIDES, strides, sizeof(int64_t) * 4},
        {OH_NN_INT64, OH_NN_CONV2D_PAD, pads, sizeof(int64_t) * 4},
    };
    std::vector<uint32_t> ins = {0, 1, 2}, outs = {3};
    std::vector<const float *> in = {x, w, bias};
    std::vector<float *> out = {y};
    Runner r(*this, g);
    bool ok = r.run(OH_NN_OPS_CONV2D, params, ins, outs, in, out);
    f.modelDel(&g.model);   // model 由本函数(创建者)独占销毁; ~Runner 不碰它(见其注释)
    if (!ok) { setErr("conv2d: %s", r.why()); }
    return ok;
}

} // namespace nnrt
