// ============================================================================
//  LumenPlayer - 时域/场结构分析器
//  ---------------------------------------------------------------------------
//  所属层次：产品层（C 层）—— 片源自检的核心算法
//  职责    ：判断一段素材是「真逐行」「真隔行」还是「3:2 pulldown 电影」，
//            并给出是否需要去隔行 / 是否可还原 24p 的建议。
//
//  为什么这是 P0 而不是优化项：
//    把隔行帧直接送到超分模型，模型会把梳齿（combing）当作纹理细节放大，
//    输出比原片更糟。必须先判定时域结构，再决定处理链。
//
//  算法说明（可解释、可单测，不依赖任何第三方库）：
//    1. 梳齿像素检测：对每个像素，取其上下相邻行（在隔行帧中这两行属同一场，
//       而当前行属另一场）。若当前行像素明显偏离上下行的插值，且呈"单行尖刺"
//       形态，则该像素为梳齿。逐行画面几乎不会出现这种形态。
//    2. 场间/场内行差比：隔行画面在运动区域，跨 1 行的差（异场）会显著大于
//       跨 2 行的差（同场）。该比值作为辅助判据，抗单点噪声。
//    3. 重复帧检测：计算相邻帧的亮度 SAD。3:2 pulldown 会产生
//       "新/新/重/新/重" 的 5 帧周期，据此判定 telecine 并可规划 IVTC。
// ============================================================================
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "frame_buffer.h"
#include "frame_desc.h"
#include "types.h"

namespace lumen {

// ---------------------------------------------------------------------------
// 单帧场结构统计
// ---------------------------------------------------------------------------
struct FieldStats {
  // 梳齿像素占有效采样像素的比例。逐行素材通常 < 0.2%，隔行运动镜头可 > 3%。
  double combing_pixel_ratio = 0.0;
  // 异场行差 / 同场行差。逐行素材约 0.6—1.0；隔行运动素材 > 1.15。
  double field_diff_ratio = 0.0;
  // 水平/垂直梯度能量，用于区分"高细节"与"真梳齿"（高细节也会推高行差比）
  double detail_energy = 0.0;
  // 平均亮度，用于跳过黑场（黑场无信息，统计会污染判定）
  double mean_luma = 0.0;
  // 实际参与统计的像素数（为 0 表示该帧无效，应跳过）
  int64_t sampled_pixels = 0;
  // 综合判定：本帧是否呈现场结构
  bool has_field_structure = false;

  bool Valid() const { return sampled_pixels > 1000; }
};

// 分析参数。默认值针对 480p/720p 老片调优，兼顾精度与速度。
struct FieldAnalyzeOptions {
  int edge_guard = 8;       // 忽略四周 N 像素，避开台标、黑边、字幕遮挡
  int combing_threshold = 10; // 梳齿判定阈值（8bit 亮度差），过小会误判胶片颗粒
  int min_luma = 20;        // 低于此亮度的像素不计入统计（黑边）
  int combing_ratio_percent = 2; // 梳齿像素占比超过该百分比（%）即判为有场结构
  double field_ratio_threshold = 1.15; // 行差比阈值
  int max_sample_cols = 960; // 列方向最多采样数，超大分辨率时按步长抽稀
  bool SkipDarkFrame = true; // 是否跳过黑场
};

// 分析一帧亮度平面的场结构。
// 参数说明：y/stride 为亮度平面；w/h 为宽高；opt 见上。
// 返回值  ：统计结果；若帧过暗或过小，sampled_pixels 会很小，调用方应忽略。
FieldStats AnalyzeField(const uint8_t* y, int w, int h, int stride,
                        const FieldAnalyzeOptions& opt = FieldAnalyzeOptions());

// FrameBuffer 重载：仅接受带亮度平面的格式（YUV / NV12）。
FieldStats AnalyzeField(const FrameBuffer& frame,
                        const FieldAnalyzeOptions& opt = FieldAnalyzeOptions());

// ---------------------------------------------------------------------------
// 帧间差异（用于 telecine 检测与重复帧剔除）
// ---------------------------------------------------------------------------
struct FrameDiff {
  double sad = 0.0;           // 平均绝对差（0—255）
  bool is_duplicate = false;  // 判定为重复帧
};

// 比较两帧亮度平面。
// 参数说明：阈值 duplicate_threshold 建议 1.0—1.5；
//           压缩噪声会让"真重复帧"的 SAD 略大于 0，不能直接用 0 判定。
FrameDiff CompareFrames(const FrameBuffer& a, const FrameBuffer& b,
                        double duplicate_threshold = 1.2);

// ---------------------------------------------------------------------------
// 场级重复信息（telecine 判定的关键依据）
// ---------------------------------------------------------------------------
// 原理：3:2 pulldown 是把 24p 电影帧拆成场后按 3:2 重复排列。
//       因此相邻两帧之间，会出现"某一场与前一帧的同场完全相同"的现象，
//       而摄像机拍摄的真隔行素材（老剧、体育）几乎不会出现整场重复。
//       这是区分「可逆 telecine」与「真隔行」最可靠的单一判据。
struct FieldRepeatInfo {
  double top_sad = 0.0;     // 顶场（偶数行）与前一帧顶场的平均绝对差
  double bottom_sad = 0.0;  // 底场（奇数行）与前一帧底场的平均绝对差
  double frame_sad = 0.0;   // 整帧平均绝对差（用于排除静止画面）
  bool top_repeat = false;
  bool bottom_repeat = false;
  bool is_duplicate = false; // 整帧重复（静止镜头或动画定格）
};

// 参数说明：prev/cur 需尺寸一致；repeat_threshold 为场重复判定阈值（8bit 平均差）。
// 返回值  ：场级差异与重复标记。尺寸不一致时返回全 0 结构。
FieldRepeatInfo CompareFields(const FrameBuffer& prev, const FrameBuffer& cur,
                              double repeat_threshold = 1.5);

// ---------------------------------------------------------------------------
// 节奏判定结论
// ---------------------------------------------------------------------------
struct CadenceVerdict {
  SourceCadence cadence = SourceCadence::Unknown;
  double confidence = 0.0;   // 0—1，低于 0.4 建议人工复核
  FieldOrder suggested_order = FieldOrder::Unknown;
  bool needs_deinterlace = false; // 处理链是否需要去隔行
  bool ivtc_recoverable = false;  // 是否为可逆 telecine（可还原 24p，画质最优）
  std::string reason;             // 人可读的判定依据，直接进自检报告
};

// ---------------------------------------------------------------------------
// 节奏检测器
// ---------------------------------------------------------------------------
// 用法：逐帧 Feed()，随时 Verdict() 取当前结论。
// 设计为流式：播放时也能持续修正判断（片头可能是台标/黑场，不代表全片）。
class CadenceDetector {
 public:
  explicit CadenceDetector(FieldAnalyzeOptions opt = FieldAnalyzeOptions());

  // 送入一帧。内部会缓存上一帧用于帧间比较。
  // 注意：同一帧不要重复送入，否则重复帧统计会失真。
  void Feed(const FrameBuffer& frame);

  // 取当前结论。样本不足时返回 Unknown 且 confidence 为 0。
  CadenceVerdict Verdict() const;

  // 已送入的有效帧数
  int frame_count() const { return static_cast<int>(field_structure_.size()); }

  void Reset();

  // 最少有效帧数（低于此值不下结论）
  static constexpr int kMinFramesForVerdict = 12;

 private:
  FieldAnalyzeOptions opt_;
  std::deque<bool> field_structure_; // 每帧是否有场结构
  std::deque<FieldRepeatInfo> repeat_; // 每帧的场级重复信息
  std::deque<FieldStats> stats_;
  int max_history_ = 240;            // 最多保留 240 帧（约 8 秒），防止内存无限增长

  bool has_prev_ = false;
  FrameBuffer prev_;

  // 判定是否为 3:2 pulldown（telecine）。
  // 输入：历史帧的场重复标记；输出：detail 写入判定依据文本
  bool DetectTelecinePattern(double* ratio_out, std::string* detail) const;
};

} // namespace lumen
