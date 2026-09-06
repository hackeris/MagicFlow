/**
 * napi_init.cpp — 父进程（ArkUI）入口。
 * 暴露 NAPI 函数 startComfyChild(entryParams)，经 OH_Ability_StartNativeChildProcess
 * 把 libcomfy_child.so:Main 作为 NCP 独立子进程拉起（参考 wineohos/wine_child.cpp 的
 * StartNativeChildProcess 参数传递型入口）。
 */
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits.h>
#include <dlfcn.h>
#include <hilog/log.h>
#include <AbilityKit/native_child_process.h>
#include "napi/native_api.h"
#include "neural_network_runtime/neural_network_core.h"   // P0 探针: NNRt 类型/枚举

#undef LOG_DOMAIN
#define LOG_DOMAIN 0xD001
#undef LOG_TAG
#define LOG_TAG "ComfyNapi"

#define LOGI(...) ((void)OH_LOG_INFO(LOG_APP, __VA_ARGS__))
#define LOGE(...) ((void)OH_LOG_ERROR(LOG_APP, __VA_ARGS__))

// run82-3：子进程死因定谳——在首次拉起前注册官方退出回调，拿到 signal 通道。
static bool g_exit_cb_registered = false;

static void OnChildExit(int32_t pid, int32_t signal)
{
    // run82-3：NCP 官方退出回调——signal 是「退出信号」金标准：
    //   signal=0 → 子进程正常 exit(码 0)；signal=N → 被信号 N 杀（9=SIGKILL，15=SIGTERM，2=INT）。
    //   真机两次死亡时子进程侧无任何出口日志（run_path 仍阻塞），此回调一举定谳死亡通道。
    LOGI("NCP-EXIT pid=%{public}d signal=%{public}d", pid, signal);
}

static void EnsureExitCallbackRegistered(void)
{
    if (!g_exit_cb_registered) {
        auto rc = OH_Ability_RegisterNativeChildProcessExitCallback(OnChildExit);
        LOGI("NCP-EXIT-cb register rc=%{public}d", (int)rc);
        g_exit_cb_registered = (rc == 0);
    }
}

static napi_value StartComfyChild(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    char entryParams[2048] = {0};
    size_t len = 0;
    if (argc >= 1 && argv[0] != nullptr) {
        napi_get_value_string_utf8(env, argv[0], entryParams, sizeof(entryParams) - 1, &len);
    }
    LOGI("startComfyChild entryParams=%{public}s", entryParams);

    EnsureExitCallbackRegistered();  // run82-3：先注册退出回调再拉起子进程

    NativeChildProcess_Args args{};
    args.entryParams = strdup(entryParams);
    NativeChildProcess_FdList fdList{};
    args.fdList = fdList;

    NativeChildProcess_Options opts{};
    opts.isolationMode = NCP_ISOLATION_MODE_NORMAL; // 与 App 共享网络 → 能绑 127.0.0.1 给 ArkWeb

    int32_t pid = 0;
    auto rc = OH_Ability_StartNativeChildProcess("libcomfy_child.so:Main", args, opts, &pid);
    LOGI("OH_Ability_StartNativeChildProcess rc=%{public}d pid=%{public}d", (int)rc, (int)pid);

    // ★ diag 父进程通道：hilog 白名单过滤了第三方 app 的 domain，这里把 rc/pid 一并落地 sandbox
    //   <pyroot>/napi.log（hdc 可读），确认 StartComfyChild 是否被调 + NCP 是否真的拉起子进程。
    //   与 comfy_child.cpp 的 diag.log 通道形成「父→子」全链可观测。
    {
        char pyroot[PATH_MAX] = {0};
        const char *pp = strstr(entryParams, "pyroot=");
        if (pp) {
            pp += strlen("pyroot=");
            size_t n = 0;
            while (pp[n] && pp[n] != '|' && n < sizeof(pyroot) - 1) { pyroot[n] = pp[n]; n++; }
            pyroot[n] = '\0';
        }
        if (pyroot[0]) {
            char nlog[PATH_MAX * 2];
            snprintf(nlog, sizeof(nlog), "%s/napi.log", pyroot);
            FILE *f = fopen(nlog, "a");
            if (f) {
                fprintf(f, "startComfyChild rc=%d pid=%d entryParams=%s\n", (int)rc, (int)pid, entryParams);
                fclose(f);
            }
        }
    }

    napi_value ret;
    napi_create_int32(env, rc == 0 ? pid : 0, &ret);
    return ret;
}

// ── P0 对照探针(2026-09-06, 临时): UIAbility 进程内直接调 NNRt C API 枚举设备 ──
//   判决链: ArkTS(MindSporeLite Kit) count=1 vs NCP 子进程(C API) count=0 →
//   本函数回答「父进程 C API count=?」: 若父进程 C API 也是 0, 则 ArkTS 的 MSLite
//   Kit 走的是与 libneural_network_core.so 不同的系统服务通道(设备只有系统服务可见),
//   第三方进程(父/子)都不行 —— P0 判据 = 「设备列表仅系统服务可见」= 第三方不可用。
// ── P0 执行探针(2026-09-06): UIAbility 进程内跑官方「Add 单算子构图→编译→执行」链 ──
//   dlopen+dlsym 实现(不链接 nnrt 库), 符号缺失返回 API-ABSENT 清单。
//   触发同 npuEnumerate(Index.ets 探针块, startApp 后)。
extern "C" int nnrt_run_add_graph(char *out, size_t outsz, const char *cacheDir);
static napi_value NpuRunAddGraph(napi_env env, napi_callback_info info)
{
    // 参数: cacheDir(App 可写目录, 如 filesDir/nncache); 2026-09-06 定谳 Build 需 SetCache
    size_t argc = 1;
    char cacheDir[512] = {0};
    napi_value argv[1] = {nullptr};
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) == napi_ok &&
        argc >= 1 && argv[0] != nullptr) {
        size_t len = 0;
        napi_get_value_string_utf8(env, argv[0], cacheDir, sizeof(cacheDir) - 1, &len);
    }
    char buf[4096] = {0};
    int rc = nnrt_run_add_graph(buf, sizeof(buf) - 1, cacheDir);
    if (buf[0] == '\0') snprintf(buf, sizeof(buf), "NNRT-ADD-GRAPH: rc=%d (no output)\n", rc);
    napi_value ret;
    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &ret);
    return ret;
}

// ── P0 离线模型执行探针(2026-09-07): .omc → ConstructWithOfflineModelFile → Build → RunSync
//   参数: modelPath(.omc 绝对路径) + cacheDir(filesDir/nncache)
extern "C" int nnrt_run_offline(char *out, size_t outsz, const char *modelPath, const char *cacheDir);
extern "C" int nnrt_run_offline_buffer(char *out, size_t outsz, const void *modelBuf, size_t modelLen,
                                       const char *cacheDir);
static napi_value NpuRunOfflineModelBuffer(napi_env env, napi_callback_info info)
{
    // 参数: ArrayBuffer(.omc 内存) + cacheDir
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    char cacheDir[512] = {0};
    if (argc >= 2 && argv[1] != nullptr) {
        size_t len = 0;
        napi_get_value_string_utf8(env, argv[1], cacheDir, sizeof(cacheDir) - 1, &len);
    }
    void *data = nullptr;
    size_t len = 0;
    if (argc >= 1 && argv[0] != nullptr) {
        napi_typedarray_type tt;
        napi_value ab = nullptr;
        size_t off = 0;
        napi_get_typedarray_info(env, argv[0], &tt, &len, &data, &ab, &off);
    }
    char buf[4096] = {0};
    int rc = nnrt_run_offline_buffer(buf, sizeof(buf) - 1, data, len, cacheDir);
    if (buf[0] == '\0') snprintf(buf, sizeof(buf), "NNRT-OFFLINE-BUF: rc=%d (no output)\n", rc);
    napi_value ret;
    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &ret);
    return ret;
}
static napi_value NpuRunOfflineModel(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    char modelPath[512] = {0};
    char cacheDir[512] = {0};
    napi_value argv[2] = {nullptr, nullptr};
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) == napi_ok) {
        size_t len = 0;
        if (argc >= 1 && argv[0] != nullptr) {
            napi_get_value_string_utf8(env, argv[0], modelPath, sizeof(modelPath) - 1, &len);
        }
        if (argc >= 2 && argv[1] != nullptr) {
            napi_get_value_string_utf8(env, argv[1], cacheDir, sizeof(cacheDir) - 1, &len);
        }
    }
    char buf[4096] = {0};
    int rc = nnrt_run_offline(buf, sizeof(buf) - 1, modelPath, cacheDir);
    if (buf[0] == '\0') snprintf(buf, sizeof(buf), "NNRT-OFFLINE: rc=%d (no output)\n", rc);
    napi_value ret;
    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &ret);
    return ret;
}

static napi_value NpuEnumerate(napi_env env, napi_callback_info info)
{
    char buf[512] = "npuEnum: FAIL\n";
    void *hRt = dlopen("libneural_network_runtime.so", RTLD_NOW | RTLD_GLOBAL);
    void *hCore = dlopen("libneural_network_core.so", RTLD_NOW | RTLD_GLOBAL);
    if (hRt && hCore) {
        using F0 = OH_NN_ReturnCode (*)(const size_t **, uint32_t *);
        using F1 = OH_NN_ReturnCode (*)(size_t, const char **);
        using F2 = OH_NN_ReturnCode (*)(size_t, OH_NN_DeviceType *);
        auto f0 = (F0)dlsym(hRt, "OH_NNDevice_GetAllDevicesID");
        auto f1 = (F1)dlsym(hCore, "OH_NNDevice_GetName");
        auto f2 = (F2)dlsym(hCore, "OH_NNDevice_GetType");
        if (f0 && f1 && f2) {
            const size_t *ids = nullptr;
            uint32_t n = 0;
            OH_NN_ReturnCode rc = f0(&ids, &n);
            snprintf(buf, sizeof(buf), "npuEnum: rc=%d count=%u", (int)rc, n);
            for (uint32_t i = 0; i < n && i < 8; i++) {
                const char *nm = nullptr;
                OH_NN_DeviceType tp = (OH_NN_DeviceType)-1;
                f1(ids[i], &nm);
                f2(ids[i], &tp);
                char line[128];
                snprintf(line, sizeof(line), "; [%u] id=%zu name=%s type=%d", i, ids[i],
                         nm ? nm : "?", (int)tp);
                strncat(buf + strlen(buf), line, sizeof(buf) - strlen(buf) - 1);
            }
        } else {
            snprintf(buf, sizeof(buf), "npuEnum: dlsym fail");
        }
    }
    napi_value ret;
    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &ret);
    return ret;
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        {"startComfyChild", nullptr, StartComfyChild, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"npuEnumerate", nullptr, NpuEnumerate, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"npuRunAddGraph", nullptr, NpuRunAddGraph, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"npuRunOfflineModel", nullptr, NpuRunOfflineModel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"npuRunOfflineModelBuffer", nullptr, NpuRunOfflineModelBuffer, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module comfy_module = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "entry",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterEntryModule(void)
{
    napi_module_register(&comfy_module);
}
