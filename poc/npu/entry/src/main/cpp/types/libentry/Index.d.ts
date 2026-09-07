// poc/npu NAPI 类型声明 —— 与 napi_init.cpp 的导出清单一一对应。
export const npuEnum: () => string;
export const npuOpsMatrix: (cacheDir: string) => string;
export const npuPerfRepeat: (cacheDir: string) => string;
export const npuOffline: (model: ArrayBuffer, cacheDir: string) => string;
export const npuSingleOpProbe: () => string;
export const npuStartChild: (resultPath: string) => number;
