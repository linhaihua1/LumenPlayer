// ============================================================================
//  LumenPlayer - 基础类型实现
//  所属层次：产品层（C 层）
// ============================================================================
#include "types.h"

#include <cctype>

namespace lumen {

int BitsPerPixel(PixelFormat fmt) {
  switch (fmt) {
    case PixelFormat::YUV420P:   return 12; // Y 8bit + UV 各 2bit
    case PixelFormat::YUV420P10: return 15; // 10bit 4:2:0
    case PixelFormat::YUV422P:   return 16;
    case PixelFormat::YUV444P:   return 24;
    case PixelFormat::NV12:      return 12;
    case PixelFormat::RGB24:     return 24;
    case PixelFormat::RGBA32:    return 32;
    case PixelFormat::Unknown:
    default:                     return 0;
  }
}

bool Is420(PixelFormat fmt) {
  return fmt == PixelFormat::YUV420P || fmt == PixelFormat::YUV420P10 ||
         fmt == PixelFormat::NV12;
}

const char* ToString(PixelFormat fmt) {
  switch (fmt) {
    case PixelFormat::YUV420P:   return "yuv420p";
    case PixelFormat::YUV420P10: return "yuv420p10le";
    case PixelFormat::YUV422P:   return "yuv422p";
    case PixelFormat::YUV444P:   return "yuv444p";
    case PixelFormat::NV12:      return "nv12";
    case PixelFormat::RGB24:     return "rgb24";
    case PixelFormat::RGBA32:    return "rgba32";
    case PixelFormat::Unknown:
    default:                     return "unknown";
  }
}

const char* ToString(ColorSpace cs) {
  switch (cs) {
    case ColorSpace::BT601: return "bt601";
    case ColorSpace::BT709: return "bt709";
    case ColorSpace::BT2020: return "bt2020";
    case ColorSpace::Unspecified:
    default: return "unspecified";
  }
}

const char* ToString(ColorRange cr) {
  switch (cr) {
    case ColorRange::Limited: return "limited";
    case ColorRange::Full: return "full";
    case ColorRange::Unspecified:
    default: return "unspecified";
  }
}

const char* ToString(FieldOrder fo) {
  switch (fo) {
    case FieldOrder::Progressive: return "progressive";
    case FieldOrder::TopFieldFirst: return "tff";
    case FieldOrder::BottomFieldFirst: return "bff";
    case FieldOrder::Unknown:
    default: return "unknown";
  }
}

bool ParseFieldOrder(const std::string& s, FieldOrder* out) {
  if (out == nullptr) return false;
  // 逐字符小写化：用户可能写 TFF / Tff / tff，没必要为此报错
  std::string v;
  v.reserve(s.size());
  for (char c : s) v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

  if (v.empty() || v == "auto" || v == "unknown" || v == "detect") {
    *out = FieldOrder::Unknown;
    return true;
  }
  if (v == "prog" || v == "progressive" || v == "p") {
    *out = FieldOrder::Progressive;
    return true;
  }
  if (v == "tff" || v == "top" || v == "tt") {
    *out = FieldOrder::TopFieldFirst;
    return true;
  }
  if (v == "bff" || v == "bottom" || v == "bb") {
    *out = FieldOrder::BottomFieldFirst;
    return true;
  }
  return false;
}

const char* ToString(SourceCadence sc) {
  switch (sc) {
    case SourceCadence::Progressive: return "progressive";
    case SourceCadence::Interlaced: return "interlaced";
    case SourceCadence::Telecine: return "telecine";
    case SourceCadence::Mixed: return "mixed";
    case SourceCadence::Unknown:
    default: return "unknown";
  }
}

bool Rational::IsNtscFamily() const {
  if (!Valid()) return false;
  // NTSC 系帧率都是 1000/1001 缩放的产物，例如 30000/1001。
  // 但部分封装会把 29.97 写成 2997/100，这里用数值区间判定更稳。
  const double fps = ToDouble();
  const double candidates[] = {23.976, 29.97, 59.94, 119.88};
  for (double c : candidates) {
    if (fps > c - 0.05 && fps < c + 0.05) return true;
  }
  return false;
}

const char* ToString(Status st) {
  switch (st) {
    case Status::Ok: return "Ok";
    case Status::InvalidArgument: return "InvalidArgument";
    case Status::NotFound: return "NotFound";
    case Status::Unsupported: return "Unsupported";
    case Status::IoError: return "IoError";
    case Status::OutOfMemory: return "OutOfMemory";
    case Status::DeviceLost: return "DeviceLost";
    case Status::Timeout: return "Timeout";
    case Status::Internal:
    default: return "Internal";
  }
}

} // namespace lumen
