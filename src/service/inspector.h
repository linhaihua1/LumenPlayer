// ============================================================================
//  LumenPlayer - 片源自检服务
//  ---------------------------------------------------------------------------
//  所属层次：服务层（编排 core 的算法 + platform 的子进程能力）
//  职责    ：一条命令完成"元数据解析 → 抽样 → 像素分析 → 生成处理链 → 报告"。
//
//  设计意图：
//    这是 W1 评估与 W2 播放器共用的同一套逻辑。播放器在打开文件时调用它，
//    bench 工具在批量评估时也调用它——保证"评估看到的效果"就是
//    "播放器实际会做处理"，不会出现两套口径。
//
//  依赖外部程序 ffprobe / ffmpeg（LGPL 合规的独立进程调用）。
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "core/color_policy.h"
#include "core/field_analyzer.h"
#include "core/grain_policy.h"
#include "core/processing_chain.h"
#include "core/source_info.h"
#include "core/types.h"

namespace lumen {

struct InspectOptions {
  // 外部程序路径；留空则自动在 PATH 中查找
  std::string ffmpeg_path;
  std::string ffprobe_path;

  int sample_points = 6;    // 全片均匀采样几个位置（避开片头片尾）
  int frames_per_point = 6; // 每个位置连续抽几帧（供场结构分析）
  int probe_frame_index = 2; // 用采样点内的第几帧做色彩/颗粒探测

  QualityPreset preset = QualityPreset::Balanced;
  bool anime = false;
  bool keep_temp = false; // 保留临时文件，便于排查

  // 用户手动指定场序（Unknown = 自动判定）。
  // 对应命令行 --field-order 与配置 field_order，详见 OverrideFieldOrder 的说明。
  FieldOrder field_order_override = FieldOrder::Unknown;

  int process_timeout_ms = 120000; // 单次 ffmpeg 调用超时
};

struct InspectionReport {
  std::string path;
  SourceInfo info;
  std::vector<Diagnosis> metadata_diagnosis;

  CadenceVerdict cadence;
  ColorDecision color;
  GrainEstimate grain;
  GrainDecision grain_decision;
  LumaRangeProbe luma_probe;

  ProcessingChain chain;

  int frames_analyzed = 0;
  int sample_points_done = 0;
  std::vector<std::string> warnings; // 评估过程中的非致命问题

  // 人可读的完整报告（Markdown 风格纯文本），直接可粘进文档
  std::string ToText() const;

  // 机器可读摘要（单行 JSON），供 bench 汇总成 CSV
  std::string ToJson() const;
};

// 执行自检。
// 参数说明：path 为视频文件路径（支持中文）；opt 见上。
// 返回值：Ok 表示成功；NotFound 表示文件不存在或 ffprobe 缺失；
//         InvalidArgument 表示不是有效的视频文件。
Status InspectSource(const std::string& path, const InspectOptions& opt,
                     InspectionReport* report, std::string* err);

} // namespace lumen
