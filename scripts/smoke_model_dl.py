#!/usr/bin/env python3
"""smoke_model_dl.py —— patch 16(OHOS 模型下载)逻辑冒烟, 零第三方依赖。

为何不 import 全 server.py: 它拖 torch/nodes 全家; 本脚本用正则从 server.py 源码
提取 OHOS 段(标记 "# OHOS_MODEL_DL v1" 起至 "def create_origin_only_middleware":)
+ stub folder_paths 后 exec, 单测其模块级函数与 OHOSModelDownloader 状态机/落盘。
端到端(HTTP 层)由真机验证矩阵 Q2 覆盖, 勿在宿主等 torch 环境重复。

用法: python3 scripts/smoke_model_dl.py   (退出码 0 = 全绿)
"""
import asyncio
import os
import re
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRV_SRC = os.path.join(ROOT, "externals/comfyui-src/server.py")

FAILURES = []


def check(name, cond, detail=""):
    tag = "OK " if cond else "FAIL"
    print(f"  [{tag}] {name}" + ("" if not detail else f" — {detail}"))
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


class FakeResp:
    def __init__(self, data, status=200, headers=None, chunk_delay=0.0, chunk_n=64 * 1024):
        self._data = data
        self.status = status
        self.headers = {"Content-Length": str(len(data))} if headers is None else headers
        self._cd = chunk_delay
        self._cn = chunk_n
    async def __aenter__(self):
        return self
    async def __aexit__(self, *a):
        return False
    async def iter_chunked(self, n):
        n = min(n, self._cn)
        for i in range(0, len(self._data), n):
            if self._cd:
                await asyncio.sleep(self._cd)
            yield self._data[i:i + n]
    # run() 锚点: resp.content.iter_chunked(...)
    content = None
    def _attach_content(self):
        self.content = self
        return self


class FakeSession:
    def __init__(self, data, status=200, headers=None, chunk_delay=0.0, chunk_n=64 * 1024):
        self._args = (data, status, headers, chunk_delay, chunk_n)
    def get(self, url, **kw):
        return FakeResp(*self._args)._attach_content()


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

    print("== new_task ==")
    os.makedirs(dest, exist_ok=True)
    stale = os.path.join(dest, ".dead.ohosdl-dead000.part")
    open(stale, "wb").close()
    td = new_task("https://huggingface.co/a/z.safetensors", "checkpoints", "z.safetensors")
    check("part 命名", td["part"].endswith(".part") and ".ohosdl-" in td["part"])
    check("陈旧 part 清理", not os.path.exists(stale))

    print("== run: happy ==")
    data = os.urandom(300 * 1024)
    session = FakeSession(data)
    td2 = new_task("http://127.0.0.1:18000/z.safetensors", "checkpoints", "z.safetensors")
    await run(session, td2)
    check("status completed", td2["status"] == "completed", td2["status"])
    check("落盘内容一致", os.path.exists(td2["dest"]) and
          open(td2["dest"], "rb").read() == data)
    check("part 无残留", not os.path.exists(td2["part"]))
    check("bytes 上报", td2["bytes_received"] == len(data))

    print("== run: sha 不匹配 ==")
    session2 = FakeSession(data)
    td3 = new_task("http://127.0.0.1:18000/z.safetensors", "checkpoints", "z2.safetensors",
                   sha256="0" * 64)
    await run(session2, td3)
    check("status error", td3["status"] == "error", td3["status"])
    check("sha 错误码", td3["error"]["code"] == "SHA_MISMATCH", str(td3["error"]))
    check("error 时 part 清除", not os.path.exists(td3["part"]))

    print("== run: cancelled ==")
    session3 = FakeSession(os.urandom(64 * 1024 * 4), chunk_delay=0.01, chunk_n=1024)
    td4 = new_task("http://127.0.0.1:18000/z.safetensors", "checkpoints", "z3.safetensors")
    task = asyncio.get_event_loop().create_task(run(session3, td4))
    await asyncio.sleep(0.05)
    td4["status"] = "cancelled"
    await task
    check("cancelled 终态", td4["status"] == "cancelled", td4["status"])
    check("cancelled part 清除", not os.path.exists(td4["part"]))

    print("== run: paused 后恢复 ==")
    session4 = FakeSession(os.urandom(256 * 1024), chunk_delay=0.02, chunk_n=2048)
    td5 = new_task("http://127.0.0.1:18000/z.safetensors", "checkpoints", "z4.safetensors")
    waiter = asyncio.get_event_loop().create_task(run(session4, td5))
    await asyncio.sleep(0.05)
    td5["status"] = "paused"
    await asyncio.sleep(0.1)
    before = td5["bytes_received"]
    await asyncio.sleep(0.1)
    check("paused 停读", td5["bytes_received"] == before, f"{before}->{td5['bytes_received']}")
    td5["status"] = "in_progress"
    await waiter
    check("resume 后 completed", td5["status"] == "completed", td5["status"])

    print()
    if FAILURES:
        print(f"FAILED: {len(FAILURES)} 项 — {FAILURES}")
        return 1
    print("ALL GREEN: patch 16 逻辑冒烟通过")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
