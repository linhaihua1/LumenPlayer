// ============================================================================
//  LumenPlayer - ONNX Runtime 超分后端（ISRBackend 的实现之一）
//  ---------------------------------------------------------------------------
//  所属层次：引擎层
//  职责    ：跑一个 ONNX 超分模型，把 low-res 帧变成 high-res 帧。
//
//  ★ 为什么用「运行时动态加载」而不是链接 onnxruntime.lib：
//    1. 许可与分发：onnxruntime.dll 是 MIT，但把 DLL 打进安装包意味着我们要
//       跟着它一起升级；动态加载可以让用户自己放 DLL，我们只声明"需要它"。
//    2. 体积：链接导入库会让"没装运行时的用户"根本无法启动程序。
//       动态加载则可以在运行时给出"缺少 onnxruntime.dll，请放到这里"的提示。
//    3. 构建：编译期只需要一个头文件（third_party/onnxruntime/），
//       不需要下载 100MB 的 SDK，CI 与本地构建都不被运行时绑架。
//
//  ★ 执行提供程序（EP）选择：
//    auto  → 先试 DML（N/A/Intel 通用 GPU），再试 CUDA，最后 CPU
//    dml   → DirectML（Windows 全平台 GPU，A 卡/核显也能跑）
//    cuda  → CUDA（N 卡最快，但需要 CUDA/cuDNN）
//    cpu   → 纯 CPU（用于验证正确性，不追求速度）
//
//  ★ 分块推理：
//    大帧直接整帧喂给模型会瞬间打爆显存（4x 模型处理 1080p 需要 >10GB）。
//    分块 + 重叠 + 羽化加权，既控住显存，又不会在块边界留下接缝。
//    接缝是这类实现最容易翻车的地方：简单切块拼接会看到明显的方格，
//    因为模型在块边界的感受野是"截断"的。
// ============================================================================
#pragma once

#include <memory>
#include <string>

#include "engine/sr_backend.h"

namespace lumen {

class OnnxBackend final : public ISRBackend {
 public:
  OnnxBackend();
  ~OnnxBackend() override;

  const char* Name() const override;
  Status Init(const SRBackendConfig& config) override;
  void Shutdown() override;
  Status Warmup() override;
  Status Process(const FrameDesc& in, FrameDesc* out) override;
  SRStats Stats() const override;
  bool IsDeviceLost() const override;

  // 最近一次失败的详细原因（中文），供 UI/HUD 与命令行直接展示。
  // 设计意图：初始化失败的原因往往非常具体（DLL 缺失 / 模型 opset 过高 /
  // 显存不足），只回一个 Status 枚举用户根本无从下手。
  std::string LastError() const;

  // 实际生效的执行提供程序名称（"DML" / "CUDA" / "CPU"）。
  // 用途：HUD 上显示"你现在跑在什么后端上"，避免用户以为在用 GPU 其实在跑 CPU。
  std::string ActiveProvider() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lumen
