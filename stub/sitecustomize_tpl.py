"""run72 — 模型层 stub 全局化(sitecustomize 注入,内容并入 python312.zip 的 sitecustomize.py)。

执行时机:CPython 启动最深处(site 模块自动 import,run29/30 已实证机制有效),
任何用户代码(comfy / runpy main.py)之前 ⇒ 无需再走 main_sim 前置注入,真 main.py 零改动。

内容 = main_sim.py run68 版 STUB_SECT 的移植,调整:
  - 去掉 _s.argv / _stg / PING / TOUCHPOINT(那是 sim 前置测试,真 main.py 自带参数自理)
  - _chain 不能 importlib(此刻 comfyui 不在 sys.path)→ 手工建父链:
      每个父链前缀都是普通 ModuleType,__path__ 指向真磁盘目录
      (由 PYTHONHOME 推导 comfyui/comfy),保证 comfy.ldm.modules 白名单真链可被解析;
      叶子 __path__=[] + __getattr__ 兜底 + 元属性真形(run67 教训)。
  - 父链绝不 stub(run68 教训:stub 父包 __path__=[] 会让真链 ModuleNotFoundError)。
  - 'comfy' 自身也绝不 stub:它会被真 import(comfy/__init__.py),只预置其 __path__ 供搜索。
"""
import os as _o
import sys as _s
import types as _types
import time as _t


def _rss():
    try:
        with open('/proc/self/statm') as _f:
            _p = _f.read().split()
        return int(_p[1]) * _o.sysconf('SC_PAGE_SIZE') // 1024
    except Exception:
        return -1


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


def _spec_for(nm):
    """run85（2026-09-03):stub 模块 __spec__ 改真形 ModuleSpec —— 见 stub_global.py 同款注。
    run87:sitecustomize 执行期极早,__import__ 强制加载不依赖 sys.modules 缓存时机。"""

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
    # ⚠ 2026-09-04 本地推理验证: 'comfy.ldm.models.autoencoder' 已从名单移除 ——
    #   SD-Turbo/SD1.5 VAE 构造(comfy/sd.py VAE.__init__ → AutoencoderKL)需要真身;
    #   名单化时整模块占位 → '_StubCls' object is not iterable. import 链全真身(同
    #   stub_global.py 注释),放行即活. 其余 VAE/模型族与 SD-Turbo 无关,保持 stub.
    'comfy.ldm.cascade.stage_a',
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


def _comfy_dir():
    """comfy 包根目录:默认 <PYTHONHOME>/comfyui/comfy;找不到时回退若干常见位置。"""
    _home = _o.environ.get('PYTHONHOME')
    _cands = []
    if _home:
        _cands.append(_o.path.join(_home, 'comfyui', 'comfy'))
    _cands += [
        '/data/storage/el2/base/haps/entry/files/pyroot/comfyui/comfy',
        '/data/storage/el1/bundle/entry/files/pyroot/comfyui/comfy',
    ]
    for _c in _cands:
        if _o.path.isdir(_c):
            return _c
    return None


def _real_dir(prefix_parts):
    """前缀(父链)对应的真磁盘目录:基于 _CDIR 的绝对路径(comfy/ldm/xxx)。
    ⚠ 不能按 cwd 相对拼:sitecustomize 执行于 chdir(comfyui) 之前,相对路径会解析到
    别的目录或不存在 → __path__=[] → comfy.ldm.modules 命名空间包真链断裂
    (run72B 死点 No module named 'comfy.ldm.modules' 即此根)。"""
    _d = _o.path.join(_CDIR, *prefix_parts[1:])
    return _d if _o.path.isdir(_d) else None


_NST = 0
_CDIR = _comfy_dir()
if _CDIR is None:
    print('SIM57-ERR no comfy dir found (PYTHONHOME=%r)' % _o.environ.get('PYTHONHOME'), flush=True)
else:
    # 手工父链:逐级建 ModuleType;目录型前缀 __path__ 指向真磁盘目录(命名空间包/空 init
    #   目录零执行,保真成本为零 —— run68 定稿:父包链绝不 stub)。
    #   叶子则预置 __path__=[] 的 stub(sys.modules 命中即返回,永不走磁盘进死点)。
    for _n in STUB_SECT:
        _parts = _n.split('.')
        # 父链(不含叶子本身)
        _parent = None
        for _i in range(1, len(_parts)):
            _p = '.'.join(_parts[:_i])
            if _p in _s.modules:
                _parent = _s.modules[_p]
                continue
            if _p == 'comfy':
                _pm = _types.ModuleType('comfy')
                _pm.__path__ = [_CDIR]
                _pm.__package__ = 'comfy'
                _pm.__dict__['__file__'] = '<ohos-comfy-pkg>'
                _pm.__dict__['__spec__'] = _spec_for('comfy')  # run85:None→真形 spec
                _pm.__dict__['__loader__'] = None
                _pm.__dict__['__cached__'] = None
                _pm.__dict__['__doc__'] = ''
            else:
                _pm = _types.ModuleType(_p)
                _d = _real_dir(_parts[:_i])
                _pm.__path__ = [_d] if _d else []
                _pm.__package__ = _p
                _pm.__dict__['__file__'] = '<ohos-ns:%s>' % _p
                _pm.__dict__['__spec__'] = _spec_for(_p)  # run85:None→真形 spec
                _pm.__dict__['__loader__'] = None
                _pm.__dict__['__cached__'] = None
                _pm.__dict__['__doc__'] = ''
            _s.modules[_p] = _pm
            if _parent is not None:
                setattr(_parent, _parts[_i - 1], _pm)
            _parent = _pm
        # 叶子 stub
        if _n not in _s.modules:
            _m = _modstub(_n)
            _s.modules[_n] = _m
            setattr(_s.modules['.'.join(_parts[:-1])], _parts[-1], _m)
            _NST += 1
    del _parent
    # run84（2026-09-03):torchvision 全树 stub — zip 内 torchvision 仅有纯 py 装箱罐
    #   (0.25.0),无 _C.so → torch.ops.torchvision 算子不注册,torchvision::nms 不存在
    #   → c10::Error → __cxa_throw hook → _exit(17)(真机 run8: NCP-EXIT pid=59915
    #   signal=17 / CXA17-EXIT what='operator torchvision::nms does not exist')。
    #   主链(server 节点加载期)经 comfy_extras/rtdetr_v4/birefnet/gemma4 等引之,
    #   只要 import 过即可;算子实际调用均在推理期 → 整树 stub(188 名,含包级名)。
    #   注:本段与 stub_global.py 双胞胎,改动须同步。
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
    n = 0
    for _tv in _TV_NAMES:
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
    print('TV-STUB n=%d (run84)' % n, flush=True)
    # run91（2026-09-03):av（PyAV）树 stub —— 与 stub_global.py 双胞胎(缩进差 4)。
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
    _avn = 0
    for _av in _AV_NAMES:
        if _av in _s.modules:
            continue
        _spa = _av.split('.')
        _pa2 = None
        for _i2 in range(1, len(_spa)):
            _q2 = '.'.join(_spa[:_i2])
            _qm2 = _s.modules.get(_q2)
            if _qm2 is None:
                _qm2 = _types.ModuleType(_q2)
                _qm2.__path__ = []
                _qm2.__package__ = _q2
                _qm2.__dict__['__file__'] = '<ohos-av:%s>' % _q2
                _qm2.__dict__['__spec__'] = _spec_for(_q2)
                _qm2.__dict__['__loader__'] = None
                _qm2.__dict__['__cached__'] = None
                _qm2.__dict__['__doc__'] = ''
                _qm2.__dict__['__getattr__'] = _stubattr
                _s.modules[_q2] = _qm2
            if _pa2 is not None:
                setattr(_pa2, _spa[_i2 - 1], _qm2)
            _pa2 = _qm2
        if len(_spa) == 1:
            if _av not in _s.modules:
                _s.modules[_av] = _modstub(_av)
                _avn += 1
            continue
        if _av not in _s.modules:
            _ma = _modstub(_av)
            _s.modules[_av] = _ma
            setattr(_s.modules['.'.join(_spa[:-1])], _spa[-1], _ma)
            _avn += 1
    print('AV-STUB n=%d (run91)' % _avn, flush=True)
    # run92（2026-09-03):av 任意子名兜底 finder —— 与 stub_global.py 双胞胎(缩进差 4)。
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
    # run90（2026-09-03):scipy HiGHS 绑定 stub —— 与 stub_global.py 双胞胎(文本一致,
    #   缩进差 4)。_highs_wrapper 预置,防止真载触碰 libs/_core.so(ObjSense 双注册
    #   → run12 'already registered' 死)。
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
print('SIM57-STUB n=%d (sitecustomize) rss=%dKB' % (
    _NST, _rss(), ), flush=True)

# ── NPU-P0 诊断(2026-09-11): 自建 torch 扩展在 Python 进程视图里是否可见 ──
#   背景: torch 的 out-of-tree 后端自动加载报
#     ImportError: Error loading shared library .../site-packages/_nnrt_bootstrap.so:
#                  No such file or directory
#   而同一文件经 hdc shell 视图可见、md5 与宿主产物一致 —— 需区分「文件在该进程视图
#   不可见」与「依赖解析失败」两类。此处刻意在 torch 加载**之前**用 ctypes 试探:
#   若为依赖问题, 错误措辞会点名缺失的库; 若为文件问题, 则是纯 ENOENT。
#   每步 print(flush), 输出进 diag.log —— 不依赖任何文件写入是否成功。
print('NNRT-DIAG A start', flush=True)
try:
    _nd = _o.path.dirname(_o.__file__)                  # .../lib/python3.12
    print('NNRT-DIAG B lib=%s' % _nd, flush=True)
    _np = _o.path.join(_nd, 'site-packages', '_nnrt_bootstrap.so')
    print('NNRT-DIAG C target=%s exists=%s' % (_np, _o.path.exists(_np)), flush=True)
    try:
        _nst = _o.stat(_np)
        print('NNRT-DIAG D size=%d mode=%o' % (_nst.st_size, _nst.st_mode), flush=True)
    except BaseException as _ne:
        print('NNRT-DIAG D stat-exc %r' % (_ne,), flush=True)
    print('NNRT-DIAG E sys.path[:3]=%r' % (_s.path[:3],), flush=True)
    # 写文件验证(两处候选, 逐一报告成败)
    for _cand in (_o.path.join(_o.path.dirname(_nd), 'nnrt-diag.log'),
                  _o.path.join(_nd, 'nnrt-diag.log')):
        try:
            with open(_cand, 'w') as _nf:
                _nf.write('ok\n')
            print('NNRT-DIAG F wrote=%s' % _cand, flush=True)
            break
        except BaseException as _ne:
            print('NNRT-DIAG F write-exc %s :: %r' % (_cand, _ne), flush=True)
    # dlopen 试探(此时 torch 未加载: 依赖问题会点名缺失库, 文件问题则是 ENOENT)
    try:
        import ctypes as _ct
        print('NNRT-DIAG G ctypes-ok', flush=True)
        try:
            _nh = _ct.CDLL(_np)
            print('NNRT-DIAG H dlopen=OK', flush=True)
        except BaseException as _ne:
            print('NNRT-DIAG H dlopen-exc %r' % (_ne,), flush=True)
    except BaseException as _ne:
        print('NNRT-DIAG G ctypes-exc %r' % (_ne,), flush=True)
except BaseException as _ne:
    print('NNRT-DIAG X outer-exc %r' % (_ne,), flush=True)
