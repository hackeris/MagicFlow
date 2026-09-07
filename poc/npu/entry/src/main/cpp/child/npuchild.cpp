// npuchild.cpp — poc/npu CH-06: 官方 NCP native 子进程内的 NNRt 设备枚举。
//
// 回答: 「原生子进程(无 Ability 上下文)能否看到 NNRt 设备?」
// 背景: 主项目 comfyui NCP 子进程已知 count=0(P1a 源码级判定: HDI proxy 获取资格按进程身份);
//        本探针在独立 demo 里复现同一形态(官方 StartNativeChildProcess 拉起的纯 native 进程),
//        使「PyTorch 推理形态(子进程)不可用 NNRt」成为可独立复现的结论。
// 结果: 写入父进程传入的 resultPath(NCP NORMAL isolation 与 App 同 uid, 可写 App sandbox)。
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>
#include <AbilityKit/native_child_process.h>
#include "neural_network_runtime/neural_network_runtime.h"

extern "C" void Main(NativeChildProcess_Args args)
{
    const char *resultPath = (args.entryParams && args.entryParams[0]) ? args.entryParams : "/data/local/tmp/npuchild-result.txt";
    char out[2048];
    snprintf(out, sizeof(out), "npuchild: pid=%d start\n", (int)getpid());

    void *rt = dlopen("libneural_network_runtime.so", RTLD_NOW | RTLD_GLOBAL);
    void *core = dlopen("libneural_network_core.so", RTLD_NOW | RTLD_GLOBAL);
    if (!rt || !core) {
        snprintf(out, sizeof(out), "npuchild: dlopen fail rt=%p core=%p\n", rt, core);
    } else {
        using FDev = OH_NN_ReturnCode (*)(const size_t **, uint32_t *);
        using FName = OH_NN_ReturnCode (*)(size_t, const char **);
        using FType = OH_NN_ReturnCode (*)(size_t, OH_NN_DeviceType *);
        FDev fDev = (FDev)dlsym(core, "OH_NNDevice_GetAllDevicesID");
        FName fName = (FName)dlsym(core, "OH_NNDevice_GetName");
        FType fType = (FType)dlsym(core, "OH_NNDevice_GetType");
        if (!fDev) {
            snprintf(out, sizeof(out), "npuchild: dlsym GetAllDevicesID fail\n");
        } else {
            const size_t *ids = nullptr;
            uint32_t n = 0;
            int rc = fDev(&ids, &n);
            char line[512];
            snprintf(out, sizeof(out), "npuchild: dev-rc=%d count=%u\n", rc, (unsigned)n);
            for (uint32_t i = 0; i < n && i < 4; i++) {
                const char *nm = nullptr;
                if (fName && fType) {
                    OH_NN_DeviceType tp = (OH_NN_DeviceType)-1;
                    fName(ids[i], &nm);
                    fType(ids[i], &tp);
                    snprintf(line, sizeof(line), "  [%u] name=%s type=%d\n", i, nm ? nm : "?", (int)tp);
                } else {
                    snprintf(line, sizeof(line), "  [%u] name=?\n", i);
                }
                strncat(out, line, sizeof(out) - strlen(out) - 1);
            }
        }
    }
    // 进程内已注册的 HDF proxy 通道模拟(结果文件中注明判定)
    strncat(out, "\nnpuchild: if count==0 => native child cannot access NNRt (P1a repro)\n",
            sizeof(out) - strlen(out) - 1);

    // 写结果文件(尽力; 失败则把结果也写 stderr, 由 hilog 通道抓捕)
    FILE *f = fopen(resultPath, "w");
    if (f) {
        fwrite(out, 1, strlen(out), f);
        fclose(f);
    } else {
        fprintf(stderr, "npuchild result file open failed(%s); inline:</op>\n%.*s\n",
                resultPath, (int)strlen(out), out);
    }
    _exit(0);
}
