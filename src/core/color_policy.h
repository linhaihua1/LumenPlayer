// ============================================================================
//  LumenPlayer - 色彩策略
//  ---------------------------------------------------------------------------
//  所属层次：产品层（C 层）
//  职责    ：决定"这帧到底该按哪个色彩空间解码、要不要转换矩阵"。
//
//  背景（为什么老片必须处理这一层）：
//    90 年代标清素材按 BT.601 制作。若播放器/超分链按 BT.709 解释，
//    红绿通道权重不同，结果是整片偏色（肤色发灰、绿色发黄）。
//    反过来，把 HD 素材当 601 处理会让红色过饱和。
//    更隐蔽的是 range 错标：Limited(16-235) 被当 Full(0-255) 读取，
//    画面整体发灰、暗部失去层次——用户只会觉得"修复后不如原片"。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

#include "frame_desc.h"
#include "frame_buffer.h"
#include "types.h"

namespace lumen {

// ---------------------------------------------------------------------------
// 亮度分布探测结果
// ---------------------------------------------------------------------------
struct LumaRangeProbe {
  int p_low = 0;    // 0.5% 分位亮度
  int p_high = 255; // 99.5% 分位亮度
  bool suggests_full_range = false; // 是否像 Full range（0—255）素材
  std::string detail;
};

// 探测亮度分布，用于纠正未标记或错标的 range。
// 参数说明：按 256 桶直方图统计，抽样列以控制成本。
// 判定依据：真实 Limited 素材极少同时出现 <8 与 >247 的大面积像素；
//           若两端都有可观分布，则更可能是被错标为 Limited 的 Full 素材。
LumaRangeProbe ProbeLumaRange(const uint8_t* y, int w, int h, int stride);

LumaRangeProbe ProbeLumaRange(const FrameBuffer& frame);

// ---------------------------------------------------------------------------
// 色彩决策
// ---------------------------------------------------------------------------
struct ColorDecision {
  ColorSpace source_space = ColorSpace::Unspecified; // 实际应按什么空间解码
  ColorRange source_range = ColorRange::Unspecified; // 实际应按什么范围解码
  ColorSpace target_space = ColorSpace::BT709;       // 输出空间（本产品固定 709）
  TransferFunction target_transfer = TransferFunction::BT709;

  bool needs_matrix_conversion = false; // 需 601 → 709 矩阵转换
  bool needs_range_conversion = false;  // 需 Limited ↔ Full 拉伸
  bool guessed = false;                 // 任一维度（空间/范围）来自推断即为 true
  std::string reason;                   // 判定依据，进自检报告
};

// 根据帧描述（含元数据标记）与可选的亮度探测结果做决策。
// 参数说明：
//   desc  —— 帧描述，读取 colorspace / range / height / sar / fps
//   probe —— 可为 nullptr；传入则用实测分布纠正 range 错标
// 返回值：决策结果。此函数是纯函数，便于单测。
ColorDecision DecideColor(const FrameDesc& desc, const LumaRangeProbe* probe = nullptr);

// 便捷重载：直接从帧缓冲推断（内部会跑一次 ProbeLumaRange）
ColorDecision DecideColor(const FrameBuffer& frame);

} // namespace lumen
