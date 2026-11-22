// ============================================================================
//  LumenPlayer - 胶片颗粒策略实现
//  所属层次：产品层（C 层）
// ============================================================================
#include "grain_policy.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <vector>

namespace lumen {
namespace {

// 拉普拉斯响应绝对值的上界（8bit：4*255 - 4*0 = 1020）
constexpr int kLapMax = 1020;

// 取直方图中位数
double MedianFromHist(const int64_t* hist, int bins, int64_t total) {
  if (total <= 0) return 0.0;
  const int64_t target = total / 2;
  int64_t acc = 0;
  for (int i = 0; i < bins; ++i) {
    acc += hist[i];
    if (acc >= target) return static_cast<double>(i);
  }
  return static_cast<double>(bins - 1);
}

} // namespace

GrainEstimate EstimateGrain(const uint8_t* y, int w, int h, int stride, int block_size) {
  GrainEstimate est;
  if (!y || w < 32 || h < 32 || stride < w) return est;
  if (block_size < 8) block_size = 8;

  const int bw = w / block_size;
  const int bh = h / block_size;
  if (bw < 2 || bh < 2) return est;

  // -------------------------------------------------------------------------
  // 第一步：分块算平均梯度，用于挑"平坦块"
  // -------------------------------------------------------------------------
  struct BlockStat {
    int bx = 0;
    int by = 0;
    double grad = 0.0;
  };
  std::vector<BlockStat> blocks;
  blocks.reserve(static_cast<size_t>(bw) * bh);

  for (int by = 0; by < bh; ++by) {
    for (int bx = 0; bx < bw; ++bx) {
      const int x0 = bx * block_size;
      const int y0 = by * block_size;
      double grad_sum = 0.0;
      int n = 0;
      for (int i = 1; i < block_size; ++i) {
        const uint8_t* row = y + static_cast<size_t>(y0 + i) * stride;
        const uint8_t* prev = row - stride;
        for (int x = 1; x < block_size; ++x) {
          const int c = row[x0 + x];
          grad_sum += std::abs(c - row[x0 + x - 1]) + std::abs(c - prev[x0 + x]);
          ++n;
        }
      }
      if (n == 0) continue;
      BlockStat s;
      s.bx = bx;
      s.by = by;
      s.grad = grad_sum / n;
      blocks.push_back(s);
    }
  }
  if (blocks.empty()) return est;

  // 取梯度最低的 25% 作为平坦块样本。
  // 只取一部分而不是全部：既能保证"够平坦"，又能保留足够样本量做中位数统计。
  std::sort(blocks.begin(), blocks.end(),
            [](const BlockStat& a, const BlockStat& b) { return a.grad < b.grad; });
  const size_t use = std::max<size_t>(4, blocks.size() / 4);

  // -------------------------------------------------------------------------
  // 第二步：在平坦块上统计拉普拉斯响应的中位数
  // -------------------------------------------------------------------------
  std::vector<int64_t> hist(kLapMax + 1, 0);
  int64_t total = 0;
  for (size_t k = 0; k < use; ++k) {
    const BlockStat& s = blocks[k];
    const int x0 = s.bx * block_size;
    const int y0 = s.by * block_size;
    for (int i = 1; i < block_size - 1; ++i) {
      const uint8_t* row = y + static_cast<size_t>(y0 + i) * stride;
      const uint8_t* up = row - stride;
      const uint8_t* dn = row + stride;
      for (int x = 1; x < block_size - 1; ++x) {
        const int c = row[x0 + x];
        const int lap = std::abs(4 * c - up[x0 + x] - dn[x0 + x] -
                                 row[x0 + x - 1] - row[x0 + x + 1]);
        ++hist[std::min(lap, kLapMax)];
        ++total;
      }
    }
  }

  if (total < 256) return est;

  const double median_lap = MedianFromHist(hist.data(), kLapMax + 1, total);
  // Donoho 稳健估计：σ = median(|L|) / 0.6745 / √6
  // 常数 0.6745 是标准正态分布的 75% 分位（MAD → σ 的换算），
  // √6 来自 5 点拉普拉斯核的系数平方和（16+1+1+1+1 = 20，归一化后为 6）。
  const double sigma = median_lap / 0.6745 / std::sqrt(6.0);

  est.sigma = sigma;
  est.blocks_used = static_cast<int>(use);
  est.flat_ratio = static_cast<double>(use) / static_cast<double>(blocks.size());

  char buf[200];
  std::snprintf(buf, sizeof(buf),
                "平坦块 %d/%zu（%.0f%%）参与统计，拉普拉斯中位数 %.1f → σ≈%.2f",
                est.blocks_used, blocks.size(), est.flat_ratio * 100.0,
                median_lap, sigma);
  est.detail = buf;
  return est;
}

GrainEstimate EstimateGrain(const FrameBuffer& frame, int block_size) {
  if (frame.empty()) return GrainEstimate();
  const PlaneView y = frame.Plane(0);
  if (!y.data) return GrainEstimate();
  return EstimateGrain(y.data, frame.width(), frame.height(), y.stride, block_size);
}

GrainDecision DecideGrain(const GrainEstimate& est, bool is_animation) {
  GrainDecision d;
  d.sigma = est.sigma;

  // 估计不可信（平坦区域过少，例如满是细节的画面）时不启用手动回注，
  // 交给处理链的默认轻度回注，避免按错误强度把画面弄脏。
  if (est.blocks_used == 0 || est.flat_ratio <= 0.0) {
    d.enabled = false;
    d.amplitude = 0.0;
    d.reason = "颗粒估计不可用（无有效平坦区域），跳过回注";
    return d;
  }

  if (est.sigma < 0.8) {
    d.enabled = false;
    d.reason = "σ<0.8，近似无颗粒（数字源或已强降噪），无需回注";
    return d;
  }

  if (est.sigma < 2.5) {
    d.enabled = true;
    d.amplitude = 0.35;
    d.reason = "细颗粒（σ 0.8—2.5），轻度回注以保留细微质感";
  } else if (est.sigma < 5.5) {
    d.enabled = true;
    d.amplitude = 0.65;
    d.reason = "明显胶片颗粒（σ 2.5—5.5），中强度回注以维持胶片感";
  } else {
    d.enabled = true;
    d.amplitude = 0.5;
    d.needs_denoise = true;
    d.reason = "重颗粒/噪声（σ>5.5），先轻度降噪再回注一半强度，避免噪点被放大";
  }

  // 动画素材：平涂区域多，颗粒回注过强会在大色块上形成明显噪点，
  // 观感上比"被抹平"更糟，所以统一压低 40%。
  if (is_animation) {
    d.amplitude *= 0.6;
    d.reason += "；动画素材强度下调 40%";
  }

  if (d.amplitude < 0.05) d.enabled = false;
  return d;
}

} // namespace lumen
