#!/usr/bin/env python3
"""smoke_api_nodes.py —— W4 真机 Q1-Q4: OHOS_API_* 三节点打宿主 mock API(经 rport 隧道)。

链路: 设备 ComfyUI 节点 → http://127.0.0.1:18081(设备侧) → rport → 宿主 18002 mock_api_server。
**不接外网**(smoke 铁律): mock 由本进程 threads 内起, 不依赖公网。

⚠ 端口分工(2026-09-12 实测): 设备 18080→宿主 18001 是**模型下载源**(/tmp/models 的
http.server, W3 起常驻), 不是本项目可占用的 —— W4 mock 另开 18081→18002, 两条隧道互不干扰。

判据(Q0/Q5 在 verify_smoke.sh 里, 本脚本只管 Q1-Q4):
    Q1 OHOS_API_Text  → PreviewAny: 输出文本 == mock 应答
    Q2 OHOS_API_Image → PreviewImage: 出图且 /view 拉回校验为 1x1 PNG(mock 图)
    Q3 OHOS_API_HTTP  → PreviewAny: 双输出(TEXT+JSON)且 JSON 含嵌套路径值
    Q4 错误路径 404 / 非 JSON / 超时: 三者皆 status=error 且消息可读, 后端不死

用法:
    python3 scripts/smoke_api_nodes.py [--device 192.168.1.5:44959] [--comfy http://127.0.0.1:8189]
    python3 scripts/smoke_api_nodes.py --selftest   # 不连设备: 只校验 workflow 与节点定义的键名对齐
前置: 后端已就绪(先 bash scripts/verify_smoke.sh 或手工点「启动 梦幻之流」); 本脚本自建 rport。
"""
import argparse
import io
import json
import os
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NODES_PY = os.path.join(
    ROOT, "externals/comfyui-src/custom_nodes/ohos_external_api/nodes.py")
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from mock_api_server import Handler  # noqa: E402  (同目录, 复用 mock 路由)

DEV_PORT = 18081                 # 设备侧隧道口(见头注释「端口分工」)
DEV = f"http://127.0.0.1:{DEV_PORT}"   # 设备侧看到的 mock 地址(另一端是本脚本的 mock)
FAILURES = []


def check(name, cond, detail=""):
    print(f"  [{'OK' if cond else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))
    if not cond:
        FAILURES.append(name)


# ── HTTP 小工具(仅 stdlib) ────────────────────────────────────────────────────
def _req(url, data=None, method=None, timeout=20):
    body = json.dumps(data).encode() if data is not None else None
    r = urllib.request.Request(url, data=body, method=method,
                               headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(r, timeout=timeout) as resp:
        return resp.read().decode("utf-8", "replace")


def post_prompt(comfy, workflow, timeout=30):
    try:
        d = json.loads(_req(f"{comfy}/prompt", {"prompt": workflow}, timeout=timeout))
        return d.get("prompt_id", ""), ""
    except urllib.error.HTTPError as e:
        return "", f"HTTP {e.code}: {e.read().decode('utf-8', 'replace')[:400]}"
    except Exception as e:
        return "", f"{type(e).__name__}: {e}"


def wait_history(comfy, pid, timeout=180):
    """轮询 /history/<pid>, 返回 (entry, err)。entry 有 outputs 或 status=error 即返回。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            d = json.loads(_req(f"{comfy}/history/{pid}", timeout=15))
            h = d.get(pid)
            if h and (h.get("outputs") or h.get("status", {}).get("status_str") == "error"):
                return h, ""
        except Exception:
            pass
        time.sleep(5)
    return None, f"超时({timeout}s)"


def history_text(entry, node_id):
    o = entry.get("outputs", {}).get(node_id, {})
    ui = o.get("text") or o.get("ui", {}).get("text", [])
    return ui[0] if ui else ""


def history_error(entry):
    msgs = entry.get("status", {}).get("messages", [])
    for m in msgs:
        if m and m[0] == "execution_error":
            p = m[1] if len(m) > 1 else {}
            return p.get("exception_message") or p.get("exception_type") or str(p)
    return ""


# ── workflow 构造(输入名对齐 nodes.py INPUT_TYPES) ───────────────────────────
def wf_text(url, prompt="你好，云"):
    return {
        "1": {"class_type": "OHOS_API_Text", "inputs": {
            "url": url, "provider": "(none)",
            "headers": '{"Content-Type": "application/json"}',
            "body": '{"model": "mock", "messages": [{"role": "user", "content": {{prompt}}}]}',
            "path": "choices[0].message.content", "timeout": 30, "prompt": prompt}},
        "2": {"class_type": "PreviewAny", "inputs": {"source": ["1", 0]}},
    }


def wf_image(url):
    return {
        "1": {"class_type": "OHOS_API_Image", "inputs": {
            "url": url, "provider": "(none)",
            "headers": '{"Content-Type": "application/json"}',
            "body": '{"model": "mock", "prompt": {{prompt}}, '
                    '"size": "{{width}}x{{height}}"}',
            "field": "data[0].b64_json", "timeout": 60, "prompt": "一只猫",
            "width": 256, "height": 256, "seed": 7}},
        "2": {"class_type": "PreviewImage", "inputs": {"images": ["1", 0]}},
    }


def wf_http_get(url):
    return {
        "1": {"class_type": "OHOS_API_HTTP", "inputs": {
            "method": "GET", "url": url, "provider": "(none)",
            "headers": "{}", "body": "{}", "timeout": 30}},
        "2": {"class_type": "PreviewAny", "inputs": {"source": ["1", 1]}},  # JSON 输出
    }


def wf_err(url, timeout=30):
    """错误路径复用 Text 节点(无下游, OUTPUT_NODE=True 保证可执行)。"""
    w = wf_text(url)
    w.pop("2")
    w["1"]["inputs"]["timeout"] = timeout
    return w


def _nodes_default(name):
    """取节点模块**真身**的默认模板(不复制字面量 —— 副本会与真身漂移)。"""
    import importlib
    pkg_parent = os.path.join(ROOT, "externals/comfyui-src/custom_nodes")
    if pkg_parent not in sys.path:
        sys.path.insert(0, pkg_parent)
    return getattr(importlib.import_module("ohos_external_api.nodes"), name)


def wf_text_auth(url, provider="custom"):
    """Q5b: provider(密钥来自设备存储) + 真身默认 headers/body → mock 回显 Authorization。"""
    w = wf_text(url)
    i = w["1"]["inputs"]
    i["provider"] = provider
    i["headers"] = _nodes_default("_HEADERS_DEFAULT")
    i["body"] = _nodes_default("_BODY_TEXT_DEFAULT")
    i["path"] = "auth"
    i["prompt"] = "鉴权探测"
    return w


# ── 离线自测: workflow 输入键 vs nodes.py 的 INPUT_TYPES ─────────────────────
def selftest():
    import re
    src = open(NODES_PY, encoding="utf-8").read()
    allow = {}
    for cls in ("OHOS_API_Text", "OHOS_API_Image", "OHOS_API_HTTP"):
        body = src.split(f"class {cls}:")[1].split("\nclass ")[0]
        keys = set(re.findall(r'"(\w+)": \(', body.split("RETURN_TYPES")[0]))
        allow[cls] = keys
    print("  节点允许输入键:", {k: sorted(v) for k, v in allow.items()})
    wfs = {"Q1": wf_text(f"{DEV}/v1/chat/completions"),
           "Q2": wf_image(f"{DEV}/v1/images/generations"),
           "Q3": wf_http_get(f"{DEV}/v1/json"),
           "Q4": wf_err(f"{DEV}/v1/nope"),
           "Q5b": wf_text_auth(f"{DEV}/v1/echo-auth")}
    bad = 0
    for q, wf in wfs.items():
        for nid, node in wf.items():
            ct = node["class_type"]
            if ct not in allow:
                continue  # PreviewAny / PreviewImage 是 core 节点
            extra = set(node["inputs"]) - allow[ct]
            if extra:
                bad += 1
                print(f"  [FAIL] {q} 节点 {nid}({ct}) 含未定义输入: {sorted(extra)}")
    print("  [OK] workflow 键名与 nodes.py 对齐" if not bad else f"  [FAIL] {bad} 处不对齐")
    return 1 if bad else 0


# ── 主流程 ───────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", default="192.168.1.5:44959")
    ap.add_argument("--comfy", default="http://127.0.0.1:8189")
    ap.add_argument("--port", type=int, default=18002)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()

    print(f"== [0] mock server(宿主 :{a.port}) + rport(设备 {DEV_PORT} → 宿主 {a.port}) ==")
    import http.server
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", a.port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print(f"  mock 已起: 0.0.0.0:{a.port}")
    r = subprocess.run(["hdc", "-t", a.device, "rport", f"tcp:{DEV_PORT}", f"tcp:{a.port}"],
                       capture_output=True, text=True, timeout=30)
    print(f"  rport rc={r.returncode} {(r.stdout or r.stderr).strip()[:100]}")

    print("== [1] 后端就绪 ==")
    try:
        st = _req(f"{a.comfy}/system_stats", timeout=10)
        check("后端可达(8189)", "comfyui" in st)
    except Exception as e:
        check("后端可达(8189)", False, str(e))
        print("  → 先跑 bash scripts/verify_smoke.sh 或点『启动 梦幻之流』")
        return 1

    print("== [Q1] OHOS_API_Text → PreviewAny ==")
    pid, err = post_prompt(a.comfy, wf_text(f"{DEV}/v1/chat/completions"))
    if not pid:
        check("Q1 prompt 受理", False, err)
    else:
        h, err = wait_history(a.comfy, pid)
        txt = history_text(h, "2") if h else ""
        check("Q1 文本输出正确", txt == "mock 应答: 你好, 云!", f"got={txt!r} {err}")

    print("== [Q2] OHOS_API_Image → PreviewImage ==")
    pid, err = post_prompt(a.comfy, wf_image(f"{DEV}/v1/images/generations"))
    if not pid:
        check("Q2 prompt 受理", False, err)
    else:
        h, err = wait_history(a.comfy, pid)
        imgs = (h or {}).get("outputs", {}).get("2", {}).get("images", [])
        check("Q2 出图", bool(imgs), f"{err} err={history_error(h) if h else ''}")
        if imgs:
            q = urllib.parse.urlencode({"filename": imgs[0]["filename"],
                                        "subfolder": imgs[0].get("subfolder", ""),
                                        "type": imgs[0].get("type", "output")})
            try:
                raw = urllib.request.urlopen(f"{a.comfy}/view?{q}", timeout=20).read()
                from PIL import Image
                im = Image.open(io.BytesIO(raw))
                check("Q2 图为 mock 的 1x1(mock 图确实来自 API)",
                      im.size == (1, 1), f"size={im.size} bytes={len(raw)}")
            except Exception as e:
                check("Q2 图可拉回校验", False, str(e))

    print("== [Q3] OHOS_API_HTTP(GET) → PreviewAny(JSON 输出) ==")
    pid, err = post_prompt(a.comfy, wf_http_get(f"{DEV}/v1/json"))
    if not pid:
        check("Q3 prompt 受理", False, err)
    else:
        h, err = wait_history(a.comfy, pid)
        txt = history_text(h, "2") if h else ""
        check("Q3 JSON 输出含嵌套值 a.b[0].c=42", '"c": 42' in txt, f"got={txt[:120]!r} {err}")

    print("== [Q5b] 密钥 → 节点闭环(provider + 真身默认模板 + 设备存储的密钥) ==")
    SK = "sk-smoke-0000000000000000"
    try:
        r = json.loads(_req(f"{a.comfy}/ohos/apikeys", {"provider": "custom", "key": SK}))
        check("写入 custom 探针密钥", r.get("ok") is True, str(r)[:80])
    except Exception as e:
        check("写入 custom 探针密钥", False, str(e))
    pid, err = post_prompt(a.comfy, wf_text_auth(f"{DEV}/v1/echo-auth"))
    if not pid:
        check("Q5b prompt 受理", False, err)
    else:
        h, err = wait_history(a.comfy, pid)
        txt = history_text(h, "2") if h else ""
        check("Q5b Authorization 带上存储的密钥(Bearer 组装正确)",
              txt == f"Bearer {SK}", f"got={txt!r} {err}")

    print("== [Q4] 错误路径(404 / 非 JSON / 超时) ==")
    cases = [
        ("404", f"{DEV}/v1/nope", 30, "404"),
        ("非 JSON", f"{DEV}/v1/notjson", 30, "不是合法 JSON"),
        ("超时", f"{DEV}/v1/slow", 2, "请求超时"),
    ]
    for label, url, tmo, want in cases:
        pid, err = post_prompt(a.comfy, wf_err(url, tmo))
        if not pid:
            # 提交即被拒也算「可读错误」——但节点级校验失败应发生在执行期, 此处记 FAIL
            check(f"Q4 {label} prompt 受理", False, err)
            continue
        h, err = wait_history(a.comfy, pid, timeout=120)
        msg = history_error(h) if h else ""
        st = (h or {}).get("status", {}).get("status_str", "")
        check(f"Q4 {label} 报错可读(含「{want}」)", st == "error" and want in msg,
              f"status={st} msg={msg[:160]!r} {err}")

    print("== [Q4b] 后端不死 ==")
    try:
        st = _req(f"{a.comfy}/system_stats", timeout=10)
        check("错误路径后后端仍活", "comfyui" in st)
    except Exception as e:
        check("错误路径后后端仍活", False, str(e))

    srv.shutdown()
    print()
    if FAILURES:
        print(f"FAILED: {len(FAILURES)} 项 — {FAILURES}")
        return 1
    print("ALL GREEN")
    return 0


if __name__ == "__main__":
    sys.exit(main())
