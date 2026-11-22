// ============================================================================
//  LumenPlayer - 超分引擎接口（ISRBackend）
//  ---------------------------------------------------------------------------
//  所属层次：跨层契约（接缝 2，见 ADR-001）
//  职责    ：屏蔽具体推理后端（libaji / ONNX Runtime / NCNN / 未来的自研）。
//
//  ★ 契约约束（这三条是"能不能换引擎"的生死线）：
//    1. 只做"帧进帧出"。不做解码、不做色彩转换、不做缩放决策——
//       那些属于策略层与渲染层，引擎只管把 low-res 变 high-res。
//    2. 禁止异常跨边界。所有失败通过 Status 返回，错误码为闭集
//       （见 types.h 的 Status）。GPU 回调、实时线程里抛异常是未定义行为。
//    3. 业务层不得知道后端类型。禁止在 UI/策略层出现 "tensorrt"、"dml"
//       这类字样——一旦泄漏，切换后端就要改业务代码，分层就白做了。
//
//  验证分层的唯一标准（W8 决策门）：
//    把 DummyBackend 换成 AjiBackend，策略层与 UI 零改动且画质性能不退化。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/frame_desc.h"
#include "core/types.h"

namespace lumen {

// 引擎配置
struct SRBackendConfig {
  std::string model_path;   // ONNX / engine 文件路径（各后端解释不同）
  int scale = 2;            // 放大倍数（2 或 4）
  bool allow_fp16 = true;   // 是否允许半精度（画质差异通常不可见，速度翻倍）
  bool allow_fp32_fallback = true; // fp16 不被支持时是否自动回落
  void* device = nullptr;   // 外部设备句柄（如 ID3D11Device*），可为空
  int tile_size = 0;        // 分块推理边长；0 表示整帧（显存不足时调小）
  int max_inflight = 1;     // 允许的并发帧数（实时路径为 1—2）

  // --- 以下字段为 2026-11 补齐（接 ONNX Runtime 时新增，均有默认值，不破坏既有代码）---
  // 推理运行时（onnxruntime.dll 等）所在目录。留空表示交给系统搜索（PATH 与 exe 同级）。
  // 显式配置的理由：用户通常把 DLL 放在模型旁边，而不是系统目录。
  std::string runtime_dir;
  // 执行提供程序偏好："auto" | "cpu" | "dml" | "cuda"。
  // 注意 auto 的语义是"按 DML → CUDA → CPU 顺序尝试"，与"自动选最快"不完全等价：
  // 不做基准对比，是因为建两次 session 的时间成本（秒级）远大于收益。
  std::string provider = "auto";
  // 模型输入通道数：3 = RGB（Real-ESRGAN 系），1 = 仅亮度（速度快 3 倍）。
  // 必须由配置显式给出：ONNX 的输入维度常是动态的（N,C,-1,-1），查不出通道数。
  int channels = 3;
};

// 运行统计。UI 的 HUD 直接消费这个结构。
struct SRStats {
  int64_t frames_done = 0;     // 已处理帧数
  double last_ms = 0.0;        // 最近一帧推理耗时（毫秒，不含上传下载）
  double avg_ms = 0.0;         // 滑动平均耗时
  double max_ms = 0.0;         // 峰值耗时（用于判断卡顿风险）
  int64_t tiles_used = 0;      // 累计分块数
  int64_t fallbacks = 0;       // 回退次数（分块失败/精度回落等）
};

// ---------------------------------------------------------------------------
// 引擎接口
// ---------------------------------------------------------------------------
class ISRBackend {
 public:
  virtual ~ISRBackend() = default;

  // 后端标识，仅用于日志与 HUD 显示，不参与任何逻辑分支。
  virtual const char* Name() const = 0;

  // 初始化。可能较慢（TensorRT 首次构建 engine 可达分钟级），
  // 因此实时路径必须在"开始播放前"完成，不能在首帧时同步调用。
  virtual Status Init(const SRBackendConfig& config) = 0;

  // 释放资源。允许重复调用。
  virtual void Shutdown() = 0;

  // 预热：跑几帧空数据，把 GPU 的时钟/缓存/pipeline 拉到稳态。
  // 不做预热的后果是前 10—20 帧明显偏慢，表现为"刚打开视频时卡一下"。
  virtual Status Warmup() = 0;

  // 核心：处理一帧。
  // 参数说明：
  //   in  —— 输入帧（必须已去隔行、已转目标色彩空间）
  //   out —— 输出帧描述。in 与 out 可以是同一对象（原地替换纹理句柄）
  // 返回值：Ok 成功；DeviceLost 需要上层重建整个渲染设备；
  //         OutOfMemory 建议上层降低分块尺寸后重试一次。
  virtual Status Process(const FrameDesc& in, FrameDesc* out) = 0;

  virtual SRStats Stats() const = 0;

  // GPU 设备是否已丢失（驱动重置 / TDR / 拔掉外置显卡）。
  // 实时线程每帧检查一次，丢失后应暂停播放并提示用户，而不是继续输出黑帧。
  virtual bool IsDeviceLost() const = 0;
};

// 工厂：按名称创建后端。
// 参数说明：name = "dummy" | "aji" | "ort"；未编译进的后端会返回 nullptr。
// 说明：用工厂而非直接 new，是为了让业务层只依赖接口与字符串配置。
ISRBackend* CreateBackend(const std::string& name);

// 列出当前构建可用（已编译进）的后端名称，用于启动日志与设置页。
std::vector<std::string> AvailableBackends();

} // namespace lumen
