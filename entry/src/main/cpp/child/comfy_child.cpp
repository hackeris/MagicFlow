/**
 * comfy_child.cpp — ComfyUI 后端 NCP 子进程入口。
 * 由父进程 OH_Ability_StartNativeChildProcess("libcomfy_child.so:Main") 拉起。
 *
 * Phase 0 验证目标（一次实测多个不确定点）：
 *   1. NCP 子进程可被拉起并正常运行         —— Main 打印 hello
 *   2. 子进程能 fork/exec 再派生子进程       —— ComfyUI 多进程 fan-out / DataLoader 前提
 *   3. 子进程能绑定 127.0.0.1 端口           —— ComfyUI 后端绑 127.0.0.1:8188 给 ArkWeb 前提
 *   4. 真实 CPU 速度基线                    —— 评估 ComfyUI CPU 推理性能
 * 后续 Step 0.3b：在此 dlopen skh 的 libpython3.12.so + Py_Initialize，接入 torch 栈（libc++ __1）。
 * 命名空间：真实 torch 栈链「系统 libc++.so.1 = std::__1」（非应用 libc++_shared.so = __n1），
 * comfy_child 已复链到 __1（见 CMakeLists），全进程统一 __1，避免同进程双 libc++（命门③碰撞）。
 */
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <cstdlib>
#include <cstdio>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <hilog/log.h>
#include <Python.h>   // 嵌入式宿主：把 libs/ 的 C++ 扩展 dlopen+PyInit+注册进 sys.modules（Python.h 提供 PyObject/PyImport 等）
#include <exception>  // std::exception：捕获 torch/pybind11 初始化异常（error_already_set→libc++abi terminate 前）
#include <typeinfo>   // typeid/type_info::name()：挖抛掷异常的真实类型名
#include <cxxabi.h>   // abi::__cxa_demangle：type name 反名字解析
#include <pthread.h>  // watchdog 守护线程（诊断 PyInit__C 死锁 vs 慢初始化）；heartbeat（0.4c 诊断）同用 pthread
#include <time.h>     // time() 供 watchdog 计时
#include <signal.h>   // SIGUSR1：watchdog→主线程，异步打断 PyInit__C 以打印卡死栈
#include <unwind.h>   // _Unwind_Backtrace：基于 .eh_frame 回溯（musl/OHOS 的 backtrace() 常返回 0，改用此）
#include <execinfo.h> // backtrace/backtrace_symbols（备用/回退）
#include <AbilityKit/native_child_process.h>
#include "dynload_exts.h"  // skh 3.12 lib-dynload 70 个内建 C 扩展清单（模块名可由文件名推导，Step 0.3b 生成）
#include <pybind11/pytypes.h>  // pybind11::error_already_set：catch 后打 what() 拿 torch 初始化真错（与 torch 同源头）

// ★ hilog domain 必须非零：log.h 默认 #define LOG_DOMAIN 0，而 domain=0 属受保护域，
//   会被 hilog 过滤导致 ComfyChild 子进程日志完全不可见（此前排查 PyInit__C 卡死全靠 36 帧，
//   一旦走了弯路真机就"一片黑"）。此处 override 为非零，子进程日志才稳定输出。
#undef LOG_DOMAIN
#define LOG_DOMAIN 0xD001
#undef LOG_TAG
#define LOG_TAG "ComfyChild"

#define LOGI(...) ((void)OH_LOG_INFO(LOG_APP, __VA_ARGS__))
#define LOGE(...) ((void)OH_LOG_ERROR(LOG_APP, __VA_ARGS__))

// ★ diag 文件诊断通道：stdout/stderr 被 freopen 到 <pyroot>/diag.log（app sandbox）。
//   ⚠ 2026-09-02 实证：这台设备 shell(uid 2000, u:r:sh:s0) 对 app 沙箱 /data/storage/el1/bundle/*
//   一律 EACCES（含 /data/app/el1 路径），smode 亦被「undebuggable」拒绝 → 沙箱文件通道对 hdc
//   完全封闭。故 DIAG 改为「stderr→diag.log + OH_LOG_INFO→hilog」双写：hilog 侧 domain=LOG_APP
//   （SDK log.h 内建 0xD001110），shell 属 log 组，`hilog | grep ComfyChild` 可旁路。
#define DIAG(...) do { \
    OH_LOG_INFO(LOG_APP, __VA_ARGS__); \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); \
    fflush(stderr); \
} while (0)

// ── __cxa_throw hook：挖全进程所有 C++ 抛掷的真实类型名 ─────────────────────────
// 现象：torch._C 初始化抛异常 → catch(::std::exception&) 不命中（case 3 的 probe）→
//   libc++abi 报 "pybind11::error_already_set" 后 terminate（若无人兜底）。
// 疑问：错误到底封装了什么、为何 std::exception 捕获不了 → hook 拦截 __cxa_throw，打印
//   typeinfo 反名字。若 libc++abi 先于本库被解析（依赖序 breadth-first），此处不生效，
//   catch(...) 里另有 __cxa_current_exception_type 兜底（两路都对同一事实取证）。
typedef void (*cxa_throw_fn)(void *, std::type_info *, void (*)(void *));
static cxa_throw_fn g_real_cxa_throw = nullptr;
static cxa_throw_fn g_saved_real_cxa_throw = nullptr;
// 在 dlopen 完成后（任何 C++ 抛掷发生前）用 dlsym(RTLD_NEXT) 预解析真实现。
// 抛掷路径（__cxa_throw）内禁止 dlsym（ld-musl dlsym 非 async-safe，且已在 v1 实证崩）。
static void init_cxa_throw_hook(void)
{
    g_saved_real_cxa_throw = (cxa_throw_fn)dlsym(RTLD_NEXT, "__cxa_throw");
    g_real_cxa_throw = g_saved_real_cxa_throw;
}
extern "C" void __cxa_throw(void *obj, std::type_info *ti, void (*dtor)(void *))
{
    // ★ v6（2026-09-02）：纯裸读版。v5 实证 uv=7f（判据/二进制对位成功），但 PyObject_Str
    //   在 value 对象上 SIGSEGV（fault=0x88, libpython+0x300f40）——C API 调用不可靠。
    //   v6 一个 Python 函数都不调：① dump 拉长到 256B（复盖 std::string SSO 区，上一轮
    //   0x20 区已见 "registered at ..." 明文；② 裸读 m_value 的对象头 ob_type 与 tp_name
    //   （CPython 3.12：PyTypeObject = PyVarObject(16B) + tp_name(8B) → offset 24）。
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "EXC obj=%p ti=%p dump:", obj, (void *)ti);
    if (n > 0) write(2, buf, n);
    const unsigned char *p = (const unsigned char *)obj;
    for (int i = 0; i < 256; i += 16) {
        char lb[400];
        int m = snprintf(lb, sizeof(lb), "\n  %03x:", i);
        for (int j = 0; j < 16 && m < (int)sizeof(lb) - 40; j++) {
            unsigned char c = p[i + j];
            m += snprintf(lb + m, sizeof(lb) - m, " %02x", c);
            if (c >= 0x20 && c < 0x7f) lb[m++] = (char)c;
            else lb[m++] = '.';
        }
        write(2, lb, m);
    }
    // ★ v8（2026-09-02）：typeinfo 点名 + 三对象 dump。
    //   证据链：value 对象 [0] == 异常 vptr+0x58（两轮吻合）→ value 是带 vptr 的 C++
    //   对象而非 CPython PyObject，「type/value/trace 三元组」讲不通；异常对象的
    //   typeinfo（ti）与 vtable 在 libtorch_python 段同址（0x7f…e640/e658 差 0x18）。
    //   ★ 决定性一步：裸读 ti+8 的 mangled typeinfo name（Itanium ABI：type_info{ vptr,
    //   name, ... }），直接确定抛掷的 C++ 异常类型；并 dump 三个对象各 256B。
    const void *pT = *(const void **)(p + 8);
    const void *pV = *(const void **)(p + 16);
    const char *tiname = (const char *)ti ? *(const char **)((const char *)ti + 8) : nullptr;
    int m1 = snprintf(buf, sizeof(buf), "\nTI-NAME: %s", tiname ? tiname : "<null>");
    write(2, buf, m1);
    int m1b = snprintf(buf, sizeof(buf), "\nPTRS pT=%p pV=%p", pT, pV);
    write(2, buf, m1b);
    // ★ v10（2026-09-02）：直取 m_msg 字符串。
    //   v9 chase 实证：fault=0x726f7272（"rror"）证明字符串遍布可达，且 OBJ 本身
    //   +0x60 = 字符串指针（例 0x7f5c23b920）、+0x68 = 0xa0（len=160）——即 pybind11
    //   error_already_set 的 m_msg（std::string）已填充。直接 dump 该指针处 160B。
    const void *mmp = *(const void **)(p + 0x60);
    unsigned long mmlen = *(const unsigned long *)(p + 0x68);
    int m2 = snprintf(buf, sizeof(buf), "\nMMSG ptr=%p len=%lu", mmp, mmlen);
    write(2, buf, m2);
    if ((uintptr_t)mmp > 0x100000 && (uintptr_t)mmp < 0x7f0000000000ull) {
        const unsigned char *mp = (const unsigned char *)mmp;
        for (int i = 0; i < (int)(mmlen > 176 ? 176 : mmlen); i += 16) {
            char lb[400];
            int m = snprintf(lb, sizeof(lb), "\n  %03x:", i);
            for (int j = 0; j < 16 && m < (int)sizeof(lb) - 40; j++) {
                unsigned char c = mp[i + j];
                m += snprintf(lb + m, sizeof(lb) - m, " %02x", c);
                if (c >= 0x20 && c < 0x7f) lb[m++] = (char)c;
                else lb[m++] = '.';
            }
            write(2, lb, m);
        }
    }
    // ★ v11：vtable 槽直调 what()。Itanium std::exception vtbl：[0]=dtor,[1]=dtor,[2]=what。
    //   以「函数指针直调」而非 C++ 虚拟调用（规避 v1 的 vptr/ODR 崩因）；what() 内部完成
    //   m_msg 懒构造——排空的错误正文现场打出。若崩则已打全前置信息。
    const unsigned char *vptable = *(const unsigned char **)p;
    {
        int mw = snprintf(buf, sizeof(buf), "\nVTBL-0=%p VTBL-1=%p VTBL-2=%p VTBL-3=%p",
                          *(void **)(vptable + 0), *(void **)(vptable + 8),
                          *(void **)(vptable + 16), *(void **)(vptable + 24));
        write(2, buf, mw);
        typedef const char *(*what_fn)(void *);
        what_fn wf = (what_fn)(*(void **)(vptable + 16));
        const char *w = wf(obj);
        int m9 = snprintf(buf, sizeof(buf), "\nWHAT: %s", w ? w : "<null>");
        write(2, buf, m9);
        // run82-3：write(2)→stderr 不进 hilog——前几轮 dump 全丢，NCP-EXIT signal=17 因此
        //   一直是无日志死因。LOGI 单行摘要：TI-NAME/WHAT 即「主链哪处的 C++ 抛掷杀死子进程」。
        LOGI("CXA17-EXIT tiname=%{public}s what=%{public}s obj=%{public}p",
             tiname ? tiname : "<null>", w ? w : "<null>", (void *)p);
        // run90（2026-09-03):移除 run89 backtrace —— 异常路径上 _Unwind_Backtrace/dladdr
        //   触发二次崩溃(run13: FaultLogger 栈 __cxa_throw+1044,且 LOGI 后 hilog 未送达),
        //   放弃调用栈抓取,回归纯 LOGI 单行摘要(_exit(17) 前可稳定送达)。
    }
    write(2, "\n", 1);
    _exit(17);
}

// ── torch._C PyInit__C 卡死诊断 watchdog ──────────────────────────────────────
// 真机观测：PyInit__C 进入后主线程自旋(R) + 2 worker(S)，不再返回。此 watchdog 每 3s 打印
// 「仍在 PyInit__C」以区分「真死锁(永久=1 处)」vs「慢初始化(还在推进)」。volatile 即可，
// watchdog 只做旁路观测，绝不触碰 Python GIL/状态，避免二次干扰。
static volatile int g_pyinit_done = 0;
static pthread_t g_main_thread;         // 主线程 handle，供 watchdog pthread_kill
static volatile int g_in_bt;            // handler 重入防抖
struct bt_frame { void *ips[100]; int n; };
static _Unwind_Reason_Code bt_cb(struct _Unwind_Context *ctx, void *arg)
{
    bt_frame *f = (bt_frame *)arg;
    if (f->n < 100) f->ips[f->n++] = (void *)_Unwind_GetIP(ctx);
    return _URC_NO_REASON;
}
static void pyinit_bt_handler(int sig)
{
    // 异步打断主线程(PyInit__C 中)，打印其当前栈。用例：watchdog 超时后 SIGUSR1 → 主线程在此 unwind。
    // ⚠ 非 async-signal-safe（dladdr/hilog），但 PyInit__C 内不会恰好打断 hilog，实际可用。
    // 用 _Unwind_Backtrace（.eh_frame）而非 backtrace()（musl 该实现常返 0），更鲁棒。
    if (__sync_lock_test_and_set(&g_in_bt, 1)) return;
    LOGI("BT: SIGUSR1 caught in main thread, PyInit__C backtrace follows");
    bt_frame f; memset(&f, 0, sizeof(f));
    _Unwind_Backtrace(bt_cb, &f);
    LOGE("BT: frame count = %{public}d", f.n);
    for (int i = 0; i < f.n; i++) {
        Dl_info di; memset(&di, 0, sizeof(di));
        const char *name = "?", *file = "?";
        unsigned long off = 0;
        if (dladdr(f.ips[i], &di)) {
            if (di.dli_sname) name = di.dli_sname;
            if (di.dli_fname) file = di.dli_fname;
            off = (unsigned long)((char *)f.ips[i] - (char *)di.dli_fbase);
        }
        LOGE("BT[%02d] pc=%{public}p so=%{public}s off=0x%{public}lx sym=%{public}s",
             i, f.ips[i], file, off, name);
    }
    __sync_lock_release(&g_in_bt);
}
static void *pyinit_watchdog(void *)
{
    time_t t0 = time(nullptr);
    while (!g_pyinit_done) {
        sleep(3);
        if (g_pyinit_done) break;
        long el = (long)(time(nullptr) - t0);
        LOGI("WATCHDOG: torch._C PyInit__C still running, elapsed=%{public}ld s", el);
        if (el >= 5) {
            // 向主线程发 SIGUSR1 → 其信号 handler 打印 PyInit__C 卡死栈到 hilog。
            // 不靠 crash/faultlog（无 root 读不了），靠异步打断主线程当前点 unwind。
            if (g_main_thread) pthread_kill(g_main_thread, SIGUSR1);
        }
        if (el >= 12) {
            LOGI("WATCHDOG: torch._C PyInit__C >12s, ABORT for backtrace");
            fflush(nullptr);
            ::abort();
        }
    }
    return nullptr;
}

// ── 崩溃信号落盘捕获（致命：faultloggerd 对 NCP 子进程零记录，hilog 白名单遮第三方域）──
// _asyncio/_decimal 的 PyInit/ExecDef 静默死亡（最后一探针后无任何日志）——用本机崩溃信号
// handler 把 SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE/SIGSYS 的 unwind PC 链裸 write 到 diag.log
// （async-signal-safe：不用 fprintf/hilog/dladdr），后续本地 addr2line 还原罪魁函数。
static volatile int g_crashfd = -2;
static _Unwind_Reason_Code crash_bt_cb(struct _Unwind_Context *ctx, void *arg)
{
    bt_frame *f = (bt_frame *)arg;
    if (f->n < 100) f->ips[f->n++] = (void *)_Unwind_GetIP(ctx);
    return _URC_NO_REASON;
}
static void crash_handler(int sig, siginfo_t *si, void *uc)
{
    // ★ 崩溃双通道：diag.log 落盘（同 uid 可读系 C++ 自证）之外，加一条 LOGE 直达 hilog 白名单。
    //   OHOS hilog C 路径为 write 系 syscall 链（不 malloc），信号上下文基本可用；
    //   关键三元组：sig / si_addr(FA) / ucontext pc——快速定位再取 so 名前缀。
    {
        unsigned long ucpc = 0;
        if (uc) ucpc = (unsigned long)((ucontext_t *)uc)->uc_mcontext.pc;
        Dl_info di; memset(&di, 0, sizeof(di));
        const char *fname = "?";
        unsigned long off = ucpc;
        if (dladdr((void *)ucpc, &di) && di.dli_fname) { fname = di.dli_fname; off = ucpc - (unsigned long)di.dli_fbase; }
        LOGI("CRASH-ANNOUNCE sig=%{public}d fa=%{public}p pc=%{public}p so=%{public}s off=0x%{public}lx",
             sig, si ? (void *)si->si_addr : nullptr, (void *)ucpc, fname, (unsigned long)off);
    }
    int fd = (g_crashfd >= 0) ? g_crashfd : 2;
    static const char hx[] = "0123456789abcdef";
    char b[112];
    auto e = [fd](const char *s, size_t n) { ssize_t r = write(fd, s, n); (void)r; };
    // "CRASH sig=<num> ip-chain"：手写十进制（snprintf 非 AS-safe）
    char hdr[] = "\n==== CRASH sig=\n"; e(hdr, 17);
    char digits[16]; int nd = 0;
    unsigned int v = (unsigned int)sig;
    do { digits[nd++] = (char)('0' + v % 10); v /= 10; } while (v && nd < 15);
    char rbuf[16]; for (int i = 0; i < nd; i++) rbuf[i] = digits[nd - 1 - i];
    e(rbuf, (size_t)nd); e("\n", 1);
    // ★ 真实崩溃 PC（ucontext.mcontext.pc，aarch64）与故障地址（siginfo.si_addr）——unwind 在
    //   信号处理时刻是"handler 栈"，PC[000] 只会是 crash_handler 自己；这两行才是命门。
    {
        unsigned long ucpc = 0, fa = (unsigned long)si->si_addr;
        if (uc) {
            ucontext_t *ucp = (ucontext_t *)uc;
            ucpc = (unsigned long)ucp->uc_mcontext.pc;
        }
        e("CRASH_FAULT_ADDR=", 17);
        for (int k = 0; k < 16; k++) b[k] = hx[(fa >> (60 - 4 * k)) & 0xf];
        b[16] = '\n'; e(b, 17);
        e("CRASH_UC_PC=", 12);
        for (int k = 0; k < 16; k++) b[k] = hx[(ucpc >> (60 - 4 * k)) & 0xf];
        b[16] = '\n'; e(b, 17);
    }
    bt_frame f; memset(&f, 0, sizeof(f));
    _Unwind_Backtrace(crash_bt_cb, &f);
    for (int i = 0; i < f.n; i++) {
        unsigned long ip = (unsigned long)f.ips[i];
        // 裸 write 版 hex PC（保底）
        b[0] = 'C'; b[1] = 'R'; b[2] = 'A'; b[3] = 'S'; b[4] = 'H'; b[5] = '_';
        b[6] = 'P'; b[7] = 'C'; b[8] = '[';
        b[9] = hx[(i >> 8) & 0xf]; b[10] = hx[(i >> 4) & 0xf]; b[11] = hx[i & 0xf];
        b[12] = ']'; b[13] = ' '; b[14] = '0'; b[15] = 'x';
        for (int k = 0; k < 16; k++) b[16 + k] = hx[(ip >> (60 - 4 * k)) & 0xf];
        b[32] = '\n';
        e(b, 33);
        // dladdr 补充 so 名 + 偏移（非严格 AS-safe，诊断版可接受；失败则打印 '?'）
        Dl_info di; memset(&di, 0, sizeof(di));
        const char *fname = "?";
        unsigned long off = ip;
        if (dladdr((void *)ip, &di) && di.dli_fname) {
            fname = di.dli_fname;
            off = ip - (unsigned long)di.dli_fbase;
        }
        // "S0: <name> O0x<off>"
        b[0] = 'S'; b[1] = (char)('0' + (i % 10)); b[2] = ':'; b[3] = ' '; b[4] = '0';
        b[5] = 'x'; b[6] = hx[(off >> 60) & 0xf];
        for (int k = 1; k < 16; k++) b[6 + k] = hx[(off >> (60 - 4 * k)) & 0xf];
        b[22] = ' '; b[23] = ' ';
        size_t yn = 0;
        while (fname[yn] && yn < 70) { b[24 + yn] = fname[yn]; yn++; }
        b[24 + yn] = '\n';
        e(b, 25 + yn);
    }
    e("==== CRASH END ====\n", 20);
    _exit(1);
}
static void install_crash_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;   // SA_SIGINFO: 拿 ucontext(真 PC)+si_addr; 单次崩溃即 _exit
    static const int crash_sigs[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE, SIGSYS};
    for (int s : crash_sigs) sigaction(s, &sa, nullptr);
}

// 用于 CPU 基线计时（volatile 防止被优化掉）
static unsigned long cpu_baseline(void)
{
    struct timeval t0, t1;
    gettimeofday(&t0, nullptr);
    volatile unsigned long sum = 0;
    for (unsigned long i = 0; i < 20000000UL; i++) {
        sum += i;
    }
    gettimeofday(&t1, nullptr);
    unsigned long us =
        (t1.tv_sec - t0.tv_sec) * 1000000UL + (t1.tv_usec - t0.tv_usec);
    LOGI("CPU baseline: ~20M adds = %{public}lu us (sum=%{public}lu)", us, sum);
    return us;
}

// fork/exec 一个 /system/bin/sh 子命令，验证子进程池能否再派生
static void fork_exec_check(void)
{
    int fds[2];
    pipe(fds);
    pid_t c = fork();
    if (c == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        execl("/system/bin/sh", "sh", "-c", "uname -a; echo -n child-ok", (char *)nullptr);
        _exit(127);
    }
    if (c > 0) {
        close(fds[1]);
        char buf[256] = {0};
        int n = read(fds[0], buf, sizeof(buf) - 1);
        waitpid(c, nullptr, 0);
        LOGI("fork/exec check: child pid=%{public}d, read=%{public}d, out=%{public}s",
             (int)c, n, buf);
    } else {
        LOGE("fork failed");
    }
}

// 绑定 127.0.0.1 端口并自连 one-shot，验证网络能力
static void socket_check(void)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(38375);
    int br = bind(lfd, (sockaddr *)&sa, sizeof(sa));
    listen(lfd, 1);

    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(38375);
    int cr = connect(cfd, (sockaddr *)&dst, sizeof(dst));
    int afd = accept(lfd, nullptr, nullptr);

    const char *msg = "comfy-child-ok";
    if (afd >= 0) {
        write(afd, msg, strlen(msg));
    }
    char buf[64] = {0};
    int n = cfd >= 0 ? read(cfd, buf, sizeof(buf) - 1) : -1;
    LOGI("socket check: bind=%{public}d connect=%{public}d accept=%{public}d recv=%{public}d '%{public}s'",
         br, cr, afd, n, buf);

    if (cfd >= 0) close(cfd);
    if (afd >= 0) close(afd);
    close(lfd);
}

// Step 0.2（P0 命门②实测）：dlopen 一个由 OHOS/musl 工具链交叉编译的 libpython3.12.so，
// 再 dlsym 调 Py_GetVersion / Py_Initialize，真机确认 libc++ 双命名空间共存与 SOABI。
// 若真机加载执行成功 → 证明嵌入式 CPython 的交叉工具链 + 加载 ABI 全链路可用。
// 从 entryParams 形如 "phase0|127.0.0.1|8188|--cpu|pyroot=/data/.../files/py" 里抠出 pyroot=<路径>。
static void extract_pyroot(const char *entryParams, char *out, size_t outsz)
{
    out[0] = '\0';
    if (!entryParams) return;
    const char *p = strstr(entryParams, "pyroot=");
    if (!p) return;
    p += strlen("pyroot=");
    size_t n = 0;
    while (p[n] && p[n] != '|' && n < outsz - 1) { out[n] = p[n]; n++; }
    out[n] = '\0';
}

// ★ 多相(multiphase)模块 API：skh 的 python3.12 头是「裁剪版」（moduleobject.h 仅 119 行，
//   无 FromDefAndSpec2/ExecDef 声明；官方 3.12.7 头亦在 import.h 只有 PyImport_*），但真机
//   libpython3.12.so.1.0 动态表已导出这三个符号（readelf 已验证）。签名照 CPython 3.12.7
//   moduleobject.c / namespaceobject.c 抄，链接自 comfy_child 的 NEEDED libpython，直接可调。
extern "C" PyObject *PyModule_FromDefAndSpec2(PyModuleDef *def, PyObject *spec, int module_api_version);
extern "C" int PyModule_ExecDef(PyObject *module, PyModuleDef *def);
extern "C" PyObject *_PyNamespace_New(PyObject *attrs);
// PYTHON_API_VERSION(3.12)=1013：check_api_version 接受 PYTHON_API_VERSION(1013) 或 PYTHON_ABI_VERSION。
#define COMFY_PY_API_VERSION 1013

//
// ★ multiphase(moduledef) → 真模块 的官方三步流程（import.c bootstrap_imp / _PyImport_LoadDynamicModuleWithSpec）：
//   spec = _PyNamespace_New({"name": fullname})   // ⚠ spec 必须非 NULL：
//   m = PyModule_FromDefAndSpec2(def, spec, PYTHON_API_VERSION)  // 首行 GetAttrString(spec,"name")，NULL 即段错误
//   PyModule_ExecDef(m, def)                      // 执行 m_slots 的 Py_mod_exec（真正填充模块内容）
// 失败返回 nullptr（PyErr 已取走清掉）；成功返回带 __dict__ 的真 module。
static PyObject *multiphase_materialize(const char *fullname, PyObject *moduledef)
{
    DIAG("MP[%s] step1 spec-build", fullname);
    PyObject *nm = PyUnicode_FromString(fullname);
    PyObject *attrs = nm ? Py_BuildValue("{sO}", "name", nm) : nullptr;
    Py_XDECREF(nm);
    PyObject *spec = attrs ? _PyNamespace_New(attrs) : nullptr;
    Py_XDECREF(attrs);
    if (!spec) {
        PyErr_Fetch(nullptr, nullptr, nullptr);
        PyErr_Clear();
        return nullptr;
    }
    DIAG("MP[%s] step2 FromDefAndSpec2", fullname);
    PyObject *m = PyModule_FromDefAndSpec2((PyModuleDef *)moduledef, spec, COMFY_PY_API_VERSION);
    Py_DECREF(spec);
    DIAG("MP[%s] step3 m=%p", fullname, (void *)m);
    if (!m) {
        PyErr_Fetch(nullptr, nullptr, nullptr);
        PyErr_Clear();
        return nullptr;
    }
    // ★ 半注册（官方 _load_unlocked 语义）：importlib 在 exec_module 前就把模块放入 sys.modules，
    //   供再入导入（exec slot 内 "import asyncio" 类回调 / 循环引用）命中。此前我们 ExecDef 后才注册
    //   → _asyncio 的 module_init 里 PyImport_ImportModule("asyncio") 链若反向 _asyncio 找不到。
    //   失败时把半注册条目清掉（保持"失败即不存在"语义）。
    PyObject *sysdict = PyImport_GetModuleDict();
    if (PyDict_SetItemString(sysdict, fullname, m) != 0) {
        DIAG("MP[%s] pre-register FAILED", fullname);
        PyErr_Fetch(nullptr, nullptr, nullptr);
        PyErr_Clear();
        Py_DECREF(m);
        return nullptr;
    }
    DIAG("MP[%s] step4 ExecDef (pre-registered)", fullname);
    if (PyModule_ExecDef(m, (PyModuleDef *)moduledef) != 0) {
        PyErr_Fetch(nullptr, nullptr, nullptr);
        PyErr_Clear();
        PyDict_DelItemString(sysdict, fullname);
        Py_DECREF(m);
        return nullptr;
    }
    DIAG("MP[%s] step5 OK", fullname);
    return m;
}

// 嵌入式宿主桥：把打包在 HAP libs/<abi>/ 的 C++ 扩展 dlopen(短名, loader 命中) → 调 PyInit_<tail>
// → 塞进 sys.modules。这样 Python `import <fullname>` 命中缓存的模块对象即真正可用，绕开 importlib
// 「不会去 libs/ 找 .so」的默认行为（纯 .py 子模块仍走 filesDir/site-packages 标准 import）。
// 这是 torch._C / aiohttp C 扩展在「一切 .so 只能进 libs/」约束下的统一接入方式。
static void register_libs_ext(const char *fullname, const char *soname)
{
    DIAG("load ext [%s] soname=%s", fullname, soname);
    void *h = dlopen(soname, RTLD_LAZY | RTLD_GLOBAL);
    if (!h) { LOGE("libs ext dlopen FAILED %{public}s: %{public}s", soname, dlerror()); return; }
    const char *dot = strrchr(fullname, '.');
    const char *tail = dot ? dot + 1 : fullname;
    char init[256];
    snprintf(init, sizeof(init), "PyInit_%s", tail);
    PyObject *(*mk)(void) = (PyObject *(*)(void))dlsym(h, init);
    if (!mk) { LOGE("libs ext dlsym %{public}s FAILED: %{public}s", init, dlerror()); return; }
    PyObject *mod = mk();
    if (!mod) { LOGE("libs ext PyInit_%{public}s returned NULL", tail); return; }
    // ★ multiphase 处理（真机 _struct 实证）：CPython 3.12 标准 C 扩展的 PyInit_<x> 是 multiphase，
    //   返回 PyModuleDef_Init() 的「PyModuleDef 对象」(type 显示为 moduledef)——【不是】真正的 module！
    //   旧实现把 def 对象直接塞进 sys.modules → `from _struct import *` 报
    //   "no __dict__ and no __all__"（type=moduledef 无 md_dict）。此处判定+helper 转换（官方三步流程见上）。
    if (!PyModule_Check(mod)) {
        DIAG("ext %s: PyInit returned moduledef (multiphase) -> materialize", fullname);
        PyObject *m2 = multiphase_materialize(fullname, mod);
        if (!m2) {
            LOGE("libs ext multiphase convert FAILED %{public}s", fullname);
            DIAG("ext %s FromDefAndSpec2 FAILED", fullname);
            Py_DECREF(mod);
            return;
        }
        Py_DECREF(mod);
        mod = m2;
    }
    PyObject *sysdict = PyImport_GetModuleDict();
    PyDict_SetItemString(sysdict, fullname, mod);  // 覆写 sys.modules[fullname]
    Py_DECREF(mod);
    LOGI("libs ext registered: %{public}s (from %{public}s)", fullname, soname);
    DIAG("libs ext registered: %s", fullname);
}

// 批量注入 skh 3.12 lib-dynload 的 70 个 CPython 内建 C 扩展（纯 C，不链 libc++，与 __1/__n1 均兼容）。
// 按铁律它们也在 HAP libs/<abi>/，宿主逐个 dlopen(短名)+PyInit_<模块名>+塞 sys.modules，使 `import _asyncio`
// 等命中缓存真正可用。模块名 = 文件名去掉 ".cpython-312-aarch64-linux-ohos.so" 后缀（CPython 标准推导）。
// 编译现场探针（trigger: _elementtree crash#4 = cfg_to_instr_sequence+0x130 的 PyObject_Realloc；
// crash#5/本轮 crash = tokenizer/cfg 链；inject 后首个 compile 即 ERR + PyErr_Fetch 崩 → 异常对象坏）。
// 失败路径打 PyErr_Print 把真实错误文本落地 diag.log（前版用 PyErr_Fetch(nullptr,x3) 反而在
// fetch 内部崩（错误对象悬垂）——保留直接对撞以收集最高价值证据：打印不出来时崩溃栈同样定机位）。
static void compile_probe(PyObject *(*compile_fn)(const char *, const char *, int),
                          const char *tag, size_t nbytes)
{
    if (!compile_fn) { DIAG("compile[%s] no fn", tag); return; }
    size_t target = nbytes;
    char *csrc = (char *)malloc(target + 1);
    if (!csrc) { DIAG("compile[%s] src alloc fail", tag); return; }
    size_t ctot = 0;
    char ct[64];
    for (int v = 0; ctot < target && v < 2000; v++) {
        int cn = snprintf(ct, sizeof(ct), "v%d = %d * %d + 1\n", v, v, v + 1);
        if (ctot + cn > target) break;
        memcpy(csrc + ctot, ct, cn);
        ctot += cn;
    }
    csrc[ctot] = '\0';
    PyObject *co = compile_fn(csrc, "<probe>", 0x101 /* Py_file_input */);
    if (co) {
        DIAG("compile[%s] ok (%zuB src)", tag, ctot);
        Py_DECREF(co);
    } else {
        DIAG("compile[%s] ERR (%zuB src)", tag, ctot);
        PyErr_Print();   // 打印真实错误文本；若异常对象坏 → 崩在 PyErr_Print/PyErr_Fetch 内（证据）
    }
    free(csrc);
}

// ─────────────────────────────────────────────────────────────────────────────
// ★ run14 追加：memcg 探针 —— 实锤「RSS≈340MB 被杀」= kernel memcg OOM。
//   child 是 cgroup 成员；shell(20020000) 被 hmmac=genfs 拦（.limit_in_bytes 实读 EACCES），
//   故由 child 自身读自己 cgroup 的控制文件（失败也把 errno 打出，喂下一步）。
//   v1：<mount>/<path>/memory.limit_in_bytes + memory.max_usage_in_bytes + memory.failcnt
//       failcnt>0 = 该 cgroup 已发生过一次「内存限额触发的 OOM 杀」——一锤定音。
//   v2：<cgroup2-mount>/<path>/memory.max + memory.current + memory.events(含 oom_kill 计数)。
//   输出格式（单行，打进 hb 心跳）：
//   memcgv1=<mount><path> limit=<n> max_usage=<n> failcnt=<n>   （E<errno>=打开失败）
//   memcgv2=<mount><path> max=<n|max> current=<n> events=<oom_kill行>
static void rd_small(const char *path, char *ob, size_t osz)
{
    ob[0] = '\0';
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(ob, osz, f)) {
            size_t l = strlen(ob);
            while (l && (ob[l - 1] == '\n' || ob[l - 1] == ' ')) ob[--l] = '\0';
        } else {
            snprintf(ob, osz, "<empty>");
        }
        fclose(f);
    } else {
        snprintf(ob, osz, "E%d", errno);
    }
}

static void probe_memcg(char *out, size_t outsz)
{
    out[0] = '\0';
    // 1) cgroup 路径（memory 控制段：v1 行 `4:memory:/path`；v2 行 `0::/path`）
    char cgp[256] = {0};
    FILE *fc = fopen("/proc/self/cgroup", "r");
    if (fc) {
        char line[300];
        while (fgets(line, sizeof(line), fc)) {
            char *p1 = strchr(line, ':');
            if (!p1) continue;
            char *p2 = strchr(p1 + 1, ':');
            if (!p2) continue;
            char *path = p2 + 1;
            size_t lp = strlen(path);
            while (lp && (path[lp - 1] == '\n')) path[--lp] = '\0';
            if (line[0] == '0') {  // cgroup v2 单层级行
                if (!cgp[0]) snprintf(cgp, sizeof(cgp), "%s", path);
                break;             // v2：就这一行
            }
            if (strncmp(p1 + 1, "memory", 6) == 0) {
                snprintf(cgp, sizeof(cgp), "%s", path);
                break;             // v1：命中 memory 控制器段即可
            }
        }
        fclose(fc);
    }
    if (!cgp[0]) { snprintf(out, outsz, "memcg=cgnone"); return; }
    // 2) 挂载点：fs=cgroup2 → mv2；fs=cgroup 且 super opts 含 memory → mv1
    char mv1[192] = {0}, mv2[192] = {0};
    FILE *fm = fopen("/proc/self/mountinfo", "r");
    if (fm) {
        char line[600];
        while (fgets(line, sizeof(line), fm)) {
            char *dash = strstr(line, " - ");
            if (!dash) continue;
            char *ft = dash + 3;  // fs类型 挂载源 super_options...
            if (strncmp(ft, "cgroup2", 7) == 0) {
                char *tt = strtok(line, " ");          // id
                for (int k = 1; k < 5 && tt; k++) tt = strtok(nullptr, " ");  // 1..4 → mount point
                if (tt && !mv2[0]) snprintf(mv2, sizeof(mv2), "%s", tt);
            } else if (strncmp(ft, "cgroup ", 7) == 0) {
                // ★ run15 实测：OHOS 各控制器独立挂载，super opts 恒为 "rw,hmmac=genfs"
                //   （无控制器名!），控制器身份体现在【挂载点名称】：/dev/memcg=memory。
                char *tt = strtok(line, " ");
                for (int k = 1; k < 5 && tt; k++) tt = strtok(nullptr, " ");   // t4 = mount point
                if (tt && strstr(tt, "memcg") && !mv1[0]) snprintf(mv1, sizeof(mv1), "%s", tt);
            }
        }
        fclose(fm);
    }
    // 3) 拼路径读三件套
    char p[512], a[64], b[64], c[512];
    if (mv2[0]) {
        snprintf(p, sizeof(p), "%s%s", mv2, cgp);
        char t1[512], t2[512];
        snprintf(t1, sizeof(t1), "%s/memory.max", p);      rd_small(t1, a, sizeof(a));
        snprintf(t2, sizeof(t2), "%s/memory.current", p);  rd_small(t2, b, sizeof(b));
        snprintf(t1, sizeof(t1), "%s/memory.events", p);   rd_small(t1, c, sizeof(c));
        snprintf(out, outsz, "memcgv2=%s max=%s cur=%s events=[%s]", p, a, b, c);
    } else if (mv1[0]) {
        snprintf(p, sizeof(p), "%s%s", mv1, cgp);
        char t1[512], t2[512];
        snprintf(t1, sizeof(t1), "%s/memory.limit_in_bytes", p);      rd_small(t1, a, sizeof(a));
        snprintf(t2, sizeof(t2), "%s/memory.max_usage_in_bytes", p);  rd_small(t2, b, sizeof(b));
        snprintf(t1, sizeof(t1), "%s/memory.failcnt", p);             rd_small(t1, c, sizeof(c));
        snprintf(out, outsz, "memcgv1=%s limit=%s max_usage=%s failcnt=%s", p, a, b, c);
    } else {
        snprintf(out, outsz, "memcg=mount-none cgp=%s", cgp);
    }
}

static void inject_stdlib_dynload(PyObject *(*compile_fn)(const char *, const char *, int))
{
    static const char kSfx[] = ".cpython-312-aarch64-linux-ohos.so";
    const size_t sfxlen = sizeof(kSfx) - 1;
    const size_t nExts = sizeof(kDynloadExts) / sizeof(kDynloadExts[0]);
    // ★ 总量 vs 序列 二分：2000 次同尺寸分配释放。崩 → 总量阈值/libc 病；过 → 序列/特定 ext 病。
    {
        volatile int fails = 0;
        for (int k = 0; k < 2000; k++) {
            void *p = malloc(131051);
            if (!p) { fails++; continue; }
            free(p);
        }
        DIAG("pre-inject bulk sweep done: 2000x malloc(131051)+free, fails=%d", fails);
    }
    // ★ dry-dlopen 辨识块：只 dlopen 不动 PyInit，每 5 个后大分配一次。
    //   崩 → 病=dlopen 与 jemalloc 交互（与 Python/注册无关）→ 静态并入 inittab 是唯一根治；
    //   过 → 病=PyInit/注册侧。
    {
        for (size_t i = 0; i < nExts; i++) {
            const char *soname = kDynloadExts[i];
            void *h = dlopen(soname, RTLD_LAZY | RTLD_GLOBAL);
            if (!h) {
                DIAG("dry-dlopen[%zu] FAIL: %s", i, dlerror());
                break;
            }
            if ((i % 5) == 0 || i + 1 == nExts) {
                void *p = malloc(131051);
                DIAG("dry-dlopen[%zu] +malloc131051 %s", i, p ? "ok" : "NULL");
                free(p);
            }
        }
        DIAG("dry-dlopen sweep done");
    }
    // ★ 官方链对照：libs 目录是真实 fs 路径（/data/storage/el1/bundle/libs/arm64，loader 从之 mmap）。
    //   塞进 sys.path → importlib._bootstrap_external 官方全链 import（create_dynamic/exec_dynamic）。
    //   崩 → 官方链同样挂 → libc 病实锤；过 → 手工五步可整体替换为官方路径（直奔 torch）。
    {
        PyObject *sysPath = PySys_GetObject("path");
        if (sysPath) {
            // run83（2026-09-03):tokenizers 顶层劫持定谳 —— FileFinder 按 sys.path 顺序扫描,
            //   libs/arm64 在原 [0] 位置先于 site-packages 命中同名顶层扩展
            //   (tokenizers.abi3.so → 'tokenizers' 非包 → 'tokenizers.models' 炸, WARM 4-FAIL)。
            //   改为插在 site-packages 之后: s-p 顶层包/模块优先正常解析, libs 仅作扩展兜底
            //   (_C / numpy 等 19 扩展由 _LibsFinder 短名兜底, 与 path 顺序无关)。
            PyObject *libsDir = PyUnicode_FromString("/data/storage/el1/bundle/libs/arm64");
            long ins = 0;
            long all = PyList_Size(sysPath);
            for (long i = 0; i < all; i++) {
                PyObject *it = PyList_GetItem(sysPath, i);
                if (it && PyUnicode_Check(it)) {
                    const char *s = PyUnicode_AsUTF8(it);
                    if (s && strstr(s, "site-packages")) {
                        ins = i + 1;
                        break;
                    }
                }
            }
            PyList_Insert(sysPath, ins, libsDir);
            Py_DECREF(libsDir);
            DIAG("official-chain: sys.path libs/arm64 inserted at %ld (size %ld)", ins, all + 1);
        } else {
            DIAG("official-chain: sys.path unavailable");
        }
        const char *imports =
            "import _bisect, _blake2, _bz2, _codecs_cn, _codecs_hk, _codecs_iso2022, "
            "_codecs_jp, _codecs_kr, _codecs_tw, _contextvars; "
            "print('OFFICIAL-CHAIN-IMPORT-OK', flush=True)";
        int rc = PyRun_SimpleString(imports);
        if (rc == 0) {
            DIAG("official-chain import 10 exts OK");
        } else {
            DIAG("official-chain import ERR rc=%d (PyErr 已打印到 stderr)", rc);
        }
    }
    // ★ 治疗版：手工 multiphase 注册整体弃用（官方链对照实验证明：手工注册 3~11 次后触发
    //   libc 深层 0xb7ef0 崩，而 importlib 官方链 import 10 个 ext 完全健康）。
    //   → PyImport_ImportModule 官方链逐模块加载（dlopen 仍由 CPython _imp 完成；libs/arm64 已在 sys.path[0]）。
    {
        size_t ok = 0;
        for (size_t i = 0; i < nExts; i++) {
            char modname[160];
            const char *soname = kDynloadExts[i];
            size_t slen = strlen(soname);
            if (slen < sfxlen || strcmp(soname + slen - sfxlen, kSfx) != 0) {
                LOGE("dynload unexpected name: %{public}s", soname);
                continue;
            }
            size_t n = slen - sfxlen;
            if (n >= sizeof(modname)) n = sizeof(modname) - 1;
            memcpy(modname, soname, n);
            modname[n] = '\0';
            PyObject *m = PyImport_ImportModule(modname);
            if (!m) {
                DIAG("official import FAIL: %s", modname);
                PyErr_Print();
                break;
            }
            Py_DECREF(m);
            ok++;
            if ((ok % 10) == 0) DIAG("official import progress: %zu/%zu", ok, nExts);
        }
        DIAG("official import done: %zu/%zu exts", ok, nExts);
    }
    // ★ 代表性纯 Python 包/通路（zip 标准库 + 已注入扩展协作）
    {
        const char *probe = "import os, site, socket, ssl, math, json, threading, asyncio, decimal, ctypes, select, mmap, zlib, array, random, heapq, pickle, statistics, struct, time; print('PYLIBS-OK')";
        int rc = PyRun_SimpleString(probe);
        DIAG("py libs probe rc=%d", rc);
    }
    // ★ torch 试探：site-packages 已装则装载（P0 命门③ 验证入口）；未装则打印 SKIP 不中断。
    //   torch/__init__.py 已过 global_deps（ctypes 短名 loaded）→ 下一步 import torch._C。
    //   设备 pyroot/torch/ 缺 _C.cpython-312-...so（同步漏了），但 HAP libs/arm64/ 已有同名 so
    //   （COMFTEST 已证明其短名 dlopen+dlsym PyInit__C 成功）→ 用 importlib 官方链从 libs/ 注入。
    //   spec_from_file_location(绝对路径) → EXTENSION loader → _imp.create_dynamic/exec_dynamic 官方链
    //   （与 70/70 ext 验证同一路径，健康）；先占 sys.modules["torch._C"] 再 import torch。
    {
        // ⚠ _C 的 PyInit 抛 pybind11::error_already_set（C++ 异常）直接穿越 CPython 的 C 栈帧，
        //   无 C++ handler → std::terminate（libc++abi 打印后 abort）。Python 层 try/except 接不住
        //   C++ 异常，故必须在此 C++ 边界补 catch：能把 what() 拿到就拿到底层错误（如哪个导入失败）。
        // ★ v15（2026-09-02）：v14 实证 finder 重定向可行——torch._C 过 433，import torch 推进到
        //   __init__.py 行 1896 torch/storage.py import numpy，卡 numpy.core._multiarray_umath
        //   （同款 dotted-name 病根）。v15 泛化 finder：扫描 libs/arm64/*.cpython-312-…so，
        //   建「短名→路径」映射；meta_path 首位 finder 对任意 fullname 的短名命中即重定向到
        //   libs/ 绝对路径（so 始终留在 HAP libs/，铁律不变；numpy 19 扩展已同载入）。
        //   非 cpython 后缀（libtorch_cpu.so 等）被 endswith 过滤，dynload/ _C 短名无冲突。
        const char *probe =
            "import os, sys, importlib.util, importlib.abc\n"
            "_dirs = ['/data/storage/el1/bundle/libs/arm64',\n"
            "         '/data/storage/el1/bundle/libs/arm64-v8a']\n"
            // ScriptableC-312 扩展后缀两种：skh 栈(c-ohos)与 PyPI musllinux wheel(-musl，pydantic_core 等)。
            "_suffixes = ('.cpython-312-aarch64-linux-ohos.so', '.cpython-312-aarch64-linux-musl.so', '.abi3.so')\n"  // Step 0.4c：abi3 全 ABI 扩展（safetensors._safetensors_rust 等）
            "_exts = {}\n"            // 短名 → (实际目录, 匹配后缀)（双目录合并，先到先得）
            "for _d in _dirs:\n"
            "    try:\n"
            "        _lst = os.listdir(_d)\n"
            "    except OSError as _e:\n"
            "        print('TORCHPROBE-LIBDIR-FAIL %s %r' % (_d, _e), flush=True)\n"
            "        continue\n"
            "    _m = [f for f in _lst if f.endswith(_suffixes)]\n"
            "    print('TORCHPROBE-LIBDIR %s total=%d exts=%d sample=%s' % (_d, len(_lst), len(_m), _m[0] if _m else '<none>'), flush=True)\n"
            "    for _f in _m:\n"
            "        for _suf in _suffixes:\n"
            "            if _f.endswith(_suf):\n"
            "                _k = _f[:-len(_suf)]\n"
            "                if _k not in _exts:\n"
            "                    _exts[_k] = (_d, _suf)\n"
            "                break\n"
            "print('TORCHPROBE-T0 extcount=%d' % (len(_exts),), flush=True)\n"
            "class _LibsFinder(importlib.abc.MetaPathFinder):\n"
            "    def find_spec(self, fullname, path=None, target=None):\n"
            // v17（2026-09-02）：sympy.tensor.array 名相撞——「短名」劫持把 sympy 纯 py 子包
            //   吃成 libs/ 的 array.cpython-312…so（array.so 是标准库 array 的 dynload 扩展）。
            //   判据：父包已加载且其 __path__ 下存在同名自然目标（目录或 .py），应按正常包查找
            //   放行不劫持；torch._C/PIL._imaging/… 的 target 目录无同名纯 py → 仍命中。
            "        short = fullname.rsplit('.', 1)[-1]\n"
            "        _hit = _exts.get(short)\n"
            "        if _hit is not None:\n"
            // run82-3：tokenizers 顶层劫持崩（真机 'tokenizers' is not a package）——
            //   short='tokenizers' 命中的是 libs/tokenizers.abi3.so：
            //   ①若 fullname 无 '.'（顶层名）且 site-packages 有同名包/模块（__init__.py/.py），
            //     放行给正常 import（tokenizers zip stub 是包，其 __init__ 从
            //     'tokenizers.tokenizers' 子名引 native → 子名带点、自然目标 guard 不拦 → 仍劫持）
            //   ②dotted 名的 v17 自然目标 guard 维持原样。
            //   唯一受影响顶层 = tokenizers（_safetensors_rust/_pydantic_core 短名带下划线不撞）。
            "            if fullname.rfind('.') < 0:\n"
            "                for _sp in [p for p in sys.path if isinstance(p, str) and p]:\n"
            "                    _pp = os.path.join(_sp, short)\n"
            "                    if os.path.isfile(os.path.join(_pp, '__init__.py')) or os.path.isfile(_pp + '.py'):\n"
            "                        return None\n"
            "            if fullname.rfind('.') >= 0:\n"
            "                _parent = fullname.rsplit('.', 1)[0]\n"
            "                _par_mod = sys.modules.get(_parent)\n"
            "                if _par_mod is not None and getattr(_par_mod, '__path__', None):\n"
            "                    for _p in _par_mod.__path__:\n"
            "                        _cand = os.path.join(_p, short)\n"
            "                        if os.path.isdir(_cand) or os.path.exists(_cand + '.py'):\n"
            "                            return None\n"
            "            _d, _suf = _hit\n"
            // run88（2026-09-03):LibsFinder 劫持命中全量记录 —— run11 死点
            //   'generic_type: type "ObjSense" is already registered!'(_core 双载).
            //   记录每次劫持(fullname+路径)供事后分析二次加载者身份。
            "            print('LIBSFIND-HIT fullname=%s short=%s path=%s' % (fullname, short, _d + '/' + short + _suf), flush=True)\n"
            "            return importlib.util.spec_from_file_location(\n"
            "                fullname, os.path.join(_d, short + _suf))\n"
            "        return None\n"
            "sys.meta_path.insert(0, _LibsFinder())\n"
            "print('TORCHPROBE-T1 finder-installed', flush=True)\n"
            // run82-3：tokenizers 顶层仍被劫持成非包 → 眼见为实：
            //   dump sys.path 里 tokenizers 的真实形态（目录?__init__?pyi-only?）+
            //   直接 import tokenizers 看是谁解析它（包 or LibsFinder's so）。
            "print('TOKPROBE sys.path=', sys.path, flush=True)\n"
            "_tkp = None\n"
            "for _spx in [p for p in sys.path if isinstance(p, str) and p]:\n"
            "    _tkc = os.path.join(_spx, 'tokenizers')\n"
            "    if os.path.isdir(_tkc):\n"
            "        _tkp = _tkc\n"
            "        print('TOKPROBE dir=%s __init__=%s pyi=%s list=%s' % (_tkc,\n"
            "              os.path.isfile(os.path.join(_tkc, '__init__.py')),\n"
            "              os.path.isfile(os.path.join(_tkc, 'tokenizers.pyi')),\n"
            "              sorted(os.listdir(_tkc))[:8]), flush=True)\n"
            "    elif os.path.isfile(_tkc + '.py'):\n"
            "        print('TOKPROBE singlefile=%s' % (_tkc + '.py',), flush=True)\n"
            "if _tkp is None:\n"
            "    print('TOKPROBE not-found-in-path', flush=True)\n"
            "print('TOKPROBE exts-tokeni=', [k for k in _exts if 'token' in k.lower()], flush=True)\n"
            "try:\n"
            "    import tokenizers as _tkmod\n"
            "    print('TOKPROBE import-OK mod=%s pathattr=%s' % (_tkmod, hasattr(_tkmod, '__path__')), flush=True)\n"
            "except BaseException as _tke:\n"
            "    print('TOKPROBE import-FAIL %r' % (_tke,), flush=True)\n"
            // torch 与 aiohttp 各自独立 try：一次部署拿全两个结果，且 torch 失败不再中断 probe
            "try:\n"
            "    import torch\n"
            "    print('TORCH-OK torch_version=%r' % (getattr(torch, '__version__', '<na>'),), flush=True)\n"
            "except BaseException:\n"
            "    import traceback; traceback.print_exc()\n"
            "    print('TORCH-FAIL', flush=True)\n"
            // Step 0.4b：pydantic_core（PyPI musllinux_1_1_aarch64 wheel，-musl.so 由上述
            //   finder 短名重定向）→ import pydantic_core / pydantic 全链验证。
            "try:\n"
            "    import pydantic_core\n"
            "    import pydantic\n"
            "    from pydantic import BaseModel\n"
            "    print('PC-OK pydantic=%s pydantic_core=%s' % (pydantic.VERSION, pydantic_core.__version__), flush=True)\n"
            "except BaseException:\n"
            "    import traceback; traceback.print_exc()\n"
            "    print('PC-FAIL', flush=True)\n"
            // Step 0.3d：aiohttp 纯 Python 模式（无 C 扩展自动 fallback，零 .so 铁律负担）——
            //   host 已验证：sdist 纯 .py 树 + AIOHTTP_NO_EXTENSIONS 等价环境起 127.0.0.1 server 200。
            //   真机端到端：起 8899 server + 同进程 client GET。
            "try:\n"
            "    import asyncio, aiohttp\n"
            "    from aiohttp import web\n"
            "    async def __aio_main():\n"
            "        # 打点：确认 asyncio 3.12 current_task（带/不带 loop 参）在 run 内是否命中\n"
            "        print('CT-DEF-NONE=%r CT-LOOPARG-NONE=%r' % (asyncio.current_task() is None, asyncio.current_task(asyncio.get_running_loop()) is None), flush=True)\n"
            "        app = web.Application()\n"
            "        app.router.add_get('/', lambda r: web.json_response({'comfy': 'ok'}))\n"
            "        runner = web.AppRunner(app)\n"
            "        await runner.setup()\n"
            "        site = web.TCPSite(runner, '127.0.0.1', 8899)\n"
            "        await site.start()\n"
            "        # 用 asyncio 原生 client（绕开 aiohttp ClientSession 的 Timer/current_task 路径）：\n"
            "        #   真机目标=P7 后端 web server（不依赖 client 侧）；Timer 打点另行判读。\n"
            "        r, w = await asyncio.open_connection('127.0.0.1', 8899)\n"
            "        w.write(b'GET / HTTP/1.1\\r\\nHost: c\\r\\nConnection: close\\r\\n\\r\\n')\n"
            "        await w.drain()\n"
            "        data = await r.read(1024)\n"
            "        print('AIOHTTP-HTTP line=%r' % (data.split(b'\\r\\n')[0],), flush=True)\n"
            "        w.close()\n"
            "        await runner.cleanup()\n"
            "    asyncio.run(__aio_main())\n"
            "    print('AIOHTTP-OK ver=%s' % aiohttp.__version__, flush=True)\n"
            "except BaseException:\n"
            "    import traceback; traceback.print_exc()\n"
            "    print('AIOHTTP-FAIL', flush=True)\n";
        int rc = -100;
        try {
            rc = PyRun_SimpleString(probe);
        } catch (const std::exception &e) {
            DIAG("torch probe C++ exception: %s", e.what());
            PyErr_Clear();
            rc = -1;
        } catch (...) {
            // ★ 抛的是 pybind11::error_already_set（hook 实证）但 catch(const std::exception&) 不命中：
            //   v15004 SDK 头 vs skh libc++（__1）RTTI 名字失配。what() 已由 __cxa_throw hook 直接虚调用打出。
            DIAG("torch probe C++ unknown exception (see __cxa_throw line)");
            PyErr_Clear();
            rc = -2;
        }
        DIAG("torch probe rc=%d", rc);
    }
    LOGI("stdlib dynload injected (official chain)");
}

static void dlopen_python_check(const char *entryParams)
{
    // ⚠鸿蒙加载器只认 App 打包进 HAP libs/<abi>/ 的 .so；filesDir/系统路径 dlopen 均不可行。
    // 打包名用 CPython 原生 SONAME 名 libpython3.12.so.1.0（产物本身即如此，未作任何 hack）。
    // 该库已是 comfy_child 的 NEEDED（CMake 链入）→ dlopen 返回既有句柄，dlsym 取 Py_*。
    const char *cands[] = {
        "libpython3.12.so.1.0",
    };
    void *h = nullptr;
    for (int i = 0; i < (int)(sizeof(cands) / sizeof(cands[0])); i++) {
        h = dlopen(cands[i], RTLD_LAZY | RTLD_GLOBAL);
        if (h) { LOGI("dlopen libpython OK: %{public}s", cands[i]); break; }
    }
    if (!h) { LOGE("dlopen libpython FAILED: %{public}s", dlerror()); DIAG("dlopen libpython FAILED: %s", dlerror()); return; }
    DIAG("dlopen libpython OK h=%p", (void *)h);

    using PyGetVersion  = const char *(*)();
    using PyIsInit      = int (*)();
    using PyInit        = void (*)();
    using PyRunSimple   = int (*)(const char *);
    auto Py_GetVersion     = (PyGetVersion)dlsym(h, "Py_GetVersion");
    auto Py_IsInitialized  = (PyIsInit)dlsym(h, "Py_IsInitialized");
    auto Py_Initialize     = (PyInit)dlsym(h, "Py_Initialize");
    auto PyRun_SimpleString = (PyRunSimple)dlsym(h, "PyRun_SimpleString");
    auto Py_CompileString_ = (PyObject *(*)(const char *, const char *, int))dlsym(h, "Py_CompileString");
    DIAG("dlsym Py_CompileString = %p", (void *)Py_CompileString_);

    if (!Py_GetVersion) {
        LOGE("dlsym Py_GetVersion FAILED: %{public}s", dlerror());
        return;
    }

    // SOABI 兼容性：Py_GetVersion 里含 ABI 标识（c-313-aarch64-linux-musl 等），不依赖 stdlib，必成功
    LOGI("Py_GetVersion(SOABI): %{public}s", Py_GetVersion());
    DIAG("Py_GetVersion(SOABI): %s", Py_GetVersion());

    // 父进程已把 stdlib+site-packages 解压成 <pyroot>/lib/python3.12/ 目录树 并塞在 entryParams 的 pyroot=<dir>。
    // CPython(POSIX) bootstrap 靠 STDLIB_SUBDIR=lib/python3.12/os.py 定位 stdlib → 设 PYTHONHOME 即可。
    char pyroot[PATH_MAX] = {0};
    extract_pyroot(entryParams, pyroot, sizeof(pyroot));

    // Py_Initialize 出错会打 stderr 并可能直接 exit。Main 开头已把 stderr/stdout freopen 到
    // <pyroot>/diag.log（sandbox 文件通道，绕开 hilog 白名单），故 CPython 的 "Fatal Python error"/
    // ImportError 会自然落地 diag.log。这里不再重复 dup2，避免二次重定向互相覆盖/截断。

    if (pyroot[0]) {
        char libdir[PATH_MAX] = {0};
        char sitepack[PATH_MAX] = {0};
        char py3path[PATH_MAX * 2] = {0};
        snprintf(libdir, sizeof(libdir), "%s/lib/python3.12", pyroot);
        snprintf(sitepack, sizeof(sitepack), "%s/lib/python3.12/site-packages", pyroot);
        // PYTHONHOME 让 getpath 自动把 <pyroot>/lib/python3.12(+lib-dynload) 加进 sys.path；
        // 再显式把 <stdlib> 与 <site-packages> 放 PYTHONPATH 兜底——torch/typing_extensions 等
        // 纯 .py 在 site-packages（已在 python312.zip），site.py 未自动加时由 PYTHONPATH 兜住。
        snprintf(py3path, sizeof(py3path), "%s:%s", libdir, sitepack);
        setenv("PYTHONHOME", pyroot, 1);
        setenv("PYTHONPATH", py3path, 1);
        setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
        // torch 线程池自旋缓解（嵌入/容器最常见卡桩）：单线程并行，避免 libomp 在受限环境空转。
        // ⚠ 真机实测 OMP_NUM_THREADS=1 仍自旋（主线程 R + 2 worker S），故再压:
        //   - OMP_WAIT_POLICY=PASSIVE：libomp 等待线程由「忙等自旋」改为「阻塞」—— 直击主线程烧 CPU 元凶。
        //   - TORCH_NUM_INTEROP_THREADS=1：interop 池默认=CPU 核数，受限环境起多线程易拖死。
        //   - OMP_THREAD_LIMIT/GOMP_SPINCOUNT：进一步禁用自旋。
        setenv("OMP_NUM_THREADS", "1", 1);
        setenv("MKL_NUM_THREADS", "1", 1);
        setenv("TORCH_NUM_THREADS", "1", 1);
        setenv("TORCH_NUM_INTEROP_THREADS", "1", 1);
        setenv("OMP_THREAD_LIMIT", "1", 1);
        setenv("OMP_DYNAMIC", "FALSE", 1);
        setenv("OMP_WAIT_POLICY", "PASSIVE", 1);
        setenv("GOMP_SPINCOUNT", "0", 1);
        // ★ 真机 36 帧 backtrace + libomp 反汇编联合定位：PyInit__C 卡死在 libomp 首次初始化的
        //   ompt_pre_init→ompt_start_tool。反汇编证实 ompt_start_tool 内部仅一行：
        //   x0=RTLD_NEXT(=-1) → dlsym(RTLD_NEXT, "ompt_start_tool")；36 帧中该帧 IP=0xe1e78 恰是
        //   那条 `bl dlsym@plt` 的返回地址 ⇒ 卡点不在 dlopen 工具库，而在 OHOS/musl loader 的
        //   dlsym(RTLD_NEXT) 特殊重定位路径死锁（此前 python 侧 handle dlsym(PyInit__C) 不卡）。
        //   OMP_TOOL_LIBRARIES 只作用于 dlopen 分支，动不了 dlsym(RTLD_NEXT) 分支 ⇒ 实测无效。
        //   √ 根治：OMP_TOOL=disabled 让 ompt_pre_init 直接提前 return，根本不进 ompt_start_tool，
        //     从而完全绕开该 dlsym(RTLD_NEXT) 死锁；torch 以单线程 OpenMP 正常初始化。
        setenv("OMP_TOOL", "disabled", 1);
        setenv("KMP_INIT_AT_FORK", "FALSE", 1);
        LOGI("stdlib pyroot=%{public}s PYTHONHOME=%{public}s PYTHONPATH=%{public}s",
             pyroot, pyroot, py3path);
    } else {
        LOGE("no pyroot in entryParams -> stdlib unavailable, Py_Initialize will fail");
    }

    // 用裸 Py_Initialize() 而非已废弃的 Py_SetProgramName()：二者混用会让内部 PyConfig
    // 状态与旧式 API 冲突，诱发 init 阶段某分配走错 → "memory allocation failed"（真机已实测命中）。
    // Py_Initialize() 是合法 API（非 hack）；若将来需显式 program_name，应迁移到 PyConfig +
    // Py_InitializeFromConfig（deprecated 的 Py_SetProgramName 禁止再用）。
    if (Py_IsInitialized && Py_Initialize) {
        LOGI("Py_IsInitialized before init = %{public}d", Py_IsInitialized());
        DIAG("called Py_IsInitialized before = %d", Py_IsInitialized());
        DIAG(">>> calling Py_Initialize (watch for 'memory allocation failed' fatal below)");
        Py_Initialize();   // 若 stdlib 缺失/路径不对，可能 Fatal(exit 或 stderr 报错)；fatal 会落地 diag.log
        DIAG("Py_Initialize returned; Py_IsInitialized after = %d", Py_IsInitialized());
        LOGI("Py_IsInitialized after  init = %{public}d", Py_IsInitialized());
        if (Py_IsInitialized() && PyRun_SimpleString) {
            int r = PyRun_SimpleString("import os; print('COMFTEST py ok, os=%s' % os.__name__)");
            LOGI("PyRun_SimpleString rc=%{public}d", r);
            DIAG("PyRun import.os (COMFTEST) rc=%d", r);
            int r2 = PyRun_SimpleString("import site, sys; print('COMFTEST site path0=%s' % sys.path[0])");
            LOGI("site import rc=%{public}d", r2);
            DIAG("PyRun import.site rc=%d", r2);
        }
    } else {
        DIAG("SKIP Py_Initialize (Py_IsInitialized=%p Py_Initialize=%p)", (void *)Py_IsInitialized, (void *)Py_Initialize);
    }
    // Step 0.3b/c：真实 torch 栈（skh 的 Python 3.12 + libc++ __1）。
    // torch 包内唯一 .so = _C.cpython-312-...so（导出 PyInit__C，NEEDED libtorch_python.so,libc.so）。
    // host 把它从 libs/ dlopen+PyInit__C+注册进 sys.modules['torch._C']，之后 Python `import torch`
    // 命中缓存的 torch._C 即真正可用（importlib 不会去 libs/，这正是嵌入式宿主的职责）。
    // 其余纯 .py（stdlib + torch/site-packages）走 filesDir/pyroot 标准 import。
    if (Py_IsInitialized()) {
        // 主线程注册 SIGUSR1 handler + 记录自身 handle：watchdog 超时后 pthread_kill(主线程, SIGUSR1)，
        // 在主线程(PyInit__C 内)打印当前栈，拿到卡死函数（不依赖 crash/faultlog 的 root 权限）。
        g_main_thread = pthread_self();
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = pyinit_bt_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, nullptr);

        // ★ malloc/dlsym 通路体检（P0 调研: _decimal PyInit import numbers 时 ld-musl NULL 解引用）。
        //   若探针全部通过而 tokenizer 分配仍崩 → 非 malloc/dlsym 基础缺陷, 而是特定调用路径问题。
        {
            void *p1 = malloc(1024 * 128);
            DIAG("probe malloc(128K) = %p", p1);
            void *p2 = malloc(1024 * 2048);
            DIAG("probe malloc(2M) = %p", p2);
            void *p3 = calloc(1, 1024 * 1024);
            DIAG("probe calloc(1M) = %p", p3);
            void *(*mfp)(size_t) = (void *(*)(size_t))dlsym(RTLD_DEFAULT, "malloc");
            DIAG("probe dlsym(RTLD_DEFAULT,malloc) = %p", (void *)mfp);
            if (mfp) DIAG("probe mfp(4096) = %p", mfp(4096));
            free(p1); free(p2); free(p3);
        }

        // ★ /proc/self/maps 快照（P0 调研: 反查 crash#5 帧 [002] 0x595725dd34 / [003] 0x595725d480
        //   的映射身份——这两帧在 ld-musl base+0x50 万处，超出 ld-musl 2.19MB 大小，必属另一库）。
        {
            FILE *fp = fopen("/proc/self/maps", "r");
            if (fp) {
                char mline[600];
                while (fgets(mline, sizeof(mline), fp)) DIAG("MAP %s", mline);
                fclose(fp);
            } else {
                DIAG("MAP fopen /proc/self/maps FAILED: %s", strerror(errno));
            }
        }

        compile_probe(Py_CompileString_, "pre-inject", 7000);  // 健康对照：注入前同样 7KB 编译

        inject_stdlib_dynload(Py_CompileString_);  // 70 个内建 C 扩展（lib-dynload）先进 sys.modules，`import _asyncio` 等才可用

        // ★ 诊断 _struct 注入状态（干净 PyErr 位置，inject 后、torch._C 前）：
        //   真机 ctypes→struct→_struct 链报 "from-import-* object has no __dict__ and no __all__"，
        //   判定注入的 _struct 模块 dict 是否有问题。print(flush=True) 直写 stdout(=diag.log)。
        {
            const char *sa =
                "import sys\n"
                "m = sys.modules.get('_struct')\n"
                "print('DD|_struct in_sys=%r' % (m,), flush=True)\n"
                "if m is not None:\n"
                "    print('DD|type=%s dict=%s all=%s' % (type(m).__name__, hasattr(m,'__dict__'), hasattr(m,'__all__')), flush=True)\n"
                "    try:\n"
                "        from _struct import *\n"
                "        print('DD|star OK', flush=True)\n"
                "    except BaseException as e:\n"
                "        print('DD|star ERR %r' % (e,), flush=True)\n"
                "else:\n"
                "    try:\n"
                "        import _struct\n"
                "        print('DD|imported %r' % (_struct,), flush=True)\n"
                "    except BaseException as e:\n"
                "        print('DD|import ERR %r' % (e,), flush=True)\n";
            int rdiag = PyRun_SimpleString(sa);
            DIAG("struct-diag rc=%d (clean state)", rdiag);
        }

        // torch._C 接入改为分步日志（诊断 PyInit__C 自旋/卡死）：dlopen → dlsym → PyInit__C → 注册。
        // ⚠ 真机观测：走到此处后不再有后续日志，CPU 持续燃烧 ⇒ PyInit__C() 内部自旋（_C.so 已
        //   NEEDED+eager 加载且导出 PyInit__C，dlopen/dlsym 不会失败）。先分步确认，再定位 spin 源。
        const char *csoname = "_C.cpython-312-aarch64-linux-ohos.so";
        void *ch = dlopen(csoname, RTLD_LAZY | RTLD_GLOBAL);
        LOGI("torch._C dlopen=%{public}p err=%{public}s", ch, ch ? "" : dlerror());
        DIAG("torch._C dlopen=%p err=%s", ch, ch ? "" : dlerror());
        if (ch) {
            PyObject *(*cmk)(void) = (PyObject *(*)(void))dlsym(ch, "PyInit__C");
            LOGI("torch._C dlsym PyInit__C=%{public}p err=%{public}s", (void *)cmk, cmk ? "" : dlerror());
            DIAG("torch._C dlsym PyInit__C=%p err=%s", (void *)cmk, cmk ? "" : dlerror());
            if (cmk) {
                // 启动 watchdog 旁路观测 PyInit__C 是否真死锁（每 3s 打点）。
                g_pyinit_done = 0;
                pthread_t wtid;
                int wr = pthread_create(&wtid, nullptr, pyinit_watchdog, nullptr);
                if (wr == 0) pthread_detach(wtid);
                LOGI("torch._C calling PyInit__C ... (watchdog=%{public}d)", wr);
                DIAG(">>> calling PyInit__C (watchdog=%d, epoc=%ld) ...", wr, (long)time(nullptr));
                PyObject *cm;
                // ★ 2026-09-02：直调版弃用（历史使命完成：dlopen/dlsym OK、PyInit 抛异常已确认）。
                //   正式加载路径改为 probe 段的 importlib 官方链（exec_module→_imp.create/exec_dynamic），
                //   直调在此上下文反而触发 jemalloc SIGSEGV（进程死亡干扰后续诊断）。保留代码不删。
                try {
                    cm = nullptr;
                    g_pyinit_done = 1;
                    DIAG("PyInit__C direct-call skipped (deprecated; use importlib official chain)");
                } catch (const std::exception &e) {
                    // torch 用 pybind11，初始化异常多为 error_already_set（继承 std::exception/what() 含 Python traceback）。
                    // 此前未捕获 → libc++abi terminate → SIGABRT（进程 27502 实证）。捕获并打印 what() 拿真实 Python error：
                    //   * what()          → Python 侧 traceback（哪一步失败）
                    //   * PyErr_Print()    → 解释器积压的原始 error
                    //   经 DIAG(stderr→diag.log) 落地 sandbox，hdc 可读。
                    g_pyinit_done = 1;
                    LOGE("torch._C PyInit__C THREW std::exception: %{public}s", e.what());
                    DIAG("PyInit__C THREW std::exception what(): %s", e.what());
                    PyErr_Print();
                    cm = nullptr;
                } catch (...) {
                    g_pyinit_done = 1;
                    LOGE("torch._C PyInit__C THREW unknown-exception");
                    DIAG("PyInit__C THREW unknown-exception");
                    cm = nullptr;
                }
                if (cm) {
                    // ★ 3.12 multiphase 防御（与 libs 注入同因）：若 PyInit__C 返回的是 PyModuleDef
                    //   （moduledef 对象）而非真 module —— pybind11/标准扩展均可能——必须经
                    //   FromDefAndSpec2+ExecDef 物化，否则塞进 sys.modules 后 `import torch._C`
                    //   命中该对象会同样报 "no __dict__ and no __all__"/缺属性。物化失败则放弃注册。
                    if (!PyModule_Check(cm)) {
                        DIAG("torch._C: PyInit returned moduledef (multiphase) -> materialize");
                        PyObject *m2 = multiphase_materialize("torch._C", cm);
                        Py_DECREF(cm);
                        if (!m2) {
                            DIAG("torch._C multiphase materialize FAILED");
                            LOGE("torch._C multiphase materialize FAILED");
                            cm = nullptr;
                        } else {
                            cm = m2;
                        }
                    }
                }
                if (cm) {
                    PyObject *csys = PyImport_GetModuleDict();
                    PyDict_SetItemString(csys, "torch._C", cm);
                    Py_DECREF(cm);
                    LOGI("torch._C registered into sys.modules");
                } else {
                    LOGI("torch._C PyInit__C returned NULL (see stderr->py-init.log)");
                }
            }
        }
        // 命门③ 的「可执行」验证：不仅 import torch._C，还要跑一个真实 CPU 张量算子。
        // torch.ones(2,3).sum() 会调度到 libtorch_cpu（链系统 libc++.so.1=std::__1）执行 C++ ATen 代码，
        // 真正证明「__1 命名空间统一 + torch CPU 栈加载」都成立（而非仅符号可解析）。
        int r = PyRun_SimpleString(
            "import torch, sys; t=torch.ones(2,3); "
            "print('COMFTEST torch import ok, version=%s' % torch.__version__); "
            "print('COMFTEST torch._C=', torch._C); "
            "print('COMFTEST tensor sum=%s' % t.sum().item()); "
            "print('COMFTEST is_cuda=', torch.cuda.is_available() if hasattr(torch,'cuda') else 'na')");
        LOGI("torch import rc=%{public}d", r);
        DIAG("torch import (COMFTEST) rc=%d", r);
    }
    LOGI("libpython dlopen+init done (torch __1 stack verified)");
}

extern "C" void Main(NativeChildProcess_Args args)
{
    // ★ 诊断降级：在一切动作前把 stderr/stdout freopen 到 <pyroot>/diag.log（app sandbox，hdc 可读），
    //   用 sandbox 文件通道绕开 hilog domain 白名单。若 entryParams 缺 pyroot 则退回原 LOGI(hilog)。
    char diagproot[PATH_MAX] = {0};
    extract_pyroot(args.entryParams, diagproot, sizeof(diagproot));
    if (diagproot[0]) {
        char diaglog[PATH_MAX * 2];
        snprintf(diaglog, sizeof(diaglog), "%s/diag.log", diagproot);
        FILE *df = fopen(diaglog, "a");
        if (df) { dup2(fileno(df), STDOUT_FILENO); dup2(fileno(df), STDERR_FILENO); g_crashfd = fileno(df); }
        DIAG("---- comfy_child Main pid=%d diag.log (pyroot=%s) ----", (int)getpid(), diagproot);
    }
    // run44：MALLOC_CONF —— jemalloc（静态并入本栈，P0 命门③深层 0xb7ef0 实证）默认
    //   8 arena×线程预分配 + dirty 页保留是 anon RSS 虚胖主因（idle 基线 anon ~162MB）。
    //   压 narenas:1 + 双 decay_ms:0（垃圾立即归还内核）预期省 20-60MB；
    //   若 jemalloc 构造函数已先读 env 则此 setenv 无效——hb anon 曲线即为判官
    //   （无效则换预载/它径）。一切 malloc 前 set，最早位置。
    setenv("MALLOC_CONF", "narenas:1:dirty_decay_ms:0:muzzy_decay_ms:0:tcache:true", 1);
    // ★ 崩溃信号落地：必须在 freopen 之后、一切 dlopen/Py_* 之前安装，保证静默死亡也有 PC 链。
    install_crash_handlers();
    // ★ 退出溯源：atexit hook——区分「进程内 exit 正常路径」vs「被 SIGKILL 杀死（hook 不触发）」。
    //   实证：36607 轮 run_path 中 kernel 报 [PID 36607 KILLED][SIG 9]，C++ 侧无何日志即被 SIGKILL；
    //   若本 hook 触发 => 进程走 libc exit 路径；不触发 => 外部 SIGKILL（atoz OOM/框架杀）。
    atexit([] {
        LOGI("ATEXIT fired — libc exit path, NOT SIGKILL");
    });
    // __cxa_throw hook 预解析（任何 C++ 抛掷前的安全初始化；抛掷路径禁止 dlsym）
    init_cxa_throw_hook();
    LOGI("Main ENTER pid=%{public}d entryParams=%{public}s",
         (int)getpid(), args.entryParams ? args.entryParams : "(null)");
    DIAG("Main ENTER pid=%d entryParams=%s", (int)getpid(), args.entryParams ? args.entryParams : "(null)");

    cpu_baseline();     // 不确定点④：CPU 基线
    fork_exec_check();  // 不确定点②：子进程池 fork/exec
    socket_check();     // 不确定点③：绑定 127.0.0.1 端口
    dlopen_python_check(args.entryParams);  // Step 0.2/0.3：dlopen libpython3.12.so + Py_Initialize + import torch(__1)

    // ─────────────────────────────────────────────────────────────────────────
    // Step 0.4c：ComfyUI 主程序装载——NCP 子进程内直接 Main() 起 127.0.0.1:8188 真实后端。
    //   dlopen_python_check 已把 torch/aiohttp/pydantic 全链跑通 + meta_path finder 挂好；
    //   此处把 <pyroot>/comfyui（python312.zip 解压出的仓库根，含 main.py/server.py/comfy/）挂载：
    //     PYTHONPATH 追加；chdir；--front-end-root <pyroot>/comfyui/frontend_static（mini 静态目录，
    //     绕开 comfyui-frontend-package 硬校验）；--cpu（OHOS 无 CUDA，model_management 必须 CPUState.CPU）；
    //     --port 8188（现值，与 entryParams 的“8188”一致）。
    //   runpy.run_path(main.py, run_name="__main__") 让 __main__ 判定成立；run_forever 阻塞 = 子进程常驻。
    // ⚠ dlopen_python_check 里的 Py_* 函数指针是其局部 dlsym，不入 Main 作用域；此处经
    //   RTLD_DEFAULT 全局符号表重取（libpython 已是 comfy_child 的 NEEDED，全局表必有）。
    {
        using PyIsInit = int (*)();
        using PyRunSimple = int (*)(const char *);
        auto p_isi = (PyIsInit)dlsym(RTLD_DEFAULT, "Py_IsInitialized");
        auto p_run = (PyRunSimple)dlsym(RTLD_DEFAULT, "PyRun_SimpleString");
        if (!p_isi || !p_run) {
            LOGE("0.4c dlsym Py_IsInitialized=%{public}p PyRun_SimpleString=%{public}p -> SKIP",
                 (void *)p_isi, (void *)p_run);
            DIAG("0.4c dlsym failed isi=%p run=%p -> SKIP", (void *)p_isi, (void *)p_run);
        } else if (!p_isi()) {
            LOGE("0.4c Py_Initialize not done -> SKIP main.py");
            DIAG("0.4c Py_Initialize not done -> SKIP main.py");
        } else {
            char pyroot_c[PATH_MAX] = {0};
            extract_pyroot(args.entryParams, pyroot_c, sizeof(pyroot_c));
            if (pyroot_c[0]) {
                char comfy_root[PATH_MAX * 2] = {0};
                snprintf(comfy_root, sizeof(comfy_root), "%s/comfyui", pyroot_c);
                // PYTHONPATH 追加 comfyui 根（保持既有 <libdir>:<sitepack>，双保险同 sys.path.insert）
                const char *cur = getenv("PYTHONPATH");
                char pp[PATH_MAX * 4] = {0};
                snprintf(pp, sizeof(pp), "%s%s%s", cur ? cur : "", (cur && cur[0]) ? ":" : "", comfy_root);
                setenv("PYTHONPATH", pp, 1);
                LOGI("0.4c PYTHONPATH=%{public}s", pp);
                DIAG("0.4c PYTHONPATH=%s", pp);
                // run38：删除 run32 对照的 sleep(30)—— 它白吃启动配额 30s，而主链
                //   （PYHB t0 起）在 ~100s 处两头 child 前后脚同死（与 rss/import 无关）：
                //   配额常量≈主链 100s 或进程寿命 ~195s。省 30s + 节流 0.12 对跑竞速。
                // python 引导脚本：comfy_root 经 snprintf %s 注入（单引号安全：filesDir 无引号字符）。
                // 附带 daemon 自检线程：run_path 阻塞起 server 后轮询 8188 /system_stats，
                // 200 即打印 CF-OK-8188（diag.log 通道，真机端到端判定）；失败 240s 打 CF-FAIL。
                char scriptbuf[PATH_MAX * 4 + 1400] = {0};
                // run10：comfy main.py（已插 M-jobs 桩，定位被杀时执行到哪一步）。
                //   run9 对照（裸 socket server）证明 server/bind/8188 本身不被杀；
                //   run5/7（无桩版 comfy）死于 run+~6s——本轮用 M 桩钉死确切的语句点。
                snprintf(scriptbuf, sizeof(scriptbuf),
                         "import os, sys, runpy\n"
                         // run33：CPU 节流 meta_path hook——每个新 import 前周期性让出。判决
                         //   「连续满载 2s 杀」（随机:打断后活）vs「CPU 积分 2.1s 杀」（总账,打断也死）。
                         //   torch 链新模块 ~150+，%4 则每次平均 1/4 让 0.4s → 连续满载段被切碎 <1s、
                         //   总 CPU 积率≈90%（若积分论则总 CPU 满 2.1s ≈ 2.4s 处仍死）。
                         // run37：节流 hook 恢复(run33 设计) + 线程单化(新,两条命中的合流)。
                         //   ① 节流(meta_path.find_spec 每次新模块前 sleep 0.15s)：run34 实证
                         //      把 import 链从 2s 洪峰拖成 55s+ 缓流,累计 3s+ CPU 仍活；
                         //   ② 单线程化(OMP/MKL/OPENBLAS/NUMEXPR=1)：run36 PULSE 实证「单核
                         //      100%(1.0s 连续 busy 段)全程不死」,而主链 t=4→6 + cpu+202j/2s
                         //      (≈4 核每核 100%)2s 即死 → 死因精确化 = **多核同时满载**;
                         //      torch 大 C init(cosmos/flux dit)线程池满核即该形态,限线程后
                         //      压回「单核满」豁免区。要在任何 comfy/torch import 前生效。
                         "import time as _p_t\n"
                         "import importlib.abc as _p_abc\n"
                         "import sys as _p_sys\n"
                         "class _Thr(_p_abc.MetaPathFinder):\n"
                         "    def find_spec(self, _n, _p=None, _t=None):\n"
                         "        _p_t.sleep(0.01)\n"      // run42: 0.12→0.01。死因定谳=PYHB t0+84.3s 定时窗
                         "        return None\n"
                         "_p_sys.meta_path.insert(0, _Thr())\n"
                         "import os as _p_os\n"
                         "_p_os.environ['OMP_NUM_THREADS'] = '1'\n"
                         "_p_os.environ['MKL_NUM_THREADS'] = '1'\n"
                         "_p_os.environ['OPENBLAS_NUM_THREADS'] = '1'\n"
                         "_p_os.environ['NUMEXPR_NUM_THREADS'] = '1'\n"
                         "import gc as _p_gc\n"
                         "print('RUN37-THR-OK env=' + _p_os.environ['OMP_NUM_THREADS'] + '-' + _p_os.environ['MKL_NUM_THREADS'], flush=True)\n"
                         "import threading, urllib.request, time\n"
                         "_cf = '%s'\n"
                         "sys.path.insert(0, _cf)\n"
                         "os.chdir(_cf)\n"
                         "_fe = os.path.join(_cf, 'frontend_static')\n"
                         "sys.argv = ['main.py', '--cpu', '--port', '8188', '--front-end-root', _fe]\n"
                         "print('SCRIPTBUF-OK sys.path=', sys.path, flush=True)\n"   // run30：证 sitecustomize(sleep40) 之后的执行点 + sys.path 影像
                         "print('SCHK sc=', __import__('os').path.exists('/data/storage/el2/base/haps/entry/files/pyroot/lib/python3.12/sitecustomize.py'), ' sitepy=', __import__('site').__file__, flush=True)\n"   // run31：sitecustomize 是否解压落树 + site 本体从哪加载
                         // run82-3 tokenizers 顶层仍 is-not-a-package：此 dump 一次拿全证据——
                         //   libs 扫描键、sys.path、s-p/tokenizers 实态（dir?__init__?pyi-only?）、
                         //   直接 import 结果。放 scriptbuf 是因为其 print 稳定出现在 hilog。
                         "import importlib.util as _ilu2\n"
                         "_dirs2 = ['/data/storage/el1/bundle/libs/arm64', '/data/storage/el1/bundle/libs/arm64-v8a']\n"
                         "_exts2 = {}\n"
                         "for _d2 in _dirs2:\n"
                         "    try:\n"
                         "        _lst2 = os.listdir(_d2)\n"
                         "    except OSError as _e2:\n"
                         "        print('TOKK libdir-fail %%s %%r' %% (_d2, _e2), flush=True)\n"
                         "        continue\n"
                         "    for _f2 in _lst2:\n"
                         "        for _s2 in ('.cpython-312-aarch64-linux-ohos.so', '.cpython-312-aarch64-linux-musl.so', '.abi3.so'):\n"
                         "            if _f2.endswith(_s2):\n"
                         "                _k2 = _f2[:-len(_s2)]\n"
                         "                if _k2 not in _exts2:\n"
                         "                    _exts2[_k2] = _d2\n"
                         "                break\n"
                         "print('TOKK exts-has-tokenizers=%%s' %% ('tokenizers' in _exts2), flush=True)\n"
                         "print('TOKK sys.path=', [p for p in sys.path if isinstance(p, str) and p], flush=True)\n"
                         "for _sp2 in [p for p in sys.path if isinstance(p, str) and p]:\n"
                         "    _tk2 = os.path.join(_sp2, 'tokenizers')\n"
                         "    if os.path.isdir(_tk2):\n"
                         "        print('TOKK dir=%%s init=%%s pyi=%%s list=%%s' %% (_tk2, os.path.isfile(os.path.join(_tk2, '__init__.py')), os.path.isfile(os.path.join(_tk2, 'tokenizers.pyi')), sorted(os.listdir(_tk2))[:9]), flush=True)\n"
                         "    elif os.path.isfile(_tk2 + '.py'):\n"
                         "        print('TOKK singlefile=%%s' %% (_tk2 + '.py',), flush=True)\n"
                         "try:\n"
                         "    import tokenizers as _tk3\n"
                         "    print('TOKK import-OK mod=%%s pathattr=%%s' %% (_tk3, hasattr(_tk3, '__path__')), flush=True)\n"
                         "except BaseException as _tk4:\n"
                         "    print('TOKK import-FAIL %%r' %% (_tk4,), flush=True)\n"
                         "def _probe():\n"
                         "    for _ in range(120):\n"
                         "        try:\n"
                         "            with urllib.request.urlopen('http://127.0.0.1:8188/system_stats', timeout=2) as r:\n"
                         "                b = r.read(300)\n"
                         "                print('CF-OK-8188 status=%%d body=%%r' %% (r.status, b[:100]), flush=True)\n"
                         "                return\n"
                         "        except Exception:\n"
                         "            time.sleep(2)\n"
                         "    print('CF-FAIL-8188 no-200-in-240s', flush=True)\n"
                         "threading.Thread(target=_probe, daemon=True).start()\n"
                         // run12：__import__ tracer 由一条一打点改为「记状态不打印」——run11 实证
                         //   逐条 IMPOK 在 import 高峰 2s 就 90-121KB，把 diag.log 涨成大洪水（hb-new
                         //   全被 16K 阈值 SKIP，死前尾巴反而永远看不到）。改为只记 cur（正在 import）
                         //   /last（刚完成），由 pyhb（0.5s/拍）随身带出——**死前最后一拍 PYHB 的
                         //   cur/last 即「死点在哪个 import 内/后」**，信息量等值而量纲 1000 倍降；
                         //   同时 diag 增量瘦身 → hb-new 不再 SKIP → M 桩/traceback 尾部可见。
                         "import builtins as _bi\n"
                         "_orig_imp = _bi.__import__\n"
                         "_imp_cur = [None]\n"
                         "_imp_last = [None]\n"
                         "_imp_t0 = time.monotonic()\n"
                         "def _rss():\n"
                         "    try:\n"
                         "        with open('/proc/self/statm') as _f:\n"
                         "            return (int(_f.read().split()[1]) << 12) >> 10\n"
                         "    except Exception:\n"
                         "        return -1\n"
                         "_big = [-1]\n"
                         "def _imp(name, *a, **k):\n"
                         "    if name == 'sympy':\n"
                         "        import traceback\n"
                         "        print('SYMPY-STACK start', flush=True)\n"
                         "        traceback.print_stack(limit=14)\n"
                         "    _imp_cur[0] = name\n"
                         "    _m = _orig_imp(name, *a, **k)\n"
                         "    _imp_last[0] = _imp_cur[0]\n"
                         "    _imp_cur[0] = None\n"
                         "    _r = _rss()\n"
                         "    if _big[0] >= 0 and _r - _big[0] > 8192:\n"
                         "        print('IMPBIG +%%dKB rss=%%dKB after=%%s' %% (_r - _big[0], _r, name), flush=True)\n"
                         "    _big[0] = _r\n"
                         "    return _m\n"
                         "_bi.__import__ = _imp\n"
                         // run46：torch preflight —— ① interop 线程池 8 核→1（interop 每个线程
                         //   ATen 调度器/队列预分配，是 anon 162MB 候选之一）；② set_num_threads(1)
                         //   再压 OMP（与 OMP_NUM_THREADS=1 双保险）；③ 打 smaps 匿名区 top-15：
                         //   anon=~162MB 的归属解剖（py 堆 / torch C++ 堆 / jemalloc arena / .bss
                         //   /线程栈），看清哪些有削减空间。必须在任何 comfy import 前跑。
                         "import torch as _p_tr\n"
                         "try:\n"
                         "    _p_tr.set_num_interop_threads(1)\n"
                         "    _p_tr.set_num_threads(1)\n"
                         "    print('TORCH-SLIM-OK rss=%%dKB' %% _rss(), flush=True)\n"
                         "except Exception as _e:\n"
                         "    print('TORCH-SLIM-FAIL %%r' %% _e, flush=True)\n"
                         "def _anon_top():\n"
                         "    _seg = [None, 0, 0]\n"
                         "    _all = []\n"
                         "    try:\n"
                         "        for _l in open('/proc/self/smaps'):\n"
                         "            _f = _l.split()\n"
                         "            if len(_f) >= 2 and len(_f[1]) == 4 and _f[1][0] == 'r' and '-' in _f[0]:\n"
                         "                if _seg[0] is not None:\n"
                         "                    _all.append((_seg[0], _seg[1], _seg[2]))\n"
                         "                _seg = [_f[5] if len(_f) >= 6 else '[anon]', 0, 0]\n"
                         "            elif len(_f) == 3 and _seg[0] is not None:\n"
                         "                if _f[0] in ('Rss:', 'Anonymous:') and _f[2] == 'kB':\n"
                         "                    _v = int(_f[1])\n"
                         "                    if _f[0] == 'Rss:':\n"
                         "                        _seg[1] += _v\n"
                         "                    else:\n"
                         "                        _seg[2] += _v\n"
                         "        if _seg[0] is not None:\n"
                         "            _all.append((_seg[0], _seg[1], _seg[2]))\n"
                         "    except Exception as _e2:\n"
                         "        print('ANON-TOP-FAIL %%r' %% _e2, flush=True)\n"
                         "        return\n"
                         "    _all.sort(key=lambda _x: -_x[1])\n"
                         "    for _name, _rss, _anon in _all[:15]:\n"
                         "        print('ANON-TOP rss=%%dkB anon=%%dkB %%s' %% (_rss, _anon, _name[:80]), flush=True)\n"
                         "_anon_top()\n"
                         // ★ 判决实验（run48）：手动踩墙——「死点 rss 恒 289.2MB」是否真是
                         //   rss 墙。Phase-1：mmap 匿名区逐页触写 +400MB（每 40MB 一档打印 rss，
                         //   触页保证 rss 实涨而非 overcommit 纸面数；若 289.2 墙存在 → 在此被杀）；
                         //   Phase-2：清空释放（munmap，rss 立降）→ 继续 runpy 主链 ——
                         //   若 Phase-1 能过 289 而主链仍死于 289.2 → 死因与 rss 无关（链动作时序）。
                         "import mmap as _p_mm\n"
                         "def _wall_probe():\n"
                         "    _maps = []\n"
                         "    _chunk = 40 * 1048576\n"
                         "    try:\n"
                         "        for _k in range(10):\n"
                         "            _m = _p_mm.mmap(-1, _chunk)\n"
                         "            for _i in range(0, _chunk, 4096):\n"
                         "                _m[_i] = 1\n"
                         "            _maps.append(_m)\n"
                         "            _p_gc.collect()\n"
                         "            print('WALL-PROBE alloc=%%dMB rss=%%dKB' %% ((_k + 1) * 40, _rss()), flush=True)\n"
                         "        print('WALL-PROBE survived +400MB', flush=True)\n"
                         "    except Exception as _e3:\n"
                         "        print('WALL-PROBE-EXC %%r' %% _e3, flush=True)\n"
                         "    finally:\n"
                         "        _maps.clear()\n"
                         "        _p_gc.collect()\n"
                         "        print('WALL-PROBE freed rss=%%dKB' %% _rss(), flush=True)\n"
                         "_wall_probe()\n"
                         "def _pyhb():\n"
                         "    for _i in range(600):\n"
                         "        _p_gc.collect()\n"
                         "        print('PYHB[%%d] t=%%.1f cur=%%r last=%%r rss=%%dKB' %% (_i, time.monotonic()-_imp_t0, _imp_cur[0], _imp_last[0], _rss()), flush=True)\n"
                         "        time.sleep(0.5)\n"
                         "threading.Thread(target=_pyhb, daemon=True).start()\n"
                         "print('CF-RUN main.py started cwd=%%s argv=%%r' %% (os.getcwd(), sys.argv), flush=True)\n"
                         // ★ run53 判决:触发段 bypass —— run52 已把触发段钉在
                         //   comfy/model_base.py 顶层「import comfy.ldm.aura.mmdit / cosmos.model /
                         //   cosmos.predict2」及 sd.py 的 cosmos.vae(DS6-START 内 0.5s 即被杀)。
                         //   这些定义模块只在「加载模型类」时被引用(server 启动不实例化),故用
                         //   sys.modules 预置 stub(任意属性→占位类)。若主链自此存活并奔向
                         //   server(Starting server)即为证据闭环;若仍死 → 触发不在这些段。
                         "sys.path.insert(0, _cf)\n"
                         // ★ run54:第二层段切片 —— run53 的组 stub(aura/cosmos/vae)绕不开死点,故
                         //   触发层应在更浅的 comfy 核心包。逐段验证:
                         //   comfy.model_management(设备/线程/子进程敏感) / comfy.sd / comfy.controlnet。
                         // ★ run57(范围网):run50 死区已圈定 = 「pre-comfy.utils → post-nodes 之间
                         //   的 import 网」(M-jobs 显示 post-nodes 都未打印)。把该网切成 13 段放
                         //   main_sim.py(zip 内,带 NCP 真实 argv + --cpu 解析 → 消除 DS 假阳性断言),
                         //   一次 run 定死点段;全部 OK 后进入 300s PING 循环(区分「网内死」vs
                         //   「网外时间/行为死」)。sim 成功后再由 runpy main_sim 段继续跑真 main.py?
                         //   —— 不:sim 内含 300s 循环,先看本次段结果再决定第二轮。
                         "sys.path.insert(0, _cf)\n"
                         "import traceback as _tb\n"
                         // ★ run78 修复:comfy.options.args_parsing 先开 —— comfy 0.34 的
                         //   options.py 模块级默认 args_parsing=False,只有真 main.py:2
                         //   enable_args_parsing() 才开启;而本注入段(含 stub_global 的
                         //   _chain→import comfy 链与 run76 WARM 段)先于 main.py import
                         //   comfy 系,cli_args.py:277-280 首次解析即走 parse_args([])
                         //   → args.cpu=False 永久定格(模块已入 sys.modules,main.py:2 的
                         //   enable 无法重解析)→ model_management.py:158 if args.cpu 不成立
                         //   → :364 get_torch_device → :213 torch.cuda.current_device()
                         //   → AssertionError('Torch not compiled with CUDA enabled')
                         //   → 主链与 WARM 全灭(run77 实证 TB)。修复:在一切 comfy
                         //   import 之前先 options.enable_args_parsing(),使 parse_args()
                         //   真解析 ['--cpu','--port','8188',...] → args.cpu=True。
                         "import comfy.options as _opts\n"
                         "_opts.enable_args_parsing()\n"
                         // ★ run72 方案 B:stub 前置 —— sitecustomize 面未如期生效(无
                         //   SIM57-STUB),本注入段为已验证路径:先 run_path(stub_global.py)
                         //   预置 84 叶子 stub(与 main 同目录/同 zip),再 run_path 真 main.py。
                         //   失败即 fatal(标记 STUB-GLOBAL-FAIL,便于日志定位)。
                         "try:\n"
                         "    runpy.run_path(os.path.join(_cf, 'stub_global.py'), run_name='_ohos_stub')\n"
                         "except BaseException:\n"
                         "    sys.stdout.write('STUB-GLOBAL-FAIL\\n' + _tb.format_exc() + 'STUB-GLOBAL-FAIL-END\\n')\n"
                         "    sys.stdout.flush()\n"
                         "    raise\n"
                         // ★ run76 分案:cldm 链预热 —— 把 [cldm→ops→model_management
                         //   →pinned/memory/float/quant_ops] 与 samplers/controlnet 提前 import:
                         //     WARM-FAIL(X)=某模块抛异常(异常链);WARM 全部 OK 且 main 仍死 =>
                         //     死点不在预热链;死在某 WARM 步 => 该模块即触发器(SIGKILL)。
                         // ★ run79/80:text_encoders 深链二级分案 —— run79(修 args_parsing 后)
                         //   WARM 8/9 OK,主链死于 comfy.text_encoders.ideogram4(t=21.6,无 py-err
                         //   dump=SIGKILL 型);run80(20 列表)WARM 8 OK 后死于第 9 步
                         //   comfy.controlnet(A/B 两跑定谳,2/2)。run81:WARM 移除 controlnet,
                         //   专心覆盖主链死点 ideogram4 深链,qwen_vl→qwen35→llama
                         //   [clip_model/common_dit/model_prefetch/attention]→qwen3vl→ideogram4,
                         //   main 命中 sys.modules 直穿主链死点 → 推向下一个死点。
                         "import importlib as _il2\n"
                         "_WARM_MODS = ['comfy.ops', 'comfy.model_management', 'comfy.pinned_memory',\n"
                         "    'comfy.memory_management', 'comfy.float', 'comfy.quant_ops',\n"
                         "    'comfy.cldm.cldm', 'comfy.samplers',\n"
                         "    'comfy.clip_model', 'comfy.ldm.common_dit', 'comfy.ldm.modules.attention',\n"
                         "    'comfy.model_prefetch', 'comfy.utils', 'comfy.sd1_clip',\n"
                         "    'comfy.text_encoders.qwen_vl', 'comfy.text_encoders.qwen35',\n"
                         "    'comfy.text_encoders.llama', 'comfy.text_encoders.qwen3vl',\n"
                         "    'comfy.text_encoders.ideogram4']\n"
                         "for _wm in _WARM_MODS:\n"
                         "    try:\n"
                         "        _il2.import_module(_wm)\n"
                         "        print('WARM-OK %%s rss=%%dKB' %% (_wm, _rss()), flush=True)\n"
                         "    except BaseException as _we:\n"
                         "        print('WARM-FAIL %%s %%r' %% (_wm, _we), flush=True)\n"
                         // run82-3：WARM-FAIL 的 %r 会吞掉 __cause__（transformers 惰性包装
                         //   "Could not import module 'CLIPTokenizer'" 的原始 ImportError）——
                         //   追加打印原始 traceback（单行化防 hilog 丢块），下一轮真机拿到根因。
                         "        _tb2w = _tb.format_exc().replace(chr(10), '|')\n"
                         "        print('WARM-TB ' + _tb2w[-2200:], flush=True)\n"
                         "try:\n"
                         "    runpy.run_path(os.path.join(_cf, 'main.py'), run_name='__main__')\n"
                         "except BaseException:\n"
                         "    sys.stderr.flush()\n"
                         "    sys.stdout.write('PY-TB-BEGIN\\n' + _tb.format_exc() + 'PY-TB-END\\n')\n"
                         "    sys.stdout.flush()\n"
                         "    raise\n",
                         comfy_root);
                DIAG("0.4c running main.py ... (blocking)");
                // 记下 diag.log 当前尾部偏移：diag.log 为累积文件（含 crash handler maps dump 等
                // 旧内容），捞错时只取本 run 之后的新输出，行对齐，避免被旧尾巴冲掉。
                long runstart = -1;
                char diagpath_pre[PATH_MAX * 2] = {0};
                snprintf(diagpath_pre, sizeof(diagpath_pre), "%s/diag.log", pyroot_c);
                FILE *df_pre = fopen(diagpath_pre, "r");
                if (df_pre) {
                    fseek(df_pre, 0, SEEK_END);
                    runstart = ftell(df_pre);
                    fclose(df_pre);
                }
                // ★ pre-diag：run 前掏 diag.log 尾 64K，**先搜关键行**——每轮 child 的 probe 阶段会先写
                //   ~360KB 旧货（maps/probe）把上一轮死前 1181B 顶掉，纯尾部窗口每次只见 maps dump；
                //   定位于 CF-RUN/PY-TB/PYHB/Starting server 等关键行最后出现处，从它起打印才是
                //   上一轮 main.py 真正走到哪一步的证据（同 uid 可读，shell 无权限）。
                {
                    FILE *df2 = fopen(diagpath_pre, "r");
                    if (df2) {
                        fseek(df2, 0, SEEK_END);
                        long sz2 = ftell(df2);
                        long beg2 = (sz2 - 65536 > 0) ? (sz2 - 65536) : 0;
                        fseek(df2, beg2, SEEK_SET);
                        long span2 = sz2 - beg2;
                        char *pre2 = (char *)malloc((size_t)span2 + 1);
                        size_t n2 = fread(pre2, 1, (size_t)span2, df2);
                        pre2[n2] = '\0';
                        fclose(df2);
                        long keypos = -1;
                        const char *keys[] = {"CF-RUN", "PY-TB-", "PYHB[", "IMPBIG", "Starting server",
                                              "Traceback", "Listening on", "port 8188"};
                        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k) {
                            char *scan = pre2;
                            char *hit = nullptr;
                            while ((hit = strstr(scan, keys[k])) != nullptr) {
                                long hp = (long)(hit - pre2);
                                if (hp > keypos) keypos = hp;
                                scan = hit + 1;
                            }
                        }
                        for (size_t i2 = 0; i2 < n2; ++i2) {
                            if (pre2[i2] == '\n') pre2[i2] = '|';
                            else if (pre2[i2] == '\x1b') pre2[i2] = '^';
                        }
                        long disp = (keypos > 0) ? keypos : 0;
                        LOGI("0.4c pre-diag sz=%{public}ld n=%{public}zu keypos=%{public}ld", sz2, n2, keypos);
                        size_t off2 = (size_t)disp;
                        int ch2 = 0;
                        while (off2 < n2 && ch2 < 20) {
                            size_t l2 = (n2 - off2 > 900) ? 900 : (n2 - off2);
                            char b2[1000] = {0};
                            memcpy(b2, pre2 + off2, l2);
                            LOGI("0.4c pre[%{public}d] : %{public}s", ch2++, b2);
                            off2 += l2;
                        }
                        free(pre2);
                    }
                }
                // ★ heartbeat：p_run 阻塞期间独立线程每 2s 心跳 + 报 diag.log 尺寸——死在第几秒、
                //   死前 Python 已把多少字节写给 diag.log，一目了然；进程被 exit/杀则线程随灭。
                //   ⚠ 用 pthread：SDK <thread> 头的 std::thread 是 __n1 命名空间，-nostdlib++ 下
                //   ABI 不闭合（ld.lld undefined std::__1::thread 实测），pthread 无此问题。
                pthread_t hb_tid;
                static auto hb_fn = [](void *arg) -> void * {
                    char *proot = (char *)arg;
                    // ★ diag 增量 dump：记住上次尾部偏移，每拍把「新增字节」抽出来打 hilog——
                    //   run6 实测 diag-sz 直增（PYHB/api 心跳 +8/拍、有 +2289 大跳=server 横幅或大
                    //   traceback），而这些内容只在 diag.log（sandbox，shell 无权限）——此转帖即可判
                    //   「server 是否 UP（banner/CF-OK-8188）」vs「大栈」。首拍只记基线跳过。
                    static long last_sz = -1;
                    for (int i = 0;; i++) {
                        char p2[PATH_MAX * 2];
                        snprintf(p2, sizeof(p2), "%s/diag.log", proot);
                        long cur = -1;
                        FILE *f = fopen(p2, "r");
                        if (f) { fseek(f, 0, SEEK_END); cur = ftell(f); fclose(f); }
                        if (cur > 0) {
                            if (last_sz < 0) {
                                last_sz = cur;  // 首拍基线：CF-RUN+PYHB[0] 已在 hb[0] 前后统计过
                            } else if (cur > last_sz) {
                                long delta = cur - last_sz;
                                if (delta > 0 && delta <= 16384) {
                                    FILE *fd = fopen(p2, "r");
                                    if (fd) {
                                        fseek(fd, last_sz, SEEK_SET);
                                        char nb[16384 + 1];
                                        size_t nn = fread(nb, 1, (size_t)delta, fd);
                                        nb[nn] = '\0';
                                        fclose(fd);
                                        for (size_t x = 0; x < nn; ++x) {
                                            if (nb[x] == '\n') nb[x] = '|';
                                            else if (nb[x] == '\x1b') nb[x] = '^';
                                        }
                                        size_t off3 = 0; int ch3 = 0;
                                        while (off3 < nn) {
                                            size_t l3 = (nn - off3 > 900) ? 900 : (nn - off3);
                                            char b3[901] = {0};
                                            memcpy(b3, nb + off3, l3);
                                            LOGI("0.4c hb-new[%{public}d] : %{public}s", ch3++, b3);
                                            off3 += l3;
                                        }
                                    }
                                } else {
                                    // run12：>16K 不再 SKIP——dump 末 12KB。死点前的最后输出
                                    //   （M 桩/traceback/CF-OK-8188）全在尾部；run11 的 SKIP 把
                                    //   死点证据整个吞掉了（90-121KB/2s 的 import 洪水）。
                                    LOGI("0.4c hb-new[%{public}d] : delta %{public}ldB(>16K) -> tail", i, delta);
                                    FILE *fd = fopen(p2, "r");
                                    if (fd) {
                                        long tail_n = (delta > 12288) ? 12288 : delta;
                                        fseek(fd, cur - tail_n, SEEK_SET);
                                        char nb[12288 + 1];
                                        size_t nn = fread(nb, 1, (size_t)tail_n, fd);
                                        nb[nn] = '\0';
                                        fclose(fd);
                                        for (size_t x = 0; x < nn; ++x) {
                                            if (nb[x] == '\n') nb[x] = '|';
                                            else if (nb[x] == '\x1b') nb[x] = '^';
                                        }
                                        size_t off3 = 0; int ch3 = 0;
                                        while (off3 < nn) {
                                            size_t l3 = (nn - off3 > 900) ? 900 : (nn - off3);
                                            char b3[901] = {0};
                                            memcpy(b3, nb + off3, l3);
                                            LOGI("0.4c hb-new[%{public}d] : %{public}s", ch3++, b3);
                                            off3 += l3;
                                        }
                                    }
                                }
                                last_sz = cur;
                            }
                        }
                        // RSS 曲线：判定死因是否 cgroup OOM（若 hb 全部 rss 平缓平稳而 5.8s 被杀
                        // => 定时器类杀；若 RSS 直冲 limit => OOM）
                        // ★ run13 追加：oom_score_adj（memmgr 给进程打的"可杀分"，LMK 出手前由
                        //   0→高值突变）+ cgroup 路径（含 memcg limit）——分别指认 memmgr 手动杀
                        //   与 kernel memcg OOM 杀。run12 全家数据：所有死轮 rss 恰好冲到 ~340MB
                        //   （run9 裸 server 恒 240MB 活 72s+）→ 「RSS≈340MB 杀」嫌疑最大。
                        long rss = -1;
                        long vpeak = -1;      // VmPeak：VmSize 虚拟峰值（fork 继承父进程基线，仅参考）
                        long vhwmk = -1;      // VmHWM：**RSS 峰值**（killwater）——拍间(≤0.5s)瞬态 RSS
                                               //   尖峰只有它能事后回看；尖峰论 vs CPU 论判官
                        long vmsize = -1;     // VmSize：实时虚拟大小——run41 与死点对照
                        long nthreads = -1;
                        int oomadj = -1000;
                        FILE *fs = fopen("/proc/self/status", "r");
                        if (fs) {
                            char line[256];
                            while (fgets(line, sizeof(line), fs)) {
                                if (strncmp(line, "VmRSS:", 6) == 0) { rss = atol(line + 6); }
                                else if (strncmp(line, "VmPeak:", 7) == 0) { vpeak = atol(line + 7); }
                                else if (strncmp(line, "VmHWM:", 6) == 0) { vhwmk = atol(line + 6); }
                                else if (strncmp(line, "VmSize:", 7) == 0) { vmsize = atol(line + 7); }
                                else if (strncmp(line, "Threads:", 8) == 0) { nthreads = atol(line + 8); }
                            }
                            fclose(fs);
                        }
                        // run41：statm 分解——size/resident/shared([[页]])。AnonRSS=(resident-shared)×4K、
                        //   FileRSS=shared×4K。死亡瞬间若 Anon 平缓而 File 陡增 → 加载 .so 页计入
                        //   cgroup file cache；反之 Anon 陡增 → Python 堆/库数据段 —— 死点在谁头上。
                        long st_size = -1, st_res = -1, st_shared = -1;
                        FILE *fsm = fopen("/proc/self/statm", "r");
                        if (fsm) {
                            long d1 = -1, d2 = -1, d3 = -1, d4 = -1;
                            fscanf(fsm, "%ld %ld %ld %ld %ld %ld %ld", &st_size, &st_res, &st_shared, &d1, &d2, &d3, &d4);
                            fclose(fsm);
                        }
                        // run41：/proc/self/limits——appspawn 若设了 per-process 硬限（AS/Data/RSS），
                        //   死点对照即知是哪条 RLIMIT；unlimited 记 -2。
                        long lim_as = -2, lim_data = -2, lim_rss = -2;
                        FILE *fl = fopen("/proc/self/limits", "r");
                        if (fl) {
                            char lb[256];
                            while (fgets(lb, sizeof(lb), fl)) {
                                long *dst = nullptr;
                                if (strncmp(lb, "Max address space", 18) == 0) dst = &lim_as;
                                else if (strncmp(lb, "Max data size", 14) == 0) dst = &lim_data;
                                else if (strncmp(lb, "Max resident set", 17) == 0) dst = &lim_rss;
                                if (!dst) continue;
                                char *t2 = lb;
                                while (*t2 && *t2 != ' ') t2++;      // 跳过 "Max"
                                while (*t2 == ' ') t2++;
                                while (*t2 && *t2 != ' ') t2++;      // 跳过 "address/data/resident"
                                while (*t2 == ' ') t2++;
                                if (strncmp(t2, "unlimited", 9) == 0) *dst = -2;
                                else *dst = atol(t2);
                            }
                            fclose(fl);
                        }
                        FILE *fo = fopen("/proc/self/oom_score_adj", "r");
                        if (fo) { char ob[64] = {0}; if (fgets(ob, sizeof(ob), fo)) oomadj = atoi(ob); fclose(fo); }
                        // run28：系统内存画像——系统 12GB，若杀与 cgroup/系统内存无关，commit/avail
                        //   应保持宽松；若死前 Committed_AS 直逼 CommitLimit → overcommit 系杀。
                        long memavail = -1, committed = -1, commitlim = -1;
                        FILE *fm = fopen("/proc/meminfo", "r");
                        if (fm) {
                            char ml[128];
                            while (fgets(ml, sizeof(ml), fm)) {
                                if (strncmp(ml, "MemAvailable:", 13) == 0) { memavail = atol(ml + 13); }
                                else if (strncmp(ml, "Committed_AS:", 13) == 0) { committed = atol(ml + 13); }
                                else if (strncmp(ml, "CommitLimit:", 12) == 0) { commitlim = atol(ml + 12); }
                            }
                            fclose(fm);
                        }
                        // run28：CPU 画像——/proc/self/stat utime+stime 差分/拍（2s）。若死前一拍
                        //   cpu≈200 jiffies（100Hz×2s）→ 持续 100% 满载（≈「CPU 满载超限 ~5s 杀」）；
                        //   若 ≈0 → 则杀与 CPU 无关（剩定时器论）。与 hb 同属 static 跨拍。
                        static long cpu_prev = -1;
                        long cpu_now = -1, cpu_delta = -1;
                        FILE *fp2 = fopen("/proc/self/stat", "r");
                        if (fp2) {
                            char sb[768];
                            if (fgets(sb, sizeof(sb), fp2)) {
                                char *ep = strrchr(sb, ')');
                                if (ep) {
                                    long ut = -1, st = -1;
                                    sscanf(ep + 1, " %*c %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %ld %ld", &ut, &st);
                                    if (ut >= 0 && st >= 0) cpu_now = ut + st;
                                }
                            }
                            fclose(fp2);
                        }
                        if (cpu_now >= 0 && cpu_prev >= 0 && cpu_now >= cpu_prev) {
                            cpu_delta = cpu_now - cpu_prev;
                        }
                        cpu_prev = cpu_now;
                        char cgs[192] = {0};
                        FILE *fc = fopen("/proc/self/cgroup", "r");
                        if (fc) {
                            while (fgets(cgs, sizeof(cgs), fc)) {
                                if (strstr(cgs, "memory")) break;   // 优先 cgroup v2 或 memory 控制器段
                                cgs[0] = '\0';
                            }
                            fclose(fc);
                            if (cgs[0]) { size_t l = strlen(cgs); while (l && (cgs[l-1] == '\n')) cgs[--l] = '\0'; }
                        }
                        char mcgb[448] = {0};
                        probe_memcg(mcgb, sizeof(mcgb));
                        // run47：父进程（Main/UI）探针——child 与主进程是否同 memcg/
                        //   主进程 rss 多少，是「app 共享墙」vs「child 独墙」的终审判词。
                        //   同 uid + 同 selinux 域（app 域），/proc/<ppid>/ 应可读。
                        char ppidb[512] = {0};
                        pid_t ppid_p = getppid();
                        if (ppid_p > 0) {
                            char pp[64];
                            long prss = -1;
                            snprintf(pp, sizeof(pp), "/proc/%d/status", (int)ppid_p);
                            FILE *fp3 = fopen(pp, "r");
                            if (fp3) {
                                char line[160];
                                while (fgets(line, sizeof(line), fp3)) {
                                    if (strncmp(line, "VmRSS:", 6) == 0) {
                                        prss = strtol(line + 6, nullptr, 10);
                                        break;
                                    }
                                }
                                fclose(fp3);
                            }
                            char pcgp[192] = {0};
                            snprintf(pp, sizeof(pp), "/proc/%d/cgroup", (int)ppid_p);
                            FILE *fp4 = fopen(pp, "r");
                            if (fp4) {
                                while (fgets(pcgp, sizeof(pcgp), fp4)) {
                                    if (strstr(pcgp, "memory")) break;
                                    pcgp[0] = '\0';
                                }
                                fclose(fp4);
                                if (pcgp[0]) { size_t l2 = strlen(pcgp); while (l2 && (pcgp[l2-1] == '\n')) pcgp[--l2] = '\0'; }
                            }
                            snprintf(ppidb, sizeof(ppidb), "ppid=%d rss=%ldKB cg=[%s]", (int)ppid_p, prss, pcgp);
                        } else {
                            snprintf(ppidb, sizeof(ppidb), "ppid=none");
                        }
                        LOGI("0.4c hb[%{public}d] diag-sz=%{public}ld rss=%{public}ldKB hwm=%{public}ldKB peak=%{public}ldKB vsz=%{public}ldKB anon=%{public}ldKB file=%{public}ldKB lim(as=%{public}ld data=%{public}ld rss=%{public}ld) t=%{public}ld cpu+%{public}ldj memavail=%{public}ldKB commit=%{public}ld/%{public}ldKB oomadj=%{public}d cg=[%{public}s] %{public}s %{public}s", i, cur, rss, vhwmk, vpeak, vmsize, (st_res >= 0 && st_shared >= 0) ? (st_res - st_shared) * 4 : -1, (st_shared >= 0) ? st_shared * 4 : -1, lim_as, lim_data, lim_rss, nthreads, cpu_delta, memavail, committed, commitlim, oomadj, cgs, mcgb, ppidb);
                        sleep(1);
                    }
                    return nullptr;
                };
                pthread_create(&hb_tid, nullptr, hb_fn, pyroot_c);
                pthread_detach(hb_tid);
                int rc = -100;
                try {
                    rc = p_run(scriptbuf);
                } catch (const std::exception &e) {
                    DIAG("0.4c run_path std::exception: %s", e.what());
                    PyErr_Clear();
                    rc = -1;
                } catch (...) {
                    DIAG("0.4c run_path unknown exception");
                    PyErr_Clear();
                    rc = -2;
                }
                LOGI("0.4c run_path returned rc=%{public}d (unexpected: server should block)", rc);
                DIAG("0.4c run_path returned rc=%d (unexpected: server should block)", rc);
                // Python 报错已打到 stderr(→<pyroot>/diag.log)；沙盒内 shell 无读权限，
                // 但本进程同 uid 可读——run 一旦返回（含 SystemExit 类"正常"退出，rc 恒 != 0
                // 或 0 不定）即掏尾部经 LOGI（白名单 tag）走 hilog；run 成功阻塞则永不返回。
                {
                    char diagpath[PATH_MAX * 2] = {0};
                    snprintf(diagpath, sizeof(diagpath), "%s/diag.log", pyroot_c);
                    char tail[65537] = {0};
                    FILE *df = fopen(diagpath, "r");
                    if (df) {
                        fseek(df, 0, SEEK_END);
                        long sz = ftell(df);
                        long beg = (runstart > 0 && sz > runstart) ? runstart : (sz - 65536 > 0 ? sz - 65536 : 0);
                        fseek(df, beg, SEEK_SET);
                        size_t n = fread(tail, 1, sizeof(tail) - 1, df);
                        tail[n] = '\0';
                        fclose(df);
                        // 行对齐：跳过首行残片（窗口非文件头时）
                        if (beg > 0) {
                            char *nl = strchr(tail, '\n');
                            if (nl) {
                                size_t skip = (size_t)(nl - tail) + 1;
                                memmove(tail, nl + 1, n - skip + 1);
                                n -= skip;
                            }
                        }
                        // hilog 单条 message 被设备端截断在 ~1023B（1200B 实测丢尾）——块 900B 保全；
                        // ⚠ hilog 按 \n 把消息拆成多行显示（块内第 2+ 行不含块号，抓取时被过滤丢）
                        //   —— 先单行化：\n→'|'，ESC(ANSI 色码)→'^'（WARNING 块带 \x1b[33m）。
                        for (size_t i = 0; i < n; ++i) {
                            if (tail[i] == '\n') tail[i] = '|';
                            else if (tail[i] == '\x1b') tail[i] = '^';
                        }
                        LOGI("0.4c py-err win: size=%{public}ld runstart=%{public}ld beg=%{public}ld n=%{public}zu",
                             sz, runstart, beg, n);
                        size_t off = 0;
                        int chunk = 0;
                        while (off < n) {
                            size_t len = (n - off > 900) ? 900 : (n - off);
                            char block[1000] = {0};
                            memcpy(block, tail + off, len);
                            block[len] = '\0';
                            LOGI("0.4c py-err[%{public}d] : %{public}s", chunk++, block);
                            off += len;
                        }
                    } else {
                        LOGI("0.4c py-err rc=%{public}d diag.log unreadable", rc);
                    }
                }
            } else {
                LOGE("0.4c no pyroot in entryParams -> comfyui root unknown, SKIP");
                DIAG("0.4c no pyroot in entryParams -> SKIP");
            }
        }
    }

    LOGI("Main EXIT (phase0: ncp/fork/socket/cpu/python verified)");
    DIAG("Main EXIT (phase0 done)");
}
