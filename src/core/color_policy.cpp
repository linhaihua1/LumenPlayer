// ============================================================================
//  LumenPlayer - 色彩策略实现
//  所属层次：产品层（C 层）
// ============================================================================
#include "color_policy.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace lumen {
namespace {

// 直方图分位计算。
// 参数说明：hist 为 256 桶直方图；total 为样本总数；q 为分位（0—1）
int Percentile(const int64_t* hist, int64_t total, double q) {
  if (total <= 0) return 0;
  const int64_t target = static_cast<int64_t>(static_cast<double>(total) * q);
  int64_t acc = 0;
  for (int i = 0; i < 256; ++i) {
    acc += hist[i];
    if (acc >= target) return i;
  }
  return 255;
}

} // namespace

LumaRangeProbe ProbeLumaRange(const uint8_t* y, int w, int h, int stride) {
  LumaRangeProbe probe;
  if (!y || w <= 0 || h <= 0 || stride <= 0) return probe;

  // 抽样：最多 480 列、每 2 行取一行。统计量只需分布形状，不需要全量。
  const int col_step = (w > 480) ? (w / 480) : 1;
  int64_t hist[256] = {0};
  int64_t total = 0;

  for (int i = 0; i < h; i += 2) {
    const uint8_t* row = y + static_cast<size_t>(i) * stride;
    for (int x = 0; x < w; x += col_step) {
      ++hist[row[x]];
      ++total;
    }
  }
  if (total == 0) return probe;

  probe.p_low = Percentile(hist, total, 0.005);
  probe.p_high = Percentile(hist, total, 0.995);

  // 判定逻辑：
  //   Limited 素材理论上限 235、下限 16，即使编码噪声也很少越过 8/247。
  //   若两端都有可观占比，说明素材实际用的是 0—255（Full），只是没标或标错。
  probe.suggests_full_range = (probe.p_low <= 6 && probe.p_high >= 250);

  char buf[160];
  std::snprintf(buf, sizeof(buf), "亮度分布 p0.5=%d, p99.5=%d，%s",
                probe.p_low, probe.p_high,
                probe.suggests_full_range ? "疑似 Full range（0—255）"
                                          : "符合 Limited range（16—235）");
  probe.detail = buf;
  return probe;
}

LumaRangeProbe ProbeLumaRange(const FrameBuffer& frame) {
  if (frame.empty()) return LumaRangeProbe();
  const PlaneView y = frame.Plane(0);
  if (!y.data) return LumaRangeProbe();
  return ProbeLumaRange(y.data, frame.width(), frame.height(), y.stride);
}

ColorDecision DecideColor(const FrameDesc& desc, const LumaRangeProbe* probe) {
  ColorDecision d;

  // -------------------------------------------------------------------------
  // 第一步：确定源色彩空间
  // -------------------------------------------------------------------------
  if (desc.colorspace == ColorSpace::BT601 || desc.colorspace == ColorSpace::BT709 ||
      desc.colorspace == ColorSpace::BT2020) {
    d.source_space = desc.colorspace;
    d.reason = std::string("元数据标注为 ") + ToString(desc.colorspace);
  } else {
    // 元数据缺失：按"分辨率体现的制作年代"推断。
    // 依据：标清（<=576 行）素材按 601 制作是行业惯例；
    //       720 行及以上按 709 制作，这是 ATSC/DVB 的规范要求。
    d.guessed = true;
    if (desc.height > 0 && desc.height <= 576) {
      d.source_space = ColorSpace::BT601;
      d.reason = "未标注色彩空间，按标清分辨率（" + std::to_string(desc.height) +
                 " 行）推断为 BT.601";
    } else if (desc.height >= 720) {
      d.source_space = ColorSpace::BT709;
      d.reason = "未标注色彩空间，按高清分辨率（" + std::to_string(desc.height) +
                 " 行）推断为 BT.709";
    } else {
      // 576 < height < 720 的罕见情况（如 704x576 变形宽屏）
      d.source_space = ColorSpace::BT601;
      d.reason = "未标注色彩空间且分辨率处于过渡区间，保守推断为 BT.601";
    }
  }

  // -------------------------------------------------------------------------
  // 第二步：确定源 range
  // -------------------------------------------------------------------------
  if (desc.range == ColorRange::Limited || desc.range == ColorRange::Full) {
    d.source_range = desc.range;
  } else if (probe != nullptr && probe->suggests_full_range) {
    d.source_range = ColorRange::Full;
    d.guessed = true;
    d.reason += "；亮度实测呈 Full range 特征（" + std::to_string(probe->p_low) + "—" +
                std::to_string(probe->p_high) + "）";
  } else {
    // 视频素材默认 Limited：这是广播与 DVD 的标准，猜错的代价最小。
    d.source_range = ColorRange::Limited;
    d.guessed = true;
  }

  // -------------------------------------------------------------------------
  // 第三步：是否需要转换
  // -------------------------------------------------------------------------
  d.needs_matrix_conversion = (d.source_space != d.target_space);

  // range 转换的必要性：
  //   源 Full → 目标 Limited 要压缩；源 Limited → 目标 Limited 无需处理。
  //   注意：本产品输出目标是 BT.709 Limited（符合视频播放惯例），
  //   而不是显示器 Full。若将来直出 Full 给桌面渲染，这里需相应调整。
  const ColorRange target_range = ColorRange::Limited;
  d.needs_range_conversion = (d.source_range != target_range);

  if (d.needs_matrix_conversion) {
    d.reason += "；需做 601→709 矩阵转换（这一步对肤色正确性影响最大）";
  }
  if (d.needs_range_conversion) {
    d.reason += "；需做 Full→Limited 范围压缩";
  }

  return d;
}

ColorDecision DecideColor(const FrameBuffer& frame) {
  const FrameDesc desc = frame.ToDesc();
  const LumaRangeProbe probe = ProbeLumaRange(frame);
  return DecideColor(desc, &probe);
}

} // namespace lumen
