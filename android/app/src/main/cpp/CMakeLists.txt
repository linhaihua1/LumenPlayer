# ============================================================================
#  LumenPlayer Android - NDK 构建脚本
#  ---------------------------------------------------------------------------
#  这里直接编译桌面仓库的 src/core/*.cpp，不做拷贝、不做分支。
#  理由：core 层从第一行代码起就是「纯 C++17 + 零第三方依赖 + 零平台头文件」，
#        本身就是为跨端准备的（见根目录 CMakeLists.txt 的注释）。
#        一旦改成「复制一份给 Android」，两边就会开始漂移，
#        最终出现「同样的片源，桌面判隔行、手机判逐行」这种最难查的问题。
#
#  目录关系：
#    android/app/src/main/cpp/  ← 当前目录
#      ../../../../..           = LumenPlayer 仓库根（含 src/）
# ============================================================================
cmake_minimum_required(VERSION 3.22.1)
project(lumen_core_jni CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LUMEN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../..")
set(LUMEN_SRC  "${LUMEN_ROOT}/src")

if(NOT EXISTS "${LUMEN_SRC}/core/field_analyzer.cpp")
    message(FATAL_ERROR
        "找不到 core 源码：${LUMEN_SRC}\n"
        "android/ 目录必须留在 LumenPlayer 仓库内（不要单独拷贝出去）。")
endif()

add_library(lumen_core_jni SHARED
    lumen_jni.cpp

    # core：判定算法本体。注意没有 config.cpp ——
    # 那个文件读 Windows 路径（GetFullPathNameW），Android 侧用 Android 自己的
    # SharedPreferences 管理设置，不需要也不应该把 Windows 配置逻辑编进来。
    ${LUMEN_SRC}/core/types.cpp
    ${LUMEN_SRC}/core/json.cpp
    ${LUMEN_SRC}/core/frame_desc.cpp
    ${LUMEN_SRC}/core/frame_buffer.cpp
    ${LUMEN_SRC}/core/field_analyzer.cpp
    ${LUMEN_SRC}/core/color_policy.cpp
    ${LUMEN_SRC}/core/grain_policy.cpp
    ${LUMEN_SRC}/core/source_info.cpp
    ${LUMEN_SRC}/core/processing_chain.cpp
)

target_include_directories(lumen_core_jni PRIVATE ${LUMEN_SRC})

target_compile_options(lumen_core_jni PRIVATE
    -O2
    -Wall -Wextra -Wno-unused-parameter
    # 默认隐藏符号：只导出 JNIEXPORT 标记的那两个函数，
    # 避免和 Media3/ffmpeg 之类第三方 .so 里的同名符号冲突。
    -fvisibility=hidden
    -fvisibility-inlines-hidden
)

# liblog：android/log.h 的运行时库。少了它链接期会报 undefined reference to __android_log_print
target_link_libraries(lumen_core_jni PRIVATE log)
