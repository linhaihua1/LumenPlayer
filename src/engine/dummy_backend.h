// ============================================================================
//  LumenPlayer - 参考实现后端（Dummy）
//  ---------------------------------------------------------------------------
//  所属层次：引擎层
//  职责    ：用 CPU 双三次插值实现 ISRBackend，用于：
//            1. 打通端到端管线（解码 → 策略 → 引擎 → 编码），先保证流程正确；
//            2. 作为画质基线——AI 超分必须明显优于双三次，否则没有存在价值；
//            3. 单元测试与 CI（不依赖 GPU 与模型文件）。
//
//  注意：本后端不追求速度（1080p 输出约 100—200ms/帧），
//        绝不可用于实时播放，只用于离线与测试。
// ============================================================================
#pragma once

#include "core/frame_buffer.h"
#include "engine/sr_backend.h"

namespace lumen {

class DummyBackend final : public ISRBackend {
 public:
  DummyBackend() = default;
  ~DummyBackend() override;

  const char* Name() const override { return "dummy(bicubic)"; }

  Status Init(const SRBackendConfig& config) override;
  void Shutdown() override;
  Status Warmup() override;
  Status Process(const FrameDesc& in, FrameDesc* out) override;

  SRStats Stats() const override { return stats_; }
  bool IsDeviceLost() const override { return false; }

 private:
  SRBackendConfig config_;
  SRStats stats_;
  FrameBuffer output_;   // 复用输出缓冲，避免每帧分配
  bool initialized_ = false;

  // 记录最近若干帧耗时用于滑动平均
  double sum_ms_ = 0.0;
  int count_ms_ = 0;
};

} // namespace lumen
