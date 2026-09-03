"""run72 方案 B — 模型层 stub 全局化(run_path 前置注入,替代/并行于 sitecustomize)。

sitecustomize 机制本轮实测未如期生效(无 SIM57-STUB / 无 Error in sitecustomize,原因未明);
本文件走已验证路径:C++ 注入段在 run_path(main.py) 之前先 run_path 本文件,进程内预置
sys.modules,后续真 import 命中即放行 —— 机制与 main_sim.py run68+ 版同源。

语义(与 main_sim 定稿一致):
  - _chain(nm):按 import 系统语义父链逐级真 import。comfy 与 comfy.ldm 及全部模型族
    均为 PEP 420 命名空间包(无 __init__.py,零执行,本轮又实证 comfy/__init__.py 不存在),
    父链保真成本为零 → 父包绝不 stub(run68 教训)。叶子预置前 _chain 让属性链成立
    (run69:CPython 预置命中不建父链、不 setattr)。
  - 叶子 _modstub:__path__=[] + 模块 __getattr__ 兜底(run67 + bump)、元属性真形
    (__file__='<stub:...>' / __spec__/__loader__/__cached__=None / __doc__='')。
  - _StubCls 元类链式(_MetaStub)防 `from stub import X; X.attr` 崩(run67)。
  - comfy.ldm.modules 及三个关键子包(distributions/encoders/diffusionmodules)是真身:
    它们不在 STUB_SECT,import 时经父链 __path__ + 磁盘目录正常加载。
"""
import importlib as _il
import os as _o
import sys as _s
import types as _types


def _rss():
    try:
        with open('/proc/self/statm') as _f:
            _p = _f.read().split()
        return int(_p[1]) * _o.sysconf('SC_PAGE_SIZE') // 1024
    except Exception:
        return -1


def _spec_for(nm):
    """run85（2026-09-03):stub 模块 __spec__ 改真形 ModuleSpec。
    run9 死点:transformers/utils/import_utils._is_package_available 调
    importlib.util.find_spec('torchvision') —— CPython 对『sys.modules 已预置
    且 __spec__ is None』的模块直接 raise ValueError('torchvision.__spec__ is None')
    → import transformers 抛 → comfy.sd1_clip 等 4 个 WARM-FAIL。
    真形 spec(loader=None)让 find_spec 直接返回 → is_torchvision_available
    变 True(假阳性,benign:transformers 主包走『可用』分支,不 warn 不 raise)。
    run87（2026-09-03):sitecustomize 执行在启动极早期,importlib.machinery 可能
    尚未注册进 sys.modules(host 3.14 模拟:pop machinery 后 bootstrap 兜底成功;
    真机 run10 的 4-FAIL 仍在 → 真机 3.12 该期连 _bootstrap 都不在?)→
    改用 __import__ 强制加载,不依赖 sys.modules 缓存时机。"""
    _ms = None
    for _t in ('importlib.machinery', 'importlib._bootstrap', 'importlib._bootstrap_external'):
        _mod = _s.modules.get(_t)
        if _mod is None:
            try:
                _mod = __import__(_t, None, None, ['*'])
            except BaseException:
                _mod = None
        if _mod is not None:
            _ms = getattr(_mod, 'ModuleSpec', None)
            if _ms is not None:
                break
    if _ms is None:
        return None
    try:
        return _ms(nm, None)
    except BaseException:
        return None


class _MetaStub(type):
    """元类:类对象本身的属性访问也链式(防 `from stub import X; X.attr` 崩)。"""

    def __getattr__(cls, n):
        return cls


class _StubCls(metaclass=_MetaStub):
    """占位类:任何 attr 访问都是自身(可链式),实例化/调用均可。"""

    def __init__(self, *a, **k):
        pass

    def __call__(self, *a, **k):
        return self

    def __getattr__(self, n):
        return self


def _stubattr(name):
    return _StubCls


def _modstub(nm):
    m = _types.ModuleType(nm)
    m.__path__ = []
    m.__package__ = ''
    m.__dict__['__getattr__'] = _stubattr  # PEP 562:模块 attr miss → 兜底
    # run67 教训:元属性必须显式是真形(inspect.getmodule/getabsfile 取 __file__ 做
    # endswith;__spec__/__loader__ 有 None 语义),不能靠 __getattr__ 兜底返回类。
    m.__dict__['__file__'] = '<stub:%s>' % nm
    m.__dict__['__spec__'] = _spec_for(nm)  # run85:None→真形 spec（见 _spec_for 注）
    m.__dict__['__loader__'] = None
    m.__dict__['__cached__'] = None
    m.__dict__['__doc__'] = ''
    return m


STUB_SECT = [
    # model_base.py (59)
    'comfy.ldm.hunyuan3dv2_1', 'comfy.ldm.hunyuan3dv2_1.hunyuandit',
    'comfy.ldm.lightricks.av_model', 'comfy.ldm.minimax.model',
    'comfy.ldm.minimax_music.dit', 'comfy.ldm.lightricks.symmetric_patchifier',
    'comfy.ldm.cascade.stage_c', 'comfy.ldm.cascade.stage_b',
    'comfy.ldm.genmo.joint_model.asymm_models_joint',
    'comfy.ldm.aura.mmdit', 'comfy.ldm.pixart.pixartms', 'comfy.ldm.hydit.models',
    'comfy.ldm.audio.dit', 'comfy.ldm.audio.embedders', 'comfy.ldm.flux.model',
    'comfy.ldm.lens.model', 'comfy.ldm.lightricks.model', 'comfy.ldm.hunyuan_video.model',
    'comfy.ldm.cosmos.model', 'comfy.ldm.cosmos.predict2', 'comfy.ldm.lumina.model',
    'comfy.ldm.wan.model', 'comfy.ldm.wan.model_animate', 'comfy.ldm.wan.model_animate2',
    'comfy.ldm.wan.ar_model', 'comfy.ldm.wan.model_wandancer', 'comfy.ldm.hunyuan3d.model',
    'comfy.ldm.triposplat.model', 'comfy.ldm.hidream.model', 'comfy.ldm.chroma.model',
    'comfy.ldm.chroma_radiance.model', 'comfy.ldm.pixeldit.model', 'comfy.ldm.pixeldit.pid',
    'comfy.ldm.ace.model', 'comfy.ldm.omnigen.omnigen2', 'comfy.ldm.seedvr.model',
    'comfy.ldm.boogu.model', 'comfy.ldm.qwen_image.model', 'comfy.ldm.mage_flow.model',
    'comfy.ldm.joyimage.model', 'comfy.ldm.ideogram4.model', 'comfy.ldm.krea2.model',
    'comfy.ldm.kandinsky5.model', 'comfy.ldm.anima.model', 'comfy.ldm.trellis2.model',
    'comfy.ldm.ace.ace_step15', 'comfy.ldm.cogvideo.model', 'comfy.ldm.rt_detr.rtdetr_v4',
    'comfy.ldm.ernie.model', 'comfy.ldm.sam3.detector',
    'comfy.ldm.hidream_o1.conditioning', 'comfy.ldm.sensenova.conditioning',
    'comfy.ldm.sensenova.model', 'comfy.ldm.sensenova.sampling',
    'comfy.ldm.depth_anything_3.model',
    # sd.py (25)
    'comfy.ldm.models.autoencoder', 'comfy.ldm.cascade.stage_a',
    'comfy.ldm.cascade.stage_c_coder', 'comfy.ldm.audio.autoencoder',
    'comfy.ldm.genmo.vae.model', 'comfy.ldm.lightricks.vae.causal_video_autoencoder',
    'comfy.ldm.lightricks.vae.na_diffusion_decoder', 'comfy.ldm.lightricks.vae.audio_vae',
    'comfy.ldm.cosmos.vae', 'comfy.ldm.wan.vae', 'comfy.ldm.trellis2.vae',
    'comfy.ldm.wan.vae2_2', 'comfy.ldm.hunyuan3d.vae', 'comfy.ldm.seedvr.vae',
    'comfy.ldm.mage_flow.vae', 'comfy.ldm.triposplat.vae',
    'comfy.ldm.ace.vae.music_dcae_pipeline', 'comfy.ldm.cogvideo.vae',
    'comfy.ldm.hunyuan_video.vae', 'comfy.ldm.mmaudio.vae.autoencoder',
    'comfy.ldm.audio.vae_sa3', 'comfy.ldm.minimax_music.dav', 'comfy.ldm.minimax.vae',
    'comfy.ldm.minimax.audio_vae', 'comfy.ldm.flux.redux',
]


def _chain(nm):
    """按 import 系统语义把父链逐级真 import(命名空间包/空 init 零执行),
    让 import 系统把属性挂到父级;否则属性链 comfy.ldm.flux.model 会断。"""
    _parts = nm.split('.')
    for _i in range(1, len(_parts)):
        _p = '.'.join(_parts[:_i])
        if _p not in _s.modules:
            _il.import_module(_p)


_NST = 0
try:
    for _n in STUB_SECT:
        if _n in _s.modules:  # 叶子已有真身(如 comfy.ldm.modules 前缀),不动
            continue
        _chain(_n)  # 先建父链(CPython 预置命中不做父链,须主动建)
        _m = _modstub(_n)
        _s.modules[_n] = _m
        _pp = _n.rsplit('.', 1)[0]
        if _pp in _s.modules:
            setattr(_s.modules[_pp], _n.rsplit('.', 1)[1], _m)
        _NST += 1
except BaseException as _e:
    import traceback as _tb
    print('STUB-GLOBAL-PARTIAL n=%d err=%r' % (_NST, _e), flush=True)
    print('--TB--\n%s--TB-END--' % _tb.format_exc(), flush=True)
    raise

# run84（2026-09-03):torchvision 全树 stub — zip 内 torchvision 仅有纯 py 装箱罐
#   (0.25.0),无 _C.so → torch.ops.torchvision 算子不注册,torchvision::nms 不存在
#   → c10::Error → __cxa_throw hook → _exit(17)(真机 run8: NCP-EXIT pid=59915
#   signal=17 / CXA17-EXIT what='operator torchvision::nms does not exist')。
#   主链(server 节点加载期)经 comfy_extras/rtdetr_v4/birefnet/gemma4 等引之,
#   只要 import 过即可;算子实际调用均在推理期 → 整树 stub(188 名,含包级名)。
#   注:本段在 sitecustomize_tpl.py 与 stub_global.py 双胞胎存在,改动须同步。
_TV_NAMES = (
'__init__', '_internally_replaced_utils', '_meta_registrations', '_utils', 'datasets', 'datasets._optical_flow',
'datasets._stereo_matching', 'datasets.caltech', 'datasets.celeba', 'datasets.cifar', 'datasets.cityscapes', 'datasets.clevr',
'datasets.coco', 'datasets.country211', 'datasets.dtd', 'datasets.eurosat', 'datasets.fakedata', 'datasets.fer2013',
'datasets.fgvc_aircraft', 'datasets.flickr', 'datasets.flowers102', 'datasets.folder', 'datasets.food101', 'datasets.gtsrb',
'datasets.hmdb51', 'datasets.imagenet', 'datasets.imagenette', 'datasets.inaturalist', 'datasets.kinetics', 'datasets.kitti',
'datasets.lfw', 'datasets.lsun', 'datasets.mnist', 'datasets.moving_mnist', 'datasets.omniglot', 'datasets.oxford_iiit_pet',
'datasets.pcam', 'datasets.phototour', 'datasets.places365', 'datasets.rendered_sst2', 'datasets.samplers', 'datasets.samplers.clip_sampler',
'datasets.sbd', 'datasets.sbu', 'datasets.semeion', 'datasets.stanford_cars', 'datasets.stl10', 'datasets.sun397',
'datasets.svhn', 'datasets.ucf101', 'datasets.usps', 'datasets.utils', 'datasets.video_utils', 'datasets.vision',
'datasets.voc', 'datasets.widerface', 'extension', 'io', 'io._load_gpu_decoder', 'io._video_deprecation_warning',
'io._video_opt', 'io.image', 'io.video', 'io.video_reader', 'models', 'models._api',
'models._meta', 'models._utils', 'models.alexnet', 'models.convnext', 'models.densenet', 'models.detection',
'models.detection._utils', 'models.detection.anchor_utils', 'models.detection.backbone_utils', 'models.detection.faster_rcnn', 'models.detection.fcos', 'models.detection.generalized_rcnn',
'models.detection.image_list', 'models.detection.keypoint_rcnn', 'models.detection.mask_rcnn', 'models.detection.retinanet', 'models.detection.roi_heads', 'models.detection.rpn',
'models.detection.ssd', 'models.detection.ssdlite', 'models.detection.transform', 'models.efficientnet', 'models.feature_extraction', 'models.googlenet',
'models.inception', 'models.maxvit', 'models.mnasnet', 'models.mobilenet', 'models.mobilenetv2', 'models.mobilenetv3',
'models.optical_flow', 'models.optical_flow._utils', 'models.optical_flow.raft', 'models.quantization', 'models.quantization.googlenet', 'models.quantization.inception',
'models.quantization.mobilenet', 'models.quantization.mobilenetv2', 'models.quantization.mobilenetv3', 'models.quantization.resnet', 'models.quantization.shufflenetv2', 'models.quantization.utils',
'models.regnet', 'models.resnet', 'models.segmentation', 'models.segmentation._utils', 'models.segmentation.deeplabv3', 'models.segmentation.fcn',
'models.segmentation.lraspp', 'models.shufflenetv2', 'models.squeezenet', 'models.swin_transformer', 'models.vgg', 'models.video',
'models.video.mvit', 'models.video.resnet', 'models.video.s3d', 'models.video.swin_transformer', 'models.vision_transformer', 'ops',
'ops._box_convert', 'ops._register_onnx_ops', 'ops._utils', 'ops.boxes', 'ops.ciou_loss', 'ops.deform_conv',
'ops.diou_loss', 'ops.drop_block', 'ops.feature_pyramid_network', 'ops.focal_loss', 'ops.giou_loss', 'ops.misc',
'ops.poolers', 'ops.ps_roi_align', 'ops.ps_roi_pool', 'ops.roi_align', 'ops.roi_pool', 'ops.stochastic_depth',
'torchvision', 'transforms', 'transforms._functional_pil', 'transforms._functional_tensor', 'transforms._functional_video', 'transforms._presets',
'transforms._transforms_video', 'transforms.autoaugment', 'transforms.functional', 'transforms.transforms', 'transforms.v2', 'transforms.v2._augment',
'transforms.v2._auto_augment', 'transforms.v2._color', 'transforms.v2._container', 'transforms.v2._deprecated', 'transforms.v2._geometry', 'transforms.v2._meta',
'transforms.v2._misc', 'transforms.v2._temporal', 'transforms.v2._transform', 'transforms.v2._type_conversion', 'transforms.v2._utils', 'transforms.v2.functional',
'transforms.v2.functional._augment', 'transforms.v2.functional._color', 'transforms.v2.functional._deprecated', 'transforms.v2.functional._geometry', 'transforms.v2.functional._meta', 'transforms.v2.functional._misc',
'transforms.v2.functional._temporal', 'transforms.v2.functional._type_conversion', 'transforms.v2.functional._utils', 'tv_tensors', 'tv_tensors._bounding_boxes', 'tv_tensors._dataset_wrapper',
'tv_tensors._image', 'tv_tensors._keypoints', 'tv_tensors._mask', 'tv_tensors._torch_function_helpers', 'tv_tensors._tv_tensor', 'tv_tensors._video',
'utils', 'version',
)
# run86（2026-09-03):名单修正 —— 老名单把 torchvision 树内相对名直接当顶层注册:
#   'utils'/'version'/'io'/'models' 等裸名被 _modstub 注册到 sys.modules 顶层 →
#   run9/run10 死点实锤:main 链 app/database/db.py:5 `from utils.install_util import ...`
#   命中 <stub:utils>(__path__=[])→ ModuleNotFoundError('utils.install_util')。
#   统一还原为真名:裸名加 'torchvision.' 前缀;'__init__' 是 torchvision/__init__.py
#   的误生成,与 'torchvision' 一道归并到 'torchvision' 本身。
_TV_NAMES = tuple(
    'torchvision' if _x in ('torchvision', '__init__') else ('torchvision.' + _x)
    for _x in _TV_NAMES
)
_TV_NAMES = tuple(dict.fromkeys(_TV_NAMES))  # 去重（同名合并）

def _tree_stub(_names):
    n = 0
    for _tv in _names:
        if _tv in _s.modules:
            continue
        _sp = _tv.split('.')
        _pa = None
        for _i in range(1, len(_sp)):
            _q = '.'.join(_sp[:_i])
            _qm = _s.modules.get(_q)
            if _qm is None:
                _qm = _types.ModuleType(_q)
                _qm.__path__ = []
                _qm.__package__ = _q
                _qm.__dict__['__file__'] = '<ohos-tv:%s>' % _q
                _qm.__dict__['__spec__'] = _spec_for(_q)  # run85:None→真形 spec
                _qm.__dict__['__loader__'] = None
                _qm.__dict__['__cached__'] = None
                _qm.__dict__['__doc__'] = ''
                _qm.__dict__['__getattr__'] = _stubattr  # PEP 562:父链 stub 也兜底
                _s.modules[_q] = _qm
            if _pa is not None:
                setattr(_pa, _sp[_i - 1], _qm)
            _pa = _qm
        if len(_sp) == 1:
            if _tv not in _s.modules:
                _s.modules[_tv] = _modstub(_tv)
                n += 1
            continue
        if _tv not in _s.modules:
            _m = _modstub(_tv)
            _s.modules[_tv] = _m
            setattr(_s.modules['.'.join(_sp[:-1])], _sp[-1], _m)
            n += 1
    return n

_TV_N = _tree_stub(_TV_NAMES)
print('TV-STUB n=%d (run84)' % _TV_N, flush=True)

# run91（2026-09-03):av（PyAV）树 stub —— av 纯 py 在 zip、但 av/_core 扩展与 ffmpeg
#   动态库均未入 libs,`_core` 短名与 scipy._highspy._core 相撞(libs 里只有 highspy 版)
#   → av._core 被劫持 → run15 'cannot import name time_base' 死。主链 comfy_api/
#   video_types.py:6 `import av` 必经;视频解码属推理期功能 → 整树 stub。
_AV_NAMES = (
'av', 'av.__main__', 'av.about', 'av._core',
'av.audio', 'av.audio.codeccontext', 'av.audio.format', 'av.audio.frame',
'av.audio.plane', 'av.audio.resampler', 'av.audio.stream',
'av.codec', 'av.container', 'av.container.output', 'av.datasets',
'av.filter', 'av.filter.loudnorm', 'av.frame', 'av.packet',
'av.sidedata', 'av.stream', 'av.subtitles', 'av.subtitles.codeccontext',
'av.subtitles.stream', 'av.subtitles.subtitle', 'av.utils',
'av.video', 'av.video.frame', 'av.video.stream',
)
_AV_N = _tree_stub(_AV_NAMES)
print('AV-STUB n=%d (run91)' % _AV_N, flush=True)

# run92（2026-09-03):av 任意子名兜底 finder —— 树 stub 只覆盖已知叶名,主链
#   comfy_api/_input_impl/video_types.py:1 要 `from av.bitstream import ...`(zip 内
#   av 树缺该 py,PyAV 打包不全)→ 父 __path__=[] 找不到 → run16 死。凡 'av.' 前缀
#   未预置名当场 _modstub 预置(视频解码是推理期功能,主链只求 import 过)。
class _AvSubFinder:
    def find_spec(self, fullname, path=None, target=None):
        if fullname == 'av' or not fullname.startswith('av.'):
            return None
        if fullname in _s.modules:
            return None
        try:
            _mach = __import__('importlib.machinery', None, None, ['*'])
            return _mach.ModuleSpec(fullname, _AvLoader())
        except BaseException:
            return None


class _AvLoader:
    def create_module(self, spec):
        return None

    def exec_module(self, module):
        # 保持空壳 stub 语义:__getattr__ 兜底 + 元属性真形(与 _modstub 一致)
        _nm = module.__name__
        module.__dict__['__getattr__'] = _stubattr
        module.__path__ = []
        module.__package__ = _nm
        module.__dict__['__file__'] = '<ohos-av:%s>' % _nm
        module.__dict__['__spec__'] = getattr(module, '__spec__', None) or _spec_for(_nm)
        module.__dict__['__loader__'] = None
        module.__dict__['__cached__'] = None
        module.__dict__['__doc__'] = ''
        _pa = _nm.rsplit('.', 1)[0]
        if _pa:
            _pm = _s.modules.get(_pa)
            if _pm is not None:
                setattr(_pm, _nm.rsplit('.', 1)[1], module)


try:
    _s.meta_path.insert(0, _AvSubFinder())
except BaseException:
    pass
print('AVSUBFINDER installed (run92)', flush=True)

# run90（2026-09-03):scipy HiGHS 绑定 stub —— zip 内 _highspy 树缺 _highs_options.py,
#   _highs_wrapper.py 真载会 import scipy.optimize._highspy._core(libs 的 _core.so,
#   pybind 高类型注册)→ 主链运行期即死 run12: 'generic_type: type "ObjSense" is
#   already registered!'。主链仅经 scipy.optimize 惰性链触达(推理期 linprog 才真正
#   求解)→ 叶子 _highs_wrapper 预置 stub;_highspy 真包(空 __init__)照常导入,子名
#   命中 sys.modules 即放行,永不走磁盘链进 _core.so。
_FERRY = ('scipy.optimize._highspy._core', 'scipy.optimize._highspy._highs_wrapper')
_FN = 0
for _f in _FERRY:
    if _f in _s.modules:
        continue
    _fm = _modstub(_f)
    _s.modules[_f] = _fm
    _fp = _f.rsplit('.', 1)[0]
    _fpm = _s.modules.get(_fp)
    if _fpm is not None:
        setattr(_fpm, _f.rsplit('.', 1)[1], _fm)
    _FN += 1
print('FERRY-STUB n=%d (run90)' % _FN, flush=True)
print('TV-STUB-PRE n=%d (run85)' % sum(1 for _x in _TV_NAMES if _x in _s.modules), flush=True)

# run87（2026-09-03):spec 链自证 —— run10 中 WARM 4-FAIL 报 'torchvision.__spec__ is None'
#   与本机模拟(成功)矛盾;stub_global 执行期(importlib 完备)自证 find_spec 行为,
#   区分『sitecustomize 期 _spec_for 失效』vs『WARM 段 _imp 覆盖』。
try:
    import importlib.util as _iu
    _fr = _iu.find_spec('torchvision')
    print('TV-SPEC-CHK find_spec-ok=%s torchvision.__spec__=%r' % (
        _fr is not None, getattr(_s.modules.get('torchvision'), '__spec__', 'NOMOD')), flush=True)
except BaseException as _fe:
    print('TV-SPEC-CHK err=%r' % (_fe,), flush=True)

# run85（2026-09-03):utils 顶层探针 —— run9 死点:app/database/db.py:5
#   from utils.install_util import ... → ModuleNotFoundError('utils.install_util'),
#   而 zip 内 comfyui/utils/(5 条含 install_util.py)存在 → 需区分盘上缺失/
#   树不完整/他处劫持;以应用 uid 列盘一次定位(run10 同时对照卸载重装)。
try:
    import utils as _u
    print('UTILS-FOUND __file__=%r path=%r' % (
        getattr(_u, '__file__', '?'), getattr(_u, '__path__', '?')), flush=True)
except BaseException as _ue:
    _R = '/data/storage/el2/base/haps/entry/files/pyroot/comfyui'
    try:
        _L = _o.listdir(_R)
    except BaseException as _le:
        print('UTILS-PROBE import-err=%r listdir-err=%r' % (_ue, _le), flush=True)
    else:
        _HU = 'utils' in _L
        _SUB = 'ERR'
        if _HU:
            try:
                _SUB = _o.listdir(_R + '/utils')[:30]
            except BaseException as _se:
                _SUB = 'ERR %r' % _se
        print('UTILS-PROBE listdir-n=%d has_utils=%s utils=%s' % (
            len(_L), _HU, _SUB), flush=True)

print('SIM57-STUB n=%d (run_path) rss=%dKB' % (_NST, _rss()), flush=True)
