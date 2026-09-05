#!/usr/bin/env python3
# 流式转换 SD-Turbo checkpoint fp32 -> fp16(safetensors)。
# 逐 tensor 读(仅一次转), 内存 O(单 tensor)。dtype 保持 metadata 原值。
import sys, time
import numpy as np
from safetensors import safe_open
from safetensors.numpy import save_file as save_np

def convert(src, dst):
    t0 = time.time()
    f = safe_open(src, framework="numpy", device="cpu")
    keys = list(f.keys())
    print(f"tensors={len(keys)}")
    out = {}
    n = 0
    for k in keys:
        arr = f.get_tensor(k)
        if arr.dtype == np.float32:
            arr = arr.astype(np.float16)
        out[k] = arr
        n += 1
        if n % 200 == 0:
            print(f"  ...{n}/{len(keys)}")
    save_np(out, dst)
    print(f"done in {time.time()-t0:.0f}s")
    # 快验
    f2 = safe_open(dst, framework="numpy", device="cpu")
    ks = list(f2.keys())
    print(f"verify tensors={len(ks)}")

if __name__ == "__main__":
    convert(sys.argv[1], sys.argv[2])
