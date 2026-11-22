// ============================================================================
//  LumenPlayer - 参考实现后端（双三次插值）
//  所属层次：引擎层
// ============================================================================
#include "engine/dummy_backend.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace lumen {
namespace {

// Catmull-Rom（a = -0.5）三次卷积核权重。
// 参数说明：t 为采样点相对两个中间样本的位置（0—1），w 输出对应 -1/0/1/2 四个抽头。
// 选择 Catmull-Rom 而不是 B 样条：它在保持锐度的同时振铃较小，
// 用作"画质基线"比双线性更有说服力（AI 超分必须赢过它才算有价值）。
void CubicWeights(double t, double w[4]) {
  const double t2 = t * t;
  const double t3 = t2 * t;
  w[0] = -0.5 * t3 + t2 - 0.5 * t;         // p(-1)
  w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;        // p(0)
  w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;   // p(1)
  w[3] = 0.5 * t3 - 0.5 * t2;              // p(2)
}

inline uint8_t Clamp8(double v) {
  if (v < 0.0) return 0;
  if (v > 255.0) return 255;
  return static_cast<uint8_t>(v + 0.5);
}

inline int ClampInt(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// 单平面双三次缩放（可分离：先垂直后水平）。
// 参数说明：src/dst 为平面首地址，stride 为各自行字节数；
//           sw/sh 为源尺寸，dw/dh 为目标尺寸。
void ScalePlaneBicubic(const uint8_t* src, int sw, int sh, int sstride,
                       uint8_t* dst, int dw, int dh, int dstride) {
  if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

  // 第一遍：垂直放大，结果尺寸 sw x dh
  std::vector<double> tmp(static_cast<size_t>(sw) * dh, 0.0);

  for (int y = 0; y < dh; ++y) {
    // 像素中心对齐：目标像素 y 映射回源坐标
    const double sy = (y + 0.5) * sh / dh - 0.5;
    const int base = static_cast<int>(std::floor(sy));
    const double fy = sy - base;
    double w[4];
    CubicWeights(fy, w);

    const uint8_t* rows[4];
    for (int k = 0; k < 4; ++k) {
      const int yy = ClampInt(base - 1 + k, 0, sh - 1);
      rows[k] = src + static_cast<size_t>(yy) * sstride;
    }

    double* out_row = tmp.data() + static_cast<size_t>(y) * sw;
    for (int x = 0; x < sw; ++x) {
      out_row[x] = rows[0][x] * w[0] + rows[1][x] * w[1] + rows[2][x] * w[2] +
                   rows[3][x] * w[3];
    }
  }

  // 第二遍：水平放大，直接从 tmp 写目标平面
  for (int y = 0; y < dh; ++y) {
    const double* in_row = tmp.data() + static_cast<size_t>(y) * sw;
    uint8_t* out_row = dst + static_cast<size_t>(y) * dstride;
    for (int x = 0; x < dw; ++x) {
      const double sx = (x + 0.5) * sw / dw - 0.5;
      const int base = static_cast<int>(std::floor(sx));
      const double fx = sx - base;
      double w[4];
      CubicWeights(fx, w);

      double acc = 0.0;
      for (int k = 0; k < 4; ++k) {
        acc += in_row[ClampInt(base - 1 + k, 0, sw - 1)] * w[k];
      }
      out_row[x] = Clamp8(acc);
    }
  }
}

// 最近邻拷贝（scale == 1 时走这条快路径，避免无意义的插值开销）
void CopyPlane(const uint8_t* src, int w, int h, int sstride,
               uint8_t* dst, int dstride) {
  const int bytes = std::min(w, dstride);
  for (int i = 0; i < h; ++i) {
    std::memcpy(dst + static_cast<size_t>(i) * dstride,
                src + static_cast<size_t>(i) * sstride, bytes);
  }
}

} // namespace

DummyBackend::~DummyBackend() { Shutdown(); }

Status DummyBackend::Init(const SRBackendConfig& config) {
  if (config.scale != 1 && config.scale != 2 && config.scale != 4) {
    return Status::Unsupported; // 与真实模型的能力保持一致，避免上层写出依赖 3x 的逻辑
  }
  config_ = config;
  initialized_ = true;
  stats_ = SRStats();
  sum_ms_ = 0.0;
  count_ms_ = 0;
  return Status::Ok;
}

void DummyBackend::Shutdown() {
  output_.Reset();
  initialized_ = false;
}

Status DummyBackend::Warmup() {
  if (!initialized_) return Status::Internal;

  // 用一个 64x64 的假帧跑一遍，把代码路径与内存分配预热。
  // 真实 GPU 后端这里还要把时钟拉到稳态，CPU 后端主要是触发一次分配。
  FrameBuffer probe;
  if (probe.Allocate(64, 64, PixelFormat::YUV420P) != Status::Ok) {
    return Status::OutOfMemory;
  }
  const FrameDesc in = probe.ToDesc();
  FrameDesc out;
  return Process(in, &out);
}

Status DummyBackend::Process(const FrameDesc& in, FrameDesc* out) {
  if (!initialized_ || out == nullptr) return Status::InvalidArgument;
  if (!in.IsValid()) return Status::InvalidArgument;
  if (in.location != FrameLocation::Cpu) {
    // 本后端是纯 CPU 实现，收到 GPU 帧说明上层串了路径——
    // 直接报错而不是悄悄回读，否则实时路径会被这行代码毁掉。
    return Status::Unsupported;
  }
  if (in.plane_data[0] == nullptr) return Status::InvalidArgument;

  const auto t0 = std::chrono::steady_clock::now();

  const int dw = in.width * config_.scale;
  const int dh = in.height * config_.scale;
  const Status st = output_.Resize(dw, dh, in.format);
  if (st != Status::Ok) return st;

  // 逐平面缩放。4:2:0 的色度平面按半分辨率处理，这是 YUV 放大的关键：
  // 若把色度也按全分辨率放大，输出会变成 4:4:4 布局，后续编码器会直接报错。
  const int planes = (in.format == PixelFormat::YUV420P || in.format == PixelFormat::YUV422P ||
                      in.format == PixelFormat::YUV444P)
                         ? 3
                         : 1;

  for (int p = 0; p < planes; ++p) {
    const PlaneView dst = output_.Plane(p);
    if (!dst.data) continue;

    const int sw = (p == 0) ? in.width : ((in.format == PixelFormat::YUV444P)
                                              ? in.width
                                              : (in.width + 1) / 2);
    const int sh = (p == 0) ? in.height : ((in.format == PixelFormat::YUV420P)
                                               ? (in.height + 1) / 2
                                               : in.height);
    const int dwp = (p == 0) ? dw : ((in.format == PixelFormat::YUV444P) ? dw : (dw + 1) / 2);
    const int dhp = (p == 0) ? dh : ((in.format == PixelFormat::YUV420P) ? (dh + 1) / 2 : dh);

    if (config_.scale == 1) {
      CopyPlane(in.plane_data[p], sw, sh, in.plane_stride[p], dst.data, dst.stride);
    } else {
      ScalePlaneBicubic(in.plane_data[p], sw, sh, in.plane_stride[p], dst.data, dwp, dhp,
                        dst.stride);
    }
  }

  // 元数据透传：pts / 场序 / 色彩标记必须原样带过去，
  // 否则下游会认为"这是一帧新的、未知色彩的内容"，引发二次转换。
  output_.set_pts(in.pts);
  output_.set_index(in.frame_index);
  output_.set_field_order(FieldOrder::Progressive); // 超分后必然是逐行
  output_.set_colorspace(in.colorspace);
  output_.set_range(in.range);
  output_.set_sar(in.sar);

  *out = output_.ToDesc();

  const auto t1 = std::chrono::steady_clock::now();
  const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  ++stats_.frames_done;
  stats_.last_ms = ms;
  stats_.max_ms = std::max(stats_.max_ms, ms);
  sum_ms_ += ms;
  ++count_ms_;
  stats_.avg_ms = sum_ms_ / count_ms_;

  return Status::Ok;
}

} // namespace lumen
