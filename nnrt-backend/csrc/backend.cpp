// backend.cpp — torch PrivateUse1 后端(P1): 设备骨架 + 算子下沉 + CPU fallback。
//
// 组件(注册 API 依据本机 torch 2.10 头文件实测, 见 docs/npu-backend-roadmap.md):
//   · NnrtAllocator  : c10::Allocator   → c10::SetAllocator(PrivateUse1, &g_alloc)
//   · NnrtGuardImpl  : DeviceGuardImplInterface(8 纯虚) → C10_REGISTER_GUARD_IMPL
//   · NnrtHooks      : at::PrivateUse1HooksInterface → at::RegisterPrivateUse1HooksInterface
//   · 算子           : TORCH_LIBRARY_IMPL(aten, PrivateUse1, m) —— 已下沉的走 NNRt
//   · fallback       : TORCH_LIBRARY_IMPL(_, PrivateUse1, m) —— 未实现算子回 CPU
//
// P1 取舍(刻意为之, 非疏漏):
//   · "设备内存" = host malloc 的普通内存 —— NNRt 的 NNTensor buffer 本就是 host 可见,
//     故 CPU↔设备 的"搬运"可退化为零拷贝重新贴标签(见 shallowAs*), 无真实 memcpy。
//     真机若要 DMA 零拷贝再换 OH_NNTensor 的物理内存(接口已隔离在 engine)。
//   · 每算子一次构图+Build —— NNRt 的 Build 耗时不能靠自身缓存摊薄(P0 结论), 故本阶段
//     只求"机制通", 性能与 executor 缓存留到 P2 按实测数据做。
//
// ⚠ fallback 的两个坑(2026-09-12 设计期定谳, 实现前先读):
//   坑1 递归: 若用 `t.to(kCPU)` / `t.to(kNnrtDevice)` 做搬运, 会经 dispatcher 再次落到本
//     fallback(因为 `_to_copy` 未下沉) → 无限递归。故搬运一律走 `at::from_blob`
//     (该 API 直接构造 TensorImpl, **不经过 dispatcher**), 且因内存同源而零拷贝。
//   坑2 语义: "结果无脑转回设备"会破坏 `tensor.to("cpu")` —— 它会返回一个设备张量。
//     正确判据 = 看 schema 有没有 Device 类型参数: 有 ⇒ 输出设备由该参数决定(改写为 CPU
//     执行, 记下期望设备, 只在期望为 nnrt 时才贴回标签); 无 ⇒ 输出设备随输入, 贴回。
#include <ATen/ATen.h>
#include <ATen/detail/PrivateUse1HooksInterface.h>
#include <ATen/core/jit_type.h>
#include <c10/core/Allocator.h>
#include <c10/core/impl/DeviceGuardImplInterface.h>
#include <c10/core/impl/LocalDispatchKeySet.h>
#include <torch/library.h>
#include <torch/version.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include "nnrt_engine.h"

namespace {

constexpr c10::DeviceType kNnrtDevice = c10::DeviceType::PrivateUse1;

// 独立落盘(与 bootstrap.cpp 同文件; 此处不引 pybind11, 保持引擎层解耦)
// 实现已提到 nnrt_engine.h 的 nnrt::elogv —— 引擎层(Runner)需要同一落盘口。
void blog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    nnrt::elogv(fmt, ap);
    va_end(ap);
}

// fallback 递归深度(thread_local): 见 nnrtFallback 里的守卫说明
thread_local int g_fbDepth = 0;

// ─────────────────────────────────────────────────────────────────────────────
// ① Allocator: 设备内存 = host 内存(见文件头 P1 取舍)
// ─────────────────────────────────────────────────────────────────────────────
void nnrtMemFree(void *p)
{
    ::free(p);
}

struct NnrtAllocator : public c10::Allocator {
    c10::DataPtr allocate(size_t n) override
    {
        void *p = ::malloc(n ? n : 1);
        if (!p) {
            throw std::bad_alloc();
        }
        return {p, p, &nnrtMemFree, c10::Device(kNnrtDevice, 0)};
    }
    c10::DeleterFnPtr raw_deleter() const override
    {
        return &nnrtMemFree;
    }
    // c10::Allocator 在 torch 2.10 新增的纯虚(设备内拷贝); 本后端内存同源, memcpy 即可
    void copy_data(void *dest, const void *src, std::size_t count) const override
    {
        default_copy_data(dest, src, count);
    }
};

NnrtAllocator g_alloc;

// ─────────────────────────────────────────────────────────────────────────────
// ② DeviceGuard: 单设备(9030 只暴露 1 个 NPU 口)
// ─────────────────────────────────────────────────────────────────────────────
struct NnrtGuardImpl : public c10::impl::DeviceGuardImplInterface {
    c10::DeviceType type() const override
    {
        return kNnrtDevice;
    }
    c10::Device exchangeDevice(c10::Device d) const override
    {
        c10::Device old(kNnrtDevice, cur_);
        cur_ = d.index();
        return old;
    }
    c10::Device getDevice() const override
    {
        return c10::Device(kNnrtDevice, cur_);
    }
    void setDevice(c10::Device d) const override
    {
        cur_ = d.index();
    }
    void uncheckedSetDevice(c10::Device d) const noexcept override
    {
        cur_ = d.index();
    }
    c10::Stream getStream(c10::Device d) const override
    {
        return c10::Stream(c10::Stream::UNSAFE, d, 0);
    }
    c10::Stream exchangeStream(c10::Stream) const override
    {
        return c10::Stream(c10::Stream::UNSAFE, c10::Device(kNnrtDevice, cur_), 0);
    }
    c10::DeviceIndex deviceCount() const noexcept override
    {
        return 1;
    }
    static thread_local c10::DeviceIndex cur_;
};

thread_local c10::DeviceIndex NnrtGuardImpl::cur_ = 0;

C10_REGISTER_GUARD_IMPL(PrivateUse1, NnrtGuardImpl);

// ─────────────────────────────────────────────────────────────────────────────
// ③ Hooks
// ─────────────────────────────────────────────────────────────────────────────
struct NnrtHooks : public at::PrivateUse1HooksInterface {
    bool isBuilt() const override
    {
        return true;
    }
    bool isAvailable() const override
    {
        return nnrt::Engine::inst().ready();
    }
    bool hasPrimaryContext(c10::DeviceIndex) const override
    {
        return true;
    }
    c10::DeviceIndex deviceCount() const override
    {
        return 1;
    }
    at::Device getDeviceFromPtr(void *) const override
    {
        return at::Device(kNnrtDevice, 0);
    }
    c10::Allocator *getPinnedMemoryAllocator() const override
    {
        return &g_alloc;
    }
    // 单设备: current device 恒为 0
    void setCurrentDevice(c10::DeviceIndex) const override {}
    c10::DeviceIndex getCurrentDevice() const override
    {
        return 0;
    }
    c10::DeviceIndex exchangeDevice(c10::DeviceIndex) const override
    {
        return 0;
    }
    c10::DeviceIndex maybeExchangeDevice(c10::DeviceIndex) const override
    {
        return 0;
    }
};

NnrtHooks g_hooks;

// ─────────────────────────────────────────────────────────────────────────────
// ④ 张量搬运(见文件头 坑1/坑2/坑3)
// ─────────────────────────────────────────────────────────────────────────────
// 设备 → CPU: **真拷贝**(不是换标签的视图)。
//   ⚠ 曾用 at::from_blob 造"零拷贝同源视图", 结果 CPU kernel 一碰就挂死 —— 2026-09-12
//     真机实判: aten::sum / aten::fill_ 都卡死在 op.callBoxed 内(无异常、无崩溃、
//     faulthandler 无输出、进程仍在), 而同一 fallback 路径下的 aten::empty(输入不含该
//     视图)完全正常; 把输入换成普通 CPU 张量后即恢复。代价是一次 memcpy, P1 可接受。
at::Tensor retagToCpu(const at::Tensor &t)
{
    TORCH_CHECK(t.is_contiguous(), "nnrt: P1 搬运仅支持连续张量");
    auto out = at::empty(t.sizes(), t.options().device(c10::DeviceType::CPU));
    if (t.nbytes() > 0) {
        std::memcpy(out.data_ptr(), t.data_ptr(), t.nbytes());
    }
    return out;
}

// CPU → 设备: 本设备 allocator 分配 + 零拷贝视图(此处**必须**用 from_blob)。
//   理由: at::empty(..., device=PrivateUse1) 会再进本 fallback → 递归;
//   而 from_blob 直接构造 TensorImpl、不经过 dispatcher。
//   该张量只被两类代码使用, 均不进 CPU kernel: ① 本后端(engine/anato matmul);
//   ② 下一次 fallback —— 届时它会被 retagToCpu 拷成普通 CPU 张量。
//   ⚠ P1 已知缺口: in-place 算子(如 fill_)经 fallback 改的是副本, 结果虽贴回设备但原
//     张量未被就地更新。P1 不支持 in-place 语义; P2 需下沉 _to_copy/fill_ 彻底解决。
at::Tensor retagToDevice(const at::Tensor &cpu)
{
    auto holder = std::make_shared<at::Tensor>(cpu);
    return at::from_blob(
        cpu.data_ptr(), cpu.sizes(), cpu.strides(),
        [holder](void *) {},                       // 释放权归 holder
        cpu.options().device(kNnrtDevice));
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑤ 算子: aten::mm 下沉 NNRt(2D 矩阵乘)
//   ⚠ 不要下沉 aten::matmul! 它是 CompositeImplicitAutograd 算子 —— 调度器遇到它时
//     直接跑 composite 实现(polyfill 到 mm / bmm / mul), **根本不查 backend kernel**,
//     注册 m.impl("matmul", ...) 永远不会被调用。
//     2026-09-12 实测: 注册 matmul 后 torch.matmul(本设备张量) 一路卡死且 kernel 无任何日志;
//     而 sum/fill_ 这类走 fallback 的算子正常。**下沉要选真正的 backend 算子(mm/bmm)**。
//   P1 约束: fp32 + 2D + 连续(不满足则 TORCH_CHECK 明确报错, 不退化为静默错值)
// ─────────────────────────────────────────────────────────────────────────────
at::Tensor nnrt_mm(const at::Tensor &a, const at::Tensor &b)
{
    // 分步日志: 本 kernel 不经 fallback(直接注册在 PrivateUse1 上), 故 FB 日志里看不到它 ——
    //   2026-09-12 排查时这点曾把注意力引偏。有它才能把"卡在 dispatch 前"与"卡在 kernel 内"分开。
    blog("NNRT-MM enter da=%lld db=%lld", (long long)a.dim(), (long long)b.dim());
    TORCH_CHECK(a.dim() == 2 && b.dim() == 2, "nnrt matmul: P1 仅支持 2D");
    TORCH_CHECK(a.scalar_type() == at::kFloat && b.scalar_type() == at::kFloat,
                "nnrt matmul: P1 仅支持 float32");
    TORCH_CHECK(a.is_contiguous() && b.is_contiguous(), "nnrt matmul: 需连续张量");
    const int64_t M = a.size(0), K = a.size(1), N = b.size(1);
    TORCH_CHECK(b.size(0) == K, "nnrt matmul: K 维不匹配(", K, " vs ", b.size(0), ")");

    blog("NNRT-MM checks-ok M=%lld K=%lld N=%lld", (long long)M, (long long)K, (long long)N);
    // 输入数值留痕(2026-09-12): 实测 4×4 全 1 相乘得 2.0 而非 4.0 ——
    //   打印 a/b 前 4 元素可把病因劈成两半: 数据不对(此处置位) vs NNRt 算错(此处正确)。
    {
        const float *pa = a.data_ptr<float>(), *pb = b.data_ptr<float>();
        const size_t na = (size_t)a.numel(), nb = (size_t)b.numel();
        blog("NNRT-MM in-a[0..3]=%.1f,%.1f,%.1f,%.1f (numel=%zu)",
             (double)pa[0], (double)pa[na > 1 ? 1 : 0], (double)pa[na > 2 ? 2 : 0],
             (double)pa[na > 3 ? 3 : 0], na);
        blog("NNRT-MM in-b[0..3]=%.1f,%.1f,%.1f,%.1f (numel=%zu)",
             (double)pb[0], (double)pb[nb > 1 ? 1 : 0], (double)pb[nb > 2 ? 2 : 0],
             (double)pb[nb > 3 ? 3 : 0], nb);
    }
    auto y = at::empty({M, N}, a.options());
    blog("NNRT-MM empty-ok");
    auto &e = nnrt::Engine::inst();
    if (!e.matmul(a.data_ptr<float>(), b.data_ptr<float>(), y.data_ptr<float>(), M, K, N)) {
        TORCH_CHECK(false, "nnrt matmul 失败: ", e.err());
    }
    blog("NNRT-MM engine-done");
    {
        const float *py = y.data_ptr<float>();
        const size_t ny = (size_t)y.numel();
        blog("NNRT-MM out[0..3]=%.1f,%.1f,%.1f,%.1f (numel=%zu)",
             (double)py[0], (double)py[ny > 1 ? 1 : 0], (double)py[ny > 2 ? 2 : 0],
             (double)py[ny > 3 ? 3 : 0], ny);
    }
    return y;
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑥ fallback: 未下沉算子 → 回 CPU 执行(见文件头 坑2: device 参数决定输出归属)
// ─────────────────────────────────────────────────────────────────────────────
// ⚠ c10::Type::castRaw<T>() 不是安全的向下转型: 它 = static_cast, 既不校验 kind 也不返回
//   null。曾经写成 `if (auto *o = t->castRaw<OptionalType>())` —— 对任何类型都"成立",
//   于是把普通类型当 Optional 读 getElementType() 读野内存 → SIGSEGV。
//   2026-09-12 真机实判: 表现为自检日志停在 `.to("nnrt")` 前一行、进程直接死(非 Python 异常,
//   故无 EX 行), 后端随 torch import 一起没起来。**必须先判 kind 再 castRaw**。
bool typeMentionsDevice(const c10::TypePtr &t)
{
    switch (t->kind()) {
    case c10::TypeKind::DeviceObjType:
        return true;
    case c10::TypeKind::OptionalType:
        return typeMentionsDevice(t->castRaw<c10::OptionalType>()->getElementType());
    case c10::TypeKind::ListType:
        return typeMentionsDevice(t->castRaw<c10::ListType>()->getElementType());
    default:
        return false;
    }
}

void nnrtFallback(const c10::OperatorHandle &op, c10::Stack *stack)
{
    // ── 递归守卫(2026-09-12 实测定谳的必备手段)──
    //   实测现象: `.to("nnrt")` 处日志戛然而止, 且 faulthandler 连 SIGSEGV 都写不出来
    //   (栈已耗尽 = 无栈可写) —— 静默爆栈, Python 层 except 也抓不到, 无从定位。
    //   故在此主动截断: 把"静默爆栈"变成"带算子名的可捕获异常"。若本行触发, 即证明
    //   有算子在 fallback 内又回调进了本后端(自回调), 算子名会直接指出是谁。
    TORCH_CHECK(g_fbDepth < 8,
                "nnrt fallback 递归过深 depth=", g_fbDepth, " op=", op.schema().name());
    struct DepthGuard {
        DepthGuard() { ++g_fbDepth; }
        ~DepthGuard() { --g_fbDepth; }
    } dg;
    blog("NNRT-FB depth=%d op=%s", g_fbDepth, op.schema().name().c_str());

    const auto &args = op.schema().arguments();

    // 1) 有 Device 参数在本设备上 ⇒ 记下期望设备, 改写为 CPU 后执行(结果可能不是设备张量)
    bool hasDeviceArg = false, wantDevice = false;
    for (size_t i = 0; i < args.size() && i < stack->size(); i++) {
        if (!typeMentionsDevice(args[i].type())) {
            continue;
        }
        hasDeviceArg = true;
        auto &iv = (*stack)[i];
        if (!iv.isDevice()) {
            continue;
        }
        if (iv.toDevice().type() != kNnrtDevice) {
            continue;
        }
        wantDevice = true;
        iv = c10::Device(c10::DeviceType::CPU);
    }
    blog("NNRT-FB s1 hasDev=%d wantDev=%d narg=%zu", int(hasDeviceArg), int(wantDevice), args.size());

    // 2) 入参设备张量 → 零拷贝贴回 CPU 标签后交 CPU 执行
    for (size_t i = 0; i < stack->size(); i++) {
        auto &iv = (*stack)[i];
        if (iv.isTensor() && iv.toTensor().device().type() == kNnrtDevice) {
            iv = retagToCpu(iv.toTensor());
        }
    }
    blog("NNRT-FB s2 retag-in done");
    // s2 校验: 逐个张量确认已脱离本设备 —— 若这里仍打印 pu1=1, 说明 retag 没生效,
    //   callBoxed 会再次 dispatch 回本后端(表现为"卡死"而非抛错)。
    for (size_t i = 0; i < stack->size(); i++) {
        auto &civ = (*stack)[i];
        if (civ.isTensor()) {
            const auto &tt = civ.toTensor();
            blog("NNRT-FB s2x i=%zu dev=%s pu1=%d nbytes=%zu",
                 i, tt.device().str().c_str(),
                 int(tt.unsafeGetTensorImpl()->key_set().has(c10::DispatchKey::PrivateUse1)),
                 (size_t)tt.nbytes());
        }
    }

    // 3) 排除本 dispatch key 再执行, 否则再次命中本 fallback(死循环)
    // 3) 用 CPU 的 dispatch key 重新派发(kernel 在 CPU 上, 输入也已贴成 CPU 张量)。
    //   ⚠ 不要写成 ExcludeDispatchKeyGuard + callBoxed —— 那条路要让 dispatcher 再从
    //     stack / TLS 推导一次 key set, 本后端的功能键(AutogradPrivateUse1)会参与其中。
    //     2026-09-12 实测: 该路径必卡死在 callBoxed 内, 且无异常、无崩溃、faulthandler 无输出。
    //     分水岭在 key set 的来源: at::empty(本设备) 的 key set 来自 **device 参数** → 能过;
    //     aten::sum / aten::fill_ 这类由 **张量** 推导 key set 的 → 必卡(stack 里张量当时
    //     已确认为纯 CPU, 故与"张量没转干净"无关)。
    //   redispatchBoxed 直接给定目标 key set、绕开推导; 亦为 torch 自带
    //   MathBitsFallback(ATen/native/MathBitsFallback.h)采用的写法。
    blog("NNRT-FB s3 about-redispatch");
    op.redispatchBoxed(c10::DispatchKeySet(c10::DispatchKey::CPU), stack);
    blog("NNRT-FB s4 redispatch-ok rsize=%zu", stack->size());

    // 4) 结果归属: 无 Device 参数 ⇒ 随输入(必贴回); 有 ⇒ 仅当期望设备是本设备时贴回
    if (hasDeviceArg && !wantDevice) {
        blog("NNRT-FB s5 keep-cpu-return");
        return;
    }
    const size_t nReturn = op.schema().returns().size();
    if (nReturn == 0 || nReturn > stack->size()) {
        blog("NNRT-FB s5 no-ret nret=%zu", nReturn);
        return;
    }
    const size_t base = stack->size() - nReturn;
    for (size_t i = base; i < stack->size(); i++) {
        auto &iv = (*stack)[i];
        if (iv.isTensor() && iv.toTensor().device().is_cpu()) {
            iv = retagToDevice(iv.toTensor());
        }
    }
    blog("NNRT-FB s6 done");
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 注册入口(由 bootstrap.cpp 的 _register() 在 torch import 末尾调用)
// ─────────────────────────────────────────────────────────────────────────────
namespace nnrt_backend {

// 注册表部分(allocator/hooks)须运行时调用; guard 由 C10_REGISTER_GUARD_IMPL 静态注册。
void registerBackend()
{
    c10::SetAllocator(kNnrtDevice, &g_alloc);
    if (!at::isPrivateUse1HooksRegistered()) {
        at::RegisterPrivateUse1HooksInterface(&g_hooks);
    }
}

} // namespace nnrt_backend

TORCH_LIBRARY_IMPL(aten, PrivateUse1, m)
{
    // mm = 真正的 backend 算子(2D 矩阵乘)。torch.matmul 会 polyfill 到这里。
    m.impl("mm", &nnrt_mm);
}

TORCH_LIBRARY_IMPL(_, PrivateUse1, m)
{
    m.fallback(torch::CppFunction::makeFromBoxedFunction<&nnrtFallback>());
}
