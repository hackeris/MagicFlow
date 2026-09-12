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
#include <atomic>
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
// ⑥ 算子: aten::_softmax 下沉 NNRt
//   ⚠ 同 matmul 的教训: `aten::softmax` 是 CompositeImplicitAutograd, 注册它**永远不被调用**;
//     真正该下沉的是 backend 算子 `aten::_softmax`(composite 的 softmax 会 polyfill 到它)。
//   P1 约束: fp32 + 2D + 连续 + dim=1/-1(即沿最后一维)。不满足则 TORCH_CHECK 明确报错,
//     绝不静默退化(静默错值比崩溃更难查 —— 缓存串图那轮的教训)。
// ─────────────────────────────────────────────────────────────────────────────
at::Tensor nnrt_softmax(const at::Tensor &x, int64_t dim, bool half_to_float)
{
    blog("NNRT-SM enter dim=%lld half=%d", (long long)dim, int(half_to_float));
    TORCH_CHECK(!half_to_float, "nnrt softmax: P1 不支持 half_to_float");
    TORCH_CHECK(x.dim() == 2, "nnrt softmax: P1 仅支持 2D, 实际 dim=", x.dim());
    TORCH_CHECK(x.scalar_type() == at::kFloat, "nnrt softmax: P1 仅支持 float32");
    TORCH_CHECK(x.is_contiguous(), "nnrt softmax: 需连续张量");
    const int64_t d = dim < 0 ? dim + x.dim() : dim;
    TORCH_CHECK(d == 1, "nnrt softmax: P1 仅支持 dim=1(最后一维), 实际 dim=", dim);
    const int64_t rows = x.size(0), cols = x.size(1);
    blog("NNRT-SM checks-ok rows=%lld cols=%lld", (long long)rows, (long long)cols);
    auto y = at::empty(x.sizes(), x.options());
    auto &e = nnrt::Engine::inst();
    if (!e.softmax(x.data_ptr<float>(), y.data_ptr<float>(), rows, cols)) {
        TORCH_CHECK(false, "nnrt softmax 失败: ", e.err());
    }
    blog("NNRT-SM done");
    return y;
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑦ 算子: aten::convolution 下沉 NNRt
//   ⚠ 下沉目标**不是** `aten::conv2d`: 它是 structured_delegate(composite), 注册了也不被调用;
//     真正的 backend 算子是 `aten::convolution`(conv2d 会 polyfill 到它) —— 同 matmul→mm 的
//     关系。也正因为是 backend 算子, 签名是纯 int[] 而非 SymInt[], 省去符号整数的处理。
//   P1 约束: fp32 + 4D(NCHW 输入 / OIHW 权重) + 非转置 + groups=1 + dilation=1 + 带 bias。
//     不满足则 TORCH_CHECK 明确报错(不静默退化 —— 缓存串图那轮的教训)。
// ─────────────────────────────────────────────────────────────────────────────
// ⚠⚠ **不满足条件时返回未定义张量(而非抛异常)**, 由 nnrtFallback 回 CPU —— 见下方"为什么".
//   返回定义了的张量 = 下沉成功(结果已在本设备上)。
//
// 全局熔断: NNRt 侧一旦失败(2026-09-12 实测 9030 的 conv2d 恒 build-rc=1), 后续不再尝试 ——
//   否则**每一次卷积**都要白跑一遍"构图→编译失败"并做两次 host 重排, SD 出图会被拖垮。
//   ⚠ 代价是"某个形状失败 ⇒ 全部形状不再下沉"。对当前情形(设备侧整体不支持)这是对的;
//     若将来设备支持了, 该失败不再发生, 熔断自然不会被触发。
std::atomic<bool> g_convSinkEnabled{true};

at::Tensor nnrtConvTry(const at::Tensor &input, const at::Tensor &weight,
                       const std::optional<at::Tensor> &bias,
                       const std::vector<int64_t> &stride, const std::vector<int64_t> &padding,
                       const std::vector<int64_t> &dilation, bool transposed,
                       const std::vector<int64_t> &output_padding, int64_t groups)
{
    if (!g_convSinkEnabled.load(std::memory_order_relaxed)) { return {}; }   // 已熔断
    blog("NNRT-CONV enter transposed=%d groups=%lld bias=%d",
         int(transposed), (long long)groups, int(bias.has_value()));
    blog("NNRT-CONV s1");
    // 不可下沉的情形一律 **静默回 CPU**(不打 TORCH_CHECK): 这些在 SD 的 UNet 里都是常规形态
    //   (大量无 bias 的 conv 与 groups≠1 的分组卷积), 抛异常 = 出图必中断。
    if (transposed) { blog("NNRT-CONV skip transposed"); return {}; }
    if (groups != 1) { blog("NNRT-CONV skip groups=%lld", (long long)groups); return {}; }
    if (!bias.has_value()) { blog("NNRT-CONV skip no-bias"); return {}; }
    blog("NNRT-CONV s2");
    if (input.dim() != 4 || weight.dim() != 4) { blog("NNRT-CONV skip dim"); return {}; }
    blog("NNRT-CONV s3");
    if (input.scalar_type() != at::kFloat || weight.scalar_type() != at::kFloat) {
        blog("NNRT-CONV skip dtype"); return {};
    }
    if (!input.is_contiguous() || !weight.is_contiguous()) { blog("NNRT-CONV skip contig"); return {}; }
    // ⚠ int[] 参数在 dispatch 之后可能是**单元素广播形式** —— 2026-09-12 实测: `at::conv2d` 的
    //   默认 `int[2] stride=1` 传到本 kernel 时 size 竟是 **1**(而非 2), 原断言 size==2 直接失败;
    //   当时还误以为是越界读, 直到把实际 size 打出来才看清(sizeS=1 sizeP=1 sizeD=1)。
    //   **凡接 int[] 参数, 一律按 1/2 两种长度处理, 不要假设维度数。**
    const size_t szS = stride.size(), szP = padding.size(), szD = dilation.size();
    blog("NNRT-CONV s4 sizeS=%zu sizeP=%zu sizeD=%zu", szS, szP, szD);
    if (!((szS == 1 || szS == 2) && (szP == 1 || szP == 2) && (szD == 1 || szD == 2))) {
        blog("NNRT-CONV skip int-list-len"); return {};
    }
    const int64_t sH = stride[0], sW = szS == 2 ? stride[1] : stride[0];
    const int64_t pH = padding[0], pW = szP == 2 ? padding[1] : padding[0];
    const int64_t dH = dilation[0], dW = szD == 2 ? dilation[1] : dilation[0];
    blog("NNRT-CONV s4b sH=%lld sW=%lld pH=%lld pW=%lld dH=%lld dW=%lld",
         (long long)sH, (long long)sW, (long long)pH, (long long)pW,
         (long long)dH, (long long)dW);
    if (dH != 1 || dW != 1) { blog("NNRT-CONV skip dilation"); return {}; }
    const int64_t n = input.size(0), c = input.size(1);
    const int64_t ih = input.size(2), iw = input.size(3);
    const int64_t oc = weight.size(0), kh = weight.size(2), kw = weight.size(3);
    if (weight.size(1) != c || bias->numel() != oc) { blog("NNRT-CONV skip shape-mismatch"); return {}; }
    const int64_t oh = (ih + 2 * pH - kh) / sH + 1;
    const int64_t ow = (iw + 2 * pW - kw) / sW + 1;
    blog("NNRT-CONV checks-ok N=%lld C=%lld H=%lld W=%lld OC=%lld KH=%lld KW=%lld -> OH=%lld OW=%lld",
         (long long)n, (long long)c, (long long)ih, (long long)iw,
         (long long)oc, (long long)kh, (long long)kw, (long long)oh, (long long)ow);
    auto y = at::empty({n, oc, oh, ow}, input.options());
    blog("NNRT-CONV s5 empty-ok");
    // ⚠⚠ NNRt 的 conv2d 权重约定是 **[OC, KH, KW, C]**(OHWI), **不是** PyTorch 的 OIHW。
    //   权威依据: build/nnrt-src/frameworks/native/neural_network_runtime/ops/conv2d_builder.cpp
    //   —— SetChannel 取 inChannel = weightShape[3]、outChannel = weightShape[0];
    //   SetKernelSize 取 kernelSize = [weightShape[1], weightShape[2]]。
    //   传 OIHW 的后果**不是报错而是 compBuild 挂起**: 编译器拿到 kernel=[IC,KH]、inChannel=KW
    //   的自相矛盾图。2026-09-12 实测: 输入 C=2、OIHW [3,2,3,3] → 声明 inChannel=3 与输入
    //   通道 2 不符, 日志停在 `e3 before-build`, 110s 无 `e4 build-ok`。
    //   ⚠ poc/npu 的 CONV2D 样本是**全 1 数据 + kH=kW=inC=3**, 布局怎么排结果都一样 ——
    //     别拿"POC 过了"当反证; 凡尺寸不等的 weight, 布局一错必现。
    //   ⚠ 转换**必须手工在 host 上做**, 不能用 `weight.permute(..).contiguous()` ——
    //     PrivateUse1 张量上的拷贝是 aten op, 会走 fallback→CPU redispatch: 2026-09-12 实测
    //     该写法的表现是 comfy_child 直接消失(日志停在 checks-ok, 无 e0 无 done)。
    //     引擎接口本就只收裸 float*, 手工重排零 aten op, 是这里唯一安全的做法。
    std::vector<float> wOhwi((size_t)oc * kh * kw * c);
    const float *wp = weight.data_ptr<float>();          // OIHW 行主序
    for (int64_t o = 0; o < oc; o++)
        for (int64_t h = 0; h < kh; h++)
            for (int64_t x = 0; x < kw; x++)
                for (int64_t i = 0; i < c; i++)
                    wOhwi[((o * kh + h) * kw + x) * c + i] = wp[((o * c + i) * kh + h) * kw + x];
    blog("NNRT-CONV s6 w-ohwi-ready(%zu)", wOhwi.size());
    // ⚠⚠ 布局: NNRt 走 MindIR Conv2DFusion, **张量按 NHWC 解读** —— 传 NCHW 的 [1,C,H,W]
    //   会被读成 N=1,H=C,W=H,C=W, 与权重反推的 inChannel=C 直接冲突。2026-09-12 实测矩阵
    //   见 bootstrap.cpp 的 CvTry 注释(s2/s4 × nchw/nhwc 四组)。
    //   故这里做 **NCHW→NHWC 的手工重排**(同 weight: 零 aten op, 不用 permute().contiguous(),
    //   后者在 PrivateUse1 张量上会 fallback→CPU redispatch 并让子进程消失)。
    std::vector<float> xNhwc((size_t)n * ih * iw * c);
    {
        const float *xp = input.data_ptr<float>();        // NCHW [n,c,ih,iw] 行主序
        for (int64_t ni = 0; ni < n; ni++)
            for (int64_t hi = 0; hi < ih; hi++)
                for (int64_t wi = 0; wi < iw; wi++)
                    for (int64_t ci = 0; ci < c; ci++)
                        xNhwc[((ni * ih + hi) * iw + wi) * c + ci] =
                            xp[((ni * c + ci) * ih + hi) * iw + wi];
    }
    // ⚠ strides 是 **rank=2** 的 [sH, sW], **不是** 4 元素 —— 权威依据 build/nnrt-src/.../
    //   ops/conv2d_builder.cpp 的 SetStrides: 它**不校验 rank**, 按 GetElementCount() 整段拷进
    //   m_strides 再交给 MindIR_Conv2DFusion; 而单测用 m_stride_dim{2}、GetPrimitive 断言
    //   GetStride() 返回 {1,1}(2 元素)。传 4 元素 ⇒ 设备侧拿到畸形 stride ⇒ compBuild rc=1。
    //   (pads 相反: PAD 的 padList 必须是 4 元素, 见 SetPad 的 elementCount != 4 判断。)
    int64_t st[2] = {sH, sW};
    int64_t pd[4] = {pH, pH, pW, pW};   // [top,bottom,left,right] 顺序
    std::vector<float> yNhwc((size_t)n * oh * ow * oc);
    auto &e = nnrt::Engine::inst();
    if (!e.ready()) { blog("NNRT-CONV skip engine-not-ready"); return {}; }
    // ⚠ 下沉失败(如 9030 设备侧不支持 CONV2D, build-rc=1)也**回 CPU**, 不抛异常 ——
    //   这是 P1 的既定策略: 下沉是加速, 不是正确性的前提。
    if (!e.conv2d(xNhwc.data(), wOhwi.data(), bias->data_ptr<float>(),
                  yNhwc.data(), n, c, ih, iw, oc, kh, kw, st, pd)) {
        g_convSinkEnabled.store(false, std::memory_order_relaxed);
        blog("NNRT-CONV nnrt-failed -> cpu(并熔断后续尝试): %s", e.err());
        return {};
    }
    // NHWC [n,oh,ow,oc] → NCHW [n,oc,oh,ow] 手工重排(同理, 零 aten op)
    {
        float *yp = y.data_ptr<float>();
        for (int64_t ni = 0; ni < n; ni++)
            for (int64_t oi = 0; oi < oc; oi++)
                for (int64_t hi = 0; hi < oh; hi++)
                    for (int64_t wi = 0; wi < ow; wi++)
                        yp[((ni * oc + oi) * oh + hi) * ow + wi] =
                            yNhwc[((ni * oh + hi) * ow + wi) * oc + oi];
    }
    blog("NNRT-CONV done");
    return y;
}

// ── ⑧b conv 的注册入口: **convolution_overrideable**(不是 convolution!) ────────────────
// 依据(externals/pytorch-src 的 native_functions.yaml + Convolution.cpp):
//   conv2d(structured_delegate) / convolution / _convolution **三者都是
//   `CompositeExplicitAutograd`** —— composite 算子在所有 backend 上都有实现,
//   **永不进 fallback**,注册它们等于没注册(实测: 注册 convolution 后 c27 仍抛
//   "convolution_overrideable not implemented", 且日志里连一行 NNRT-FB 都没有)。
//   真正对"源码外后端(out-of-source backend)"开放的是:
//     select_conv_backend() 末尾 `else { return ConvBackend::Overrideable; }`
//       → `at::convolution_overrideable(...)`,其默认实现直接
//         TORCH_CHECK_NOT_IMPLEMENTED("... please use TORCH_LIBRARY_IMPL to override this function")
//   —— 这句话就是 PyTorch 给自定义后端留的**官方入口**,本函数注册的正是它。
//   ⚠ 用 **boxed** 注册(而非 unboxed 函数指针): boxed 直接绑定到已注册 schema,不经过
//     "从函数指针推断 schema"那一步,少一个出错源;参数从 stack 按 schema 顺序取。
void nnrtConvBoxed(const c10::OperatorHandle &, c10::Stack *stack)
{
    // schema: convolution_overrideable(Tensor, Tensor, Tensor?, SymInt[]×3, bool, SymInt[], SymInt)
    auto toVec = [](const c10::IValue &v) {
        std::vector<int64_t> out;
        if (v.isSymIntList()) {
            // ListElementReference 不直接暴露 expect_int, 先落到 vector<SymInt> 再取值
            for (const c10::SymInt &s : v.toSymIntList().vec()) { out.push_back(s.expect_int()); }
        } else if (v.isIntList()) {
            out = v.toIntList().vec();
        }
        return out;
    };
    const at::Tensor input = (*stack)[0].toTensor();
    const at::Tensor weight = (*stack)[1].toTensor();
    std::optional<at::Tensor> bias;
    if (!(*stack)[2].isNone()) { bias = (*stack)[2].toTensor(); }
    const std::vector<int64_t> st = toVec((*stack)[3]), pd = toVec((*stack)[4]);
    const std::vector<int64_t> dl = toVec((*stack)[5]), op = toVec((*stack)[7]);
    const bool transposed = (*stack)[6].toBool();
    const int64_t groups = (*stack)[8].toInt();

    at::Tensor r = nnrtConvTry(input, weight, bias, st, pd, dl, transposed, op, groups);
    if (r.defined()) {
        blog("NNRT-CONV boxed sunk-ok");
    } else {
        // 不可下沉 / 下沉失败 → **回 CPU**。
        //   ⚠ 不能借用其它算子的 fallback redispatch: CPU 上的 convolution_overrideable
        //     就是那个"未实现"的默认实现, redispatch 过去照样抛异常。
        //     要真正回 CPU, 必须让 select_conv_backend **按 CPU 重新选 backend** —— 那取决于
        //     **输入张量本身的 device**, 所以先把输入拷成 CPU 张量, 再调 at::conv2d
        //     (CPU 上它选 Slow2d/Slow3d/Mkldnn 等真实实现, 不会再落回 Overrideable)。
        auto ci = retagToCpu(input);
        auto cw = retagToCpu(weight);
        std::optional<at::Tensor> cb;
        if (bias.has_value()) { cb = retagToCpu(*bias); }
        blog("NNRT-CONV boxed -> cpu(slow path)");
        r = transposed ? at::conv_transpose2d(ci, cw, cb, st, pd, op, groups, dl)
                       : at::conv2d(ci, cw, cb, st, pd, dl, groups);
    }
    // boxed kernel 约定: 返回时 stack 里**只留返回值**(参数已被消费)
    stack->clear();
    stack->push_back(r);
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑧ fallback: 未下沉算子 → 回 CPU 执行(见文件头 坑2: device 参数决定输出归属)
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
    // ⚠ conv 的注册**必须放在运行时**而不是 TORCH_LIBRARY_IMPL 里:
    //   静态注册的异常**无法捕获**, 会让整个扩展 dlopen 失败 —— 表现是"子进程起来了但
    //   什么日志都没有"(2026-09-12 实测: 连 `NNRT-P0A s1 register-entered` 都没写, 而
    //   napi.log 显示 startComfyChild rc=0)。放这里失败也能留痕。
    //   分段日志(a/b/c/d)是必需的: 2026-09-12 上一版只打一条"注册完成", 结果崩在
    //   哪一步完全看不出来(表现为日志停在 registerBackend 之前一行、无异常)。
    blog("NNRT-B s-conv a: enter");
    try {
        static auto lib = torch::Library(torch::Library::IMPL, "aten",
                                         std::make_optional(c10::DispatchKey::PrivateUse1),
                                         __FILE__, __LINE__);
        blog("NNRT-B s-conv b: library-constructed");
        lib.impl("convolution_overrideable",
                 torch::CppFunction::makeFromBoxedFunction<&nnrtConvBoxed>());
        blog("NNRT-B s-conv c: conv-registered(convolution_overrideable)");
    } catch (const std::exception &e) {
        blog("NNRT-B s-conv d: register FAILED: %s", e.what());
    } catch (...) {
        blog("NNRT-B s-conv d: register FAILED(unknown)");
    }
}

} // namespace nnrt_backend

TORCH_LIBRARY_IMPL(aten, PrivateUse1, m)
{
    // mm = 真正的 backend 算子(2D 矩阵乘)。torch.matmul 会 polyfill 到这里。
    m.impl("mm", &nnrt_mm);
    m.impl("_softmax", &nnrt_softmax);
    // ⚠ conv **不在此注册**: 见 registerBackend() 里的运行时注册段(静态注册失败会拖垮 dlopen)。
}

TORCH_LIBRARY_IMPL(_, PrivateUse1, m)
{
    m.fallback(torch::CppFunction::makeFromBoxedFunction<&nnrtFallback>());
}
