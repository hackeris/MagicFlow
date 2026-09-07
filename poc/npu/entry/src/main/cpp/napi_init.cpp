// napi_init.cpp — poc/npu 父进程(UIAbility)入口。
// 暴露 6 个 NAPI 函数(每个对应一个探针通道, 均由 ArkTS Index.ets 调用并汇总报告):
//   npuEnum          → CH-01 设备枚举
//   npuOpsMatrix     → CH-02 SD 链 33 算子覆盖矩阵
//   npuPerfRepeat    → CH-03 Add 五轮时序(缓存复用)
//   npuOffline       → CH-04 离线 .omc buffer(Build 预期 rc=1)
//   npuSingleOpProbe → CH-05 单算子直调候选库存在性(不调用未知签名)
//   npuStartChild    → CH-06 NCP 子进程枚举(结果写子进程文件)
// 每个通道独立调用、独立缓冲 —— 单一通道崩溃不影响其他通道(探测用独立 NAPI 调用)。
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <dlfcn.h>
#include <hilog/log.h>
#include <AbilityKit/native_child_process.h>
#include "napi/native_api.h"

#undef LOG_DOMAIN
#define LOG_DOMAIN 0xD001
#undef LOG_TAG
#define LOG_TAG "NpuPocNapi"

#define LOGI(...) ((void)OH_LOG_INFO(LOG_APP, __VA_ARGS__))

extern "C" int nnrt_ch_enum(char *out, size_t outsz);
extern "C" int nnrt_ch_ops(char *out, size_t outsz, const char *cacheDir);
extern "C" int nnrt_ch_perf(char *out, size_t outsz, const char *cacheDir);
extern "C" int nnrt_ch_offline(char *out, size_t outsz, const void *modelBuf, size_t modelLen,
                               const char *cacheDir);
extern "C" int nnrt_ch_singleop(char *out, size_t outsz);

static napi_value MakeString(napi_env env, const char *s)
{
    napi_value ret;
    napi_create_string_utf8(env, s ? s : "", NAPI_AUTO_LENGTH, &ret);
    return ret;
}

static napi_value MakeInt(napi_env env, int v)
{
    napi_value ret;
    napi_create_int32(env, v, &ret);
    return ret;
}

// 取第 i 参的字符串(缺省返回 defaults)
static void GetStrArg(napi_env env, napi_callback_info info, size_t idx, char *buf, size_t bufsz,
                      const char *defaults)
{
    size_t argc = idx + 1;
    napi_value argv[8] = {nullptr};
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) == napi_ok && argc > idx && argv[idx] != nullptr) {
        size_t len = 0;
        napi_get_value_string_utf8(env, argv[idx], buf, bufsz - 1, &len);
        buf[bufsz - 1] = '\0';
        return;
    }
    snprintf(buf, bufsz, "%s", defaults ? defaults : "");
}

static napi_value NpuEnum(napi_env env, napi_callback_info info)
{
    char buf[4096];
    nnrt_ch_enum(buf, sizeof(buf));
    return MakeString(env, buf);
}

static napi_value NpuOpsMatrix(napi_env env, napi_callback_info info)
{
    char cacheDir[512];
    GetStrArg(env, info, 0, cacheDir, sizeof(cacheDir), ".");
    char buf[16384];
    nnrt_ch_ops(buf, sizeof(buf), cacheDir);
    return MakeString(env, buf);
}

static napi_value NpuPerfRepeat(napi_env env, napi_callback_info info)
{
    char cacheDir[512];
    GetStrArg(env, info, 0, cacheDir, sizeof(cacheDir), ".");
    char buf[4096];
    nnrt_ch_perf(buf, sizeof(buf), cacheDir);
    return MakeString(env, buf);
}

static napi_value NpuOffline(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value argv[2] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const uint8_t *buf = nullptr;
    size_t len = 0;
    char cacheDir[512] = ".";
    if (argc >= 1 && argv[0] != nullptr) {
        bool isArray = false;
        napi_is_arraybuffer(env, argv[0], &isArray);
        if (isArray) {
            void *data = nullptr;
            napi_get_arraybuffer_info(env, argv[0], &data, &len);
            buf = (const uint8_t *)data;
        } else {
            // 失败 = 无法取模型数据
            return MakeString(env, "[CH-04] arg0 not ArrayBuffer\n");
        }
    }
    if (argc >= 2 && argv[1] != nullptr) {
        size_t l = 0;
        napi_get_value_string_utf8(env, argv[1], cacheDir, sizeof(cacheDir) - 1, &l);
    }
    char out[4096];
    nnrt_ch_offline(out, sizeof(out), buf ? buf : (const uint8_t *)"", len, cacheDir);
    return MakeString(env, out);
}

static napi_value NpuSingleOpProbe(napi_env env, napi_callback_info info)
{
    char buf[4096];
    nnrt_ch_singleop(buf, sizeof(buf));
    return MakeString(env, buf);
}

// CH-06: 拉起 native 子进程(libnpuchild.so:Main), exeName 传结果文件路径, 返回 pid
static napi_value NpuStartChild(napi_env env, napi_callback_info info)
{
    char resultPath[1024] = "";
    GetStrArg(env, info, 0, resultPath, sizeof(resultPath), "");

    NativeChildProcess_Args args{};
    args.entryParams = strdup(resultPath);
    NativeChildProcess_FdList fdList{};
    args.fdList = fdList;
    NativeChildProcess_Options opts{};
    opts.isolationMode = NCP_ISOLATION_MODE_NORMAL;

    int32_t pid = 0;
    auto rc = OH_Ability_StartNativeChildProcess("libnpuchild.so:Main", args, opts, &pid);
    LOGI("npuchild start rc=%d pid=%d", (int)rc, (int)pid);
    return MakeInt(env, rc == 0 ? pid : -1);
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        {"npuEnum", nullptr, NpuEnum, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"npuOpsMatrix", nullptr, NpuOpsMatrix, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"npuPerfRepeat", nullptr, NpuPerfRepeat, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"npuOffline", nullptr, NpuOffline, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"npuSingleOpProbe", nullptr, NpuSingleOpProbe, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"npuStartChild", nullptr, NpuStartChild, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module npupoc_module = {
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
    napi_module_register(&npupoc_module);
}
