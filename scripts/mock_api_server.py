#!/usr/bin/env python3
"""mock_api_server.py —— W4 真机 Q 轮的宿主侧 mock 第三方 API(零第三方依赖)。

用途: 设备上的 OHOS_API_* 节点经 rport 隧道(设备 127.0.0.1:18080 → 宿主 18001)打到这里,
验证「节点 → HTTP → 响应解析 → 张量/文本输出」全链, **不接外网**(smoke 铁律)。

用法:
    python3 scripts/mock_api_server.py [--port 18001] [--host 0.0.0.0]
    # 真机侧 URL 一律写 http://127.0.0.1:18080/...  (隧道: hdc rport tcp:18080 tcp:18001)

路由(与 OpenAI 兼容形态对齐, 便于同一 workflow 切真实服务商):
    POST /v1/chat/completions    → {"choices":[{"message":{"content":"..."}}]}   (Q1)
    POST /v1/images/generations  → {"data":[{"b64_json":"<1px PNG>"}]}          (Q2)
    GET  /v1/json                → {"a":{"b":[{"c":42}]}}                       (Q3)
    POST /v1/echo-auth           → {"auth":"<Authorization 头原值>"}            (Q5b)
    POST /v1/notjson             → 200 + HTML 非 JSON                            (Q4)
    POST /v1/slow                → 延迟 8s 后返回(验节点 timeout 路径)            (Q4)
    POST /v1/boom                → 500 + {"error": ...}                         (Q4)
    其余                          → 404 + {"error":"not found"}
每个请求打印一行 [REQ] 供驱动脚本/Q 轮观察。
"""
import argparse
import base64
import http.server
import json
import struct
import sys
import time
import zlib


def one_px_png(r=255, g=0, b=0):
    """纯 stdlib 造 1x1 PNG(不经 PIL): 节点侧 base64 → PIL.open → 张量 全链可验。"""
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)  # 1x1, 8bit, truecolor RGB
    raw = b"\x00" + bytes([r, g, b])                      # filter 0 + pixel
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


PNG_B64 = base64.b64encode(one_px_png()).decode()
LAST = {"path": None, "body": None}


class Handler(http.server.BaseHTTPRequestHandler):
    def _send(self, status, body, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass  # 客户端超时主动断开(如 /v1/slow 验 timeout)属预期

    def _route(self, method):
        LAST["path"] = f"{method} {self.path}"
        print(f"  [REQ] {method} {self.path}", flush=True)
        if self.path.startswith("/v1/chat/completions"):
            return self._send(200, {"choices": [{"message": {
                "role": "assistant", "content": "mock 应答: 你好, 云!"}}],
                "usage": {"total_tokens": 7}})
        if self.path.startswith("/v1/images/generations"):
            return self._send(200, {"created": 1, "data": [
                {"b64_json": PNG_B64}, {"b64_json": PNG_B64}]})
        if self.path.startswith("/v1/json"):
            return self._send(200, {"a": {"b": [{"c": 42}]}})
        if self.path.startswith("/v1/echo-auth"):
            # Q5b: 回显 Authorization 头 —— 验证「密钥经 provider 注入到 {{auth_header}}」闭环
            return self._send(200, {"auth": self.headers.get("Authorization") or ""})
        if self.path.startswith("/v1/notjson"):
            return self._send(200, "<html>not json</html>", ctype="text/html")
        if self.path.startswith("/v1/slow"):
            time.sleep(8)
            return self._send(200, {"ok": True})
        if self.path.startswith("/v1/boom"):
            return self._send(500, {"error": {"message": "mock 内部错误"}})
        return self._send(404, {"error": "not found"})

    def do_GET(self):
        self._route("GET")

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        LAST["body"] = self.rfile.read(n) if n else b""
        if LAST["body"]:
            print(f"        body: {LAST['body'][:160].decode('utf-8', 'replace')}", flush=True)
            # 判定力(2026-09-12): 请求体必须能解析成 JSON —— 否则模板 bug(占位符写在引号里
            # → `""你好""` 双引号嵌套)会被静默放过(节点只负责发, 不校验自身模板)。
            # 非法即 400, 让这类缺陷在冒烟里显性失败而非"通过"。
            if "json" in (self.headers.get("Content-Type") or ""):
                try:
                    json.loads(LAST["body"])
                except ValueError as e:
                    print(f"        [BAD-BODY] 请求体不是合法 JSON: {e}", flush=True)
                    return self._send(400, {"error": {
                        "message": f"mock: 请求体不是合法 JSON: {e}"}})
        self._route("POST")

    def log_message(self, *a):
        pass  # 只留 [REQ] 行, 屏蔽 BaseHTTPRequestHandler 默认 stderr 噪声


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18001)
    ap.add_argument("--host", default="0.0.0.0")
    a = ap.parse_args()
    srv = http.server.ThreadingHTTPServer((a.host, a.port), Handler)
    print(f"[MOCK] 监听 {a.host}:{a.port} — 设备侧 URL: http://127.0.0.1:18080/..."
          f" (rport tcp:18080 → tcp:{a.port})", flush=True)
    print(f"[MOCK] 1x1 PNG base64 前 32 字符: {PNG_B64[:32]}…", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[MOCK] 停止")
    return 0


if __name__ == "__main__":
    sys.exit(main())
