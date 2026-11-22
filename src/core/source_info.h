// ============================================================================
//  LumenPlayer - 片源信息解析
//  ---------------------------------------------------------------------------
//  所属层次：产品层（C 层）
//  职责    ：把 ffprobe 的 JSON 输出解析成统一的 SourceInfo，
//            并做第一轮"元数据层面"的自检与告警。
//
//  为什么用 ffprobe 而不是自己解析容器：
//    容器解析（MKV/AVI/MP4/TS）是一大块纯苦力活，且极易踩边界情况。
//    ffprobe 是 LGPL/GPL 双许可的独立可执行程序，以子进程方式调用
//    不构成衍生作品，是许可上最干净的选项（详见 06 文档许可红线）。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "types.h"

namespace lumen {

// 音轨摘要（本产品一期不做音频处理，仅用于判断片源完整性与同步风险）
struct AudioInfo {
  std::string codec;
  int sample_rate = 0;
  int channels = 0;
  std::string language;
};

struct SourceInfo {
  std::string path;      // 文件路径
  std::string container; // 容器名，如 "matroska,webm"

  // --- 视频 ---
  std::string video_codec;   // 如 "h264" / "mpeg2video"
  int width = 0;
  int height = 0;
  PixelFormat format = PixelFormat::Unknown;
  Rational fps{0, 1};        // 优先取 avg_frame_rate（r_frame_rate 可能是时基）
  Rational sar{1, 1};        // 像素宽高比
  Rational dar{0, 1};        // 容器标注的显示宽高比（可能为 0，表示未标注）

  // --- 色彩（容器标记，可能全部未标注，这是老片的常态）---
  ColorSpace colorspace = ColorSpace::Unspecified;
  ColorRange range = ColorRange::Unspecified;
  TransferFunction transfer = TransferFunction::Unspecified;

  // --- 场结构（容器标记，DVD/TS 常有，MKV 常丢）---
  FieldOrder field_order = FieldOrder::Unknown;

  // --- 体量 ---
  int64_t duration_us = 0;
  int64_t bit_rate = 0;
  int64_t nb_frames = 0;

  std::vector<AudioInfo> audio;

  // --- 派生属性 ---
  // 显示宽高比：优先用容器 dar，其次 width/height * sar
  double DisplayAspect() const;
  // 有效像素数（用于估算超分负载）
  int64_t PixelCount() const { return static_cast<int64_t>(width) * height; }
  bool IsSd() const { return height > 0 && height <= 576; }
};

// 解析 ffprobe -print_format json -show_streams -show_format 的输出。
// 参数说明：json 为完整输出文本；out 非空时写入结果；err 非空时写入错误描述。
// 返回值  ：Ok 表示成功；NotFound 表示没有视频流；InvalidArgument 表示 JSON 非法。
Status ParseFfprobeJson(const std::string& json, SourceInfo* out, std::string* err);

// 单条自检告警
struct Diagnosis {
  enum class Level { Info, Warning, Critical };
  Level level = Level::Info;
  std::string code;    // 机器可读码，如 "SAR_NON_SQUARE"
  std::string message; // 人可读说明
};

// 元数据层面的自检（不含像素分析，像素分析见 inspector）。
// 参数说明：info 为已解析的片源信息
// 返回值  ：告警列表，按严重程度排序（Critical 在前）
std::vector<Diagnosis> DiagnoseMetadata(const SourceInfo& info);

const char* ToString(Diagnosis::Level level);

} // namespace lumen
