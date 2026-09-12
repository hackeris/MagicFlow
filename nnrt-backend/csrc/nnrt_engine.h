// nnrt_engine.h — NNRt 在线执行引擎(薄封装)。
//
// 设计原则(见 docs/npu-backend-roadmap.md §2): 面向"同体系下一代"而非"可换后端",
//   故不做过度抽象 —— 只把 NNRt 调用集中在这一层, 换代时集中修改。
//
// 铁律(沿袭 poc/npu): 全部 dlopen+dlsym, 不链接 NNRt 库(符号缺失不拖垮宿主进程)。
// 构图参数规格照抄 poc/npu/entry/src/main/cpp/nnrt_probe.cpp 的实测版本
//   (33 项算子已在 9030 上验证; MATMUL 的 transpose 标志实测须 OH_NN_BOOL 而非 INT64)。
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace nnrt {

// 诊断日志: 追加写 <PYTHONHOME>/nnrt-p0a.log(与 backend.cpp 的 blog、bootstrap.cpp 同落盘口)。
//   放头文件是因为引擎层(尤其 Runner 的"构图/编译/执行"三段)此前零日志 —— 2026-09-12 实测
//   aten::mm 卡死时, 无法区分卡在 dispatch 前 / kernel 内 / 引擎内, 白跑一轮真机。
//   elogv 供已有 va_list 的转发函数(backend.cpp 的 blog)复用, 避免同一实现写三遍。
inline void elogv(const char *fmt, va_list ap)
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
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fclose(f);
}

inline void elog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    elogv(fmt, ap);
    va_end(ap);
}

class Engine {
public:
    static Engine &inst();

    // dlopen + dlsym + 设备枚举(设备枚举在 9030 上需重试: HDI 异步注册)。
    //   cacheDir: Build 前必须 SetCache 的目录(须已存在)。
    bool init(const char *cacheDir);
    bool ready() const { return ready_; }
    size_t deviceId() const { return device_; }

    // 编译性能模式(0=NONE 1=LOW 2=MEDIUM 3=EXTREME), 直通 OH_NNCompilation_SetPerformanceMode。
    //   2026-09-12 实测: EXTREME 下 MATMUL 的相对误差 ~4.2e-3(疑似内部降精度换性能),
    //   远超 fp32 累加应有的 ~1e-6 —— 故做成可切换, 便于按需在"精度/速度"间取值。
    //   ⚠ 模式参与缓存子目录名: 不同模式的编译结果不可复用(否则又是"串图"那类 bug 的翻版)。
    // ⚠ 上界原为 3 —— 使 OH_NN_PERFORMANCE_EXTREME(=4) **不可达**, 与 poc/npu 的核心差异之一
    //   (POC 一直用 EXTREME)。2026-09-12 放宽到 4; 默认值仍为 3(HIGH), 不改既有行为。
    void setPerfMode(int m) { perfMode_ = (m < 0 || m > 4) ? 3 : m; }
    int perfMode() const { return perfMode_; }
    const char *cacheDir() const { return cacheDir_; }   // Runner(内部组件)要用
    const char *err() const { return err_; }

    // y[M,N] = a[M,K] @ b[K,N]  (fp32, 行主序)
    bool matmul(const float *a, const float *b, float *y, int64_t M, int64_t K, int64_t N);

    // y[rows,cols] = softmax(x[rows,cols], axis=1)  (fp32)
    bool softmax(const float *x, float *y, int64_t rows, int64_t cols);

    // y[n,oh,ow,oc] = conv2d(x[n,ih,iw,c], w[oc,kh,kw,c], bias[oc])   —— 全部 **NHWC**
    //   ⚠ 权重是 **OHWI** 而非 PyTorch 的 OIHW: NNRt 的 conv2d_builder 取
    //     inChannel = weightShape[3]、kernelSize = [weightShape[1], weightShape[2]]。
    //   ⚠ 张量是 **NHWC**(MindIR Conv2DFusion 语义), **不是** PyTorch 的 NCHW:
    //     传 NCHW 会被读成 N=1,H=C,W=H,C=W, 与权重反推的 inChannel 冲突 ⇒ compBuild rc=1。
    //   ⚠ **布局重排由调用方在 host 上手工做**(本层不做; 用 permute().contiguous() 会因
    //     PrivateUse1 上的 aten 拷贝 op 走 fallback 而让子进程消失)。
    //   strides = [sH,sW] (**rank 2**, 不是 4); pads = [top,bottom,left,right] (rank 4)。
    //   OH=(H+top+bottom-KH)/sH+1 同理。
    bool conv2d(const float *x, const float *w, const float *bias, float *y,
                int64_t n, int64_t c, int64_t ih, int64_t iw,
                int64_t oc, int64_t kh, int64_t kw,
                const int64_t *strides, const int64_t *pads);

private:
    Engine() = default;
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    void setErr(const char *fmt, ...);

    bool ready_ = false;
    size_t device_ = 0;
    int perfMode_ = 3;   // 默认 EXTREME(与 poc/npu 实测一致)
    char cacheDir_[512] = "";
    char err_[512] = "";
};

} // namespace nnrt
