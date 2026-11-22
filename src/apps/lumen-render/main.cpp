// ============================================================================
//  LumenPlayer - 离线导出管线（lumen-render）
//  ---------------------------------------------------------------------------
//  所属层次：应用层
//  职责    ：把"自检结论"真正跑成一段视频：
//            解码 → 前处理（去块/降噪/场处理/色彩）→ 超分 → 编码。
//
//  设计说明（重要）：
//    1. 前处理与色彩转换暂时交给 ffmpeg 的成熟滤镜执行，
//       我们的代码只负责"决策用哪个滤镜、参数是多少"（策略层）。
//       这样一期就能产出正确结果，且不会因为自己写 bwdif 而出画面事故。
//       等策略层稳定后，再把关键滤镜换成自有实现（性能与可控性）。
//    2. 超分环节走 ISRBackend 接口，当前注册的实现是 CPU 双三次。
//       接上 libaji / ONNX Runtime 后，本文件一行都不用改——
//       这就是 ADR-001 接缝契约的价值所在。
//    3. 音轨不进入本管线（视频先出，最后再 mux），
//       避免音视频两条流在管道里互相阻塞。
//
//  用法：
//    lumen-render in.mkv out.mp4
//    lumen-render in.mkv out.mp4 --limit 300 --crf 20
//    lumen-render in.mkv out.mp4 --backend ort --model models/x4.onnx --provider dml
//    lumen-render in.mkv out.mp4 --dry-run     (只打印命令，不执行)
//
//  配置优先级：命令行 > 配置文件 > 内置默认值。
//  这样"平时用配置、临时改一项用参数"两种习惯都能满足。
// ============================================================================
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "core/config.h"
#include "engine/dummy_backend.h"
#include "engine/onnx_backend.h"
#include "engine/sr_backend.h"
#include "platform/process.h"
#include "platform/stream_pipe.h"
#include "service/inspector.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

void PrintUsage() {
  std::cout <<
      "LumenPlayer 离线导出 v0.1\n"
      "\n"
      "用法: lumen-render <输入> <输出.mp4> [选项]\n"
      "\n"
      "选项:\n"
      "  --preset <档位>   performance | balanced | quality | archive\n"
      "  --anime           按动画素材处理\n"
      "  --backend <名称>  超分后端（dummy = CPU 双三次基线，ort = ONNX Runtime）\n"
      "  --model <路径>    ONNX 模型文件（ort 后端必填）\n"
      "  --provider <名称> auto | cpu | dml | cuda（默认取配置）\n"
      "  --runtime-dir <目录> onnxruntime.dll 所在目录（换后端版本时临时指定）\n"
      "  --field-order <序>  auto | tff | bff | progressive（覆盖自动场序判定）\n"
      "  --tile <N>        分块边长，0 = 整帧；显存不足时试 256\n"
      "  --config <路径>   配置文件（默认找当前目录的 lumen.json）\n"
      "  --crf <N>         x264 质量参数，越小越清晰（默认 18）\n"
      "  --limit <N>       只处理前 N 帧（快速验证用）\n"
      "  --dry-run         只打印将要执行的 ffmpeg 命令\n"
      "  -h, --help        显示帮助\n"
      "\n"
      "输出说明: 视频由本程序处理；音轨若存在会从源文件直接复制过来。\n";
}

// 数字转字符串（固定小数位），避免各处重复写 snprintf
std::string Num(double v, int decimals = 3) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  return buf;
}

// ---------------------------------------------------------------------------
// 根据处理链生成 ffmpeg 解码端滤镜链
// ---------------------------------------------------------------------------
// 顺序很重要，且不能随意调换：
//   去块 → 降噪 → 场处理 → 色彩转换
// 理由：去块必须在场处理前（块效应会干扰场匹配）；
//       色彩转换必须最后（场处理在原始色彩空间上做才准确）。
std::string BuildDecodeFilter(const lumen::ProcessingChain& c) {
  std::vector<std::string> f;

  if (c.deblock) {
    f.push_back("deblock=filter=strong:block=8");
  }

  switch (c.denoise) {
    case lumen::DenoiseMode::Light:
      f.push_back("hqdn3d=1.5:1.5:6:6");
      break;
    case lumen::DenoiseMode::Medium:
      f.push_back("hqdn3d=3:3:9:9");
      break;
    case lumen::DenoiseMode::Off:
    default:
      break;
  }

  const bool tff = (c.assumed_field_order != lumen::FieldOrder::BottomFieldFirst);
  const char* parity = tff ? "tff" : "bff";

  if (c.field_mode == lumen::FieldMode::Deinterlace) {
    // send_frame：每帧输入出一帧（保持帧率），不插帧。
    // 不用 send_field（bob），因为那会翻倍帧率并让播放器时间轴变复杂。
    f.push_back(std::string("bwdif=mode=send_frame:parity=") + parity + ":deint=all");
  } else if (c.field_mode == lumen::FieldMode::IVTC) {
    // fieldmatch 找出重复场，decimate 每 5 帧丢 1 帧 → 还原 24p。
    // 这一步是"凭空多出 20% 画质预算"的关键：丢掉的是重复帧，不是信息。
    f.push_back(std::string("fieldmatch=order=") + parity +
                ":combmatch=full:combdbg=none,decimate=cycle=5");
  }

  if (c.convert_matrix || c.convert_range) {
    std::string s = "scale=";
    s += "in_color_matrix=";
    s += (c.source_space == lumen::ColorSpace::BT601) ? "bt601" : "bt709";
    s += ":out_color_matrix=bt709";
    s += ":in_range=";
    s += (c.source_range == lumen::ColorRange::Full) ? "pc" : "tv";
    s += ":out_range=tv";
    f.push_back(s);
  }

  // 统一像素格式，保证下游解析简单
  f.push_back("format=yuv420p");

  std::string out;
  for (size_t i = 0; i < f.size(); ++i) {
    if (i) out += ",";
    out += f[i];
  }
  return out;
}

// 把裸 YUV420P 字节流送进 FrameBuffer
void LoadRawFrame(const std::vector<uint8_t>& raw, lumen::FrameBuffer* fb) {
  const int w = fb->width();
  const int h = fb->height();
  const lumen::PlaneView y = fb->Plane(0);
  const lumen::PlaneView u = fb->Plane(1);
  const lumen::PlaneView v = fb->Plane(2);

  const uint8_t* src = raw.data();
  for (int r = 0; r < h; ++r) {
    std::memcpy(y.data + static_cast<size_t>(r) * y.stride,
                src + static_cast<size_t>(r) * w, static_cast<size_t>(w));
  }
  src += static_cast<size_t>(w) * h;

  const int cw = (w + 1) / 2;
  const int ch = (h + 1) / 2;
  for (int r = 0; r < ch; ++r) {
    std::memcpy(u.data + static_cast<size_t>(r) * u.stride,
                src + static_cast<size_t>(r) * cw, static_cast<size_t>(cw));
  }
  src += static_cast<size_t>(cw) * ch;
  for (int r = 0; r < ch; ++r) {
    std::memcpy(v.data + static_cast<size_t>(r) * v.stride,
                src + static_cast<size_t>(r) * cw, static_cast<size_t>(cw));
  }
}

// 把 FrameDesc 描述的一帧按 YUV420P 裸格式写出
bool WriteRawFrame(const lumen::FrameDesc& d, lumen::StreamPipe* out) {
  const int w = d.width;
  const int h = d.height;
  const int cw = (w + 1) / 2;
  const int ch = (h + 1) / 2;

  for (int r = 0; r < h; ++r) {
    const uint8_t* p = d.plane_data[0] + static_cast<size_t>(r) * d.plane_stride[0];
    if (out->Write(p, w) != w) return false;
  }
  for (int r = 0; r < ch; ++r) {
    const uint8_t* p = d.plane_data[1] + static_cast<size_t>(r) * d.plane_stride[1];
    if (out->Write(p, cw) != cw) return false;
  }
  for (int r = 0; r < ch; ++r) {
    const uint8_t* p = d.plane_data[2] + static_cast<size_t>(r) * d.plane_stride[2];
    if (out->Write(p, cw) != cw) return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif

  if (argc < 2) {
    PrintUsage();
    return 2;
  }

  std::string input;
  std::string output;
  std::string backend_name;
  std::string config_path;
  std::string model_override;
  std::string provider_override;
  std::string runtime_dir_override;
  int tile_override = -1;
  int channels_override = 0;
  lumen::InspectOptions iopt;
  int crf = 18;
  int limit_frames = 0;
  bool dry_run = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_next = (i + 1 < argc);
    if (a == "-h" || a == "--help") {
      PrintUsage();
      return 0;
    } else if (a == "--anime") {
      iopt.anime = true;
    } else if (a == "--dry-run") {
      dry_run = true;
    } else if (a == "--preset" && has_next) {
      const std::string p = argv[++i];
      if (p == "performance") iopt.preset = lumen::QualityPreset::Performance;
      else if (p == "balanced") iopt.preset = lumen::QualityPreset::Balanced;
      else if (p == "quality") iopt.preset = lumen::QualityPreset::Quality;
      else if (p == "archive") iopt.preset = lumen::QualityPreset::Archive;
      else {
        std::cerr << "未知档位: " << p << "\n";
        return 2;
      }
    } else if (a == "--backend" && has_next) {
      backend_name = argv[++i];
    } else if (a == "--model" && has_next) {
      model_override = argv[++i];
    } else if (a == "--provider" && has_next) {
      provider_override = argv[++i];
    } else if (a == "--runtime-dir" && has_next) {
      // 指定 onnxruntime.dll 所在目录。
      // 有这个选项才能在不改配置的情况下对比"CPU / DML / CUDA"三种运行时。
      runtime_dir_override = argv[++i];
    } else if (a == "--field-order" && has_next) {
      lumen::FieldOrder fo = lumen::FieldOrder::Unknown;
      if (!lumen::ParseFieldOrder(argv[++i], &fo)) {
        std::cerr << "未知场序: " << argv[i] << "（可选 auto/tff/bff/progressive）\n";
        return 2;
      }
      iopt.field_order_override = fo;
    } else if (a == "--tile" && has_next) {
      tile_override = std::atoi(argv[++i]);
    } else if (a == "--channels" && has_next) {
      channels_override = std::atoi(argv[++i]);
    } else if (a == "--config" && has_next) {
      config_path = argv[++i];
    } else if (a == "--crf" && has_next) {
      crf = std::atoi(argv[++i]);
    } else if (a == "--limit" && has_next) {
      limit_frames = std::atoi(argv[++i]);
    } else if (!a.empty() && a[0] == '-') {
      std::cerr << "未知选项: " << a << "\n";
      return 2;
    } else if (input.empty()) {
      input = a;
    } else if (output.empty()) {
      output = a;
    }
  }

  if (input.empty() || output.empty()) {
    std::cerr << "需要同时指定输入与输出文件\n";
    return 2;
  }

  // -------------------------------------------------------------------------
  // 0. 配置：命令行 > 配置文件 > 默认值
  // -------------------------------------------------------------------------
  lumen::AppConfig app_cfg = lumen::DefaultAppConfig();
  {
    // 未显式指定时，尝试当前目录的 lumen.json（存在就用，不存在静默跳过）
    const std::string cfg_path = config_path.empty() ? "lumen.json" : config_path;
    std::string cfg_err;
    const lumen::Status st = lumen::LoadAppConfig(cfg_path, &app_cfg, &cfg_err);
    if (st == lumen::Status::InvalidArgument) {
      // 语法错误必须报出来：静默用默认值会让用户以为自己的改动生效了
      std::cerr << "配置文件有误：" << cfg_err << "\n";
      return 2;
    }
    if (st == lumen::Status::Ok) {
      std::cout << "配置: " << cfg_path << "\n";
      // 命令行里没写的项，以配置为准
      if (backend_name.empty()) backend_name = app_cfg.engine.backend;
      if (crf == 18 && app_cfg.crf != 18) crf = app_cfg.crf;
      if (!iopt.anime) iopt.anime = app_cfg.anime;
      if (app_cfg.preset == "performance") iopt.preset = lumen::QualityPreset::Performance;
      else if (app_cfg.preset == "quality") iopt.preset = lumen::QualityPreset::Quality;
      else if (app_cfg.preset == "archive") iopt.preset = lumen::QualityPreset::Archive;
      // 场序：只有命令行没写时才用配置里的（命令行 > 配置 > 自动）
      if (iopt.field_order_override == lumen::FieldOrder::Unknown) {
        lumen::FieldOrder fo = lumen::FieldOrder::Unknown;
        if (lumen::ParseFieldOrder(app_cfg.field_order, &fo)) {
          iopt.field_order_override = fo;
        }
      }
    } else if (!config_path.empty()) {
      // 用户明确指定了配置却读不到，必须报错
      std::cerr << cfg_err << "\n";
      return 2;
    }
  }
  if (backend_name.empty()) backend_name = "dummy";
  if (!model_override.empty()) app_cfg.engine.model_path = model_override;
  if (!provider_override.empty()) app_cfg.engine.provider = provider_override;
  if (!runtime_dir_override.empty()) app_cfg.engine.runtime_dir = runtime_dir_override;
  if (tile_override >= 0) app_cfg.engine.tile_size = tile_override;
  if (channels_override == 1 || channels_override == 3) {
    app_cfg.engine.channels = channels_override;
  }

  // -------------------------------------------------------------------------
  // 1. 自检：决定这条片子该怎么处理
  // -------------------------------------------------------------------------
  std::cout << "[1/4] 分析片源 ...\n";
  lumen::InspectionReport report;
  std::string err;
  if (lumen::InspectSource(input, iopt, &report, &err) != lumen::Status::Ok) {
    std::cerr << "自检失败: " << err << "\n";
    return 1;
  }

  const lumen::ProcessingChain& chain = report.chain;
  std::cout << chain.Describe() << "\n";

  const int src_w = report.info.width;
  const int src_h = report.info.height;
  if (src_w <= 0 || src_h <= 0) {
    std::cerr << "源分辨率无效\n";
    return 1;
  }

  // -------------------------------------------------------------------------
  // 2. 创建超分后端
  // -------------------------------------------------------------------------
  lumen::ISRBackend* backend = lumen::CreateBackend(backend_name);
  if (backend == nullptr) {
    std::cerr << "未知后端: " << backend_name << "\n可用: ";
    for (const auto& n : lumen::AvailableBackends()) std::cerr << n << " ";
    std::cerr << "\n";
    return 1;
  }

  lumen::SRBackendConfig cfg;
  cfg.model_path = app_cfg.engine.model_path;
  cfg.runtime_dir = app_cfg.engine.runtime_dir;
  cfg.provider = app_cfg.engine.provider;
  cfg.channels = app_cfg.engine.channels;
  cfg.tile_size = app_cfg.engine.tile_size;
  cfg.allow_fp16 = app_cfg.engine.allow_fp16;
  // 放大倍数：0 表示自动。
  // 选择整数倍的理由：480p→1080p 是 2.25 倍，用 2 倍最接近且成本最低，
  // 剩余部分交给最后的 lanczos 缩放补齐（多出的 1.125 倍不会引入明显软化）。
  if (app_cfg.engine.scale == 2 || app_cfg.engine.scale == 4) {
    cfg.scale = app_cfg.engine.scale;
  } else {
    const double need = 1080.0 / src_h;
    cfg.scale = (need > 3.0) ? 4 : (need > 1.5 ? 2 : 1);
  }

  const lumen::Status init_st = backend->Init(cfg);
  if (init_st != lumen::Status::Ok) {
    std::cerr << "后端初始化失败（" << lumen::ToString(init_st) << "）\n";
    // 具体原因通常非常关键（缺 DLL / 模型不对 / 显存不足），必须原样打印。
    if (auto* ort = dynamic_cast<lumen::OnnxBackend*>(backend)) {
      const std::string why = ort->LastError();
      if (!why.empty()) std::cerr << "原因：" << why << "\n";
    }
    delete backend;
    return 1;
  }
  if (backend->Warmup() != lumen::Status::Ok) {
    std::cerr << "预热失败（模型可能不接受当前分辨率，可尝试 --tile 256）\n";
    if (auto* ort = dynamic_cast<lumen::OnnxBackend*>(backend)) {
      std::cerr << "原因：" << ort->LastError() << "\n";
    }
    delete backend;
    return 1;
  }

  const int mid_w = src_w * cfg.scale;
  const int mid_h = src_h * cfg.scale;
  std::cout << "[2/4] 超分后端: " << backend->Name() << "  " << src_w << "x" << src_h
            << " → " << mid_w << "x" << mid_h << " → " << chain.target_width << "x"
            << chain.target_height << "\n";

  // 初始化成功但带告警（最典型的是 GPU 不可用已回退 CPU）也必须打出来。
  // 否则用户看到 "ort(CPU)" 只会以为程序"用了 CPU 加速"，实际是 GPU 没启用，
  // 白等几小时之后才发现导出的吞吐只有 4 fps。
  if (auto* ort = dynamic_cast<lumen::OnnxBackend*>(backend)) {
    const std::string warn = ort->LastError();
    if (!warn.empty()) std::cerr << "      [警告] " << warn << "\n";
  }

  // -------------------------------------------------------------------------
  // 3. 组装命令
  // -------------------------------------------------------------------------
  const std::string ffmpeg = lumen::FindInPath("ffmpeg.exe");
  if (ffmpeg.empty()) {
    std::cerr << "未找到 ffmpeg.exe\n";
    delete backend;
    return 1;
  }

  const std::string vf = BuildDecodeFilter(chain);

  // 输出帧率：IVTC 会还原 24p（29.97 × 0.8 = 23.976）
  double fps_out = report.info.fps.Valid() ? report.info.fps.ToDouble() : 25.0;
  if (chain.field_mode == lumen::FieldMode::IVTC) fps_out *= 0.8;

  std::vector<std::string> dec_args = {
      "-v", "error", "-i", input,
      "-vf", vf,
      "-fps_mode", "passthrough", // 不做帧率重采样，否则 IVTC 的结果会被改回去
      "-an", "-sn", "-dn",
      "-f", "rawvideo", "-pix_fmt", "yuv420p",
      "pipe:1"};

  // -------------------------------------------------------------------------
  // 计算内容区尺寸：按"显示宽高比"等比填充，绝不拉伸
  // -------------------------------------------------------------------------
  // 这是老片处理最容易做错的一步：
  //   720x480 + SAR 8:9 的真实画面是 4:3，若直接拉到 1920x1080（16:9），
  //   画面横向被拉宽 33%，人脸会变胖、圆形变椭圆。观众第一眼就能看出"不对"，
  //   但往往说不上来哪不对。
  // 默认策略（对应 D6 决策的保守选项）：等比缩放后居中，两侧留黑边，
  //   完整保留导演构图。裁边填满作为可选预设，不在默认路径上。
  const double src_dar = (report.info.DisplayAspect() > 0.1)
                             ? report.info.DisplayAspect()
                             : static_cast<double>(src_w) / src_h;
  const double target_dar =
      static_cast<double>(chain.target_width) / chain.target_height;

  int content_w = chain.target_width;
  int content_h = chain.target_height;
  if (src_dar > target_dar) {
    // 源比目标更宽（如 2.35 宽银幕）：宽度贴合，上下留黑边
    content_h = static_cast<int>(std::lround(chain.target_width / src_dar));
  } else {
    // 源比目标更窄（如 4:3）：高度贴合，左右留黑边
    content_w = static_cast<int>(std::lround(chain.target_height * src_dar));
  }
  // yuv420p 要求宽高为偶数，否则编码器会报错或自动裁一列
  content_w &= ~1;
  content_h &= ~1;
  const int pad_x = (chain.target_width - content_w) / 2;
  const int pad_y = (chain.target_height - content_h) / 2;

  std::string enc_filter =
      "scale=" + std::to_string(content_w) + ":" + std::to_string(content_h) +
      ":flags=lanczos,setsar=1";
  if (pad_x > 0 || pad_y > 0) {
    enc_filter += ",pad=" + std::to_string(chain.target_width) + ":" +
                  std::to_string(chain.target_height) + ":" + std::to_string(pad_x) +
                  ":" + std::to_string(pad_y) + ":black";
  }

  if (std::fabs(src_dar - target_dar) > 0.02) {
    std::cout << "       注意: 源显示比例 " << Num(src_dar, 3) << " 与目标 "
              << Num(target_dar, 3) << " 不同，按原比例居中"
              << ((pad_x > 0) ? "（左右黑边 " + std::to_string(pad_x) + "px）" : "")
              << ((pad_y > 0) ? "（上下黑边 " + std::to_string(pad_y) + "px）" : "")
              << "\n";
  }

  // 源有音轨时，视频先写中间文件，最后再把音轨复制进来。
  // 不把音频塞进同一条管道的原因：音频不需要处理，走管道只会增加一个阻塞点，
  // 而 ffmpeg 的 -c copy 直接复制音轨既快又无损。
  const bool has_audio = !report.info.audio.empty();
  const std::string video_only = has_audio ? (output + ".v.mp4") : output;

  std::vector<std::string> enc_args = {
      "-v", "error",
      "-f", "rawvideo", "-pix_fmt", "yuv420p",
      "-s", std::to_string(mid_w) + "x" + std::to_string(mid_h),
      "-r", Num(fps_out),
      "-i", "pipe:0",
      "-vf", enc_filter,
      "-c:v", "libx264", "-preset", "medium", "-crf", std::to_string(crf),
      "-pix_fmt", "yuv420p",
      // 必须显式写入色彩标记：我们已把画面转到 BT.709，
      // 若容器里不标，播放器会按 601 再解释一次，颜色又回到错的状态。
      // 这是"看起来处理了但颜色就是不对"的典型根因。
      "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709",
      "-color_range", "tv",
      "-movflags", "+faststart",
      "-y", video_only};

  if (dry_run) {
    std::cout << "\n[解码命令]\nffmpeg " << lumen::BuildCommandLine("ffmpeg", dec_args) << "\n";
    std::cout << "\n[编码命令]\nffmpeg " << lumen::BuildCommandLine("ffmpeg", enc_args) << "\n";
    if (has_audio) {
      const std::vector<std::string> mux_args = {
          "-v", "error", "-i", video_only, "-i", input,
          "-map", "0:v:0", "-map", "1:a?",
          "-c", "copy", "-movflags", "+faststart", "-y", output};
      std::cout << "\n[封装音轨]\nffmpeg "
                << lumen::BuildCommandLine("ffmpeg", mux_args) << "\n";
    }
    delete backend;
    return 0;
  }

  // -------------------------------------------------------------------------
  // 4. 跑管线
  // -------------------------------------------------------------------------
  lumen::StreamPipe decoder;
  if (decoder.Open(ffmpeg, dec_args, lumen::PipeMode::Read) != lumen::Status::Ok) {
    std::cerr << "无法启动解码进程\n";
    delete backend;
    return 1;
  }

  lumen::StreamPipe encoder;
  if (encoder.Open(ffmpeg, enc_args, lumen::PipeMode::Write) != lumen::Status::Ok) {
    std::cerr << "无法启动编码进程\n";
    decoder.Close();
    delete backend;
    return 1;
  }

  lumen::FrameBuffer in_frame;
  if (in_frame.Allocate(src_w, src_h, lumen::PixelFormat::YUV420P) != lumen::Status::Ok) {
    std::cerr << "内存不足\n";
    encoder.Close();
    decoder.Close();
    delete backend;
    return 1;
  }

  const size_t in_bytes = static_cast<size_t>(src_w) * src_h * 3 / 2;
  std::vector<uint8_t> raw(in_bytes);

  std::cout << "[3/4] 开始处理 ...\n";
  const auto t_start = std::chrono::steady_clock::now();

  int64_t frames = 0;
  bool write_ok = true;
  while (limit_frames <= 0 || frames < limit_frames) {
    // 读满一帧（管道读取可能只返回部分数据，必须循环补齐）
    size_t got = 0;
    while (got < in_bytes) {
      const int n = decoder.Read(raw.data() + got, static_cast<int>(in_bytes - got));
      if (n <= 0) break;
      got += static_cast<size_t>(n);
    }
    if (got < in_bytes) break; // 输入结束

    LoadRawFrame(raw, &in_frame);
    in_frame.set_pts(static_cast<lumen::Timestamp>(frames * 1000000.0 / fps_out));
    in_frame.set_index(frames);

    lumen::FrameDesc in_desc = in_frame.ToDesc();
    lumen::FrameDesc out_desc;
    if (backend->Process(in_desc, &out_desc) != lumen::Status::Ok) {
      std::cerr << "\n第 " << frames << " 帧处理失败\n";
      write_ok = false;
      break;
    }

    if (!WriteRawFrame(out_desc, &encoder)) {
      std::cerr << "\n第 " << frames << " 帧写出失败（编码进程可能已退出）\n";
      write_ok = false;
      break;
    }

    ++frames;
    if (frames % 30 == 0) {
      const auto now = std::chrono::steady_clock::now();
      const double sec = std::chrono::duration<double>(now - t_start).count();
      std::cout << "\r  已处理 " << frames << " 帧  (" << Num(frames / sec, 1)
                << " fps, 平均 " << Num(backend->Stats().avg_ms, 1) << " ms/帧)"
                << std::flush;
    }
  }

  // 先关解码（读端已到 EOF），再关编码以触发收尾写盘
  decoder.Close();
  std::cout << "\n[4/4] 收尾编码 ...\n";
  const int enc_code = encoder.Close();

  // 音轨封装：-c copy 不重新编码，音质与源完全一致
  bool mux_ok = true;
  if (has_audio && write_ok && enc_code == 0) {
    const lumen::ProcessResult mux = lumen::RunProcess(
        ffmpeg, {"-v", "error", "-i", video_only, "-i", input, "-map", "0:v:0",
                 "-map", "1:a?", "-c", "copy", "-movflags", "+faststart", "-y", output},
        600000);
    mux_ok = mux.launched && mux.exit_code == 0;
    if (!mux_ok) {
      std::cerr << "音轨封装失败：" << mux.output.substr(0, 300) << "\n";
    }
    std::remove(video_only.c_str());
  }

  const auto t_end = std::chrono::steady_clock::now();
  const double total_sec = std::chrono::duration<double>(t_end - t_start).count();

  const lumen::SRStats stats = backend->Stats();
  delete backend;

  std::cout << "\n=========== 导出结果 ===========\n";
  std::cout << "帧数      : " << frames << "\n";
  std::cout << "总耗时    : " << Num(total_sec, 1) << " s\n";
  if (total_sec > 0) {
    std::cout << "吞吐      : " << Num(frames / total_sec, 2) << " fps\n";
  }
  std::cout << "单帧均值  : " << Num(stats.avg_ms, 2) << " ms  峰值 "
            << Num(stats.max_ms, 2) << " ms\n";
  std::cout << "编码退出码: " << enc_code;
  if (has_audio) std::cout << "  音轨封装: " << (mux_ok ? "成功" : "失败");
  else std::cout << "  (源无音轨)";
  std::cout << "\n";
  std::cout << "输出文件  : " << output << "\n";

  if (!write_ok || enc_code != 0 || frames == 0) {
    std::cerr << "\n导出未完成，请检查上方错误信息。\n";
    return 1;
  }
  std::cout << "\n完成。\n";
  return 0;
}
