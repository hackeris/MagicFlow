# poc/npu — NPU 能力验证 demo(独立工程)。
# 产物:
#   libentry.so   = NAPI 层 + 全部探针通道(CH-01..05, dlopen+dlsym 形式, 不链接 NNRt 库)
#   libnpuchild.so= CH-06 native 子进程(libnpuchild.so:Main, 官方 StartNativeChildProcess 形态)
# 说明: NNRt 探针故意不 link neural_network_runtime(符号缺失不会拖垮 libentry.so),
#       全部 dlsym 可选 —— 与主项目探针同构。CMake 无需任何 prebuilt 栈。
cmake_minimum_required(VERSION 3.5.0)
project(npupoc)
set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})
if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()
include_directories(${NATIVERENDER_ROOT_PATH} ${NATIVERENDER_ROOT_PATH}/include)

add_library(entry SHARED
    napi_init.cpp
    nnrt_probe.cpp)
target_link_libraries(entry PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    libchild_process.so
    libdl.so)

add_library(npuchild SHARED
    child/npuchild.cpp)
target_link_libraries(npuchild PUBLIC
    libhilog_ndk.z.so
    libchild_process.so
    libdl.so)
