#!/usr/bin/env python3
"""Step 0.3b — 把 skh-run 的 Python 3.12 栈（stdlib + site-packages 纯 .py）打进 python312.zip。

产物落位 entry/src/main/resources/rawfile/python312.zip，条目形如 lib/python3.12/*，
与 3.13 的 python313.zip 同构（HarmonyOS zlib.decompressFile 解 zip → 目录树，真机已验证 3.13 跑通）。
铁律：一切被加载 .so 必须走 HAP libs/<abi>/（不打包进此 zip）；此 zip 只装纯 .py 运行时文件。
"""
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "build/skh-run-extract/skh-run/usr/lib/python3.12")
OUT = os.path.join(ROOT, "entry/src/main/resources/rawfile/python312.zip")
# run97-4：外部输入统一 externals/（fetch_externals.sh 产物）；EXT_DIR 可覆盖
EXT_DIR = os.environ.get("EXT_DIR", os.path.join(ROOT, "externals"))
PREFIX = "lib/python3.12"
# Step 0.4b — 第二源：comfyui 根 + 新纯 py 依赖（scripts/make_comfyui_stage.py 产出）。
#   comfyui/**（仓库根）→ zip 条目 comfyui/**（解压到 <pyroot>/comfyui，PYTHONPATH 挂它）；
#   依赖 → 条目 lib/python3.12/site-packages/**（与既有站点包段合并落盘）。
STAGE = os.path.join(ROOT, "build/pyroot-stage")
# run82 — 纯 py 依赖站点（run97-4 正规化：数据源从 /tmp/hostcv venv 改为
#   fetch_externals.sh 的 externals/py-site —— venv 退为对照，可复现链不再依赖 /tmp）。
#   仅用其 pyyaml-6.0.3.dist-info 作元数据补给（yaml 包本体来自 skh 树）。
VENV_SP = os.path.join(EXT_DIR or (os.path.join(ROOT, "externals")), "py-site")
# run72 — sitecustomize 全局 stub:stdlib 内置 sitecustomize 缺失由 site 正常跳过(见下),
# 而我们要把它做成「模型层 stub + 手工父链」全局注入点(内容 = build/stub/sitecustomize_tpl.py),
# 执行时机 = CPython 启动最深处、任何用户代码之前 ⇒ 真 main.py 零改动即可享受 84 叶子预置。
# run97（2026-09-03):stub 模板「双区定位」—— 正规区(/data/share/comfyui)模板在仓库根
#   stub/,研究区(comfy-ohos-port)遗留 build/stub/;顶层 stub/ 优先,回退 build/stub/
def _stub_ok(rel):
    _a = os.path.join(ROOT, "stub", rel)
    if os.path.isfile(_a):
        return "stub/" + rel
    return os.path.join("build/stub", rel)


SITECUSTOMIZE_SRC = os.path.join(ROOT, _stub_ok("sitecustomize_tpl.py"))

# 不打包的目录名（命中即整目录跳过）
EXCLUDE_DIRS = {
    "__pycache__",
    "test", "tests", "testing",        # 测试数据/单测，运行不需要，torch/test 就有 84MB
    "include",                          # 头文件（.h/.hpp），运行时不需要
    "lib-dynload",                      # 70 个 .so 已按铁律走 libs/<abi>/，由宿主注册桥加载
    "config-3.12-aarch64-linux-ohos",   # 编译期 Makefile/.o，运行时不需要
}
# "bin" 目录特例：python 本体 bin（lib/python3.12/bin）不需要；但 torch/bin/torch_shm_manager
#   必须保留——torch/__init__.py:2140 _manager_path() 起进程用，缺了 _C._initExtension 直接
#   抛 RuntimeError（真机 2026-09-02 实证）。可执行 ELF（非 .so），走 filesDir 不违反铁律。
BIN_KEEP_PREFIXES = ("site-packages/torch/bin/torch_shm_manager",)
# 不打包的文件后缀
EXCLUDE_SUFFIXES = (".so", ".a", ".o", ".pyc", ".pyo", ".pyi", ".pth", ".dylib")


def is_excluded(rel: str, is_dir: bool) -> bool:
    parts = rel.split("/")
    if "bin" in parts:
        # torch/bin 目录自身放行，其下仅保留白名单可执行（见 BIN_KEEP_PREFIXES）；
        # 其余 place 的 bin（如 lib/python3.12/bin）整目录排除
        if rel == "site-packages/torch/bin":
            return False
        if rel.startswith("site-packages/torch/bin/"):
            return not any(rel.startswith(p) for p in BIN_KEEP_PREFIXES)
        return True
    # torch/testing 例外：torch/autograd/gradcheck.py 的运行时链（torch.distributed.rpc → profiler_legacy）
    #   会 import torch.testing，虽名为 "testing" 却是运行时依赖（真机 2026-09-02 实证），不可整包排除。
    #   torch/test（单测 84MB）仍按 EXCLUDE_DIRS 排除。
    if rel == "site-packages/torch/testing" or rel.startswith("site-packages/torch/testing/"):
        return False
    if any(p in EXCLUDE_DIRS for p in parts):
        return True
    if rel.endswith(EXCLUDE_SUFFIXES):
        return True
    return False


# torch._dynamo 空壳（见上 STUB 段）。API 面兜底：
#   - 装饰器/编译入口（disable/compile/optimize）→ 恒返回原函数（无人真正 compile，等价）
#   - 状态类（is_compiling）→ False；reset 等副作用类 → no-op
#   - 未知属性（__getattr__ 兜底）→ 再给一个 no-op callable，防 from x import y 崩
# run74 — scipy.sparse.linalg._propack 子包补齐（2026-09-02）：
#   run73 死点 = k_diffusion/sampling.py:4 `from scipy import integrate` → scipy 深链
#   → scipy/sparse/linalg/_eigen/_svds.py:10 → ._svdp → _svdp.py:23
#   `from ._propack import _spropack` → ModuleNotFoundError: No module named
#   'scipy.sparse.linalg._propack'。
#   根因：scipy wheel 的 sparse/linalg/_propack/ 是【裸 .so 目录，无 __init__.py】；
#   其 4 个 C 扩展（_spropack/_dpropack/_zpropack/_cpropack）已由官桥以【顶层名】从
#   HAP libs/arm64 经 importlib 官方链注册进 sys.modules（"official import done 70/70"）。
#   只要把子包 __init__.py 补成"完整键预注册"的 stub，相对导入
#   （. _propack → ._spropack 解析为 scipy.sparse.linalg._propack._spropack）
#   即命中 sys.modules —— 启动链本无需 PROpack 数值功能（采样运行时才用）。
PROPACK_STUB = '''"""OHOS 移植补齐（2026-09-02，run74）—— scipy wheel 的 sparse/linalg/_propack 为裸 .so
目录（无 __init__.py）；其 C 扩展已由官桥从 HAP libs/arm64 以顶层名（_spropack 等）
经 importlib 官方链注册。本文件以【完整键预注册】方式让子包可 import：
相对导入 `from ._propack import _spropack` 会解析成 scipy.sparse.linalg._propack._spropack
→ 已在 sys.modules 即命中；任意子属性访问经 PEP 562 __getattr__ 链式兜底。
"""
import sys as _s
import types as _t

_SUBS = ("_spropack", "_dpropack", "_zpropack", "_cpropack")


def __getattr__(name):
    """未知子名（如 solve/svds）：动态建模块并预注册完整键，返回链式占位类。"""
    if name.startswith("_"):
        m = _t.ModuleType("scipy.sparse.linalg._propack." + name)
        m.__dict__["__getattr__"] = lambda n: m
        m.__dict__["__file__"] = "<stub:...>"
        _s.modules[m.__name__] = m
        return m
    return _t.ModuleType()


for _sub in _SUBS:
    _full = "scipy.sparse.linalg._propack." + _sub
    if _full not in _s.modules:
        # 官桥只注册了顶层名 _spropack；兜底建同键模块（防官桥侧个别失败）
        _m = _t.ModuleType(_full)
        _m.__dict__["__getattr__"] = lambda n, _mm=_m: _mm
        _m.__dict__["__file__"] = "<stub:...>"
        _s.modules[_full] = _m
    setattr(_s.modules[__name__], _sub, _s.modules[_full])
'''


# run77 — comfy_aimdo/torch.py 降级壳（2026-09-02）：
#   comfy/ops.py:31-34 顶层裸 `import comfy_aimdo.model_vbar` + `import comfy_aimdo.torch`；
#   comfy_aimdo/torch.py 顶层 `class CUDAPluggableAllocator(torch.cuda.memory.CUDAPluggableAllocator)`
#   触发 torch.cuda 懒链 —— OHOS 无 CUDA torch(2.10.0a0)：torch/cuda/_lazy_init:417
#   raise AssertionError('Torch not compiled with CUDA enabled')。
#   设备上 comfy_aimdo 已整链降级（'comfy-aimdo unsupported operating system: HarmonyOS'
#   + ctypes lib=None），仅 import 面碎裂；换空壳：类/函数兜底 no-op，CPU 后端无人触碰真逻辑。
AIMDO_TORCH_STUB = '''"""OHOS 移植降级壳（2026-09-02，run77）—— comfy_aimdo/torch.py：
原顶层 `class CUDAPluggableAllocator(torch.cuda.memory.CUDAPluggableAllocator)` 在无 CUDA 的
OHOS torch 上 import 即触发 torch.cuda._lazy_init 断言（run76 WARM-FAIL 实证）。
comfy_aimdo 本身在 OHOS 已全降级（unsupported OS + ctypes lib=None），CPU 后端只会 import 面。
"""
__version__ = "0-stub-ohos"


def _noop(*a, **k):
    return None


class _MetaStub(type):
    def __getattr__(cls, n):
        return cls


class _StubCls(metaclass=_MetaStub):
    def __init__(self, *a, **k):
        pass

    def __call__(self, *a, **k):
        return self

    def __getattr__(self, n):
        return self


CUDAPluggableAllocator = _StubCls
get_tensor_from_raw_ptr = _noop
aimdo_to_tensor = _noop
hostbuf_to_tensor = _noop


def __getattr__(name):
    return _noop
'''


DYNAMO_STUB = '''"""OHOS 移植空壳（2026-09-02）—— 原实现整链 ~30-50MB RSS，越 ~340MB 内存墙必死。

仅用于本移植：comfy 启动链无人真正触发 torch.compile（CPU 后端），属性访问兜底
no-op 后行为等价于从未加载 dynamo。
"""
from types import SimpleNamespace as _SN


def _passthrough(*args, **kwargs):
    """装饰器/编译入口：单 callable 参数恒返回原样，其余返回自身（可再当装饰器用）。"""
    if len(args) == 1 and callable(args[0]):
        return args[0]
    return _passthrough


def _noop(*args, **kwargs):
    return None


def is_compiling(*args, **kwargs):
    return False


# 常见 API 面（torch.compiler/__init__.py 的 from torch._dynamo import ... 也依赖它们）
config = _SN(compile=False, suppress_errors=True, verify=False)
disable = _passthrough
compile = _passthrough
optimize = _passthrough
reset = _noop
assume_constant_result = _noop
allow_in_graph = _passthrough
mark_dynamic = _noop

# 兜底：任何未知属性访问都返回 no-op callable（防 from torch._dynamo import X 崩）
def __getattr__(name):
    return _noop
'''


# run73 — torchaudio 全树空壳替换（2026-09-02）：
#   run72C 死因定谳：nodes.py 顶层 load_comfy_extras → nodes_audio_encoder.py →
#   comfy.audio_encoders.audio_encoders:7 裸 import torchaudio → torchaudio/__init__
#   （_extension 缺 .so 纯 py 降级成功）→ from . import ...,models,pipelines,... 真 .py
#   树潮 → 23.5s 死于 torchaudio/pipelines/_source_separation_pipeline 加载期。
#   该死点无 TB/无 crasher/无 memcg/failcnt=0（echo E13 为权限错，WALL-PROBE 已实证
#   进程可活到 682MB 内存——非内存）；机制玄机留档，对策=按 torch._dynamo 同法整树
#   替换空壳：comfy 启动链无人真正用 torchaudio 功能（audio_encoders 仅在节点文件
#   顶层 import 包名，方法体内才用 functional/transforms，空壳 __getattr__ 全兜底）。
TORCHAUDIO_STUB = '''"""OHOS 移植空壳（2026-09-02，run73）—— 原真树 ~120 文件 .py 潮在 import 时硬死（无 TB）。

仅用于本移植：comfy 启动链只把 torchaudio 当包名 import（audio_encoders.py:7 裸
import torchaudio；functional/transforms 均为类方法体内延迟使用），空壳 __getattr__
兜底后行为等价于「包存在但无功能」。
"""
from types import SimpleNamespace as _SN


def _noop(*args, **kwargs):
    return None


class _MetaStub(type):
    """元类：类对象本身的属性访问也链式（防 from x import X; X.attr 崩）。"""

    def __getattr__(cls, n):
        return cls


class _StubCls(metaclass=_MetaStub):
    """占位类：实例/属性/调用全链式兜底。"""

    def __init__(self, *a, **k):
        pass

    def __call__(self, *a, **k):
        return self

    def __getattr__(self, n):
        return self


__version__ = "2.10.0+ohos"
git_version = "ohos"
_lg = _noop
# 常见子命名空间（from torchaudio.models import X 走模块 attr 链时兜底；
# _StubCls 的元类链式 __getattr__ 保证任意 from-import 属性可解析）
models = _StubCls
functional = _StubCls
pipelines = _StubCls
transforms = _StubCls
compliance = _StubCls
datasets = _StubCls
utils = _StubCls


# PEP 562：模块任意 attr miss → no-op（防 from torchaudio import X 崩）
def __getattr__(name):
    return _noop
'''


SITECUSTOMIZE_SLEEP = '''"""run29/run30 A/B 判官（已退役）：解释器启动纯 idle 40 秒 + 加载标记（原 sleep(40)）。

判官结论已归档：run44-55 的差分实验表明「空 main / 纯导入 / socket bind」均能存活,
「sleep(40) 拖慢每次启动」的代价早已超过一条旧证据的价值 → 2026-09-02 run57 起移除
（如未来需要重新判官,恢复 sleep 即可）。
"""'''

# run82 — transformers/utils/auto_docstring.py 空壳替换（2026-09-02）：
#   原模块（5.16.1，~4700 行）第 25 行硬 `import regex as re`；而 regex 2026.9.3 的
#   _main.py:429 又硬 `from regex import _regex`（C 扩展，纯 py 不可用）→ regex 生态
#   （含 _regex.cpython-312-aarch64-linux-ohos.so 交叉编译）未闭环前，该模块是
#   transformers 主链第一死点（dependency_versions_check 同样报 regex——那把该行
#   检查剔除,见主循环 DEPS 替换）。自证零运行时依赖：utils/__init__ 仅做 from-import
#   转发，全库零人调用其函数/继承其类（grep 实证）→ 同 torchaudio/torch._dynamo 型，
#   可导入空壳即行为等价。
AUTODOC_STUB = '''"""OHOS 移植空壳（run82）—— 原 auto_docstring.py（文档生成工具，硬依赖未闭环的 regex）。

import 层即接客：`@auto_docstring`（image_processing_utils.py:382 等裸用）与
`@auto_docstring(checkpoint=...)`（granite_speech5/modular 工厂用）双形态在
import 时执行 → 空壳必须「可调用且返回原对象」；genuine 功能（docstring 自动生成）
OHOS 侧离线不存在，行为等价于 原装饰器对源码无副作用。
"""


class _DocDecorator:
    """双形态装饰器：@auto_docstring 裸用(传类) / @auto_docstring(...) 工厂(返回自身)。"""

    def __call__(self, obj=None, **k):
        return obj if obj is not None else self


def _noop(*a, **k):
    return None


class ImageProcessorArgs:
    pass


class ModelArgs(ImageProcessorArgs):
    pass


class ModelOutputArgs:
    pass


class ClassDocstring:
    pass


class ClassAttrs:
    pass


auto_class_docstring = _DocDecorator()
auto_docstring = _DocDecorator()
get_args_doc_from_source = None
parse_docstring = _noop
set_min_indent = _noop
'''

# run82 — transformers/utils/chat_parsing/__init__.py 空壳替换（2026-09-02）：
#   原 __init__.py 挂 response_parser → content_parsers（第 22 行硬 `import regex as re`，
#   response_templates.py:21 同）——比 auto_docstring 更核心：tokenization_utils_base.py:65
#   `from .utils.chat_parsing import ResponseParser`，Qwen2/CLITokenizer 主链必经。
#   运行期仅 parse_response* 方法体真正调用（3337-3455 行），comfy 启动链不触发聊天响应
#   解析 → 空壳提供同名可导入符号即等价。原三文件（content_parsers/response_parser/
#   response_templates）保留于 zip（不再被 import，无 import 成本）。
CHAT_PARSING_STUB = '''"""OHOS 移植空壳（run82）—— chat_parsing（聊天消息/模板解析，硬依赖未闭环的 regex）。

import 层仅服务 tokenization_utils_base（ResponseParser / parse_response），运行期
parse_response* 方法体才调用；OHOS 侧响应式聊天解析离线，等 regex C 扩展闭环后可恢复原版。
"""


def _noop(*a, **k):
    return None


class ResponseParser:
    """占位：实例化/调用/属性全链式兜底。"""

    def __init__(self, *a, **k):
        pass

    def __call__(self, *a, **k):
        return None

    def __getattr__(self, n):
        return _noop


def parse_response(*a, **k):
    return None
'''


def main():
    if not os.path.isdir(SRC):
        print(f"FATAL: {SRC} 不存在，先解包 skh-run.tar.gz", file=sys.stderr)
        return 1
    os.makedirs(os.path.dirname(OUT), exist_ok=True)

    # run82-3 — importlib.metadata 多版本冲突断根（真机实证：transformers 依赖检查报
    #   "found safetensors==0.7.0" 即此 —— skh 段老内容先写入，zipimport 重复条目以
    #   第一份为准 → 旧版 py 树 + 旧版 dist-info 双双生效，stage 新版永远读不到）。
    #   对策：扫 stage 段将写入的顶层名（包目录/单文件/dist-info base 三合一），skh 段
    #   同名顶层整树不写 —— py 树与 dist-info 同为 stage 版唯一落盘，无版本漂移
    #   （stage 版来自 venv，均 ≥ skh 版；2.x er 栈无历史包袱）。
    STAGE_TOP_NAMES = set()
    _stage_sp = os.path.join(STAGE, "lib/python3.12/site-packages")
    if os.path.isdir(_stage_sp):
        for _d in os.listdir(_stage_sp):
            if _d.endswith(".dist-info"):
                STAGE_TOP_NAMES.add(_d[:-10].rsplit("-", 1)[0])
            else:
                STAGE_TOP_NAMES.add(_d)
    print(f"  run82-3: stage 段顶层覆盖名({len(STAGE_TOP_NAMES)}) = {sorted(STAGE_TOP_NAMES)}")

    count = 0
    total = 0
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        # run29/run30 sitecustomize sleep(40) 判官已于 run57 退役（见 SITECUSTOMIZE_SLEEP
        #   注释）：判官结论（空 main/纯导入/socket 均活）已归档，sleep(40) 只白白拖慢每次启动。
        #   不再写入 sitecustomize.py —— stdlib 自带 sitecustomize 若缺失由 site 正常跳过。
        for root, dirs, files in os.walk(SRC):
            dirs[:] = [d for d in dirs if not is_excluded(
                os.path.relpath(os.path.join(root, d), SRC), True)]
            for f in files:
                full = os.path.join(root, f)
                rel = os.path.relpath(full, SRC).replace(os.sep, "/")
                if is_excluded(rel, False):
                    continue
                if rel == "site-packages/torch/_dynamo/__init__.py":
                    continue  # ⚠ 真版本不写；由下方 STUB 段以空壳唯一写入（重复名会让
                              #   zipimport 命中第一份的原版，run21 已实证）
                if rel.startswith("site-packages/torchaudio/"):
                    continue  # run73：torchaudio 真 .py 树整树不写（run72C 于其
                              #   pipelines 加载期无 TB 硬死），由 STUB 段空壳唯一写入
                if rel.startswith("site-packages/") and STAGE_TOP_NAMES:
                    _segs = rel.split("/")
                    if len(_segs) >= 2:
                        _top = _segs[1]
                        if _top.endswith(".dist-info"):
                            # 目录名 = <发行名>-<版本>.dist-info → 归一为发行名
                            #   （safetensors-0.7.0.dist-info → safetensors）
                            _top = _top[:-10].rsplit("-", 1)[0]
                        if _top in STAGE_TOP_NAMES:
                            # run82-3：skh 老版顶层（如 safetensors-0.7.0 的 py 树 +
                            #   dist-info）整树不写，stage 段新版唯一落盘 → zipimport
                            #   无首份歧义、metadata 不再读到旧版本号、零版本漂移
                            continue
                arc = f"{PREFIX}/{rel}"
                z.write(full, arc)
                count += 1
                total += os.path.getsize(full)

        # Step 0.4c — torch._dynamo 空壳替换（OHOS 内存墙断根，2026-09-02）：
        #   torch._dynamo 全链（torch.fx/_inductor/utils/_sympy/sympy，约 30-50MB RSS）在
        #   本设备上为必死药（~340MB 内存墙实测 SIGKILL），而启动链无人真正触发 torch.compile
        #   （CPU 后端）。√ 证据：comfy_kitchen/tensor/base.py:13 裸 `import torch._dynamo`
        #   为纯副作用 import（全库零 `_dynamo.` 使用）；seedvr 补丁后仍复现即此根。
        #   以空壳替代：import 成功、属性兜底 no-op，行为等价于从未加载。
        for root, dirs, files in os.walk(SRC):
            for f in files:
                full = os.path.join(root, f)
                rel = os.path.relpath(full, SRC).replace(os.sep, "/")
                if rel == "site-packages/torch/_dynamo/__init__.py":
                    z.writestr(f"{PREFIX}/{rel}", DYNAMO_STUB)
                    count += 1
                    total += len(DYNAMO_STUB)
                    print("  [STUB] torch/_dynamo/__init__.py 已替换为空壳")

        # run73 — torchaudio 空壳（见 TORCHAUDIO_STUB 注）：真树已在上方 walk skip 整树
        #   不写（勿重复写——zipimport 命中第一份），此处唯一写入空壳 __init__.py。
        z.writestr(f"{PREFIX}/site-packages/torchaudio/__init__.py", TORCHAUDIO_STUB)
        count += 1
        total += len(TORCHAUDIO_STUB)
        print("  [STUB] torchaudio/__init__.py 已替换为空壳（真树剔除）")

        # run74 — scipy sparse/linalg/_propack 子包补齐（见 PROPACK_STUB 注）：
        #   scipy wheel 该目录是裸 .so（4 个 C 扩展已由官桥以顶层名注册）；zip 内
        #   无同名 __init__.py（skh 树带的是原版相对导入版，且尚未进 zip——若有
        #   首个就写入才是真身,本行在 walk 之后落盘,pytest 防重复由校验项把关）。
        z.writestr(
            f"{PREFIX}/site-packages/scipy/sparse/linalg/_propack/__init__.py",
            PROPACK_STUB,
        )
        count += 1
        total += len(PROPACK_STUB)
        print("  [STUB] scipy/_propack 子包 __init__.py 已补齐（完整键预注册）")

        # run72 — sitecustomize.py 全局注入（模型层 stub + 手工父链）：
        #   内容 = build/stub/sitecustomize_tpl.py（84 叶子预置 + comfy.__path__ 指向磁盘
        #   真目录）。SRC 与 stage 均确认无同名文件 → 无双份条目、zipimport 无歧义。
        if os.path.isfile(SITECUSTOMIZE_SRC):
            with open(SITECUSTOMIZE_SRC, encoding="utf-8") as f:
                sc = f.read()
            z.writestr(f"{PREFIX}/sitecustomize.py", sc)
            count += 1
            total += len(sc)
            print("  [STUB] sitecustomize.py 注入（模型层 stub 全局化）")
        else:
            print(f"  !! 缺 {SITECUSTOMIZE_SRC}（run72 sitecustomize 模板）", file=sys.stderr)
            return 2

        # run72 方案 B — stub_global.py(与 comfyui/main.py 同目录):C++ 注入段在
        #   run_path(main.py) 之前先 run_path 本文件,进程内预置 84 叶子 stub(机制与
        #   main_sim run68 版同源,已验证);sitecustomize 版保留于 lib/python3.12/ 作为
        #   第二道保险(幂等:已有 sys.modules 命中即跳过)。
        STUB_GLOBAL_SRC = os.path.join(ROOT, _stub_ok("stub_global.py"))
        if os.path.isfile(STUB_GLOBAL_SRC):
            with open(STUB_GLOBAL_SRC, encoding="utf-8") as f:
                sg = f.read()
            z.writestr("comfyui/stub_global.py", sg)
            count += 1
            total += len(sg)
            print("  [STUB] comfyui/stub_global.py 注入（run_path 前置 stub）")
        else:
            print(f"  !! 缺 {STUB_GLOBAL_SRC}（run72 方案 B 模板）", file=sys.stderr)
            return 2

        # Step 0.4b — 第二源（stage）：comfyui 根 + 站点包段合并。
        # stage 本身已按铁律滤过 .so，这里仅兜底二次排除（防有人手放进 .so）。
        stage_subs = [
            ("comfyui", "comfyui"),                                      # 仓库根 → comfyui/*
            ("lib/python3.12/site-packages", "lib/python3.12/site-packages"),  # 依赖 → 站点包段
        ]
        for sub, arc_prefix in stage_subs:
            sroot = os.path.join(STAGE, sub)
            if not os.path.isdir(sroot):
                print(f"  !! stage 缺 {sroot}（先跑 make_comfyui_stage.py）", file=sys.stderr)
                continue
            for root, dirs, files in os.walk(sroot):
                dirs[:] = [d for d in dirs if d != "__pycache__"]
                for f in files:
                    if f.endswith(EXCLUDE_SUFFIXES) or f.endswith(".so"):
                        continue
                    rel = os.path.relpath(os.path.join(root, f), sroot).replace(os.sep, "/")
                    arc = f"{arc_prefix}/{rel}"
                    # run77：comfy_aimdo/torch.py 整文件换降级壳（见 AIMDO_TORCH_STUB 注）——
                    # 原版顶层 CUDA 惰性链断言，留着即死；此处替换保证 zip 内唯一条目。
                    if arc.endswith("site-packages/comfy_aimdo/torch.py"):
                        z.writestr(arc, AIMDO_TORCH_STUB)
                        count += 1
                        total += len(AIMDO_TORCH_STUB)
                        print("  [STUB] comfy_aimdo/torch.py 已替换为降级壳（原 CUDA 惰性链剔除）")
                        continue
                    # run82：transformers/utils/auto_docstring.py → 无 regex 空壳
                    #   （见 AUTODOC_STUB 注；zipimport 以首份为准，故必须在此唯一捕获）。
                    if arc.endswith("site-packages/transformers/utils/auto_docstring.py"):
                        z.writestr(arc, AUTODOC_STUB)
                        count += 1
                        total += len(AUTODOC_STUB)
                        print("  [STUB] transformers/utils/auto_docstring.py 已替换为空壳（regex 未闭环）")
                        continue
                    # run82：utils/chat_parsing/__init__.py → 无 regex 空壳
                    #   （见 CHAT_PARSING_STUB 注；为主链经 tokenization_utils_base 必经点）。
                    if arc.endswith(
                            "site-packages/transformers/utils/chat_parsing/__init__.py"):
                        z.writestr(arc, CHAT_PARSING_STUB)
                        count += 1
                        total += len(CHAT_PARSING_STUB)
                        print("  [STUB] transformers/utils/chat_parsing/__init__.py 已替换为空壳")
                        continue
                    # run82：dependency_versions_check.py 剔除 regex 运行时版本检查——
                    #   importlib.metadata.version('regex') 在无 regex 时抛
                    #   PackageNotFoundError → import transformers 直接炸（版本要求
                    #   regex>=2025.10.22 但本栈未收 regex 包，见 AUTODOC_STUB 注）。
                    if arc.endswith("site-packages/transformers/dependency_versions_check.py"):
                        with open(os.path.join(root, f), encoding="utf-8") as _df:
                            _dc = _df.read()
                        _nd = _dc.replace('    "regex",\n', '')
                        if _nd != _dc:
                            z.writestr(arc, _nd)
                            count += 1
                            total += len(_nd)
                            print("  [PATCH] dependency_versions_check.py 已剔除 regex 运行时版本检查")
                        else:
                            print("  !! dependency_versions_check.py 未命中 regex 检查行（版本漂移？）",
                                  file=sys.stderr)
                        continue
                    z.write(os.path.join(root, f), arc)
                    count += 1
                    total += os.path.getsize(os.path.join(root, f))

        # run82 — pyyaml .dist-info 补给：transformers 运行时检查 pyyaml
        #   （importlib.metadata.version，PackageNotFoundError 即炸 —— 检查项见
        #   dependency_versions_check.py pkgs_to_check_at_runtime）。yaml 包本体已由
        #   skh 树进 zip（无 dist-info），此处从 hostcv venv 补元数据文件。
        for _f in ("METADATA", "INSTALLER"):
            _pysrc = os.path.join(VENV_SP, "pyyaml-6.0.3.dist-info", _f)
            if os.path.isfile(_pysrc):
                with open(_pysrc, encoding="utf-8") as _df:
                    z.writestr(
                        f"{PREFIX}/site-packages/pyyaml-6.0.3.dist-info/{_f}", _df.read())
                count += 1
            else:
                print(f"  !! 缺 pyyaml dist-info 源 {_pysrc}", file=sys.stderr)
        print("  [OK] pyyaml-6.0.3.dist-info 元数据已补给（METADATA/INSTALLER）")

    print(f"wrote {OUT}: {count} entries, uncompressed={total / 1e6:.1f} MB")

    # 校验关键条目
    with zipfile.ZipFile(OUT) as z:
        names = set(z.namelist())
        # run82-3：dist-info 唯一性校验（同包多版本并存 = zipimport 首份为准 → 旧版
        #   覆盖新版，importlib.metadata 读到旧版本号 → transformers 依赖检查误报 FAIL）
        _distinfo_dirs = {}
        for _d in names:
            if not _d.startswith(f"{PREFIX}/site-packages/") or not _d.endswith("/METADATA"):
                continue
            _parts = _d[len(f"{PREFIX}/site-packages/"):].split("/")
            if len(_parts) == 2 and _parts[0].endswith(".dist-info"):
                _base = _parts[0][:-10].rsplit("-", 1)[0]
                _distinfo_dirs.setdefault(_base, []).append(_parts[0])
        _multi = {k: v for k, v in _distinfo_dirs.items() if len(v) > 1}
        checks = {
            "stdlib os.py": f"{PREFIX}/os.py" in names,
            "stdlib site.py": f"{PREFIX}/site.py" in names,
            "run82-3 每包 dist-info 唯一(无多版本并存)": not _multi,
            "run82-3 safetensors 仅 0.8.0":
                f"{PREFIX}/site-packages/safetensors-0.8.0.dist-info/METADATA" in names,
            "torch/__init__.py": f"{PREFIX}/site-packages/torch/__init__.py" in names,
            "run73 torchaudio 空壳(真树已剔除)": (
                f"{PREFIX}/site-packages/torchaudio/__init__.py" in names
                and sum(1 for n in names if n.startswith(
                    f"{PREFIX}/site-packages/torchaudio/")) == 1
            ),
            "run74 scipy _propack 子包已补(唯一条目)": (
                f"{PREFIX}/site-packages/scipy/sparse/linalg/_propack/__init__.py" in names
                and sum(1 for n in names if n.startswith(
                    f"{PREFIX}/site-packages/scipy/sparse/linalg/_propack/")) == 1
            ),
            "run77 comfy_aimdo/torch.py 降级壳(唯一条目)": (
                f"{PREFIX}/site-packages/comfy_aimdo/torch.py" in names
                and sum(1 for n in names if n == f"{PREFIX}/site-packages/comfy_aimdo/torch.py") == 1
            ),
            "typing_extensions.py": f"{PREFIX}/site-packages/typing_extensions.py" in names,
            "Step0.4 comfyui/main.py": "comfyui/main.py" in names,
            "run72 sitecustomize.py": f"{PREFIX}/sitecustomize.py" in names,
            "Step0.4 comfyui/server.py": "comfyui/server.py" in names,
            "Step0.4 comfyui/comfy/sd.py": "comfyui/comfy/sd.py" in names,
            "Step0.4 pydantic": f"{PREFIX}/site-packages/pydantic/__init__.py" in names,
            "Step0.4 sqlalchemy": f"{PREFIX}/site-packages/sqlalchemy/__init__.py" in names,
            "Step0.4 psutil.py": f"{PREFIX}/site-packages/psutil.py" in names,
            "Step0.4 tqdm": f"{PREFIX}/site-packages/tqdm/__init__.py" in names,
            "Step0.4 comfy_kitchen": f"{PREFIX}/site-packages/comfy_kitchen/__init__.py" in names,
            "run82 transformers/__init__.py": f"{PREFIX}/site-packages/transformers/__init__.py" in names,
            "run82 huggingface_hub/__init__.py": f"{PREFIX}/site-packages/huggingface_hub/__init__.py" in names,
            "run82 tokenizers/__init__.py": f"{PREFIX}/site-packages/tokenizers/__init__.py" in names,
            "run82 safetensors/__init__.py": f"{PREFIX}/site-packages/safetensors/__init__.py" in names,
            "run82 typer/__init__.py": f"{PREFIX}/site-packages/typer/__init__.py" in names,
            "run82 auto_docstring 唯一(空壳)": sum(1 for n in names
                if n == f"{PREFIX}/site-packages/transformers/utils/auto_docstring.py") == 1,
            "run82 regex 未收集(豁免验证)": not any(
                n.startswith(f"{PREFIX}/site-packages/regex/") for n in names),
            "run82 chat_parsing 唯一(空壳)": sum(1 for n in names
                if n == f"{PREFIX}/site-packages/transformers/utils/chat_parsing/__init__.py") == 1,
            "run82 pyyaml dist-info 补给": (
                f"{PREFIX}/site-packages/pyyaml-6.0.3.dist-info/METADATA" in names),
            "0 个 .so 混入（铁律）": sum(1 for n in names if n.endswith(".so")) == 0,
        }
        for k, v in checks.items():
            print(f"  [{'OK' if v else 'FAIL'}] {k}")
        if sum(1 for n in names if n.endswith(".so")) != 0:
            print("  !! 发现 .so 混入 zip（违反铁律）", file=sys.stderr)
            return 2
        if not all(checks.values()):
            return 2

        # run97-5 — manifest 锚（可复现深检）：
        #   docs/manifests/python312.zip.manifest.gz = sorted-namelist 的 sha256(确定性锚)
        #   + 每条目内容 sha256 逐行 → 新机器重建后与入库 manifest 比对即证完全等价。
        #   （zip 字节 sha 会因 mtime/写入顺序漂移，不作为跨机锚；sorted-namelist sha 是确定性的。）
        import gzip
        import hashlib as _h
        import json as _json
        entries = sorted(z.namelist())
        sorted_hash = _h.sha256("\n".join(entries).encode()).hexdigest()
        per_entry = {}
        for _name in entries:
            per_entry[_name] = _h.sha256(z.read(_name)).hexdigest()
        mani = {
            "zip_sha256": _h.sha256(open(OUT, "rb").read()).hexdigest(),
            "zip_size": os.path.getsize(OUT),
            "entries": len(entries),
            "sorted_namelist_sha256": sorted_hash,
            "per_entry_sha256": per_entry,
        }
        mdir = os.path.join(ROOT, "docs/manifests")
        os.makedirs(mdir, exist_ok=True)
        mpath = os.path.join(mdir, "python312.zip.manifest.gz")
        with open(mpath, "wb") as mf:
            mf.write(gzip.compress(_json.dumps(mani, sort_keys=True).encode()))
        print(f"  [OK] manifest 写 {mpath}")
        print(f"  [MANIFEST] entries={len(entries)} sorted_namelist_sha256={sorted_hash} "
              f"zip_size={os.path.getsize(OUT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
