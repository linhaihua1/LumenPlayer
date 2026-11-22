// ============================================================================
//  LumenPlayer - 时域/场结构分析器实现
//  所属层次：产品层（C 层）
// ============================================================================
#include "field_analyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace lumen {
namespace {

// 抽稀步长：控制单帧统计成本，保证 1080p 也能在 1—2ms 内跑完。
int SampleStep(int span, int max_samples) {
  if (max_samples <= 0 || span <= max_samples) return 1;
  const int s = span / max_samples;
  return s < 1 ? 1 : s;
}

double Clamp01(double v) {
  if (v < 0.0) return 0.0;
  if (v > 1.0) return 1.0;
  return v;
}

} // namespace

// ---------------------------------------------------------------------------
// 单帧场结构分析
// ---------------------------------------------------------------------------
FieldStats AnalyzeField(const uint8_t* y, int w, int h, int stride,
                        const FieldAnalyzeOptions& opt) {
  FieldStats st;
  if (!y || w < 32 || h < 32 || stride <= 0) return st;

  // 需要访问 ±2 行，因此上下各留 max(edge_guard, 2) 的边界
  const int g = std::max(opt.edge_guard, 2);
  const int y0 = g;
  const int y1 = h - 1 - g;
  const int x0 = g;
  const int x1 = w - g;
  if (y1 - y0 < 16 || x1 - x0 < 16) return st;

  const int step = SampleStep(x1 - x0, opt.max_sample_cols);
  const int thr = opt.combing_threshold;

  int64_t pixels = 0;      // 有效采样像素数
  int64_t combing = 0;     // 梳齿像素数
  double intra_sum = 0.0;  // 同场（跨 2 行）绝对差累计
  double inter_sum = 0.0;  // 异场（跨 1 行）绝对差累计
  double luma_sum = 0.0;

  for (int i = y0; i <= y1; ++i) {
    const uint8_t* cur = y + static_cast<size_t>(i) * stride;
    const uint8_t* up1 = cur - stride;         // 异场（相邻行）
    const uint8_t* dn1 = cur + stride;         // 异场
    const uint8_t* up2 = cur - 2 * stride;     // 同场（隔行）
    const uint8_t* dn2 = cur + 2 * stride;     // 同场

    for (int x = x0; x < x1; x += step) {
      const int c = cur[x];
      const int a1 = up1[x];
      const int b1 = dn1[x];

      // 亮度门限：跳过黑边与纯黑场，避免退化的统计污染判定
      const int local_mean = (a1 + b1 + c) / 3;
      if (local_mean < opt.min_luma) continue;

      const int a2 = up2[x];
      const int b2 = dn2[x];

      ++pixels;
      luma_sum += c;

      // 异场差：当前行与紧邻行（属于另一场）
      inter_sum += (std::abs(c - a1) + std::abs(c - b1)) * 0.5;
      // 同场差：当前行与隔一行（属于同一场）
      intra_sum += (std::abs(c - a2) + std::abs(c - b2)) * 0.5;

      // 梳齿判定：c 明显偏离同场上下的中点，且 c 同时高于或同时低于上下行。
      // 后一个条件很关键——它排除了"斜坡纹理"（天然的高行差），
      // 只保留真正的"单行尖刺"，这正是隔行梳齿的形态特征。
      const int mid = (a1 + b1) / 2;
      if (std::abs(c - mid) > thr && (c - a1) * (c - b1) > 0) {
        ++combing;
      }
    }
  }

  if (pixels < 1000) {
    // 帧过暗或过窄，统计不可信；保留 sampled_pixels 供调用方判断
    st.sampled_pixels = pixels;
    if (pixels > 0) st.mean_luma = luma_sum / static_cast<double>(pixels);
    return st;
  }

  const double inv = 1.0 / static_cast<double>(pixels);
  const double intra_avg = intra_sum * inv;
  const double inter_avg = inter_sum * inv;

  st.sampled_pixels = pixels;
  st.mean_luma = luma_sum * inv;
  st.detail_energy = intra_avg;
  st.combing_pixel_ratio = static_cast<double>(combing) * inv;
  st.field_diff_ratio = (intra_avg > 1e-3) ? (inter_avg / intra_avg) : 0.0;

  // 高细节画面本身就会抬高行差比，因此按细节能量动态抬高阈值，
  // 否则 4K 转制的细密纹理素材会被误判成隔行。
  const double extra = std::min(0.25, st.detail_energy / 200.0);
  const double eff_ratio_thr = opt.field_ratio_threshold + extra;

  st.has_field_structure =
      (st.combing_pixel_ratio * 100.0 >= opt.combing_ratio_percent) ||
      (st.field_diff_ratio >= eff_ratio_thr);

  return st;
}

FieldStats AnalyzeField(const FrameBuffer& frame, const FieldAnalyzeOptions& opt) {
  if (frame.empty()) return FieldStats();
  const PlaneView y = frame.Plane(0);
  if (!y.data) return FieldStats();
  return AnalyzeField(y.data, frame.width(), frame.height(), y.stride, opt);
}

// ---------------------------------------------------------------------------
// 帧间比较
// ---------------------------------------------------------------------------
FrameDiff CompareFrames(const FrameBuffer& a, const FrameBuffer& b,
                        double duplicate_threshold) {
  FrameDiff diff;
  if (a.empty() || b.empty()) return diff;
  if (a.width() != b.width() || a.height() != b.height()) return diff;

  const PlaneView pa = a.Plane(0);
  const PlaneView pb = b.Plane(0);
  if (!pa.data || !pb.data) return diff;

  const int w = a.width();
  const int h = a.height();
  const int step = SampleStep(w, 960);

  double sum = 0.0;
  int64_t n = 0;
  for (int i = 0; i < h; ++i) {
    const uint8_t* ra = pa.data + static_cast<size_t>(i) * pa.stride;
    const uint8_t* rb = pb.data + static_cast<size_t>(i) * pb.stride;
    for (int x = 0; x < w; x += step) {
      sum += std::abs(static_cast<int>(ra[x]) - static_cast<int>(rb[x]));
      ++n;
    }
  }
  if (n == 0) return diff;

  diff.sad = sum / static_cast<double>(n);
  diff.is_duplicate = diff.sad < duplicate_threshold;
  return diff;
}

FieldRepeatInfo CompareFields(const FrameBuffer& prev, const FrameBuffer& cur,
                              double repeat_threshold) {
  FieldRepeatInfo info;
  if (prev.empty() || cur.empty()) return info;
  if (prev.width() != cur.width() || prev.height() != cur.height()) return info;

  const PlaneView pa = prev.Plane(0);
  const PlaneView pb = cur.Plane(0);
  if (!pa.data || !pb.data) return info;

  const int w = prev.width();
  const int h = prev.height();
  const int step = SampleStep(w, 960);

  double top_sum = 0.0, bottom_sum = 0.0, all_sum = 0.0;
  int64_t top_n = 0, bottom_n = 0, all_n = 0;

  for (int i = 0; i < h; ++i) {
    const uint8_t* ra = pa.data + static_cast<size_t>(i) * pa.stride;
    const uint8_t* rb = pb.data + static_cast<size_t>(i) * pb.stride;
    for (int x = 0; x < w; x += step) {
      const double d = std::abs(static_cast<int>(ra[x]) - static_cast<int>(rb[x]));
      all_sum += d;
      ++all_n;
      // 约定：第 0 行为顶场（TFF 前提下）。若片源是 BFF，
      // 这里的 top/bottom 只是"偶行/奇行"的代称，不影响重复模式的周期判定。
      if ((i & 1) == 0) {
        top_sum += d;
        ++top_n;
      } else {
        bottom_sum += d;
        ++bottom_n;
      }
    }
  }

  if (all_n == 0) return info;

  info.frame_sad = all_sum / static_cast<double>(all_n);
  info.top_sad = (top_n > 0) ? top_sum / static_cast<double>(top_n) : 0.0;
  info.bottom_sad = (bottom_n > 0) ? bottom_sum / static_cast<double>(bottom_n) : 0.0;
  info.top_repeat = info.top_sad < repeat_threshold;
  info.bottom_repeat = info.bottom_sad < repeat_threshold;
  info.is_duplicate = info.frame_sad < repeat_threshold;
  return info;
}

// ---------------------------------------------------------------------------
// 检测器
// ---------------------------------------------------------------------------
CadenceDetector::CadenceDetector(FieldAnalyzeOptions opt) : opt_(opt) {}

void CadenceDetector::Reset() {
  field_structure_.clear();
  repeat_.clear();
  stats_.clear();
  has_prev_ = false;
  prev_.Reset();
}

void CadenceDetector::Feed(const FrameBuffer& frame) {
  if (frame.empty()) return;
  const PlaneView y = frame.Plane(0);
  if (!y.data) return;

  const FieldStats st =
      AnalyzeField(y.data, frame.width(), frame.height(), y.stride, opt_);

  // 黑场/超窄帧不参与判定：片头黑场会给出"无场结构"的假证据
  if (!st.Valid() && opt_.SkipDarkFrame) return;

  field_structure_.push_back(st.has_field_structure);
  stats_.push_back(st);

  if (has_prev_ && prev_.width() == frame.width() && prev_.height() == frame.height()) {
    repeat_.push_back(CompareFields(prev_, frame));
  } else {
    repeat_.push_back(FieldRepeatInfo());
  }

  // 缓存当前帧用于下一轮比较。这里必须深拷贝：
  // 解码层会复用帧缓冲，留引用会在下一帧到来时变成"自己和自己比"。
  prev_.Resize(frame.width(), frame.height(), frame.format());
  const PlaneView dst = prev_.Plane(0);
  if (dst.data) {
    const int w = frame.width();
    const int rows = std::min(frame.PlaneHeight(0), prev_.PlaneHeight(0));
    for (int i = 0; i < rows; ++i) {
      const uint8_t* src = y.data + static_cast<size_t>(i) * y.stride;
      uint8_t* d = dst.data + static_cast<size_t>(i) * dst.stride;
      std::copy(src, src + w, d);
    }
    has_prev_ = true;
  }

  // 限制历史长度，保证长片播放时内存恒定
  while (static_cast<int>(field_structure_.size()) > max_history_) {
    field_structure_.pop_front();
    stats_.pop_front();
    if (!repeat_.empty()) repeat_.pop_front();
  }
}

bool CadenceDetector::DetectTelecinePattern(double* ratio_out,
                                            std::string* detail) const {
  // 只统计最近 20 帧的场重复情况。样本太短容易把静止镜头误判为 pulldown。
  const size_t window = std::min<size_t>(repeat_.size(), 20);
  if (window < 10) return false;

  size_t single_field_repeat = 0; // 只有一场重复（另一场在动）
  size_t counted = 0;
  for (size_t i = repeat_.size() - window; i < repeat_.size(); ++i) {
    const FieldRepeatInfo& r = repeat_[i];
    if (r.frame_sad <= 0.0) continue; // 无效样本
    if (r.is_duplicate) continue;     // 整帧静止，不构成 pulldown 证据
    ++counted;
    if (r.top_repeat != r.bottom_repeat) ++single_field_repeat;
  }
  if (counted < 8) return false;

  const double ratio = static_cast<double>(single_field_repeat) / counted;
  if (ratio_out) *ratio_out = ratio;

  // 3:2 pulldown 的理论比例是 2/5 = 0.4（每 5 帧有 2 帧存在单场重复）。
  // 实测受剪辑与运动影响在 0.25—0.6 之间波动，超出则不是稳定的 pulldown。
  const bool matched = (ratio >= 0.25 && ratio <= 0.60);
  if (matched && detail) {
    *detail = "检测到单场重复比例 " + std::to_string(static_cast<int>(ratio * 100)) +
              "%（3:2 pulldown 理论值 40%），可还原 24p";
  }
  return matched;
}

CadenceVerdict CadenceDetector::Verdict() const {
  CadenceVerdict v;
  const int n = static_cast<int>(field_structure_.size());
  if (n < kMinFramesForVerdict) {
    v.reason = "有效样本不足（" + std::to_string(n) + "/" +
               std::to_string(kMinFramesForVerdict) + " 帧），暂不下结论";
    return v;
  }

  int with_fields = 0;
  for (bool b : field_structure_) {
    if (b) ++with_fields;
  }
  const double field_ratio = static_cast<double>(with_fields) / n;

  double tele_ratio = 0.0;
  std::string tele_detail;
  const bool telecine = DetectTelecinePattern(&tele_ratio, &tele_detail);

  if (telecine && field_ratio > 0.25) {
    v.cadence = SourceCadence::Telecine;
    v.confidence = Clamp01(0.55 + tele_ratio);
    v.ivtc_recoverable = true;
    // telecine 不该用 deinterlace：正确做法是 IVTC（场匹配 + 删重复帧）还原 24p，
    // 这样等于凭空多出 20% 的有效画质预算，是整条管线上收益最大的一步。
    v.needs_deinterlace = false;
    v.reason = tele_detail;
    return v;
  }

  if (field_ratio >= 0.55) {
    v.cadence = SourceCadence::Interlaced;
    v.confidence = Clamp01(field_ratio);
    v.needs_deinterlace = true;
    // 场序不在这里猜：绝大多数 NTSC DVD 是 TFF，但猜错会让运动方向反直觉。
    // 优先使用容器元数据，缺省才回落到 TFF，并在报告里明确标注。
    v.suggested_order = FieldOrder::TopFieldFirst;
    v.reason = "有场结构帧占比 " +
               std::to_string(static_cast<int>(field_ratio * 100)) +
               "%，判定为真隔行；场序默认 TFF（若运动方向异常请手动覆盖）";
    return v;
  }

  if (field_ratio <= 0.15) {
    v.cadence = SourceCadence::Progressive;
    v.confidence = Clamp01(1.0 - field_ratio);
    v.needs_deinterlace = false;
    v.suggested_order = FieldOrder::Progressive;
    v.reason = "有场结构帧占比仅 " +
               std::to_string(static_cast<int>(field_ratio * 100)) + "%，判定为逐行";
    return v;
  }

  // 中间地带：常见于"逐行主体 + 少量隔行插入镜头"的剧集，或含大量静止镜头的素材。
  v.cadence = SourceCadence::Mixed;
  v.confidence = 0.5;
  v.needs_deinterlace = true; // 保守策略：只要有隔行镜头就必须处理
  v.suggested_order = FieldOrder::TopFieldFirst;
  v.reason = "有场结构帧占比 " +
             std::to_string(static_cast<int>(field_ratio * 100)) +
             "%，判定为混合素材；按隔行保守处理，避免个别镜头出现梳齿";
  return v;
}

} // namespace lumen
