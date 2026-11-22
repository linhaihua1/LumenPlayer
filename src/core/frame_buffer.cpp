// ============================================================================
//  LumenPlayer - CPU 帧缓冲区实现
//  所属层次：产品层（C 层）
// ============================================================================
#include "frame_buffer.h"

#include <cstring>
#include <new>

namespace lumen {
namespace {

// 平面数量（按像素格式）
int PlaneCountOf(PixelFormat fmt) {
  switch (fmt) {
    case PixelFormat::YUV420P:
    case PixelFormat::YUV420P10:
    case PixelFormat::YUV422P:
    case PixelFormat::YUV444P:
      return 3;
    case PixelFormat::NV12:      // Y + 交织的 UV
    case PixelFormat::RGB24:
    case PixelFormat::RGBA32:
      return 2;                  // NV12 返回 2；RGB/RGBA 实际只用平面 0
    case PixelFormat::Unknown:
    default:
      return 0;
  }
}

// 每像素字节数（用于算行跨度）
int BytesPerPixelOf(PixelFormat fmt) {
  switch (fmt) {
    case PixelFormat::RGB24:  return 3;
    case PixelFormat::RGBA32: return 4;
    case PixelFormat::YUV420P10: return 2; // 16bit 承载 10bit
    default: return 1;
  }
}

// 行跨度按 align 向上取整。
// 原因：SIMD 与 GPU 上传都要求对齐，按 width 直接算会导致越界或性能损失。
int AlignUp(int value, int align) {
  if (align <= 1) return value;
  return (value + align - 1) / align * align;
}

} // namespace

size_t FrameBuffer::RequiredBytes(int w, int h, PixelFormat fmt, int align) {
  if (w <= 0 || h <= 0) return 0;
  const int bpp = BytesPerPixelOf(fmt);
  size_t total = 0;

  switch (fmt) {
    case PixelFormat::YUV420P:
    case PixelFormat::YUV420P10: {
      const int yStride = AlignUp(w * bpp, align);
      const int cw = AlignUp((w + 1) / 2, align) * bpp;
      const int ch = (h + 1) / 2;
      total = static_cast<size_t>(yStride) * h + 2ull * cw * ch;
      break;
    }
    case PixelFormat::YUV422P: {
      const int yStride = AlignUp(w, align);
      const int cw = AlignUp((w + 1) / 2, align);
      const int ch = h;
      total = static_cast<size_t>(yStride) * h + 2ull * cw * ch;
      break;
    }
    case PixelFormat::YUV444P: {
      const int s = AlignUp(w, align);
      total = static_cast<size_t>(s) * h * 3;
      break;
    }
    case PixelFormat::NV12: {
      const int yStride = AlignUp(w, align);
      const int uvStride = AlignUp(w, align);
      total = static_cast<size_t>(yStride) * h + static_cast<size_t>(uvStride) * ((h + 1) / 2);
      break;
    }
    case PixelFormat::RGB24:
    case PixelFormat::RGBA32: {
      const int s = AlignUp(w * bpp, align);
      total = static_cast<size_t>(s) * h;
      break;
    }
    case PixelFormat::Unknown:
    default:
      return 0;
  }
  return total;
}

Status FrameBuffer::Allocate(int w, int h, PixelFormat fmt) {
  if (w <= 0 || h <= 0) return Status::InvalidArgument;
  if (fmt == PixelFormat::Unknown) return Status::Unsupported;

  width_ = w;
  height_ = h;
  format_ = fmt;
  plane_count_ = PlaneCountOf(fmt);

  // 行按 32 字节对齐：兼顾 SSE/AVX 载入与 D3D11 上传要求
  constexpr int kAlign = 32;
  const int bpp = BytesPerPixelOf(fmt);

  switch (fmt) {
    case PixelFormat::YUV420P:
    case PixelFormat::YUV420P10: {
      strides_[0] = AlignUp(w * bpp, kAlign);
      strides_[1] = AlignUp(((w + 1) / 2) * bpp, kAlign);
      strides_[2] = strides_[1];
      plane_heights_[0] = h;
      plane_heights_[1] = (h + 1) / 2;
      plane_heights_[2] = (h + 1) / 2;
      break;
    }
    case PixelFormat::YUV422P: {
      strides_[0] = AlignUp(w, kAlign);
      strides_[1] = AlignUp((w + 1) / 2, kAlign);
      strides_[2] = strides_[1];
      plane_heights_[0] = h;
      plane_heights_[1] = h;
      plane_heights_[2] = h;
      break;
    }
    case PixelFormat::YUV444P: {
      strides_[0] = strides_[1] = strides_[2] = AlignUp(w, kAlign);
      plane_heights_[0] = plane_heights_[1] = plane_heights_[2] = h;
      break;
    }
    case PixelFormat::NV12: {
      strides_[0] = AlignUp(w, kAlign);
      strides_[1] = AlignUp(w, kAlign);
      plane_heights_[0] = h;
      plane_heights_[1] = (h + 1) / 2;
      plane_heights_[2] = 0;
      break;
    }
    case PixelFormat::RGB24:
    case PixelFormat::RGBA32: {
      strides_[0] = AlignUp(w * bpp, kAlign);
      plane_heights_[0] = h;
      strides_[1] = strides_[2] = 0;
      plane_heights_[1] = plane_heights_[2] = 0;
      break;
    }
    case PixelFormat::Unknown:
    default:
      return Status::Unsupported;
  }

  // 计算各平面相对基址的偏移，单块分配（减少碎片与分配次数）
  offsets_[0] = 0;
  for (int i = 1; i < 3; ++i) {
    offsets_[i] = offsets_[i - 1] +
                  static_cast<size_t>(strides_[i - 1]) * plane_heights_[i - 1];
  }

  const size_t total = RequiredBytes(w, h, fmt, kAlign);
  try {
    storage_.assign(total, 0);
  } catch (const std::bad_alloc&) {
    storage_.clear();
    return Status::OutOfMemory;
  }
  return Status::Ok;
}

Status FrameBuffer::Resize(int w, int h, PixelFormat fmt) {
  if (w == width_ && h == height_ && fmt == format_ && !storage_.empty()) {
    return Status::Ok; // 规格未变，直接复用
  }
  return Allocate(w, h, fmt);
}

void FrameBuffer::Reset() {
  storage_.clear();
  width_ = height_ = 0;
  format_ = PixelFormat::Unknown;
  plane_count_ = 0;
  for (int i = 0; i < 3; ++i) {
    strides_[i] = 0;
    plane_heights_[i] = 0;
    offsets_[i] = 0;
  }
  pts_ = kTimestampNone;
  index_ = 0;
}

int FrameBuffer::PlaneHeight(int index) const {
  return (index >= 0 && index < 3) ? plane_heights_[index] : 0;
}

int FrameBuffer::PlaneWidth(int index) const {
  if (index < 0 || index >= 3) return 0;
  // 有效宽度（未对齐前）：Y 为全宽，4:2:0/4:2:2 的色度为半宽
  if (index == 0) return width_ * BytesPerPixelOf(format_);
  switch (format_) {
    case PixelFormat::YUV420P:
    case PixelFormat::YUV420P10:
    case PixelFormat::YUV422P:
      return ((width_ + 1) / 2) * BytesPerPixelOf(format_);
    case PixelFormat::YUV444P:
      return width_;
    case PixelFormat::NV12:
      // NV12 的平面 1 是交织的 UV，宽度与亮度一致（高度为一半）
      return index == 1 ? width_ : 0;
    default:
      return 0;
  }
}

PlaneView FrameBuffer::Plane(int index) {
  PlaneView v;
  if (index < 0 || index >= plane_count_ || storage_.empty()) return v;
  v.data = storage_.data() + offsets_[index];
  v.stride = strides_[index];
  return v;
}

const PlaneView FrameBuffer::Plane(int index) const {
  // 复用非 const 版本再取指针：避免两处实现漂移
  return const_cast<FrameBuffer*>(this)->Plane(index);
}

FrameDesc FrameBuffer::ToDesc() const {
  FrameDesc d;
  d.width = width_;
  d.height = height_;
  d.sar = sar_;
  d.format = format_;
  d.colorspace = colorspace_;
  d.range = range_;
  d.pts = pts_;
  d.frame_index = index_;
  d.field_order = field_order_;
  d.location = FrameLocation::Cpu;
  d.texture_kind = TextureKind::None;
  d.texture = nullptr;
  d.device = nullptr;
  for (int i = 0; i < 3; ++i) {
    if (i < plane_count_ && !storage_.empty()) {
      d.plane_data[i] = storage_.data() + offsets_[i];
      d.plane_stride[i] = strides_[i];
    } else {
      d.plane_data[i] = nullptr;
      d.plane_stride[i] = 0;
    }
  }
  return d;
}

} // namespace lumen
