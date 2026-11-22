// ============================================================================
//  LumenPlayer - 片源信息解析实现
//  所属层次：产品层（C 层）
// ============================================================================
#include "source_info.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "json.h"

namespace lumen {
namespace {

// 解析 "30000/1001" 形式的比例；分隔符同时容忍 '/' 与 ':'（ffprobe 混用两者）
Rational ParseRatio(const std::string& s) {
  if (s.empty() || s == "N/A" || s == "unknown") return Rational(0, 1);
  const size_t sep = s.find_first_of("/:");
  if (sep == std::string::npos) {
    const double v = std::strtod(s.c_str(), nullptr);
    return v > 0 ? Rational(static_cast<int>(std::lround(v * 1000)), 1000) : Rational(0, 1);
  }
  const int n = static_cast<int>(std::strtol(s.c_str(), nullptr, 10));
  const int d = static_cast<int>(std::strtol(s.c_str() + sep + 1, nullptr, 10));
  if (n <= 0 || d <= 0) return Rational(0, 1);
  return Rational(n, d);
}

// 从 JSON 值取整数，兼容 ffprobe 把数字写成字符串的情况
int64_t JsonInt(const JsonValue& v, int64_t def = 0) {
  if (v.IsNumber()) return v.AsInt(def);
  if (v.IsString()) {
    const std::string& s = v.AsString();
    if (s.empty() || s == "N/A") return def;
    return std::strtoll(s.c_str(), nullptr, 10);
  }
  return def;
}

double JsonDouble(const JsonValue& v, double def = 0.0) {
  if (v.IsNumber()) return v.AsDouble(def);
  if (v.IsString()) {
    const std::string& s = v.AsString();
    if (s.empty() || s == "N/A") return def;
    return std::strtod(s.c_str(), nullptr);
  }
  return def;
}

PixelFormat MapPixelFormat(const std::string& name) {
  if (name == "yuv420p" || name == "yuvj420p") return PixelFormat::YUV420P;
  if (name == "yuv420p10le") return PixelFormat::YUV420P10;
  if (name == "yuv422p" || name == "yuvj422p") return PixelFormat::YUV422P;
  if (name == "yuv444p" || name == "yuvj444p") return PixelFormat::YUV444P;
  if (name == "nv12") return PixelFormat::NV12;
  if (name == "rgb24" || name == "bgr24") return PixelFormat::RGB24;
  if (name == "rgba" || name == "bgra") return PixelFormat::RGBA32;
  return PixelFormat::Unknown;
}

ColorSpace MapColorSpace(const std::string& s) {
  if (s == "bt709") return ColorSpace::BT709;
  // smpte170m = NTSC(SMPTE 170M)，bt470bg = PAL/BG——两者都属于 BT.601 家族
  if (s == "smpte170m" || s == "bt470bg" || s == "bt601" || s == "smpte240m")
    return ColorSpace::BT601;
  if (s == "bt2020nc" || s == "bt2020c") return ColorSpace::BT2020;
  return ColorSpace::Unspecified;
}

ColorRange MapColorRange(const std::string& s) {
  if (s == "tv" || s == "limited" || s == "mpeg") return ColorRange::Limited;
  if (s == "pc" || s == "full" || s == "jpeg") return ColorRange::Full;
  return ColorRange::Unspecified;
}

FieldOrder MapFieldOrder(const std::string& s) {
  if (s == "progressive") return FieldOrder::Progressive;
  if (s == "tt") return FieldOrder::TopFieldFirst;
  if (s == "bb") return FieldOrder::BottomFieldFirst;
  // tb / bt：场序不明确，ffmpeg 报这两个值时表示"隔行但场序未知"
  if (s == "tb" || s == "bt") return FieldOrder::Unknown;
  return FieldOrder::Unknown;
}

TransferFunction MapTransfer(const std::string& s) {
  if (s == "bt709") return TransferFunction::BT709;
  if (s == "iec61966-2-1" || s == "srgb") return TransferFunction::SRGB;
  if (s == "smpte2084") return TransferFunction::PQ;
  if (s == "arib-std-b67") return TransferFunction::HLG;
  return TransferFunction::Unspecified;
}

Diagnosis Make(Diagnosis::Level lv, const std::string& code, const std::string& msg) {
  Diagnosis d;
  d.level = lv;
  d.code = code;
  d.message = msg;
  return d;
}

} // namespace

double SourceInfo::DisplayAspect() const {
  if (dar.num > 0 && dar.den > 0) return dar.ToDouble();
  if (height <= 0) return 0.0;
  const double base = static_cast<double>(width) / height;
  return base * (sar.Valid() ? sar.ToDouble() : 1.0);
}

Status ParseFfprobeJson(const std::string& json, SourceInfo* out, std::string* err) {
  if (out == nullptr) return Status::InvalidArgument;

  std::string parse_err;
  const JsonValue root = JsonValue::Parse(json, &parse_err);
  if (root.IsNull()) {
    if (err) *err = "JSON 解析失败: " + parse_err;
    return Status::InvalidArgument;
  }

  SourceInfo info;
  const JsonValue& format = root["format"];
  info.container = format["format_name"].AsString();
  info.duration_us =
      static_cast<int64_t>(JsonDouble(format["duration"], 0.0) * 1e6);
  info.bit_rate = JsonInt(format["bit_rate"], 0);
  if (!format["filename"].IsNull()) info.path = format["filename"].AsString();

  const JsonValue& streams = root["streams"];
  if (!streams.IsArray() || streams.size() == 0) {
    if (err) *err = "输出中没有 streams 数组";
    return Status::InvalidArgument;
  }

  bool found_video = false;
  for (size_t i = 0; i < streams.size(); ++i) {
    const JsonValue& s = streams[i];
    const std::string type = s["codec_type"].AsString();

    if (type == "video" && !found_video) {
      found_video = true;
      info.video_codec = s["codec_name"].AsString();
      info.width = static_cast<int>(JsonInt(s["width"], 0));
      info.height = static_cast<int>(JsonInt(s["height"], 0));
      info.format = MapPixelFormat(s["pix_fmt"].AsString());

      // 帧率：avg_frame_rate 才是真实平均帧率；
      // r_frame_rate 在 VFR 素材上常是"最大帧率"，直接用它会导致时间轴错误。
      info.fps = ParseRatio(s["avg_frame_rate"].AsString());
      if (!info.fps.Valid()) info.fps = ParseRatio(s["r_frame_rate"].AsString());

      info.sar = ParseRatio(s["sample_aspect_ratio"].AsString());
      info.dar = ParseRatio(s["display_aspect_ratio"].AsString());

      info.colorspace = MapColorSpace(s["color_space"].AsString());
      info.range = MapColorRange(s["color_range"].AsString());
      info.transfer = MapTransfer(s["color_transfer"].AsString());
      info.field_order = MapFieldOrder(s["field_order"].AsString());

      // ffmpeg 的 yuvj* 前缀约定表示 full range。部分封装不写 color_range，
      // 但会在 pix_fmt 上体现——这是老片 full range 的重要线索，不能漏。
      if (info.range == ColorRange::Unspecified) {
        const std::string pix = s["pix_fmt"].AsString();
        if (pix.rfind("yuvj", 0) == 0) info.range = ColorRange::Full;
      }

      info.nb_frames = JsonInt(s["nb_frames"], 0);
      if (info.bit_rate == 0) info.bit_rate = JsonInt(s["bit_rate"], 0);
      if (info.duration_us == 0) {
        info.duration_us =
            static_cast<int64_t>(JsonDouble(s["duration"], 0.0) * 1e6);
      }
    } else if (type == "audio") {
      AudioInfo a;
      a.codec = s["codec_name"].AsString();
      a.sample_rate = static_cast<int>(JsonInt(s["sample_rate"], 0));
      a.channels = static_cast<int>(JsonInt(s["channels"], 0));
      a.language = s["tags"]["language"].AsString();
      info.audio.push_back(a);
    }
  }

  if (!found_video) {
    if (err) *err = "未找到视频流";
    return Status::NotFound;
  }

  *out = info;
  return Status::Ok;
}

std::vector<Diagnosis> DiagnoseMetadata(const SourceInfo& info) {
  std::vector<Diagnosis> list;

  // ---- 色彩空间与范围 ----
  if (info.colorspace == ColorSpace::Unspecified) {
    list.push_back(Make(Diagnosis::Level::Info, "COLORSPACE_UNSPECIFIED",
                        "容器未标注色彩空间，将按分辨率推断（标清→601，高清→709）"));
  } else if (info.IsSd() && info.colorspace == ColorSpace::BT709) {
    list.push_back(Make(Diagnosis::Level::Warning, "SD_TAGGED_709",
                        "标清素材却标注 BT.709，可能是转制时误标；建议按 601 处理并在 A/B 中确认肤色"));
  } else if (!info.IsSd() && info.colorspace == ColorSpace::BT601) {
    list.push_back(Make(Diagnosis::Level::Warning, "HD_TAGGED_601",
                        "高清素材标注 BT.601，请确认是否为老母带直转，否则红色会过饱和"));
  }

  if (info.range == ColorRange::Unspecified) {
    list.push_back(Make(Diagnosis::Level::Info, "RANGE_UNSPECIFIED",
                        "未标注色彩范围，默认按 Limited(16—235) 处理；若画面发灰需手动切换 Full"));
  }

  // ---- 像素宽高比 ----
  if (info.sar.Valid() && (info.sar.num != info.sar.den) && info.sar.num != 0) {
    char buf[220];
    std::snprintf(buf, sizeof(buf),
                  "非方形像素（SAR %d:%d），显示宽高比应按 %.3f 处理，"
                  "直接拉伸会让人脸变形",
                  info.sar.num, info.sar.den, info.DisplayAspect());
    list.push_back(Make(Diagnosis::Level::Critical, "SAR_NON_SQUARE", buf));
  }

  // ---- 场结构线索 ----
  if (info.field_order == FieldOrder::TopFieldFirst ||
      info.field_order == FieldOrder::BottomFieldFirst) {
    list.push_back(Make(Diagnosis::Level::Critical, "FIELD_MARKED_INTERLACED",
                        std::string("容器标记为隔行（") + ToString(info.field_order) +
                            "），必须先做场处理再超分，否则梳齿会被当作细节放大"));
  } else if (info.field_order == FieldOrder::Unknown && info.IsSd()) {
    list.push_back(Make(Diagnosis::Level::Warning, "SD_FIELD_UNKNOWN",
                        "标清且场序未知，需靠像素分析判定；90 年代剧集隔行概率很高"));
  }

  // ---- 编码与画质风险 ----
  if (info.video_codec == "mpeg2video" && info.IsSd()) {
    list.push_back(Make(Diagnosis::Level::Info, "MPEG2_SD",
                        "MPEG-2 标清片源（典型 DVD），块效应明显，超分前建议轻度去块"));
  }

  if (info.bit_rate > 0 && info.IsSd()) {
    // 标清参考位率：DVD 约 4—8Mbps，网络低码率转制常在 1—2Mbps
    const double mbps = static_cast<double>(info.bit_rate) / 1e6;
    if (mbps < 1.5) {
      char buf[200];
      std::snprintf(buf, sizeof(buf),
                    "位率仅 %.2f Mbps（标清），压缩伪影可能较重，"
                    "超分会同时放大伪影；建议降低超分强度或先做去块",
                    mbps);
      list.push_back(Make(Diagnosis::Level::Warning, "LOW_BITRATE", buf));
    }
  }

  // ---- 分辨率与放大倍数 ----
  if (info.height > 0 && info.height < 1080) {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "源分辨率 %dx%d，放大至 1080p 为 %.1f 倍线性放大",
                  info.width, info.height, 1080.0 / info.height);
    list.push_back(Make(Diagnosis::Level::Info, "UPSCALE_FACTOR", buf));
  } else if (info.height >= 1080) {
    list.push_back(Make(Diagnosis::Level::Info, "ALREADY_HD",
                        "源分辨率已达 1080p 或以上，超分收益有限，建议关闭增强以省电"));
  }

  // ---- 音轨 ----
  if (info.audio.empty()) {
    list.push_back(Make(Diagnosis::Level::Warning, "NO_AUDIO", "未检测到音轨"));
  }

  // 按严重程度排序：Critical → Warning → Info，便于报告阅读
  std::stable_sort(list.begin(), list.end(),
                   [](const Diagnosis& a, const Diagnosis& b) {
                     return static_cast<int>(a.level) > static_cast<int>(b.level);
                   });
  return list;
}

const char* ToString(Diagnosis::Level level) {
  switch (level) {
    case Diagnosis::Level::Critical: return "严重";
    case Diagnosis::Level::Warning: return "警告";
    case Diagnosis::Level::Info:
    default: return "提示";
  }
}

} // namespace lumen
