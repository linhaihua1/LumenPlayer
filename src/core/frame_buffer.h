// ============================================================================
//  LumenPlayer - CPU 帧缓冲区
//  ---------------------------------------------------------------------------
//  所属层次：产品层（C 层）
//  职责    ：在没有 GPU 的场合承载帧数据，具体用于：
//            1. 离线导出管线（解码 → 分析 → 超分 → 编码）；
//            2. 片源自检（需要随机访问像素做统计）；
//            3. 单元测试（完全不依赖 GPU）。
//
//  注意：实时播放路径不使用本类（那条路径全程在显存里，见 FrameDesc 契约）。
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "frame_desc.h"
#include "types.h"

namespace lumen {

// 单个平面的视图：数据指针 + 行跨度。
// 说明：stride（行跨度）可能大于有效宽度——ffmpeg 与 GPU 出于对齐会补齐，
//       直接按 width 计算偏移是画质错位的常见原因，所以一律显式携带 stride。
struct PlaneView {
  uint8_t* data = nullptr;
  int stride = 0; // 每行字节数
};

class FrameBuffer {
 public:
  FrameBuffer() = default;

  // 分配一帧。
  // 参数说明：w/h 为编码宽高；fmt 决定平面数与色度抽样方式。
  // 返回值  ：Ok 表示成功；Unsupported 表示该格式暂不支持。
  Status Allocate(int w, int h, PixelFormat fmt);

  // 语义同 Allocate，但复用已有内存（避免逐帧分配抖动）。
  // 尺寸或格式变化时内部会自动重新分配。
  Status Resize(int w, int h, PixelFormat fmt);

  void Reset();

  // --- 基本属性 ---
  int width() const { return width_; }
  int height() const { return height_; }
  PixelFormat format() const { return format_; }
  int plane_count() const { return plane_count_; }
  bool empty() const { return storage_.empty(); }

  // 指定平面的高度（Y 为全高，4:2:0 的 UV 为半高）
  int PlaneHeight(int index) const;

  // 指定平面的有效行字节数（不含对齐填充）
  int PlaneWidth(int index) const;

  // 返回可写平面视图。
  // 注意：参数越界时返回空视图（data == nullptr），调用方必须判空。
  PlaneView Plane(int index);
  const PlaneView Plane(int index) const;

  // --- 帧属性（与 FrameDesc 保持同步）---
  Timestamp pts() const { return pts_; }
  void set_pts(Timestamp t) { pts_ = t; }
  int64_t index() const { return index_; }
  void set_index(int64_t i) { index_ = i; }
  FieldOrder field_order() const { return field_order_; }
  void set_field_order(FieldOrder fo) { field_order_ = fo; }
  void set_colorspace(ColorSpace cs) { colorspace_ = cs; }
  ColorSpace colorspace() const { return colorspace_; }
  void set_range(ColorRange r) { range_ = r; }
  ColorRange range() const { return range_; }
  void set_sar(Rational sar) { sar_ = sar; }
  Rational sar() const { return sar_; }

  // 导出为 FrameDesc（CPU 帧），用于喂给引擎层。
  FrameDesc ToDesc() const;

  // 估算该帧占用的字节数，用于内存水位控制
  size_t byte_size() const { return storage_.size(); }

  // 计算指定像素格式下所需的缓冲区总大小（含对齐）
  // 参数说明：align 为行对齐字节数（Win32 D3D 常要求 16 或 32 对齐）
  static size_t RequiredBytes(int w, int h, PixelFormat fmt, int align = 1);

 private:
  int width_ = 0;
  int height_ = 0;
  PixelFormat format_ = PixelFormat::Unknown;
  int plane_count_ = 0;
  int strides_[3] = {0, 0, 0};
  int plane_heights_[3] = {0, 0, 0};
  size_t offsets_[3] = {0, 0, 0};

  std::vector<uint8_t> storage_;

  // 帧属性跟着缓冲区走，避免调用方另外维护一份元数据造成不一致
  Timestamp pts_ = kTimestampNone;
  int64_t index_ = 0;
  FieldOrder field_order_ = FieldOrder::Unknown;
  ColorSpace colorspace_ = ColorSpace::Unspecified;
  ColorRange range_ = ColorRange::Unspecified;
  Rational sar_{1, 1};
};

} // namespace lumen
