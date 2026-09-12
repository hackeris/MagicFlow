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

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        {"startComfyChild", nullptr, StartComfyChild, nullptr, nullptr, nullptr, napi_default, nullptr},
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
