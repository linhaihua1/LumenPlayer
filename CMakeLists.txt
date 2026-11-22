cmake_minimum_required(VERSION 3.16)
project(LumenPlayer LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 全局配置
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# 统一输出目录，便于脚本直接调用 exe
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------
# core：产品层（纯 C++17，零第三方依赖）
# 这一层必须能独立编译进 Android / HarmonyOS，因此禁止引入任何平台头文件。
# ---------------------------------------------------------------------------
add_library(lumen_core STATIC
  src/core/types.cpp
  src/core/json.cpp
  src/core/frame_desc.cpp
  src/core/frame_buffer.cpp
  src/core/field_analyzer.cpp
  src/core/color_policy.cpp
  src/core/grain_policy.cpp
  src/core/source_info.cpp
  src/core/processing_chain.cpp
  src/core/config.cpp
)
target_include_directories(lumen_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# platform：平台抽象（子进程等）
# ---------------------------------------------------------------------------
add_library(lumen_platform STATIC
  src/platform/process.cpp
  src/platform/stream_pipe.cpp
  src/platform/mpv_ipc.cpp
)
target_include_directories(lumen_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# engine：超分引擎层（ISRBackend 及实现）
# ---------------------------------------------------------------------------
add_library(lumen_engine STATIC
  src/engine/sr_backend.cpp
  src/engine/dummy_backend.cpp
  src/engine/onnx_backend.cpp
)
target_link_libraries(lumen_engine PUBLIC lumen_core)
# ONNX Runtime 只用到官方头文件里的声明，运行时通过 LoadLibrary 动态解析，
# 因此不需要导入库、也不需要联网下载 SDK（详见 onnx_backend.h 的说明）。
target_include_directories(lumen_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime)

# ---------------------------------------------------------------------------
# service：编排层
# ---------------------------------------------------------------------------
add_library(lumen_service STATIC
  src/service/inspector.cpp
)
target_link_libraries(lumen_service PUBLIC lumen_core lumen_platform)

# ---------------------------------------------------------------------------
# 应用
# ---------------------------------------------------------------------------
add_executable(lumen-probe src/apps/lumen-probe/main.cpp)
target_link_libraries(lumen-probe PRIVATE lumen_service)

add_executable(lumen-render src/apps/lumen-render/main.cpp)
target_link_libraries(lumen-render PRIVATE lumen_service lumen_engine)

add_executable(lumen-batch src/apps/lumen-batch/main.cpp)
target_link_libraries(lumen-batch PRIVATE lumen_core lumen_platform)

add_executable(lumen-player
  src/apps/lumen-player/main.cpp
  src/apps/lumen-player/player_ui.cpp
)
target_link_libraries(lumen-player PRIVATE lumen_service)
if(WIN32)
  target_link_libraries(lumen-player PRIVATE comdlg32 shell32 user32 gdi32)
endif()

add_executable(lumen_tests tests/test_main.cpp)
target_link_libraries(lumen_tests PRIVATE lumen_core lumen_engine)

enable_testing()
add_test(NAME unit_tests COMMAND lumen_tests)
