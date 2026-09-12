#!/usr/bin/env python3
"""smoke_external_api.py —— patch 23(外部 API 节点组)宿主逻辑冒烟, 零第三方依赖。

覆盖(H0/H1): api_client 的模板替换 / JSON 路径提取 / 密钥读取 / 错误面,
以及真实 HTTP 往返 —— 打本进程起的 mock server(OpenAI 兼容的 chat 与 images 两个
POST 端点 + 404 / 非 JSON / 超时三条错误路径)。
不覆盖(留给真机): 真实 torch 张量转换(宿主无 torch, 此处用 stub 验形状与归一化,
真值由 Q2 覆盖); 节点类的 INPUT_TYPES/RETURN_TYPES(由 Q0 的 /object_info 覆盖)。

用法: python3 scripts/smoke_external_api.py   (退出码 0 = 全绿)
"""
import base64
import http.server
import importlib.util
import io
import json
import os
import sys
import tempfile
import threading
import time
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODULE = os.path.join(
    ROOT, "externals/comfyui-src/custom_nodes/ohos_external_api/api_client.py")

FAILURES = []

# mock server 的路由表与最近一次请求体, 由用例改写
ROUTES = {}
LAST_BODY = []


def check(name, cond, detail=""):
    tag = "OK " if cond else "FAIL"
    print(f"  [{tag}] {name}" + (f" — {detail}" if cond and detail else ""))
    if not cond:
        FAILURES.append(name)


def load_client():
    spec = importlib.util.spec_from_file_location("ohos_api_client", MODULE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class _Handler(http.server.BaseHTTPRequestHandler):
    def _dispatch(self, method):
        route = ROUTES.get((method, self.path))
        if route is None:
            body = b'{"error": "not found"}'
            self.send_response(404)
        else:
            status, payload, delay = route
            if delay:
                time.sleep(delay)
            body = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
            self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass  # 客户端主动中止(如「超限响应应中止」用例)属预期

    def do_GET(self):
        self._dispatch("GET")

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        LAST_BODY.append(self.rfile.read(n) if n else b"")
        self._dispatch("POST")

    def log_message(self, *a):
        pass


def start_server():
    srv = http.server.HTTPServer(("127.0.0.1", 0), _Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv.server_address[1], srv


def one_px_png_b64():
    from PIL import Image
    buf = io.BytesIO()
    Image.new("RGB", (1, 1), (255, 0, 0)).save(buf, format="PNG")
    return base64.b64encode(buf.getvalue()).decode()


def main():
    m = load_client()
    ok = m.ApiError

    print("== [1] 模板渲染 ==")
    check("字符串占位符按 JSON 转义",
          m.render_template('{"p": {{prompt}}}', {"prompt": 'a"b'}) == '{"p": "a\\"b"}',
          m.render_template('{"p": {{prompt}}}', {"prompt": 'a"b'}))
    check("数字/布尔按字面量填入",
          m.render_template("[{{n}}, {{b}}]", {"n": 3, "b": True}) == "[3, true]")
    check("未提供的占位符原样保留",
          m.render_template("{{known}} {{unknown}}", {"known": 1}) == "1 {{unknown}}")
    check("不做二次展开",
          m.render_template("{{a}}", {"a": "{{b}}", "b": "x"}) == '"{{b}}"')
    check("占位符清单可枚举",
          m.template_placeholders("{{a}} {{ b }} {{a}}") == ["a", "b"])

    print("== [1b] 节点默认模板: 渲染后必须是合法 JSON ==")
    #  2026-09-12 真机 mock 截获的实况: 默认 body 模板把 {{prompt}} 写在引号里, 渲染出
    #  `"content": ""你好，云""`(双引号嵌套) —— 打真实 API 必被 400。守卫: 取出**节点模块
    #  的默认模板真身**渲染后 json.loads 必须成功(此处直接读 nodes.py 的值, 不是副本)。
    import importlib
    pkg_parent = os.path.dirname(os.path.dirname(MODULE))  # .../custom_nodes
    if pkg_parent not in sys.path:
        sys.path.insert(0, pkg_parent)
    nodes_mod = importlib.import_module("ohos_external_api.nodes")
    #  vals 必须与 _template_values() 实际提供的键一致({{auth_header}} 由它组装),
    #  否则占位符残留 → 误判模板有问题。
    vals = {"prompt": '你好"x', "width": 512, "height": 512,
            "api_key": "sk-abc1234567", "auth_header": "Bearer sk-abc1234567",
            "seed": 1, "temperature": 0.7}
    for nm in ("_BODY_TEXT_DEFAULT", "_BODY_IMAGE_DEFAULT", "_HEADERS_DEFAULT"):
        tpl = getattr(nodes_mod, nm)
        try:
            rendered = m.render_template(tpl, vals)
            json.loads(rendered)
            check(f"{nm} 渲染后合法 JSON", True, rendered.replace("\n", " ")[:64])
        except Exception as e:
            check(f"{nm} 渲染后合法 JSON", False,
                  f"{type(e).__name__}: {e} | 模板={tpl[:60]!r}")

    print("== [2] JSON 路径提取 ==")
    doc = {"data": [{"b64_json": "AAA"}, {"b64_json": "BBB"}], "n": 7}
    check("data[0].b64_json", m.extract_path(doc, "data[0].b64_json") == "AAA")
    check("data[1].b64_json", m.extract_path(doc, "data[1].b64_json") == "BBB")
    check("简单字段 n", m.extract_path(doc, "n") == 7)
    check("空路径返回原值", m.extract_path(doc, "") is doc)
    try:
        m.extract_path(doc, "data[9].nope")
        check("越界应抛 ApiError", False)
    except ok as e:
        check("越界应抛 ApiError", True, str(e)[:40])

    print("== [3] 密钥读取 ==")
    tmp = tempfile.mkdtemp(prefix="smoke-api-")
    m.comfy_root = lambda: tmp  # 重定向根目录, 不动真实设备路径
    check("密钥文件缺失时返回空表", m.load_api_keys() == {})
    with open(os.path.join(tmp, "api_keys.json"), "w", encoding="utf-8") as f:
        json.dump({"siliconflow": "sk-abcdefghijklmnop"}, f)
    check("读到已配置密钥", m.resolve_api_key("siliconflow") == "sk-abcdefghijklmnop")
    check("(none) 返回 None", m.resolve_api_key("(none)") is None)
    check("未配置的 provider 返回 None", m.resolve_api_key("dashscope") is None)
    with open(os.path.join(tmp, "api_keys.json"), "w", encoding="utf-8") as f:
        f.write("{ 坏 json")
    check("密钥文件损坏时降级为空表", m.load_api_keys() == {})

    print("== [4] HTTP 往返(mock server) ==")
    port, srv = start_server()
    base = f"http://127.0.0.1:{port}"
    try:
        ROUTES.update({
            ("POST", "/v1/chat/completions"): (
                200, {"choices": [{"message": {"content": "你好, 云!"}}]}, 0),
            ("POST", "/v1/images/generations"): (
                200, {"data": [{"b64_json": one_px_png_b64()}]}, 0),
            ("GET", "/v1/json"): (200, {"a": {"b": [{"c": 42}]}}, 0),
            ("POST", "/v1/notjson"): (200, b"<html>not json</html>", 0),
            ("POST", "/v1/slow"): (200, {"ok": True}, 1.5),
        })

        raw = m.http_request("POST", f"{base}/v1/chat/completions",
                             headers={"Content-Type": "application/json"},
                             body=b'{"model":"x"}', timeout=10)
        check("POST chat 返回可解析 JSON",
              json.loads(raw)["choices"][0]["message"]["content"] == "你好, 云!")

        body_sent = b'{"model":"m","messages":[{"role":"user","content":"hi"}]}'
        LAST_BODY.clear()
        m.http_request("POST", f"{base}/v1/chat/completions", body=body_sent, timeout=10)
        check("请求体原样送达", LAST_BODY and LAST_BODY[-1] == body_sent)

        raw = m.http_request("GET", f"{base}/v1/json", timeout=10)
        check("GET 路径可提取", m.extract_path(json.loads(raw), "a.b[0].c") == 42)

        img = json.loads(m.http_request("POST", f"{base}/v1/images/generations",
                                        body=b"{}", timeout=10))
        check("images 端点可提取 base64",
              m.extract_path(img, "data[0].b64_json").startswith("iVBOR"))

        try:
            m.http_request("POST", f"{base}/v1/nope", body=b"{}", timeout=10)
            check("404 应抛 ApiError", False)
        except ok as e:
            check("404 应抛 ApiError", True, str(e)[:48])

        try:
            m.http_request("POST", f"{base}/v1/slow", body=b"{}", timeout=1)
            check("超时应抛 ApiError", False)
        except ok as e:
            check("超时应抛 ApiError", True, str(e)[:32])

        try:
            m.http_request("POST", "ftp://x/y", body=b"", timeout=5)
            check("非 http(s) URL 应抛 ApiError", False)
        except ok as e:
            check("非 http(s) URL 应抛 ApiError", True, str(e)[:32])

        try:
            m.http_request("POST", f"{base}/v1/chat/completions", body=b"{}",
                           timeout=10, max_response_bytes=4)
            check("超限响应应中止", False)
        except ok as e:
            check("超限响应应中止", True, str(e)[:32])
    finally:
        srv.shutdown()

    print("== [5] base64 → 图像张量(stub torch: 验形状与归一化) ==")
    class _FakeTensor:  # 只需支持节点用到的 [None, ...] 批维补齐
        def __init__(self, arr):
            self.arr = arr

        def __getitem__(self, key):
            return _FakeTensor(self.arr[key])

    fake = types.ModuleType("torch")
    fake.from_numpy = _FakeTensor
    sys.modules["torch"] = fake
    tensor = m.base64_to_image_tensor(one_px_png_b64())
    check("形状为 (1,H,W,3)", tensor.arr.shape == (1, 1, 1, 3), str(tensor.arr.shape))
    check("取值归一化到 [0,1]", float(tensor.arr.max()) <= 1.0 and float(tensor.arr.min()) >= 0.0)
    check("红色像素解码正确", abs(float(tensor.arr[0, 0, 0, 0]) - 1.0) < 1e-6)
    try:
        m.base64_to_image_tensor("不是base64!!")
        check("非法 base64 应抛 ApiError", False)
    except ok:
        check("非法 base64 应抛 ApiError", True)
    try:
        m.base64_to_image_tensor(base64.b64encode(b"not an image").decode())
        check("非图像内容应抛 ApiError", False)
    except ok:
        check("非图像内容应抛 ApiError", True)

    print()
    if FAILURES:
        print(f"FAILED: {len(FAILURES)} 项 — {FAILURES}")
        return 1
    print("ALL GREEN")
    return 0


if __name__ == "__main__":
    sys.exit(main())
