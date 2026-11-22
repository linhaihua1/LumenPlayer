// ============================================================================
//  LumenPlayer - 后端工厂
//  所属层次：引擎层
//  职责    ：把"后端名字符串"翻译成具体实现，业务层只认接口不认实现。
//
//  当前可用后端：
//    dummy —— CPU 双三次，参考实现 / 画质基线 / 单元测试
//    ort   —— ONNX Runtime，动态加载 onnxruntime.dll，GPU（DML/CUDA）或 CPU
//
//  尚未接入的后端与接入条件（按 ADR-001 的 B 层计划）：
//    aji  —— 需要 libaji 的 aji.dll + 头文件，并确认其许可（当前该仓库无 LICENSE
//            文件，法律上属"保留所有权利"，商用前必须取得作者书面授权）。
//            接入点在 Process()：把 YUV 平面按 libaji 的 ABI 打包成指针数组，
//            调用一次推理，再把输出指针填回 FrameDesc。策略层无需改动。
//            它比 ort 快的地方在于「帧直接留在显存里」，不需要 RGB 往返。
// ============================================================================
#include "engine/sr_backend.h"

#include <algorithm>
#include <cctype>

#include "engine/dummy_backend.h"
#include "engine/onnx_backend.h"

namespace lumen {

ISRBackend* CreateBackend(const std::string& name) {
  // 名称比较不区分大小写，避免配置文件里写成 "Dummy" 就创建失败
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (lower == "dummy" || lower == "bicubic" || lower.empty()) {
    return new DummyBackend();
  }

  // ort：ONNX Runtime。注意它是"运行时可缺失"的后端——编译期不需要 SDK，
  // 所以这里总是创建成功，真正的失败信息在 Init() 里以中文给出。
  if (lower == "ort" || lower == "onnx" || lower == "onnxruntime") {
    return new OnnxBackend();
  }

  // 未编译进或未实现的后端一律返回 nullptr，由调用方决定降级策略。
  // 刻意不偷偷回落到 dummy：静默降级会让用户以为自己在用 AI 超分，
  // 实际看到的是双三次，这种"看起来正常"的错误最难被发现。
  return nullptr;
}

std::vector<std::string> AvailableBackends() { return {"dummy", "ort"}; }

} // namespace lumen
