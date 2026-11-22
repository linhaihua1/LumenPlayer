// ============================================================================
//  LumenPlayer - 帧描述辅助实现
//  所属层次：产品层（C 层）
// ============================================================================
#include <cmath>

#include "frame_desc.h"

namespace lumen {

AspectClass ClassifyAspect(double dar, double tolerance) {
  if (dar <= 0.01) return AspectClass::Unknown;

  // 判定顺序按"离哪个标准更近"来，而不是逐个 if 判断区间。
  // 这样即使素材是 1.37（学院画幅）或 2.20 这类非常规值，
  // 也能落到最接近的档位，避免大量 Other 无法触发任何策略。
  struct Candidate {
    double ratio;
    AspectClass cls;
  };
  static const Candidate kCandidates[] = {
      {4.0 / 3.0, AspectClass::Ratio4x3},   // 1.333
      {16.0 / 9.0, AspectClass::Ratio16x9}, // 1.778
      {1.85, AspectClass::Ratio185},
      {2.35, AspectClass::Ratio235},
      {2.39, AspectClass::Ratio235},
  };

  double best_diff = 1e9;
  AspectClass best = AspectClass::Other;
  for (const auto& c : kCandidates) {
    const double diff = std::fabs(dar - c.ratio);
    if (diff < best_diff) {
      best_diff = diff;
      best = c.cls;
      if (diff <= tolerance && (c.cls == AspectClass::Ratio4x3 ||
                                c.cls == AspectClass::Ratio16x9)) {
        // 4:3 与 16:9 是唯一两个会驱动不同策略（裁边/铺满）的档位，
        // 命中即返回，避免 1.78 被 1.85 抢走。
        return best;
      }
    }
  }
  return (best_diff <= tolerance * 1.5) ? best : AspectClass::Other;
}

const char* ToString(AspectClass ac) {
  switch (ac) {
    case AspectClass::Ratio4x3: return "4:3";
    case AspectClass::Ratio16x9: return "16:9";
    case AspectClass::Ratio185: return "1.85:1";
    case AspectClass::Ratio235: return "2.35:1";
    case AspectClass::Other: return "other";
    case AspectClass::Unknown:
    default: return "unknown";
  }
}

} // namespace lumen
