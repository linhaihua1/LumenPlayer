// ============================================================================
//  LumenPlayer - 帧契约 FrameDesc
//  ---------------------------------------------------------------------------
//  所属层次：跨层契约（接缝 1，见 ADR-001）
//  职责    ：解码层 → 策略层 → 引擎层 之间传递帧的唯一描述结构。
//
//  ★ 契约约束（改动需走 ADR，禁止私自扩展）：
//    1. GPU 路径上的帧必须是设备常驻纹理，FrameDesc 只持有句柄，
//       任何模块不得在实时路径上把帧回读到 CPU（一次 1080p 回读约 2—4ms，
//       足够吃掉整个单帧预算）。
//    2. pts 必须原样透传，禁止在策略层重算时间戳。重算必然引入漂移，
//       表现为音画不同步，且极难定位。
//    3. 色彩标记（colorspace / range / transfer）必须随帧携带。
//       若让引擎层自己猜，A/B 对比时会出现"同一帧两种颜色"的诡异现象。
//    4. 场序信息必须携带：超分引擎据此决定是否先做去隔行。
// ============================================================================
#pragma once

#include <cstdint>

#include "types.h"

namespace lumen {

// 帧在内存/显存中的位置
enum class FrameLocation {
  Cpu,  // CPU 可访问内存（离线导出、单元测试）
  Gpu,  // 设备显存（实时播放主路径）
};

// 设备纹理句柄的语义标记。
// 说明：不同后端对「纹理」的解释不同，用枚举显式标注，
//       避免把 D3D11 纹理当 Vulkan 图像用（这类错误在跨平台阶段极易发生）。
enum class TextureKind {
  None = 0,
  D3D11Texture2D, // ID3D11Texture2D*
  D3D11Nv12,      // ID3D11Texture2D*，格式 NV12（硬解直出）
  VulkanImage,    // VkImage
  MetalTexture,   // id<MTLTexture>
  OpenGLTexture,  // GLuint
};

struct FrameDesc {
  // --- 几何 ---
  int width = 0;          // 编码像素宽（不含黑边裁切）
  int height = 0;         // 编码像素高
  Rect display_crop;      // 有效画面区域（去除黑边后）；Empty 表示全帧有效
  Rational sar{1, 1};     // 像素宽高比，非方形像素老片（如 720x480 的 4:3）必须携带
  Rational fps{0, 1};     // 帧率，NTSC 系用 30000/1001 精确表示

  // --- 像素与色彩 ---
  PixelFormat format = PixelFormat::Unknown;
  ColorSpace colorspace = ColorSpace::Unspecified;
  ColorRange range = ColorRange::Unspecified;
  TransferFunction transfer = TransferFunction::Unspecified;

  // --- 时域 ---
  Timestamp pts = kTimestampNone; // 显示时间戳（微秒），原样透传
  int64_t frame_index = 0;        // 解码序号，用于多帧算法（IVTC 需要看前后帧）
  FieldOrder field_order = FieldOrder::Unknown; // 本帧原始场序（未去隔行前的真实状态）

  // --- 存储位置 ---
  FrameLocation location = FrameLocation::Cpu;
  TextureKind texture_kind = TextureKind::None;
  void* texture = nullptr; // 设备纹理句柄；CPU 帧为 nullptr
  void* device = nullptr;  // 所属设备（ID3D11Device* 等），用于校验跨设备误用

  // --- CPU 平面（仅 location == Cpu 时有效）---
  // 顺序约定：平面 0 = Y，1 = U，2 = V；RGB 打包格式只有平面 0。
  const uint8_t* plane_data[3] = {nullptr, nullptr, nullptr};
  int plane_stride[3] = {0, 0, 0};

  // --- 辅助 ---
  // 显示宽高比（考虑 SAR）。用于判定 4:3 / 16:9 / 变形宽银幕。
  // 返回值：宽/高，SAR 非法时回落为编码宽高比
  double DisplayAspect() const {
    const double base = (height > 0) ? static_cast<double>(width) / height : 0.0;
    const double sar_value = sar.Valid() ? sar.ToDouble() : 1.0;
    return base * sar_value;
  }

  bool IsGpu() const { return location == FrameLocation::Gpu; }
  bool IsValid() const { return width > 0 && height > 0 && format != PixelFormat::Unknown; }
};

// 判定显示宽高比所属档位（用于 4:3 老片策略）
enum class AspectClass {
  Unknown = 0,
  Ratio4x3,    // 1.33 附近，1950 年前后及所有标清电视
  Ratio16x9,   // 1.78 附近，HD 标准
  Ratio185,    // 1.85 宽银幕
  Ratio235,    // 2.35—2.40 变形宽银幕
  Other,
};

// 参数说明：dar 为显示宽高比；tolerance 为容差（默认 0.06，可容纳 1.33/1.37 混用）
AspectClass ClassifyAspect(double dar, double tolerance = 0.06);

const char* ToString(AspectClass ac);

} // namespace lumen
