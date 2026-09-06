# Phase 0 最小验证工程
cmake_minimum_required(VERSION 3.5.0)
project(comfyohos)
set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})
if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()
include_directories(${NATIVERENDER_ROOT_PATH} ${NATIVERENDER_ROOT_PATH}/include)

# Step 0.3b：comfy_child 需 <Python.h>（作为嵌入式宿主把 libs/ 里的 C++ 扩展 dlopen+PyInit+注册进 sys.modules）。
#               切到 skh 的 Python 3.12 + libc++ __1 栈。
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../../build/skh-run-extract/skh-run/usr/include/python3.12)
# Step 0.3c：pybind11 头（与 torch 同源：拷自 skh-run usr/include/pybind11，含 OHOS 定制 type_caster_pyobject_ptr.h）。
#   ⚠ 不能直接 -I skh-run/usr/include——那会引入 glibc 板式系统头与 musl sysroot 互踩（netinet/in.h、
#   nullptr_t 等已实证）。只把 pybind11/ 子目录拷进 prebuilt/include/ 后以它为 include 根。
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/include)

# 父进程（ArkUI）：NAPI 拉起 NCP 子进程
add_library(entry SHARED
    napi_init.cpp
    nnrt_run.cpp)   # P0 执行探针(2026-09-06): 官方 Add 单算子构图→执行, dlopen 式
target_link_libraries(entry PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    libchild_process.so
    libdl.so)

# ─────────────────────────────────────────────────────────────────────────────
# Step 0.3b/c：torch 栈（skh 的 Python 3.12 + libc++ __1）+ lib-dynload 内建扩展，全部打进 HAP libs/<abi>/。
#
# ⚠ 打包机制（已实测）：OHOS 打包器只扫「built .so 的直接 NEEDED 闭包」，且不自动打包传递依赖
#   （旧 manifest 未含 libpython 的传递依赖 libintl 即证）。因此凡是要进 libs/ 的 .so
#   **必须被直接 LINK 进 comfy_child 成为其直接 NEEDED**。
# ⚠ 代价：直接 NEEDED => comfy_child 启动时被 eager 加载，全闭包必须闭环（缺一即启动崩）。
#   故下面把 torch 栈 + lib-dynload + 其依赖库全部直接链接，保证启动解析闭环。
# ⚠ 铁律：一切被加载 .so 必须打包进 HAP libs/<abi>/；纯 .py 走 rawfile→filesDir。
# ⚠ 命名空间：真实 torch 栈链「系统 libc++.so.1 = std::__1」（非应用 libc++_shared.so = __n1）。
#   comfy_child 用 -nostdlib++ 禁 clang++ 隐式链 __n1 的 libc++_shared，改显式链 libc++.so.1(__1)，
#   全进程统一 __1，避免同进程双 libc++（命门③碰撞）。工具链 SDK clang 15.0.4 与 skh 栈同版。
# ─────────────────────────────────────────────────────────────────────────────

set(PRE "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt")
# 把 prebuilt/ 下单文件导入为 SHARED IMPORTED target（IMPORTED_LOCATION 决定进 libs/ 的拷贝名=文件basename）
function(import_so tgt relpath)
    add_library(${tgt} SHARED IMPORTED)
    set_target_properties(${tgt} PROPERTIES IMPORTED_LOCATION "${PRE}/${relpath}")
endfunction()

# 系统级 libc++ __1 栈（libtorch_cpu.so 等 NEEDED 它们，须锁进 libs/ 按 SONAME 闭环）
import_so(libcxx1     "libc++.so.1")
import_so(libcxxabi1  "libc++abi.so.1")
import_so(libunwind1  "libunwind.so.1")
import_so(libcxx_shared "libc++_shared.so")   # G2 重建 torch 的 C++ 运行时(SDK __n1 系, 新 NEEDED)
import_so(libomp      "libomp.so")
# libpython 3.12（skh 版，纯 C 不链 libc++）+ 其 NEEDED 的 libintl（打包器不自动带传递依赖，故显式）
import_so(libpython   "libpython3.12.so.1.0")
import_so(libintl     "libintl.so.8")
# torch 核心 .so（_C 由宿主桥 dlopen；其余是它 NEEDED 的链）
import_so(libtorch_cpu       "torch/libtorch_cpu.so")
import_so(libtorch_python    "torch/libtorch_python.so")
import_so(libc10             "torch/libc10.so")
import_so(libtorch           "torch/libtorch.so")
import_so(libshm             "torch/libshm.so")
import_so(libtorch_global_deps "torch/libtorch_global_deps.so")   # torch/__init__.py._load_global_deps 经 ctypes.CDLL(短名) 从 libs/ 加载
# lib-dynload 内建扩展运行时的系统库依赖（skh 编的，须一并打包保证 eager 加载闭环）
# ⚠ 传递依赖：libncursesw/libpanelw 还 NEEDED libtinfow.so.6（terminfo）——打包器不自动带传递依赖，
#   首次真机加载即崩（MUSL-LDSO: Error loading libtinfow.so.6）。故显式补链，形成完整闭环。
import_so(libz         "libz.so.1")
import_so(libbz2       "libbz2.so.1.0")
import_so(liblzma      "liblzma.so.5")
import_so(libffi       "libffi.so.8")
import_so(libcrypto    "libcrypto.so.3")
import_so(libssl       "libssl.so.3")
import_so(libncursesw  "libncursesw.so.6")
import_so(libpanelw    "libpanelw.so.6")
import_so(libtinfow    "libtinfow.so.6")

# ─────────────────────────────────────────────────────────────────────────────
# 无 SONAME 扩展导入批量器（torch._C + lib-dynload 内建 70 + numpy 19 + scipy 113 +
#   PIL 4 + _pydantic_core + _safetensors_rust + tokenizers —— 全部「无 SONAME」.so）。
#
# ★ 机制设计（2026-09-03 定谳 —— 替代旧的「-l: 短名 + 改 hvigor 插件」方案；
#   /apps/harmony 的 hvigor 插件已还原为官方 6.24.2 原版，md5 705309b0…）：
#   1. 每个扩展一个 IMPORTED target，IMPORTED_LOCATION=prebuilt 绝对路径
#      → CMake link 命令以【绝对路径】出现在 codemodel「libraries」fragment
#      → hvigor 官方版 findRuntimeFiles 对「非 sysroot 的绝对路径」直接收集进 HAP libs/<abi>/
#      （旧 24-so 事故根因 = 用 -l:xxx 带 '-' 前缀，被 handleBuildDir=join(build,"-l:xxx") 丢弃）。
#   2. 这批扩展绝不进 comfy_child 的 DT_NEEDED：无 SONAME 的路径输入一旦 NEEDED，
#      记录的是「构建机绝对路径」，真机 loader 按该路径找不到 → 加载崩（Step 0.3a 已踩；
#      so 未经 patchelf、SONAME 也无法补）。
#      → 解法：comfy_child 追加 -Wl,--as-needed（见下 target_link_options）。comfy_child 对
#      本批扩展零符号引用（全是 PyInit_xxx，由 meta_path finder 从 libs/ 按短名
#      dlopen+PyInit+注册进 sys.modules，与 torch._C 同构）→ as-needed 修剪掉这些 NEEDED；
#      收集走「libraries」fragment 与 NEEDED 无关，236 个 .so 照旧落位 HAP libs/。
#   3. 铁律不变：一切被加载的 .so 必须打包进 HAP libs/<abi>/；纯 .py 走 rawfile→filesDir。
# ⚠ 不得加 link_directories(-L)：CMake 会把「-L 目录内可解析的绝对路径」规范化为 -l<basename>
#   （去找 lib_xxx.so 而失败）。不加 -L 时 IMPORTED 目标原样输出绝对路径。
# ─────────────────────────────────────────────────────────────────────────────
set(EXT_N 0)
function(import_ext_so relpath outvar)
    math(EXPR EXT_N2 "${EXT_N}+1")
    set(EXT_N ${EXT_N2} PARENT_SCOPE)
    set(tgt "extn${EXT_N2}")
    add_library(${tgt} SHARED IMPORTED)
    set_target_properties(${tgt} PROPERTIES IMPORTED_LOCATION "${PRE}/${relpath}")
    set(_cur ${${outvar}})          # 读取父作用域已有 list（列表项无空格，直接展开）
    list(APPEND _cur ${tgt})
    set(${outvar} "${_cur}" PARENT_SCOPE)
endfunction()

# torch._C + lib-dynload 的 70 个内建扩展
set(NOSONAME_LIBS "")
import_ext_so("torch/_C.cpython-312-aarch64-linux-ohos.so" NOSONAME_LIBS)
file(GLOB DYNLOAD_SO "${PRE}/dynload/*.so")
foreach(MODSO ${DYNLOAD_SO})
    get_filename_component(FN ${MODSO} NAME)
    import_ext_so("dynload/${FN}" NOSONAME_LIBS)
endforeach()

# numpy 1.26.4 C 扩展（skh 栈同源 aarch64-ohos）：无 SONAME、仅 NEEDED libc.so（系统库，无需补依赖）。
# 与 dynload 同理经 import_ext_so 扫进 HAP libs/<abi>/；
# 运行时由 sys.meta_path finder（probe 内 install）把 numpy.* 的 dotted 名重定向到 libs/ 绝对路径。
set(NUMPY_SO_LIBS "")
file(GLOB NUMPY_SO "${PRE}/numpy/*.so")
foreach(MODSO ${NUMPY_SO})
    get_filename_component(FN ${MODSO} NAME)
    import_ext_so("numpy/${FN}" NUMPY_SO_LIBS)
endforeach()

# ─────────────────────────────────────────────────────────────────────────────
# Step 0.4c：scipy 114 个 C 扩展（skh 栈同源 aarch64-ohos，平铺 prebuilt/scipy/）。
#   均无 SONAME、NEEDED 集 = libc.so + libc++/libc++abi/libunwind（三者已有 import_so 锁 libs/）。
#   与 numpy 同流程经 import_ext_so → hap libs/arm64；finder 短名重定向自动覆盖
#   （v17 的「父目录同名自然目标放行」guard 保证 scipy.special._ufuncs 等 dotted 名命中 libs）。
#   触发链：comfy/sdpose.py:3 `from scipy.ndimage import gaussian_filter`（model_base → openaimodel）。
# ─────────────────────────────────────────────────────────────────────────────
set(SCIPY_SO_LIBS "")
file(GLOB SCIPY_SO "${PRE}/scipy/*.so")
foreach(MODSO ${SCIPY_SO})
    get_filename_component(FN ${MODSO} NAME)
    import_ext_so("scipy/${FN}" SCIPY_SO_LIBS)
endforeach()

# ─────────────────────────────────────────────────────────────────────────────
# Step 0.4b：pydantic_core（pydantic 2.46.5 的 Rust 内核，PyPI musllinux_1_1_aarch64 wheel 直装）。
#   _pydantic_core.cpython-312-aarch64-linux-musl.so 无 SONAME → 与 torch._C 同经 import_ext_so；
#   运行时由宿主桥 dlopen+PyInit_pydantic_core+注册进
#   sys.modules['pydantic_core._pydantic_core']（pydantic_core/__init__.py 的
#   `from ._pydantic_core import ...` 命中预注册，与 torch._C 同构）。
#   它 NEEDED「libgcc_s-0bf60adc.so.1」（wheel 自带 hashed SONAME）+ libc.so：hashed 名不在
#   系统里，必须以 import_so 单文件锁进 libs/（有 SONAME，按 SONAME 闭环）。
# ─────────────────────────────────────────────────────────────────────────────
import_so(libpc_gcc "libgcc_s-0bf60adc.so.1")   # pydantic_core 的 hashed libgcc_s
set(PC_CORE_LIB "")
import_ext_so("_pydantic_core.cpython-312-aarch64-linux-musl.so" PC_CORE_LIB)

# ─────────────────────────────────────────────────────────────────────────────
# Step 0.4c：Pillow C 扩展（skh 栈同源 aarch64-linux-ohos，5 个 so 全无 SONAME——除 libjpeg）。
#   PIL/Image.py 的 `from . import _imaging as core`（comfy_execution/progress.py 裸 import PIL.Image）
#   在真机首跑实证 ImportError（0.4c py-err[6]）。_imaging NEEDED libjpeg.so.62+libz.so.1+libc.so：
#   libz/libc 已在 libs/（libz 为既有 import_so）；libjpeg.so.62 有 SONAME → import_so 单文件锁进。
#   4 个无 SONAME 模块 .so 同 _C 打法经 import_ext_so → hap libs/arm64，
#   运行时由 probe 的 meta_path finder 扫 libs/arm64 自动收进 _exts（短名 _imaging 等），
#   PIL._imaging 的 dotted import 命中重定向（与 torch._C/numpy 同构，finder 无需改）。
# ─────────────────────────────────────────────────────────────────────────────
import_so(libjpeg "libjpeg.so.62")   # _imaging 的 JPEG 解码依赖（skh 栈 libjpeg-turbo）
set(PIL_LIBS "")
foreach(PILSO
    "_imaging.cpython-312-aarch64-linux-ohos.so"
    "_imagingmath.cpython-312-aarch64-linux-ohos.so"
    "_imagingmorph.cpython-312-aarch64-linux-ohos.so"
    "_imagingtk.cpython-312-aarch64-linux-ohos.so")
    import_ext_so("${PILSO}" PIL_LIBS)
endforeach()

# Step 0.4c-2：safetensors 的 Rust 扩展（abi3 全 ABI —— finder 后缀已加 .abi3.so 覆盖）。
#   无 SONAME，同 import_ext_so；NEEDED libunwind.so.1（既有 libunwind1 闭环）+ libc.so。
set(SF_LIB "")
import_ext_so("_safetensors_rust.abi3.so" SF_LIB)

# run82 — tokenizers 的 Rust 扩展（bindings/python 交叉编译产物，lib name=tokenizers →
#   PyInit_tokenizers；完整键 tokenizers.tokenizers 由 _LibsFinder 短名劫持自 libs/，
#   与 _safetensors_rust 同机制：铁律 so 只进 libs/<abi>/）。
set(TK_LIB "")
import_ext_so("tokenizers.abi3.so" TK_LIB)

# ComfyUI 后端子进程（NCP）：入口 extern "C" void Main(NativeChildProcess_Args)。
# G4 探针(2026-09-04) 使命早已完成(PROBE-DLOK 定谳); 2026-09-05 双分支拆除(USE_PROBE_MAIN/
#   PROBE_BRANCH 残根), 产品单分支; 探针/自检逻辑全部外置(见 docs/smoke-design.md)。
add_library(comfy_child SHARED
    child/comfy_child.cpp
    child/npu_probe.cpp)   # P0 NNRt 探针(诊断模式, entryParams 含 npu-probe 才执行)
# 诊断 PyInit__C 卡死栈：信号 handler 里 _Unwind_Backtrace 需 .eh_frame 才能回溯。
# 默认 clang 用 -fomit-frame-pointer 且 hvigor DoNativeStrip 后无 .eh_frame => unwinder 返 0 帧。
# 显式开 -funwind-tables + -fno-omit-frame-pointer 强制生成 .eh_frame（仅诊断用，不影响功能）。
target_compile_options(comfy_child PRIVATE
    -funwind-tables
    -fno-omit-frame-pointer)
# -nostdlib++：禁 clang++ 隐式链 libc++_shared(=>std::__n1)；改显式链 libcxx1(libc++.so.1)=>std::__1，
#   使全进程统一 __1（命门③ 关键）。comfy_child.cpp 为 C-like C++，C++ 运行时符号由 libcxxabi/__1 提供。
# 机制 v2（2026-09-03 真机第二轮定谳；v1(-l:短名+改 hvigor 插件) 与 v2-初版(纯 as-needed)均已废弃）：
#   ⚠ OHOS loader(MUSL-LDSO) 的 dlopen 解析图只含【NEEDED 树】符号——被 as-needed 修剪后，
#     dlopen(lib-dynload/math.so) 时找不到 libpython 的 PyModuleDef_Init /
#     _PyThreadState_GetCurrent → "relocating failed: symbol not found"（真机实测）。
#   ✓ 构造：进 hap libs 的收集 = 「IMPORTED 绝对路径 libraries fragment」（hvigor 官方版直接收集）；
#     loader 全局符号组(NEEDED 树) = 「target_link_options: -Wl,--no-as-needed + -l:短名」——
#     options role 不进 codemodel「libraries」→ 收集器无视之，两路互不干扰。
#     同一 .so 以绝对路径(链接输入)与 -l:短名(options)各出现一次：lld 吸收为一件，
#     DT_NEEDED 记短名（无 SONAME → basename），loader 从 HAP libs/ 按短名命中。
set(_NEEDED_SHORT "")
file(GLOB _NEEDED_SO
    "${PRE}/torch/_C.cpython-312-aarch64-linux-ohos.so"
    "${PRE}/dynload/*.so" "${PRE}/numpy/*.so" "${PRE}/scipy/*.so"
    "${PRE}/_pydantic_core.cpython-312-aarch64-linux-musl.so"
    "${PRE}/_imaging.cpython-312-aarch64-linux-ohos.so"
    "${PRE}/_imagingmath.cpython-312-aarch64-linux-ohos.so"
    "${PRE}/_imagingmorph.cpython-312-aarch64-linux-ohos.so"
    "${PRE}/_imagingtk.cpython-312-aarch64-linux-ohos.so"
    "${PRE}/_safetensors_rust.abi3.so" "${PRE}/tokenizers.abi3.so")
foreach(_S ${_NEEDED_SO})
    get_filename_component(_FN "${_S}" NAME)
    list(APPEND _NEEDED_SHORT "-l:${_FN}")
endforeach()
# ⚠ 序列（CMake 对「以 - 开头的字符串 link 项」原样保留为 linker flag,可在 libraries 段内
#   控制 as-needed 状态与 -L/-l: 位置）：
#   [libraries 区]（前面已列）--as-needed → 绝对路径输入族被修剪（NEEDED 不出现构建机路径）
#   --no-as-needed -L... -l:短名族 → NEEDED 树=短名（loader 全局符号组/真机可用）
#   这些以 "-" 开头的 flag 项在 codemodel「libraries」role 出现,但带 "-" 前缀的资源会被
#   hvigor 官方 findRuntimeFiles 丢弃,不影响收集（收集只认绝对路径项）。
target_link_options(comfy_child PRIVATE -nostdlib++)
target_link_libraries(comfy_child PUBLIC
    libhilog_ndk.z.so
    libchild_process.so
    libdl.so
    libcxx1          # libc++.so.1 (std::__1)
    libcxxabi1       # libc++abi.so.1
    libunwind1       # libunwind.so.1
    libomp           # libomp.so
    libpython        # libpython3.12.so.1.0
    libintl          # libintl.so.8
    libtorch_cpu
    libtorch_python
    libc10
    libtorch
    libshm
    libtorch_global_deps
    libz libbz2 liblzma libffi libcrypto libssl libncursesw libpanelw libtinfow
    "-Wl,--as-needed"  # 自此之后的绝对路径输入族被修剪（NEEDED 无构建机路径污染）
    ${NOSONAME_LIBS}   # torch._C + 70 个 dynload：IMPORTED 绝对路径（收集用, 被剪）
    ${NUMPY_SO_LIBS}   # numpy 19 个 C 扩展：同上
    ${SCIPY_SO_LIBS}   # scipy 114 个 C 扩展：同上
    libpc_gcc          # libgcc_s-0bf60adc.so.1（pydantic_core 的 hashed libgcc_s）
    ${PC_CORE_LIB}     # _pydantic_core.cpython-312-...-musl.so：同上
    libjpeg            # libjpeg.so.62（PIL _imaging 的 JPEG 依赖，SONAME 闭环）
    ${PIL_LIBS}        # PIL 4 个无 SONAME C 扩展：同上
    ${SF_LIB}          # safetensors._safetensors_rust.abi3.so：同上
    ${TK_LIB}          # run82 tokenizers.abi3.so：同上
    "-Wl,--no-as-needed" "-L${PRE}/torch" "-L${PRE}/dynload" "-L${PRE}/numpy" "-L${PRE}/scipy" "-L${PRE}"
    ${_NEEDED_SHORT})  # NEEDED 树=短名（loader 全局符号组）; 见上方机制 v2 注释
