// ============================================================================
//  LumenPlayer - ONNX Runtime 超分后端实现
//  ---------------------------------------------------------------------------
//  所属层次：引擎层
//
//  本文件是整个项目里唯一允许出现 "onnxruntime" 字样的地方（连同它的头文件）。
//  业务层只认 ISRBackend，换后端不需要改策略层与 UI —— 这是 ADR-001 的验收标准。
//
//  数据流：
//      YUV420P（CPU，已去隔行、已转 BT.709）
//        → RGB float（NCHW，0—255）
//        → [分块] 推理
//        → RGB float
//        → YUV420P（CPU）
//
//  为什么在 CPU 内存里转而不是 GPU 纹理直通：
//    离线导出路径本来就要把帧写进 ffmpeg 管道，本来就是 CPU 帧；
//    实时播放路径走 libmpv 的 GPU 纹理，由 AjiBackend 那条线负责。
//    两条路径共用一个 ISRBackend 接口，但内部实现不同，这是刻意的。
// ============================================================================
#include "engine/onnx_backend.h"

#include "core/frame_buffer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// 只用到声明，不产生任何链接依赖（详见头文件里的说明）。
// ORTCHAR_T 在 Windows 上是 wchar_t，路径必须转成宽字符，否则中文路径会失败。
//
// 兼容性补丁：ORT 头文件把 ORT_API_CALL 硬定义成 MSVC 的 `_stdcall` 关键字，
// MinGW 没有这个关键字（只有 __stdcall），直接包含会报一堆语法错误。
// x64 上只有一种调用约定，所以给它一个空定义与 __stdcall 完全等价。
// （32 位 Windows 不在支持范围内：ORT 早已停止发布 32 位包。）
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64)) && !defined(_stdcall)
#define _stdcall
#define LUMEN_TEMP_STDCALL 1
#endif
#include "onnxruntime_c_api.h"
#ifdef LUMEN_TEMP_STDCALL
#undef _stdcall
#undef LUMEN_TEMP_STDCALL
#endif

namespace lumen {
namespace {

// 分块下限：比这更小的块，模型感受野吃不下，块边界会明显发虚。
constexpr int kMinTile = 64;
// 块间重叠像素。主流 4x 超分模型的有效感受野约 12—20 像素，取 16 可覆盖绝大多数；
// 重叠区做羽化加权，让边界过渡自然，而不是硬切出方格。
constexpr int kTileOverlap = 16;

inline float ClampF(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// BT.709 有限范围（16—235）YUV → RGB。
// 关键点：进入引擎前策略层已把画面转到 709（见 ProcessingChain::convert_matrix），
// 这里若用 601 系数，肤色会偏。两处矩阵必须一致，否则表现为"处理完了颜色就是不对"。
inline void YuvToRgb(float y, float u, float v, float* r, float* g, float* b) {
  const float yy = 1.164383f * (y - 16.0f);
  const float uu = u - 128.0f;
  const float vv = v - 128.0f;
  *r = yy + 1.792741f * vv;
  *g = yy - 0.213249f * uu - 0.532909f * vv;
  *b = yy + 2.112402f * uu;
}

// RGB → BT.709 有限范围 YUV（上式的逆）
inline void RgbToYuv(float r, float g, float b, float* y, float* u, float* v) {
  const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
  *y = 16.0f + luma * (219.0f / 255.0f);
  *u = 128.0f + (-0.1146f * r - 0.3854f * g + 0.5f * b) * (224.0f / 255.0f);
  *v = 128.0f + (0.5f * r - 0.4542f * g - 0.0458f * b) * (224.0f / 255.0f);
}

inline uint8_t ToByte(float v) {
  return static_cast<uint8_t>(ClampF(v, 0.0f, 255.0f) + 0.5f);
}

#ifdef _WIN32
// 当前 exe 所在目录（含尾部分隔符）。用于定位"与程序放在一起的" DLL。
std::string ExeDirectory() {
  wchar_t buf[MAX_PATH] = {0};
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return std::string();
  std::wstring path(buf, n);
  const size_t pos = path.find_last_of(L"\\/");
  if (pos == std::wstring::npos) return std::string();
  path = path.substr(0, pos + 1);
  // 宽字符 → UTF-8
  const int need = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (need <= 0) return std::string();
  std::string out(static_cast<size_t>(need - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, &out[0], need, nullptr, nullptr);
  return out;
}

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                    nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
  return w;
}
#endif

} // namespace

// ===========================================================================
//  Impl：把所有 ONNX Runtime 细节关在这里，头文件保持干净
// ===========================================================================
struct OnnxBackend::Impl {
  // --- 动态库与 API 表 ---
  HMODULE dll = nullptr;
  // 预先按绝对路径加载的 DirectML.dll（避免被 system32 里的旧版抢占）
  HMODULE directml_dll = nullptr;
  const OrtApiBase* api_base = nullptr;
  const OrtApi* api = nullptr;
  std::string runtime_version;

  // --- ORT 对象 ---
  OrtEnv* env = nullptr;
  OrtSessionOptions* session_opts = nullptr;
  OrtSession* session = nullptr;
  OrtMemoryInfo* mem_info = nullptr;
  OrtAllocator* allocator = nullptr;

  std::string input_name;
  std::string output_name;

  // --- 模型属性（初始化时从会话里查出来的真实值，优先于配置）---
  int channels = 3;
  int scale = 2;
  int fixed_in_w = 0; // 模型若声明了固定输入尺寸，这里非 0
  int fixed_in_h = 0;

  std::string provider_requested = "auto";
  std::string provider_active = "CPU";
  std::string err; // 最后一次失败原因（中文，直接给用户看）

  // --- 复用缓冲 ---
  // 逐帧 new/delete 在导出路径上会造成明显抖动（每帧几十 MB），必须复用。
  std::vector<float> tile_in;  // C × T × T
  std::vector<float> tile_out; // C × (T·s) × (T·s)
  std::vector<float> acc;      // 整帧输出累加（浮点，避免分块各自量化后再平均）
  std::vector<float> weight;   // 整帧权重（羽化用；整帧无分块时为空）
  FrameBuffer out_frame;

  int tile_size = 0; // 0 = 整帧
  int last_w = 0;
  int last_h = 0;

  SRStats stats;
  double sum_ms = 0.0;
  int64_t count_ms = 0;
  bool device_lost = false;

  // -------------------------------------------------------------------------
  // 错误处理：ORT 的所有调用都返回 OrtStatus*，非空即失败
  // -------------------------------------------------------------------------
  bool Check(OrtStatus* st, const char* what) {
    if (st == nullptr) return true;
    const char* msg = api ? api->GetErrorMessage(st) : nullptr;
    err = std::string(what) + " 失败：" + (msg ? msg : "未知错误");
    if (api) api->ReleaseStatus(st);
    return false;
  }

  // 忽略返回值，但必须释放状态对象。
  // ORT 的每次调用都会 new 一个 OrtStatus，不释放就是内存泄漏——
  // 单帧看不出，导出两小时长片能累积到 GB 级。
  void Ignore(OrtStatus* st) {
    if (st != nullptr && api != nullptr) api->ReleaseStatus(st);
  }

  // -------------------------------------------------------------------------
  // 加载运行时
  // -------------------------------------------------------------------------
  bool LoadRuntime() {
    // ---------------------------------------------------------------------
    // 这里是踩过坑的地方，写法刻意保守：
    //   本机 C:/windows/system32 就躺着一个 onnxruntime.dll（Windows ML 带来的），
    //   但它不导出 OrtGetApiBase。若直接 LoadLibrary("onnxruntime.dll")，
    //   系统目录会先被命中，用户看到的是"我明明装了运行时，却报找不到接口"。
    // 因此：显式列出候选路径，逐个加载并校验导出符号，全部失败才报错。
    // ---------------------------------------------------------------------
    struct Candidate {
      std::wstring path;
      std::string shown; // 用于错误信息（UTF-8，便于用户对照）
    };
    std::vector<Candidate> candidates;

    if (!runtime_dir_hint.empty()) {
      candidates.push_back({Utf8ToWide(runtime_dir_hint + "\\onnxruntime.dll"),
                            runtime_dir_hint + "\\onnxruntime.dll"});
    }
#ifdef _WIN32
    const std::string exe_dir = ExeDirectory();
    if (!exe_dir.empty()) {
      candidates.push_back({Utf8ToWide(exe_dir + "onnxruntime.dll"),
                            exe_dir + "onnxruntime.dll"});
      candidates.push_back({Utf8ToWide(exe_dir + "runtime\\onnxruntime.dll"),
                            exe_dir + "runtime\\onnxruntime.dll"});
    }
    // 最后才交给系统搜索：优先级最低，因为系统目录里可能有"仿冒者"
    candidates.push_back({L"onnxruntime.dll", "onnxruntime.dll（系统搜索路径）"});
#endif

    std::string tried;
    for (const auto& c : candidates) {
      if (!tried.empty()) tried += "；";
      tried += c.shown;
#ifdef _WIN32
      HMODULE h = LoadLibraryW(c.path.c_str());
      if (h == nullptr) continue;
      auto fn = reinterpret_cast<void*>(GetProcAddress(h, "OrtGetApiBase"));
      if (fn == nullptr) {
        // 加载成功但不是我们要的运行时（常见于系统目录里的同名 DLL）
        FreeLibrary(h);
        tried += "（已加载，但未导出 OrtGetApiBase）";
        continue;
      }
      dll = h;
      loaded_path = c.shown;
#else
      (void)c;
#endif
      break;
    }

    if (dll == nullptr) {
      err =
          "找不到可用的 onnxruntime.dll。已尝试：" + tried +
          "。请把官方 onnxruntime.dll（以及 DML 后端需要的 DirectML.dll）放到 "
          "exe 同目录或模型同目录，并在配置里用 engine.runtime_dir 指定该目录。"
          "下载：https://github.com/microsoft/onnxruntime/releases";
      return false;
    }

#ifdef _WIN32
    // -----------------------------------------------------------------------
    // 抢先按绝对路径加载 DirectML.dll。
    // 原因：ORT 的 DML EP 内部是 LoadLibrary("DirectML.dll")，走的是「进程默认
    // 搜索顺序」——exe 目录 → system32 → PATH。系统目录里那份是 Windows 自带的
    // 1.0.x（2019 年），而 ORT 1.20 要求 >= 1.15，于是创建 EP 失败，
    // 且返回的错误消息是空字符串——这是"报错了但什么也没说"的典型现场。
    // 先按绝对路径加载一次后，按名加载会直接复用已加载的模块，问题消失。
    // -----------------------------------------------------------------------
    auto preload_directml = [&](const std::wstring& dir) {
      if (dir.empty()) return;
      std::wstring p = dir;
      if (p.back() != L'\\' && p.back() != L'/') p.push_back(L'\\');
      p += L"DirectML.dll";
      if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return;
      HMODULE h = LoadLibraryW(p.c_str());
      if (h != nullptr) {
        directml_dll = h;
      }
    };

    // ★ 第一优先：onnxruntime.dll 自己所在的目录。
    //   这是唯一"必然正确"的答案——DirectML 与它是同一个包里的搭档文件。
    //   踩过的坑：早期只按 runtime_dir 与 exe_dir 找，于是 onnxruntime.dll 放在
    //   exe\\runtime\\ 时（正是发布包的标准布局）DirectML 从未被预加载，
    //   ORT 内部按名加载会命中 System32 里的 1.0.x 旧版，创建 DML EP 失败，
    //   而 ORT 返回的错误消息是空字符串——现场只能看到"回退到 CPU"，
    //   完全查不出原因。教训：依赖文件的目录，以"主模块实际位置"为准，
    //   不要自己再猜一遍层级。
    {
      wchar_t mod[MAX_PATH * 4] = {0};
      const DWORD n = GetModuleFileNameW(dll, mod, MAX_PATH * 4);
      if (n > 0 && n < MAX_PATH * 4) {
        std::wstring full(mod, n);
        const size_t slash = full.find_last_of(L"\\/");
        if (slash != std::wstring::npos && slash > 0) {
          preload_directml(full.substr(0, slash));
        }
      }
    }
    // 回退：配置显式指定的目录 → exe 同目录 → exe\\runtime
    // （只有前面拿不到模块路径时才轮到这里，正常情况下不会走到）
    preload_directml(Utf8ToWide(runtime_dir_hint));
    if (directml_dll == nullptr && !exe_dir.empty()) {
      preload_directml(Utf8ToWide(exe_dir));
      preload_directml(Utf8ToWide(exe_dir + "runtime"));
    }
#endif

    using GetApiBaseFn = const OrtApiBase* (*)(void);
    auto fn = reinterpret_cast<GetApiBaseFn>(
        reinterpret_cast<void*>(GetProcAddress(dll, "OrtGetApiBase")));
    api_base = fn();
    if (api_base == nullptr) {
      err = "OrtGetApiBase 返回空，DLL 可能已损坏：" + loaded_path;
      return false;
    }

    const char* ver = api_base->GetVersionString();
    runtime_version = ver ? ver : "unknown";

    // 版本协商：头文件是 1.20（API 20）。用户可能装的是更老的运行时，
    // 此时 GetApi(20) 返回空，必须往下退。
    // OrtApi 是「只追加」的结构，所以我们用新头文件里的前缀成员访问老版本是安全的——
    // 这也是官方保证的 ABI 兼容策略。
    for (uint32_t v = ORT_API_VERSION; v >= 12; --v) {
      api = api_base->GetApi(v);
      if (api != nullptr) break;
    }
    if (api == nullptr) {
      err = "onnxruntime.dll 版本过旧（" + loaded_path + " 报告版本 " +
            runtime_version + "，要求 >= 1.12）。请升级运行时。";
      return false;
    }
    return true;
  }

  std::string runtime_dir_hint;
  std::string loaded_path; // 实际加载的 DLL 路径（诊断用）

  // -------------------------------------------------------------------------
  // 选择执行提供程序
  // -------------------------------------------------------------------------
  // provider 名称在不同版本里写法不一致（"DmlExecutionProvider" / "DML" / "Dml"），
  // 逐个尝试是最省事也最稳的做法：失败的 append 不会破坏 session_options，
  // 只会返回一个错误状态，我们释放掉继续下一个即可。
  bool AppendProvider(const std::string& preference) {
    if (preference == "cpu" || preference.empty()) {
      provider_active = "CPU";
      return true;
    }

    std::vector<std::string> names;
    if (preference == "dml") {
      names = {"DmlExecutionProvider", "DML", "Dml"};
    } else if (preference == "cuda") {
      names = {"CUDAExecutionProvider", "CUDA"};
    } else { // auto：先 GPU 后 CPU。先 DML 后 CUDA 是因为 Windows 上 DML 的
             // 覆盖率高（A 卡、核显都能跑），而 CUDA 只对 N 卡有效。
      names = {"DmlExecutionProvider", "DML", "Dml",
               "CUDAExecutionProvider", "CUDA"};
    }

    // 逐个尝试时把每个失败原因留下来。
    // 以前这里直接 ReleaseStatus 丢掉 message，结果用户只看到"已回退到 CPU"，
    // 而真正的原因（DirectML.dll 版本过低 / 该 build 不含 DML / 显卡不支持）
    // 全部丢失——这正是"报错没用"的典型来源。
    std::string attempts;
    for (const auto& n : names) {
      OrtStatus* st = api->SessionOptionsAppendExecutionProvider(
          session_opts, n.c_str(), nullptr, nullptr, 0);
      if (st == nullptr) {
        provider_active = (n.find("Dml") != std::string::npos ||
                           n.find("DML") != std::string::npos)
                              ? "DML"
                              : "CUDA";
        return true;
      }
      const char* msg = api ? api->GetErrorMessage(st) : nullptr;
      if (!attempts.empty()) attempts += " | ";
      attempts += n + ": " + (msg != nullptr ? msg : "未知错误");
      if (api) api->ReleaseStatus(st);
    }

    // -----------------------------------------------------------------------
    // 通用字符串接口的覆盖范围是有限的。
    // 实测 ORT 1.20 的 SessionOptionsAppendExecutionProvider 只认
    // OPENVINO / SNPE / XNNPACK / QNN / WEBNN / AZURE；DML 与 CUDA 走的是
    // 「专用导出符号」：OrtSessionOptionsAppendExecutionProvider_DML(options, device_id)。
    // 这也是为什么会出现"GetAvailableProviders 报告有 DML，但 append 说
    // Unknown provider name"这种自相矛盾的现象——两处用的是不同的注册表。
    // 解决办法：直接 GetProcAddress 取这个符号来调用（不需要头文件，也不需要导入库）。
    // -----------------------------------------------------------------------
    using EpFn = OrtStatus* (*)(OrtSessionOptions*, int);
    auto try_dedicated = [&](const char* symbol, const char* label) -> bool {
      if (dll == nullptr) return false;
      auto fn = reinterpret_cast<EpFn>(
          reinterpret_cast<void*>(GetProcAddress(dll, symbol)));
      if (fn == nullptr) return false;
      OrtStatus* st = fn(session_opts, 0); // device_id = 0：多卡场景后续再加配置项
      if (st == nullptr) {
        provider_active = label;
        return true;
      }
      const char* msg = api ? api->GetErrorMessage(st) : nullptr;
      if (!attempts.empty()) attempts += " | ";
      // 空消息几乎总是"DirectML.dll 版本过低/被旧版抢先加载"，直接给出可执行的建议，
      // 比让用户对着一个冒号发呆有用得多。
      attempts += std::string(symbol) + ": " +
                  (msg != nullptr && *msg != '\0'
                       ? msg
                       : "（运行时未给出说明；常见原因是 DirectML.dll 版本过低）");
      if (api) api->ReleaseStatus(st);
      return false;
    };

    if (preference == "dml" || preference == "auto") {
      if (try_dedicated("OrtSessionOptionsAppendExecutionProvider_DML", "DML")) return true;
    }
    if (preference == "cuda" || preference == "auto") {
      if (try_dedicated("OrtSessionOptionsAppendExecutionProvider_CUDA", "CUDA")) return true;
    }

    // 全部失败不是致命错误：CPU 一定可用，只是慢。
    // 但必须记录，否则用户会以为自己在用 GPU（"怎么这么卡"的典型来源）。
    provider_active = "CPU";
    err = "未能在运行时中启用 GPU 执行提供程序（请求=" + preference +
          "），已回退到 CPU。若使用 DML 后端，请确认 DirectML.dll 与 onnxruntime.dll 同目录。";
    if (!attempts.empty()) err += " 底层原因：" + attempts;

    // 把"这个 DLL 到底支持哪些 provider"一并打出来。
    // 没有这一行时，排查只能靠猜：DLL 版本不对 / 加载到别的同名 DLL /
    // 该 build 未编译 DML —— 三者的现象都是"Unknown provider name"。
    char** providers = nullptr;
    int count = 0;
    if (api->GetAvailableProviders(&providers, &count) == nullptr && providers != nullptr) {
      std::string list;
      for (int i = 0; i < count; ++i) {
        if (providers[i] == nullptr) continue;
        if (!list.empty()) list += ", ";
        list += providers[i];
      }
      // ReleaseAvailableProviders 也返回 OrtStatus，返回非空时同样要释放，
      // 否则这条"诊断路径"本身会漏一个状态对象（GCC 的 warn_unused_result 就是在提这个）。
      OrtStatus* rel = api->ReleaseAvailableProviders(providers, count);
      if (rel != nullptr) api->ReleaseStatus(rel);
      if (!list.empty()) err += " 该运行时支持的 provider：" + list;
    }
    return true;
  }

  // -------------------------------------------------------------------------
  // 从会话里读模型输入/输出维度
  // -------------------------------------------------------------------------
  // 目的：模型自己声明的通道数/固定尺寸比配置可靠。配置写错了（比如把 RGB 模型
  // 按单通道跑）会得到"能跑但全是灰的"这种最难查的结果，所以以模型为准。
  void ProbeModelIo() {
    if (session == nullptr) return;

    auto read = [&](bool input, std::vector<int64_t>* dims) {
      OrtTypeInfo* type_info = nullptr;
      OrtStatus* st = input ? api->SessionGetInputTypeInfo(session, 0, &type_info)
                            : api->SessionGetOutputTypeInfo(session, 0, &type_info);
      if (st != nullptr) {
        api->ReleaseStatus(st);
        return;
      }
      const OrtTensorTypeAndShapeInfo* shape = nullptr;
      if (api->CastTypeInfoToTensorInfo(type_info, &shape) == nullptr && shape) {
        size_t nd = 0;
        if (api->GetDimensionsCount(shape, &nd) == nullptr && nd > 0 && nd <= 5) {
          dims->assign(nd, 0);
          // ORT 的这些查询接口带 warn_unused_result，显式忽略即可
          Ignore(api->GetDimensions(shape, dims->data(), nd));
        }
      }
      api->ReleaseTypeInfo(type_info);
    };

    std::vector<int64_t> in_dims, out_dims;
    read(true, &in_dims);
    read(false, &out_dims);

    if (in_dims.size() == 4) {
      if (in_dims[1] == 1 || in_dims[1] == 3) channels = static_cast<int>(in_dims[1]);
      if (in_dims[2] > 0) fixed_in_h = static_cast<int>(in_dims[2]);
      if (in_dims[3] > 0) fixed_in_w = static_cast<int>(in_dims[3]);
    }
    if (in_dims.size() == 4 && out_dims.size() == 4 && in_dims[2] > 0 &&
        out_dims[2] > 0) {
      const int s = static_cast<int>(out_dims[2] / in_dims[2]);
      if (s > 0) scale = s;
    }
  }

  // -------------------------------------------------------------------------
  // 单块推理
  // -------------------------------------------------------------------------
  // 参数说明：in 指向 C×th×tw 的连续浮点数据；结果写进 tile_out。
  bool RunOne(const float* in, int tw, int th, std::string* why) {
    const int64_t shape[4] = {1, channels, th, tw};

    OrtValue* in_val = nullptr;
    // 说明：这里把 const 指针转成 void* 是 ORT C API 的设计（它不会写这块内存）。
    // 用 CreateTensorWithData 而不是 CreateTensor 是为了避免多一次内部拷贝。
    if (!Check(api->CreateTensorWithDataAsOrtValue(
                   mem_info, const_cast<float*>(in),
                   static_cast<size_t>(channels) * tw * th * sizeof(float), shape, 4,
                   ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in_val),
               "创建输入张量")) {
      if (why) *why = err;
      return false;
    }

    const char* in_names[1] = {input_name.c_str()};
    const char* out_names[1] = {output_name.c_str()};
    const OrtValue* inputs[1] = {in_val};
    OrtValue* outputs[1] = {nullptr};

    OrtStatus* st = api->Run(session, nullptr, in_names, inputs, 1, out_names, 1, outputs);
    api->ReleaseValue(in_val);

    if (st != nullptr) {
      const char* msg = api->GetErrorMessage(st);
      // 显存不足与"模型要求固定尺寸"是最常见的两种失败，给出可执行的建议。
      std::string text = msg ? msg : "未知错误";
      if (why) {
        *why = text;
        if (text.find("memory") != std::string::npos ||
            text.find("Memory") != std::string::npos ||
            text.find("alloc") != std::string::npos) {
          *why += "（建议：把 engine.tile_size 调小，例如 256 或 192）";
        } else if (text.find("dimension") != std::string::npos ||
                   text.find("shape") != std::string::npos) {
          *why += "（建议：该模型要求固定输入尺寸 " + std::to_string(fixed_in_w) + "x" +
                  std::to_string(fixed_in_h) +
                  "，请改用动态尺寸模型，或把 tile_size 设成该值）";
        }
      }
      api->ReleaseStatus(st);
      return false;
    }

    // 取出输出形状与数据
    OrtTensorTypeAndShapeInfo* info = nullptr;
    if (!Check(api->GetTensorTypeAndShape(outputs[0], &info), "读取输出形状")) {
      api->ReleaseValue(outputs[0]);
      if (why) *why = err;
      return false;
    }
    size_t nd = 0;
    Ignore(api->GetDimensionsCount(info, &nd));
    int64_t dims[6] = {0};
    Ignore(api->GetDimensions(info, dims, std::min<size_t>(nd, 6)));
    api->ReleaseTensorTypeAndShapeInfo(info);

    int ow = tw * scale;
    int oh = th * scale;
    if (nd == 4) {
      // 模型若声明了通道数，必须与我们的解释一致：不一致会导致下面的拷贝按错误
      // 长度读取，轻则花屏，重则访问越界。
      if (dims[1] > 0 && static_cast<int>(dims[1]) != channels) {
        if (why) {
          *why = "模型输出通道数 " + std::to_string(dims[1]) + " 与配置的 " +
                 std::to_string(channels) + " 不一致，请在配置里改 engine.channels";
        }
        api->ReleaseValue(outputs[0]);
        return false;
      }
      ow = static_cast<int>(dims[3]);
      oh = static_cast<int>(dims[2]);
    }
    const size_t need = static_cast<size_t>(channels) * ow * oh;
    tile_out.resize(need);

    void* data = nullptr;
    if (!Check(api->GetTensorMutableData(outputs[0], &data), "读取输出数据")) {
      api->ReleaseValue(outputs[0]);
      if (why) *why = err;
      return false;
    }
    std::memcpy(tile_out.data(), data, need * sizeof(float));
    api->ReleaseValue(outputs[0]);
    return true;
  }
};

// ===========================================================================
//  OnnxBackend
// ===========================================================================
OnnxBackend::OnnxBackend() : impl_(new Impl()) {}

OnnxBackend::~OnnxBackend() { Shutdown(); }

const char* OnnxBackend::Name() const {
  // 名称里带上真实生效的 provider：HUD 上写 "ort(DML)" 比只写 "ort" 有用得多，
  // 用户一眼能看出自己到底跑在 GPU 还是 CPU 上。
  static thread_local std::string label;
  label = "ort(" + (impl_ ? impl_->provider_active : std::string("CPU")) + ")";
  return label.c_str();
}

std::string OnnxBackend::LastError() const { return impl_ ? impl_->err : std::string(); }

std::string OnnxBackend::ActiveProvider() const {
  return impl_ ? impl_->provider_active : std::string("CPU");
}

Status OnnxBackend::Init(const SRBackendConfig& config) {
  if (!impl_) return Status::Internal;
  Impl& d = *impl_;
  d.err.clear();
  d.device_lost = false;

  if (config.model_path.empty()) {
    d.err = "未指定模型文件。请在配置里设置 engine.model_path（例如 models/realesr-general-x4v3.onnx）。";
    return Status::NotFound;
  }
  {
    FILE* f = nullptr;
#ifdef _WIN32
    const std::wstring wp = Utf8ToWide(config.model_path);
    f = _wfopen(wp.c_str(), L"rb");
#else
    f = std::fopen(config.model_path.c_str(), "rb");
#endif
    if (f == nullptr) {
      d.err = "模型文件不存在或无法读取：" + config.model_path;
      return Status::NotFound;
    }
    std::fclose(f);
  }

  d.runtime_dir_hint = config.runtime_dir;
  d.provider_requested = config.provider.empty() ? "auto" : config.provider;
  d.channels = (config.channels == 1) ? 1 : 3;

  if (!d.LoadRuntime()) return Status::NotFound;

  // --- 环境 ---
  // 日志级别用 Warning：Info 级别在 DML 下会刷屏（每个节点都打一行），
  // 而离线导出是逐帧跑的，日志 I/O 会直接拖慢吞吐。
  if (!d.Check(d.api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "lumen", &d.env), "创建环境")) {
    return Status::Internal;
  }

  if (!d.Check(d.api->CreateSessionOptions(&d.session_opts), "创建会话选项")) {
    return Status::Internal;
  }
  d.Ignore(d.api->SetSessionGraphOptimizationLevel(d.session_opts, ORT_ENABLE_ALL));

  // 线程数：CPU 路径吃满物理核收益明显，但留一半给 ffmpeg 解码/编码更划算。
  // 这里不读 CPU 核数（避免引入平台依赖），固定 4 是经验值；GPU 路径该值无意义。
  d.Ignore(d.api->SetIntraOpNumThreads(d.session_opts, 4));

  d.AppendProvider(d.provider_requested);

  d.Ignore(d.api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &d.mem_info));
  if (d.mem_info == nullptr) {
    d.err = "创建 CPU 内存描述失败";
    return Status::Internal;
  }
  d.Ignore(d.api->GetAllocatorWithDefaultOptions(&d.allocator));

  // --- 建会话（最慢的一步：首次可能几秒到几十秒）---
  std::wstring wpath;
#ifdef _WIN32
  wpath = Utf8ToWide(config.model_path);
#endif
#ifdef _WIN32
  OrtStatus* st = d.api->CreateSession(d.env, wpath.c_str(), d.session_opts, &d.session);
#else
  OrtStatus* st = d.api->CreateSession(d.env, config.model_path.c_str(), d.session_opts,
                                       &d.session);
#endif
  if (!d.Check(st, "加载模型")) {
    // 把底层信息补上：DML 对 opset 有硬要求（<=19），这条提示能省掉几小时的排查。
    d.err += "（若使用 DML 后端，请确认模型 opset <= 19；超出时请用 "
             "python -m onnxruntime.tools.convert_onnx_models_to_ort 或重新导出模型）";
    return Status::InvalidArgument;
  }

  // --- 输入输出名 ---
  char* in_name = nullptr;
  char* out_name = nullptr;
  if (!d.Check(d.api->SessionGetInputName(d.session, 0, d.allocator, &in_name),
               "读取输入名")) {
    return Status::Internal;
  }
  if (!d.Check(d.api->SessionGetOutputName(d.session, 0, d.allocator, &out_name),
               "读取输出名")) {
    return Status::Internal;
  }
  d.input_name = in_name ? in_name : "";
  d.output_name = out_name ? out_name : "";
  if (d.allocator) {
    if (in_name) d.allocator->Free(d.allocator, in_name);
    if (out_name) d.allocator->Free(d.allocator, out_name);
  }
  if (d.input_name.empty() || d.output_name.empty()) {
    d.err = "模型没有可用的输入或输出张量";
    return Status::InvalidArgument;
  }

  d.ProbeModelIo();

  // 配置里的 scale 只在模型未声明时作为声明值使用；
  // 模型声明了就以模型为准（否则输出尺寸算错，画面会错位/裁切）。
  if (config.scale > 0 && d.scale <= 1) d.scale = config.scale;
  if (d.scale != 2 && d.scale != 4 && d.scale != 1 && d.scale != 3) {
    d.err = "模型放大倍数异常：" + std::to_string(d.scale);
    return Status::Unsupported;
  }

  // 分块尺寸：必须是 8 的倍数（方便对齐），且不小于下限。
  if (config.tile_size > 0) {
    int t = config.tile_size;
    t = std::max(t, kMinTile);
    t = (t / 8) * 8;
    // 模型若要求固定尺寸，分块必须正好等于它，否则直接失败。
    if (d.fixed_in_w > 0) t = d.fixed_in_w;
    d.tile_size = t;
  } else if (d.fixed_in_w > 0) {
    d.tile_size = d.fixed_in_w; // 固定尺寸模型只能按它的尺寸切
  }

  d.stats = SRStats();
  d.sum_ms = 0.0;
  d.count_ms = 0;
  return Status::Ok;
}

void OnnxBackend::Shutdown() {
  if (!impl_) return;
  Impl& d = *impl_;
  if (d.api) {
    if (d.session) d.api->ReleaseSession(d.session);
    if (d.session_opts) d.api->ReleaseSessionOptions(d.session_opts);
    if (d.mem_info) d.api->ReleaseMemoryInfo(d.mem_info);
    if (d.env) d.api->ReleaseEnv(d.env);
  }
  d.session = nullptr;
  d.session_opts = nullptr;
  d.mem_info = nullptr;
  d.env = nullptr;
  d.tile_in.clear();
  d.tile_out.clear();
  d.acc.clear();
  d.weight.clear();
  d.out_frame.Reset();
  d.last_w = 0;
  d.last_h = 0;
#ifdef _WIN32
  if (d.dll) {
    FreeLibrary(d.dll);
    d.dll = nullptr;
  }
#endif
  d.api = nullptr;
  d.api_base = nullptr;
}

Status OnnxBackend::Warmup() {
  if (!impl_ || impl_->session == nullptr) return Status::Internal;

  // 用一块纯灰做预热。必须做，否则前几帧会明显偏慢：
  // DML/CUDA 的首次推理包含内核编译与显存池建立，耗时是稳态的 10—40 倍。
  FrameBuffer probe;
  if (probe.Allocate(64, 64, PixelFormat::YUV420P) != Status::Ok) {
    return Status::OutOfMemory;
  }
  for (int p = 0; p < 3; ++p) {
    const PlaneView v = probe.Plane(p);
    if (!v.data) continue;
    std::memset(v.data, p == 0 ? 126 : 128,
                static_cast<size_t>(v.stride) * probe.PlaneHeight(p));
  }
  const FrameDesc in = probe.ToDesc();
  FrameDesc out;
  return Process(in, &out);
}

SRStats OnnxBackend::Stats() const { return impl_ ? impl_->stats : SRStats(); }

bool OnnxBackend::IsDeviceLost() const { return impl_ ? impl_->device_lost : true; }

// ---------------------------------------------------------------------------
// 核心：处理一帧
// ---------------------------------------------------------------------------
Status OnnxBackend::Process(const FrameDesc& in, FrameDesc* out) {
  if (!impl_ || impl_->session == nullptr || out == nullptr) return Status::InvalidArgument;
  Impl& d = *impl_;

  if (!in.IsValid()) return Status::InvalidArgument;
  if (in.location != FrameLocation::Cpu) {
    // 与 DummyBackend 一致：GPU 帧不该进这条路径。静默回读会毁掉实时预算。
    d.err = "ONNX 后端只接受 CPU 帧（实时 GPU 路径请用 libaji 后端）";
    return Status::Unsupported;
  }
  if (in.format != PixelFormat::YUV420P) {
    // 只支持 YUV420P：这是解码管线唯一会送进来的格式。
    // NV12 与 10bit 目前一律拒绝，而不是"尽力而为"——静默转格式会让画质问题
    // 变得难以归因（用户看到偏色，却不知道该怪模型还是怪转换）。
    d.err = "ONNX 后端当前只接受 YUV420P 输入，实际收到：" +
            std::string(ToString(in.format));
    return Status::Unsupported;
  }
  if (in.plane_data[0] == nullptr) return Status::InvalidArgument;

  const auto t0 = std::chrono::steady_clock::now();

  const int w = in.width;
  const int h = in.height;
  const int s = d.scale;
  const int ow = w * s;
  const int oh = h * s;

  if (d.out_frame.Resize(ow, oh, PixelFormat::YUV420P) != Status::Ok) {
    d.err = "输出缓冲分配失败（内存不足）";
    return Status::OutOfMemory;
  }

  // -----------------------------------------------------------------------
  // 1. 分块推理 → 浮点整帧缓冲
  // -----------------------------------------------------------------------
  const bool tiled = (d.tile_size > 0) && (w > d.tile_size || h > d.tile_size);

  d.acc.assign(static_cast<size_t>(ow) * oh * d.channels, 0.0f);
  if (tiled) {
    d.weight.assign(static_cast<size_t>(ow) * oh, 0.0f);
  }

  auto fill_tile = [&](int x0, int y0, int tw, int th) {
    // 从整帧里取一块，越界处用边缘像素钳制（不是补黑边！补黑会让模型在块边界
    // "看到"一条黑线，输出出现暗边，这是分块拼接最常见的画质事故）。
    d.tile_in.resize(static_cast<size_t>(d.channels) * tw * th);

    // FrameDesc 只给裸指针 + stride，这里包一层视图便于按行访问
    const uint8_t* y_data = in.plane_data[0];
    const int y_stride = in.plane_stride[0];
    const uint8_t* u_data = in.plane_data[1];
    const uint8_t* v_data = in.plane_data[2];
    const int c_stride = in.plane_stride[1];
    const int cw = (w + 1) / 2;
    const size_t plane_in = static_cast<size_t>(tw) * th;

    for (int ty = 0; ty < th; ++ty) {
      const int sy = std::min(std::max(y0 + ty, 0), h - 1);
      const uint8_t* yrow = y_data + static_cast<size_t>(sy) * y_stride;
      const uint8_t* urow = (d.channels == 3 && u_data)
                                ? u_data + static_cast<size_t>(sy / 2) * c_stride
                                : nullptr;
      const uint8_t* vrow = (d.channels == 3 && v_data)
                                ? v_data + static_cast<size_t>(sy / 2) * c_stride
                                : nullptr;
      for (int tx = 0; tx < tw; ++tx) {
        const int sx = std::min(std::max(x0 + tx, 0), w - 1);
        const size_t idx = static_cast<size_t>(ty) * tw + tx;
        if (d.channels == 1) {
          d.tile_in[idx] = static_cast<float>(yrow[sx]);
        } else {
          const int cx = std::min(sx / 2, cw - 1);
          const float yy = static_cast<float>(yrow[sx]);
          const float uu = urow ? static_cast<float>(urow[cx]) : 128.0f;
          const float vv = vrow ? static_cast<float>(vrow[cx]) : 128.0f;
          float r = 0, g = 0, b = 0;
          YuvToRgb(yy, uu, vv, &r, &g, &b);
          d.tile_in[idx] = ClampF(r, 0.0f, 255.0f);
          d.tile_in[idx + plane_in] = ClampF(g, 0.0f, 255.0f);
          d.tile_in[idx + plane_in * 2] = ClampF(b, 0.0f, 255.0f);
        }
      }
    }
  };

  if (!tiled) {
    fill_tile(0, 0, w, h);
    std::string why;
    if (!d.RunOne(d.tile_in.data(), w, h, &why)) {
      d.err = why;
      if (why.find("device") != std::string::npos) d.device_lost = true;
      return Status::Internal;
    }
    // 模型可能返回与期望不同的尺寸（固定尺寸模型或非常规倍数），必须先校验，
    // 否则下面的索引计算会越界——这类错误在 Release 下不报错，只表现为花屏。
    if (d.tile_out.size() != static_cast<size_t>(d.channels) * ow * oh) {
      d.err = "模型输出尺寸与期望不符：期望 " + std::to_string(ow) + "x" +
              std::to_string(oh) + "，" + std::to_string(d.channels) +
              " 通道，实际得到 " + std::to_string(d.tile_out.size()) + " 个浮点数";
      return Status::Unsupported;
    }
    d.acc = d.tile_out;
  } else {
    const int T = d.tile_size;
    const int overlap = std::min(kTileOverlap, T / 4);
    const int stride = T - overlap;
    if (stride <= 0) {
      d.err = "分块尺寸过小，无法计算重叠步长";
      return Status::InvalidArgument;
    }

    for (int ty = 0; ty < h; ty += stride) {
      for (int tx = 0; tx < w; tx += stride) {
        const int tw = std::min(T, w - tx);
        const int th = std::min(T, h - ty);
        fill_tile(tx, ty, T, T); // 始终按 T×T 喂：固定尺寸模型才不会报错

        std::string why;
        if (!d.RunOne(d.tile_in.data(), T, T, &why)) {
          d.err = why;
          if (why.find("device") != std::string::npos) d.device_lost = true;
          return Status::Internal;
        }
        const size_t expect = static_cast<size_t>(d.channels) * (T * s) * (T * s);
        if (d.tile_out.size() != expect) {
          d.err = "分块输出尺寸异常：期望 " + std::to_string(expect) + " 个浮点数，实际 " +
                  std::to_string(d.tile_out.size()) + "（请把 tile_size 设为模型要求的固定尺寸）";
          return Status::Unsupported;
        }

        // 累加 + 羽化权重
        const int tox = tx * s;
        const int toy = ty * s;
        const int tws = tw * s; // 实际有效宽度（不含右侧越界部分）
        const int ths = th * s;
        const int ramp = overlap * s;

        for (int y = 0; y < ths; ++y) {
          const int gy = toy + y;
          if (gy >= oh) break;
          // 竖直方向权重：只在"不是画面边界"的块内边缘做斜坡
          float wy = 1.0f;
          if (ty > 0 && y < ramp) {
            wy = static_cast<float>(y + 1) / static_cast<float>(ramp);
          } else if (ty + th < h && y >= ths - ramp) {
            wy = static_cast<float>(ths - y) / static_cast<float>(ramp);
          }
          for (int x = 0; x < tws; ++x) {
            const int gx = tox + x;
            if (gx >= ow) break;
            float wx = 1.0f;
            if (tx > 0 && x < ramp) {
              wx = static_cast<float>(x + 1) / static_cast<float>(ramp);
            } else if (tx + tw < w && x >= tws - ramp) {
              wx = static_cast<float>(tws - x) / static_cast<float>(ramp);
            }
            const float wt = wx * wy;
            const size_t gi = static_cast<size_t>(gy) * ow + gx;
            const size_t si = static_cast<size_t>(y) * (T * s) + x;
            const size_t plane = static_cast<size_t>(T * s) * (T * s);
            for (int c = 0; c < d.channels; ++c) {
              d.acc[gi + static_cast<size_t>(c) * ow * oh] +=
                  d.tile_out[si + static_cast<size_t>(c) * plane] * wt;
            }
            d.weight[gi] += wt;
          }
        }
        ++d.stats.tiles_used;
      }
    }

    // 归一化：除以权重和，消除羽化带来的亮度衰减
    const size_t plane_out = static_cast<size_t>(ow) * oh;
    for (size_t i = 0; i < plane_out; ++i) {
      const float wsum = d.weight[i];
      if (wsum <= 0.0f) continue; // 理论上不会发生；真发生了就留黑，便于发现 bug
      const float inv = 1.0f / wsum;
      for (int c = 0; c < d.channels; ++c) {
        d.acc[i + static_cast<size_t>(c) * plane_out] *= inv;
      }
    }
  }

  // -----------------------------------------------------------------------
  // 2. 浮点结果 → YUV420P
  // -----------------------------------------------------------------------
  const PlaneView dy = d.out_frame.Plane(0);
  const PlaneView du = d.out_frame.Plane(1);
  const PlaneView dv = d.out_frame.Plane(2);
  const size_t plane_out = static_cast<size_t>(ow) * oh;

  if (d.channels == 1) {
    for (int y = 0; y < oh; ++y) {
      const float* row = d.acc.data() + static_cast<size_t>(y) * ow;
      uint8_t* dst = dy.data + static_cast<size_t>(y) * dy.stride;
      for (int x = 0; x < ow; ++x) dst[x] = ToByte(row[x]);
    }
    // 单通道模型只放大亮度，色度用双线性放大（色度带宽需求低，不值得过模型）
    auto up_chroma = [&](const PlaneView& src, const PlaneView& dst) {
      if (!src.data || !dst.data) return;
      const int sw = (w + 1) / 2;
      const int sh = (h + 1) / 2;
      const int dw = (ow + 1) / 2;
      const int dh = (oh + 1) / 2;
      for (int y = 0; y < dh; ++y) {
        const float sy = (y + 0.5f) * sh / dh - 0.5f;
        const int y0 = std::min(std::max(static_cast<int>(std::floor(sy)), 0), sh - 1);
        const int y1 = std::min(y0 + 1, sh - 1);
        const float fy = ClampF(sy - y0, 0.0f, 1.0f);
        const uint8_t* r0 = src.data + static_cast<size_t>(y0) * src.stride;
        const uint8_t* r1 = src.data + static_cast<size_t>(y1) * src.stride;
        uint8_t* out_row = dst.data + static_cast<size_t>(y) * dst.stride;
        for (int x = 0; x < dw; ++x) {
          const float sx = (x + 0.5f) * sw / dw - 0.5f;
          const int x0 = std::min(std::max(static_cast<int>(std::floor(sx)), 0), sw - 1);
          const int x1 = std::min(x0 + 1, sw - 1);
          const float fx = ClampF(sx - x0, 0.0f, 1.0f);
          const float top = r0[x0] * (1 - fx) + r0[x1] * fx;
          const float bot = r1[x0] * (1 - fx) + r1[x1] * fx;
          out_row[x] = static_cast<uint8_t>(top * (1 - fy) + bot * fy + 0.5f);
        }
      }
    };
    // 注意：in.plane_data 是只读的，这里临时构造视图
    PlaneView su{const_cast<uint8_t*>(in.plane_data[1]), in.plane_stride[1]};
    PlaneView sv{const_cast<uint8_t*>(in.plane_data[2]), in.plane_stride[2]};
    up_chroma(su, du);
    up_chroma(sv, dv);
  } else {
    const int cw = (ow + 1) / 2;
    for (int y = 0; y < oh; ++y) {
      uint8_t* yrow = dy.data + static_cast<size_t>(y) * dy.stride;
      const float* r0 = d.acc.data() + static_cast<size_t>(y) * ow;
      const float* g0 = r0 + plane_out;
      const float* b0 = r0 + plane_out * 2;
      for (int x = 0; x < ow; ++x) {
        float yy, uu, vv;
        RgbToYuv(r0[x], g0[x], b0[x], &yy, &uu, &vv);
        yrow[x] = ToByte(yy);
      }
      // 色度：2x2 块取平均后转一次，比逐像素转再抽样更稳（等效于盒式滤波）
      if ((y & 1) == 0 && (y / 2) < ((oh + 1) / 2)) {
        uint8_t* urow = du.data + static_cast<size_t>(y / 2) * du.stride;
        uint8_t* vrow = dv.data + static_cast<size_t>(y / 2) * dv.stride;
        const int y2 = std::min(y + 1, oh - 1);
        const float* r1 = d.acc.data() + static_cast<size_t>(y2) * ow;
        const float* g1 = r1 + plane_out;
        const float* b1 = r1 + plane_out * 2;
        for (int x = 0; x < cw; ++x) {
          const int x0 = x * 2;
          const int x1 = std::min(x0 + 1, ow - 1);
          float yy, uu, vv;
          const float rr = (r0[x0] + r0[x1] + r1[x0] + r1[x1]) * 0.25f;
          const float gg = (g0[x0] + g0[x1] + g1[x0] + g1[x1]) * 0.25f;
          const float bb = (b0[x0] + b0[x1] + b1[x0] + b1[x1]) * 0.25f;
          RgbToYuv(rr, gg, bb, &yy, &uu, &vv);
          urow[x] = ToByte(uu);
          vrow[x] = ToByte(vv);
        }
      }
    }
  }

  // 元数据透传（契约要求：pts / 色彩标记 / SAR 一律原样带过去）
  d.out_frame.set_pts(in.pts);
  d.out_frame.set_index(in.frame_index);
  d.out_frame.set_field_order(FieldOrder::Progressive);
  d.out_frame.set_colorspace(in.colorspace);
  d.out_frame.set_range(in.range);
  d.out_frame.set_sar(in.sar);

  *out = d.out_frame.ToDesc();

  const auto t1 = std::chrono::steady_clock::now();
  const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  ++d.stats.frames_done;
  d.stats.last_ms = ms;
  d.stats.max_ms = std::max(d.stats.max_ms, ms);
  d.sum_ms += ms;
  ++d.count_ms;
  d.stats.avg_ms = d.sum_ms / static_cast<double>(d.count_ms);

  d.last_w = w;
  d.last_h = h;
  return Status::Ok;
}

} // namespace lumen
