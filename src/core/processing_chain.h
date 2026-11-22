// ============================================================================
//  LumenPlayer - 处理链（策略契约）
//  ---------------------------------------------------------------------------
//  所属层次：跨层契约（接缝 3，见 ADR-001）
//  职责    ：把"片源自检结论"翻译成"可执行的处理参数"，并序列化给 UI 展示。
//
//  ★ 契约约束：
//    1. 本结构必须是纯数据 + 可序列化，不得持有任何实现对象（纹理、句柄）。
//       原因：它要跨进程传给渲染进程，也要落盘用于复现同一个处理效果。
//    2. UI 对本结构只读。用户可改的只是"预设档位"，由档位映射生成新链路，
//       不允许 UI 直接拼装处理步骤（否则实时与离线导出的画质会不一致）。
//    3. 实时播放与离线导出必须复用同一个 ProcessingChain，
//       这是保证"所见即所得"的唯一办法。
// ============================================================================
#pragma once

#include <string>

#include "color_policy.h"
#include "field_analyzer.h"
#include "grain_policy.h"
#include "source_info.h"
#include "types.h"

namespace lumen {

// ---------------------------------------------------------------------------
// 处理模式枚举
// ---------------------------------------------------------------------------
// 场处理方式。注意 IVTC 与 Deinterlace 是两件不同的事：
//   IVTC  = 场匹配 + 删重复帧，还原 24p，信息量无损，是影视素材的最优解；
//   Deinterlace = 每帧内插出缺失的场，会损失一半垂直信息，只在真隔行时使用。
enum class FieldMode {
  Passthrough = 0, // 逐行，直接过
  Deinterlace,     // 反交错（bwdif/yadif 类）
  IVTC,            // 逆 telecine（还原电影帧率）
};

enum class DenoiseMode {
  Off = 0,
  Light,  // 轻度：只压块效应与色度噪声，保留颗粒
  Medium, // 中度：用于高噪点/低保真片源
};

enum class SharpenMode {
  Off = 0,
  Light,  // 模型输出后再做一点细节增强
};

// 画质预设档位。用户只需在这四档之间选，细节由引擎决定。
enum class QualityPreset {
  Performance = 0, // 性能优先（弱显卡、笔记本）
  Balanced,        // 默认
  Quality,         // 画质优先
  Archive,         // 离线导出/收藏级
};

const char* ToString(FieldMode m);
const char* ToString(DenoiseMode m);
const char* ToString(SharpenMode m);
const char* ToString(QualityPreset p);

// ---------------------------------------------------------------------------
// 处理链
// ---------------------------------------------------------------------------
struct ProcessingChain {
  // --- 时域 ---
  FieldMode field_mode = FieldMode::Passthrough;
  FieldOrder assumed_field_order = FieldOrder::Unknown; // 去隔行时使用的场序
  bool bob_to_progressive = false; // 是否为 bob（双倍帧率输出），本期不做

  // --- 色彩 ---
  ColorSpace source_space = ColorSpace::Unspecified;
  ColorRange source_range = ColorRange::Limited;
  ColorSpace target_space = ColorSpace::BT709;
  bool convert_matrix = false; // 601 → 709
  bool convert_range = false;  // Full → Limited

  // --- 颗粒 ---
  bool grain_restore = false;
  double grain_amplitude = 0.0; // 0—1

  // --- 前处理 ---
  DenoiseMode denoise = DenoiseMode::Off;
  bool deblock = false; // 去块效应（MPEG-2 标清常用）

  // --- 超分 ---
  int target_width = 1920;
  int target_height = 1080;
  double scale_x = 1.0; // 水平放大倍数
  double scale_y = 1.0;
  bool use_anime_model = false; // 是否走动画专用模型
  SharpenMode sharpen = SharpenMode::Off;
  double sharpen_amount = 0.0;

  // --- 元信息 ---
  QualityPreset preset = QualityPreset::Balanced;
  std::string rationale; // 决策依据汇总，UI 的"为什么这样处理"面板直接显示

  // 序列化为 JSON，供 UI 只读展示与落盘复现。
  // 输出为单行紧凑格式，字段名固定（改动需同步 UI 与离线导出）。
  std::string ToJson() const;

  // 人可读摘要，用于日志与自检报告
  std::string Describe() const;

  // 帧率是否因 IVTC 变化（24p 还原后需要通知播放器重建时间轴）
  bool ChangesFrameRate() const { return field_mode == FieldMode::IVTC; }
};

// ---------------------------------------------------------------------------
// 决策入口
// ---------------------------------------------------------------------------
// 把四路分析结论合成为一条处理链。
// 参数说明：
//   src    —— 片源元数据（分辨率、SAR、编码、位率）
//   cadence—— 时域判定（来自 CadenceDetector）
//   color  —— 色彩判定（来自 DecideColor）
//   grain  —— 颗粒判定（来自 DecideGrain）
//   preset —— 用户档位
//   anime  —— 是否为动画素材（影响模型与颗粒强度）
// 返回值：完整处理链。纯函数，便于单测与"同输入必同输出"的复现性保证。
ProcessingChain BuildChain(const SourceInfo& src, const CadenceVerdict& cadence,
                           const ColorDecision& color, const GrainDecision& grain,
                           QualityPreset preset = QualityPreset::Balanced,
                           bool anime = false);

// 档位 → 目标分辨率与增强强度
// 参数说明：preset 为用户档位；src_height 为源高，用于决定是否需要放大
void ApplyPreset(QualityPreset preset, ProcessingChain* chain);

// 用户手动指定场序（覆盖自动判定结论）。
// 参数说明：
//   fo    —— 用户指定的场序；FieldOrder::Unknown 表示不覆盖（保持原结论）
//   chain —— 待修改的处理链
// 语义约定：
//   * 指定 Progressive → 取消场处理（用户认为自动判定"误判隔行"）
//   * 指定 TFF / BFF   → 若原结论是逐行，则强制改为反交错（用户认为"漏判隔行"）
//   * 无论哪种都会在 rationale 里留痕，HUD 上能看到"这是人工指定的"
// 为什么需要：场序判定依赖行间差统计，在高速运动/强噪点镜头上会出错；
//   而"场序选反"的视觉表现是画面上下轻微抖动，用户很难用语言描述给客服。
void OverrideFieldOrder(FieldOrder fo, ProcessingChain* chain);

} // namespace lumen
