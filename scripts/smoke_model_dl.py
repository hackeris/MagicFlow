#!/usr/bin/env python3
"""smoke_model_dl.py —— patch 16/18(OHOS 模型下载/区域化)逻辑冒烟, 零第三方依赖。

为何不 import 全 server.py: 它拖 torch/nodes 全家; 本脚本用正则从 server.py 源码
提取 OHOS 段(标记 "# OHOS_MODEL_DL v1" 起至 "def create_origin_only_middleware":)
+ stub folder_paths 后 exec, 单测其模块级函数与 OHOSModelDownloader 状态机/落盘。
v2 引擎(2026-09-05): 下载 = stdlib urllib 同步流式(executor 线程), 故 run 用例
在本进程起一个 thread 版 HTTP 静态服务器(http.server)供 URL; 白名单放行 127.0.0.1。
端到端(HTTP 层)由真机验证矩阵 Q2 覆盖, 勿在宿主等 torch 环境重复。

用法: python3 scripts/smoke_model_dl.py   (退出码 0 = 全绿)
"""
import asyncio
import os
import re
import sys
import tempfile
import threading
import http.server

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRV_SRC = os.path.join(ROOT, "externals/comfyui-src/server.py")

FAILURES = []


def check(name, cond, detail=""):
    tag = "OK " if cond else "FAIL"
    print(f"  [{tag}] {name}" + ("" if not cond else f" — {detail}"))
    if not cond:
        FAILURES.append(name)


def extract_ns():
    src = open(SRV_SRC, encoding="utf-8").read()
    start = src.index("# OHOS_MODEL_DL v1")
    end = src.index("def create_origin_only_middleware():")
    block = src[start:end]
    tmp = tempfile.mkdtemp(prefix="smoke-dl-")
    fpaths = {
        "checkpoints": ([os.path.join(tmp, "checkpoints")], {".safetensors"}),
        "vae": ([os.path.join(tmp, "vae")], {".safetensors"}),
        "diffusion_models": ([os.path.join(tmp, "diffusion_models")], {".safetensors"}),
        # 2026-09-11 patch 22(catalog 扩充): 新条目目录 —— 与真实 folder_paths 对齐
        #   (controlnet/loras/upscale_models 均在 core folder_paths.py 中)
        "upscale_models": ([os.path.join(tmp, "upscale_models")], {".pth", ".safetensors"}),
        "controlnet": ([os.path.join(tmp, "controlnet")], {".pth", ".safetensors"}),
        "loras": ([os.path.join(tmp, "loras")], {".safetensors"}),
    }
    class FakeFolderPaths:
        folder_names_and_paths = fpaths
        @staticmethod
        def is_within_directory(base, name):
            base_r = os.path.realpath(base)
            return os.path.realpath(name).startswith(base_r + os.sep)
    class _AiohttpStub:  # run() 仅引用 aiohttp.ClientError(异常类), stub 免安装
        class ClientError(Exception):
            pass
    ns = {"folder_paths": FakeFolderPaths(), "shutil": __import__("shutil"),
          "hashlib": __import__("hashlib"), "os": os, "uuid": __import__("uuid"),
          "asyncio": asyncio, "time": __import__("time"), "urllib": __import__("urllib"),
          "socket": __import__("socket"), "errno": __import__("errno"),
          "urllib": __import__("urllib.parse"), "aiohttp": _AiohttpStub}
    exec(block, ns)
    return ns, tmp


# ── v2 实测用: 线程内启动 HTTP 静态服务器(内容/chunk 行为按路径表) ──────────────
def start_static_server(routes):
    """routes: {path: (data: bytes, chunk_delay_s: float, chunk_size: int)} → (port, server)"""
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            route = routes.get(self.path)
            if route is None:
                self.send_response(404)
                self.end_headers()
                return
            data, delay, chunk = route
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            i = 0
            while i < len(data):
                part = data[i:i + chunk]
                try:
                    self.wfile.write(part)
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    break
                i += len(part)
                if delay:
                    import time as _t
                    _t.sleep(delay)

        def log_message(self, *a):
            pass

    srv = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv.server_address[1], srv


async def main():
    ns, tmp = extract_ns()
    v_url, v_name = ns["_ohos_dl_validate_url"], ns["_ohos_dl_validate_filename"]
    new_task = ns["OHOSModelDownloader"].new_task
    run = ns["OHOSModelDownloader"].run
    catalog = ns["_ohos_dl_catalog"]()
    print("== 校验函数 ==")
    try:
        v_url("https://huggingface.co/a/b.safetensors"); check("url hf 通过", True)
    except Exception:
        check("url hf 通过", False)
    # 2026-09-08 P0(区域化): 镜像域名入白名单
    for u in ("https://modelscope.cn/x/y.safetensors", "https://hf-mirror.com/x/y.safetensors"):
        try:
            v_url(u); check(f"url 镜像通过 {u.split('/')[2]}", True)
        except Exception:
            check(f"url 镜像通过 {u.split('/')[2]}", False)
    for bad in ("ftp://hf.co/x.safetensors", "https://evil.com/x.safetensors"):
        try:
            v_url(bad); check(f"url 拒 {bad}", False)
        except ns["_OhosDlError"]:
            check(f"url 拒 {bad}", True)
    try:
        v_url("http://127.0.0.1:18000/sd_turbo.safetensors"); check("url 回环通过", True)
    except Exception:
        check("url 回环通过", False)
    dest = ns["folder_paths"].folder_names_and_paths["checkpoints"][0][0]
    try:
        v_name("x.safetensors", dest); check("name 正常", True)
    except Exception:
        check("name 正常", False)
    for bad in ("../x.safetensors", "x.exe", "a/" * 60 + "x.safetensors"):
        try:
            v_name(bad, dest); check(f"name 拒 {bad[:20]}", False)
        except ns["_OhosDlError"]:
            check(f"name 拒 {bad[:20]}", True)

    print("== catalog ==")
    check("catalog 非空", len(catalog) > 0)
    check("catalog 含 sd-turbo required", any(
        e["id"] == "sd-turbo" and e["tier"] == "required" for e in catalog))
    # 2026-09-08 P0: 主源国产镜像 + 预量化变体条目
    sd_turbo = next((e for e in catalog if e["id"] == "sd-turbo"), None)
    # 2026-09-10 P0.3: fp32/fp16 档位修正后 主源=hf-mirror(fp16 2.6G), MS(fp32 5.2G)=url_alt
    check("sd-turbo 主源=hf-mirror(fp16)",
          bool(sd_turbo) and sd_turbo["url"].startswith("https://hf-mirror.com"))
    check("sd-turbo url_alt=MS(fp32 大内存档)", bool(sd_turbo) and
          str(sd_turbo.get("url_alt", "")).startswith("https://modelscope.cn"))
    check("sd-turbo size 锚(2.6G)", bool(sd_turbo) and sd_turbo["size_bytes"] > 0)
    esr = next((e for e in catalog if e["id"] == "realesrgan-x4plus"), None)
    check("realesrgan 主源=hf-mirror",
          bool(esr) and esr["url"].startswith("https://hf-mirror.com"))
    check("catalog 含预量化变体 flux1-dev-fp8", any(
        e["id"] == "flux1-dev-fp8" and e["size_bytes"] > 0 and
        e["url"].startswith("https://modelscope.cn") for e in catalog))
    # 2026-09-11 P0.4(catalog 扩充, patch 22): 用户拍板「<12GB 条目可试」——
    #   7 条新增(SDXL/LCM-LoRA×2/ControlNet v1.1×3/vae)必须齐全、size 锚 >0 且 <12GiB、
    #   主源为国内镜像(MS 优先 / hf-mirror 备)。
    exp_ids = {"sdxl-base", "sdxl-vae", "controlnet-canny-sd15", "controlnet-depth-sd15",
               "controlnet-openpose-sd15", "lcm-lora-sd15", "lcm-lora-sdxl"}
    got = {e["id"]: e for e in catalog if e["id"] in exp_ids}
    check("catalog 扩充 7 条齐全(patch 22)", set(got) == exp_ids)
    check("扩充条目 size 锚全 >0 且 <12GiB", got and all(
        0 < e["size_bytes"] < 12 * 1024**3 for e in got.values()))
    check("扩充条目主源=国内镜像", got and all(
        e["url"].startswith(("https://modelscope.cn", "https://hf-mirror.com"))
        for e in got.values()))
    # 目录合法性: catalog 每条 directory 必须在 folder_paths 中(downloader 同判据, 否则 400)
    fpset = set(ns["folder_paths"].folder_names_and_paths)
    bad_dir = sorted({e["directory"] for e in catalog if e["directory"] not in fpset})
    check("catalog 目录全部合法(folder_paths)", not bad_dir)

    print("== new_task ==")
    os.makedirs(dest, exist_ok=True)
    stale = os.path.join(dest, ".dead.ohosdl-dead000.part")
    open(stale, "wb").close()
    td = new_task("https://huggingface.co/a/z.safetensors", "checkpoints", "z.safetensors")
    check("part 命名", td["part"].endswith(".part") and ".ohosdl-" in td["part"])
    check("陈旧 part 清理", not os.path.exists(stale))

    # v2 实例服务器: routes 各用例独立数据
    happy_data = os.urandom(300 * 1024)
    port, srv = start_static_server({
        "/happy": (happy_data, 0.0, 64 * 1024),
        "/sha": (happy_data, 0.0, 64 * 1024),
        "/slow": (os.urandom(1024 * 1024), 0.03, 32 * 1024),
        "/pause": (os.urandom(1024 * 1024), 0.02, 16 * 1024),
    })
    base = f"http://127.0.0.1:{port}"
    session = object()  # v2 run() 仅判非 None(BUSY), 真IO 走 urllib

    print("== run: happy ==")
    td2 = new_task(f"{base}/happy", "checkpoints", "z.safetensors")
    await run(session, td2)
    check("status completed", td2["status"] == "completed", td2["status"])
    check("落盘内容一致", os.path.exists(td2["dest"]) and
          open(td2["dest"], "rb").read() == happy_data)
    check("part 无残留", not os.path.exists(td2["part"]))
    check("bytes 上报", td2["bytes_received"] == len(happy_data))

    print("== run: sha 不匹配 ==")
    td3 = new_task(f"{base}/sha", "checkpoints", "z2.safetensors", sha256="0" * 64)
    await run(session, td3)
    check("status error", td3["status"] == "error", td3["status"])
    check("sha 错误码", td3["error"]["code"] == "SHA_MISMATCH", str(td3["error"]))
    check("error 时 part 清除", not os.path.exists(td3["part"]))

    print("== run: cancelled ==")
    td4 = new_task(f"{base}/slow", "checkpoints", "z3.safetensors")
    task = asyncio.get_event_loop().create_task(run(session, td4))
    await asyncio.sleep(0.05)
    td4["status"] = "cancelled"
    await task
    check("cancelled 终态", td4["status"] == "cancelled", td4["status"])
    check("cancelled part 清除", not os.path.exists(td4["part"]))

    print("== run: paused 后恢复 ==")
    td5 = new_task(f"{base}/pause", "checkpoints", "z4.safetensors")
    waiter = asyncio.get_event_loop().create_task(run(session, td5))
    await asyncio.sleep(0.2)
    td5["status"] = "paused"
    await asyncio.sleep(0.35)
    before = td5["bytes_received"]
    await asyncio.sleep(0.35)
    check("paused 停读", td5["bytes_received"] == before, f"{before}->{td5['bytes_received']}")
    td5["status"] = "in_progress"
    await waiter
    check("resume 后 completed", td5["status"] == "completed", td5["status"])
    srv.shutdown()

    print()
    if FAILURES:
        print(f"FAILED: {len(FAILURES)} 项 — {FAILURES}")
        return 1
    print("ALL GREEN: patch 16/18 逻辑冒烟通过")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
