#!/usr/bin/env bash
# ============================================================================
#  LumenPlayer 构建脚本（Linux/macOS/MSYS2 通用）
#  用法：
#    ./build.sh              编译全部
#    ./build.sh test         编译并运行单元测试
#    GXX=/path/to/g++ ./build.sh
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$ROOT/build"
mkdir -p "$OUT"

# 依次尝试：环境变量 → PATH → 家目录下的便携工具链
if [ -z "${GXX:-}" ]; then
  if command -v g++ >/dev/null 2>&1; then
    GXX="g++"
  elif [ -x "$HOME/tools/mingw64/bin/g++.exe" ]; then
    GXX="$HOME/tools/mingw64/bin/g++.exe"
  else
    echo "错误：未找到 g++，请设置 GXX 环境变量" >&2
    exit 1
  fi
fi

CXXFLAGS=(-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -I"$ROOT/src" \
          -I"$ROOT/third_party/onnxruntime")
LDFLAGS=(-static -static-libgcc -static-libstdc++)

CORE=(
  "$ROOT/src/core/types.cpp"
  "$ROOT/src/core/json.cpp"
  "$ROOT/src/core/frame_desc.cpp"
  "$ROOT/src/core/frame_buffer.cpp"
  "$ROOT/src/core/field_analyzer.cpp"
  "$ROOT/src/core/color_policy.cpp"
  "$ROOT/src/core/grain_policy.cpp"
  "$ROOT/src/core/source_info.cpp"
  "$ROOT/src/core/processing_chain.cpp"
  "$ROOT/src/core/config.cpp"
)
PLATFORM=(
  "$ROOT/src/platform/process.cpp"
  "$ROOT/src/platform/stream_pipe.cpp"
  "$ROOT/src/platform/mpv_ipc.cpp"
)
ENGINE=(
  "$ROOT/src/engine/sr_backend.cpp"
  "$ROOT/src/engine/dummy_backend.cpp"
  "$ROOT/src/engine/onnx_backend.cpp"
)
SERVICE=("$ROOT/src/service/inspector.cpp")

echo "编译器: $GXX"
"$GXX" --version | head -1

echo "[1/5] 单元测试"
"$GXX" "${CXXFLAGS[@]}" -o "$OUT/lumen_tests.exe" \
  "$ROOT/tests/test_main.cpp" "${CORE[@]}" "${ENGINE[@]}" "${LDFLAGS[@]}"

echo "[2/5] lumen-probe"
"$GXX" "${CXXFLAGS[@]}" -o "$OUT/lumen-probe.exe" \
  "$ROOT/src/apps/lumen-probe/main.cpp" "${CORE[@]}" "${PLATFORM[@]}" "${SERVICE[@]}" \
  "${LDFLAGS[@]}"

echo "[3/5] lumen-render"
"$GXX" "${CXXFLAGS[@]}" -o "$OUT/lumen-render.exe" \
  "$ROOT/src/apps/lumen-render/main.cpp" "${CORE[@]}" "${PLATFORM[@]}" "${ENGINE[@]}" \
  "${SERVICE[@]}" "${LDFLAGS[@]}"

echo "[4/5] lumen-batch"
"$GXX" "${CXXFLAGS[@]}" -o "$OUT/lumen-batch.exe" \
  "$ROOT/src/apps/lumen-batch/main.cpp" "${CORE[@]}" "${PLATFORM[@]}" \
  "${LDFLAGS[@]}"

echo "[5/5] lumen-player"
# 播放器要 GUI 相关的系统库：comdlg32 文件对话框、shell32 拖放、user32/gdi32 窗口绘制
"$GXX" "${CXXFLAGS[@]}" -o "$OUT/lumen-player.exe" \
  "$ROOT/src/apps/lumen-player/main.cpp" "${CORE[@]}" "${PLATFORM[@]}" "${SERVICE[@]}" \
  -lcomdlg32 -lshell32 -luser32 -lgdi32 "${LDFLAGS[@]}"

echo
echo "构建完成："
ls -lh "$OUT"/*.exe 2>/dev/null || ls -lh "$OUT" | tail -5

if [ "${1:-}" = "test" ]; then
  echo
  echo "===== 运行单元测试 ====="
  "$OUT/lumen_tests.exe"
fi
