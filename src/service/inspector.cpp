// ============================================================================
//  LumenPlayer - 片源自检服务实现
//  所属层次：服务层
// ============================================================================
#include "service/inspector.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#include "platform/process.h"

namespace lumen {
namespace {

// 生成临时文件名。
// 用"时间戳 + 自增序号"而非 pid：跨平台无需额外头文件，且并发调用也安全。
std::string MakeTempPath(const char* tag) {
  static std::atomic<unsigned> seq{0};
  const char* tmp = std::getenv("TEMP");
  if (tmp == nullptr) tmp = std::getenv("TMPDIR");
  std::string dir = (tmp != nullptr) ? tmp : ".";
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::ostringstream os;
  os << dir << (dir.back() == '\\' || dir.back() == '/' ? "" : "\\") << "lumen_" << tag
     << "_" << now << "_" << seq.fetch_add(1) << ".tmp";
  return os.str();
}

// RAII 临时文件：析构即删除，异常/提前返回都不会漏文件。
struct ScopedFile {
  std::string path;
  bool keep = false;
  ~ScopedFile() {
    if (!keep && !path.empty()) std::remove(path.c_str());
  }
};

// 从 ffmpeg 抽出的 rawvideo 文件填充 FrameBuffer 数组。
// 参数说明：raw 路径、帧宽高、期望帧数、输出数组
// 说明：rawvideo 无行对齐（stride == width），必须逐行拷贝到我们按 32 对齐的缓冲。
int LoadRawFrames(const std::string& raw_path, int w, int h, int want,
                  std::vector<FrameBuffer>* frames) {
  std::ifstream in(raw_path, std::ios::binary);
  if (!in) return 0;

  const size_t y_size = static_cast<size_t>(w) * h;
  const size_t c_size = static_cast<size_t>((w + 1) / 2) * ((h + 1) / 2);
  const size_t frame_bytes = y_size + 2 * c_size;

  std::vector<uint8_t> buf(frame_bytes);
  int loaded = 0;
  for (int i = 0; i < want; ++i) {
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(frame_bytes));
    if (static_cast<size_t>(in.gcount()) < frame_bytes) break;

    FrameBuffer fb;
    if (fb.Allocate(w, h, PixelFormat::YUV420P) != Status::Ok) break;

    const PlaneView py = fb.Plane(0);
    const PlaneView pu = fb.Plane(1);
    const PlaneView pv = fb.Plane(2);
    const uint8_t* src = buf.data();

    for (int y = 0; y < h; ++y) {
      std::copy(src + static_cast<size_t>(y) * w, src + static_cast<size_t>(y) * w + w,
                py.data + static_cast<size_t>(y) * py.stride);
    }
    src += y_size;
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;
    for (int y = 0; y < ch; ++y) {
      std::copy(src + static_cast<size_t>(y) * cw, src + static_cast<size_t>(y) * cw + cw,
                pu.data + static_cast<size_t>(y) * pu.stride);
    }
    src += c_size;
    for (int y = 0; y < ch; ++y) {
      std::copy(src + static_cast<size_t>(y) * cw, src + static_cast<size_t>(y) * cw + cw,
                pv.data + static_cast<size_t>(y) * pv.stride);
    }

    frames->push_back(std::move(fb));
    ++loaded;
  }
  return loaded;
}

std::string FormatDuration(int64_t us) {
  const int64_t total_sec = us / 1000000;
  const int h = static_cast<int>(total_sec / 3600);
  const int m = static_cast<int>((total_sec % 3600) / 60);
  const int s = static_cast<int>(total_sec % 60);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
  return buf;
}

double MedianOf(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return (n % 2 == 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) * 0.5;
}

} // namespace

Status InspectSource(const std::string& path, const InspectOptions& opt,
                     InspectionReport* report, std::string* err) {
  if (report == nullptr) return Status::InvalidArgument;
  *report = InspectionReport();
  report->path = path;

  // -------------------------------------------------------------------------
  // 1. 定位外部程序
  // -------------------------------------------------------------------------
  const std::string ffprobe =
      opt.ffprobe_path.empty() ? FindInPath("ffprobe.exe") : opt.ffprobe_path;
  const std::string ffmpeg =
      opt.ffmpeg_path.empty() ? FindInPath("ffmpeg.exe") : opt.ffmpeg_path;

  if (ffprobe.empty()) {
    if (err) *err = "未找到 ffprobe，请安装 ffmpeg 或在配置中指定路径";
    return Status::NotFound;
  }
  if (ffmpeg.empty()) {
    if (err) *err = "未找到 ffmpeg，请安装 ffmpeg 或在配置中指定路径";
    return Status::NotFound;
  }

  // -------------------------------------------------------------------------
  // 2. 元数据
  // -------------------------------------------------------------------------
  const ProcessResult probe = RunProcess(
      ffprobe,
      {"-v", "error", "-print_format", "json", "-show_streams", "-show_format", path},
      opt.process_timeout_ms);

  if (!probe.launched) {
    if (err) *err = "无法启动 ffprobe：" + ffprobe;
    return Status::NotFound;
  }
  {
    std::string parse_err;
    const Status st = ParseFfprobeJson(probe.output, &report->info, &parse_err);
    if (st != Status::Ok) {
      if (err) {
        *err = "解析 ffprobe 输出失败：" + parse_err;
        if (!probe.output.empty()) {
          // ffprobe 的报错通常在 output 里，直接带出来，免得用户瞎猜
          *err += " | ffprobe: " + probe.output.substr(0, 300);
        }
      }
      return st;
    }
  }
  report->metadata_diagnosis = DiagnoseMetadata(report->info);

  const SourceInfo& info = report->info;
  if (info.width < 16 || info.height < 16) {
    if (err) *err = "视频分辨率异常（" + std::to_string(info.width) + "x" +
                    std::to_string(info.height) + "）";
    return Status::InvalidArgument;
  }

  // -------------------------------------------------------------------------
  // 3. 抽样分析
  // -------------------------------------------------------------------------
  double duration_sec = static_cast<double>(info.duration_us) / 1e6;
  if (duration_sec <= 1.0) {
    duration_sec = 60.0; // 时长未知（部分 TS/MKV）时假设 60 秒，仅影响采样位置
    report->warnings.push_back("未读到有效时长，采样位置按 60 秒估算");
  }

  CadenceDetector detector;
  std::vector<double> grain_sigmas;
  GrainEstimate grain_best;      // 记录样本量最大的一次估计，作为展示用
  LumaRangeProbe luma_probe;
  bool have_luma = false;

  const int points = std::max(1, opt.sample_points);
  for (int p = 0; p < points; ++p) {
    // 采样区间避开片头（台标/黑场）与片尾（演职员表/黑场）
    const double frac =
        (points == 1) ? 0.5 : (0.08 + (0.92 - 0.08) * p / (points - 1));
    const double t = duration_sec * frac;

    ScopedFile tmp;
    tmp.path = MakeTempPath("sample");
    tmp.keep = opt.keep_temp;

    char ss[32];
    std::snprintf(ss, sizeof(ss), "%.3f", t);

    // -ss 放在 -i 前：快速定位到关键帧，速度远快于解码到目标位置。
    // 对"抽几帧看一下统计特征"这个用途，关键帧精度完全够用。
    const ProcessResult ex =
        RunProcess(ffmpeg,
                   {"-v", "error", "-ss", ss, "-i", path, "-frames:v",
                    std::to_string(std::max(1, opt.frames_per_point)), "-an", "-sn", "-dn",
                    "-f", "rawvideo", "-pix_fmt", "yuv420p", "-y", tmp.path},
                   opt.process_timeout_ms);

    if (!ex.launched || ex.exit_code != 0) {
      report->warnings.push_back("采样点 " + std::to_string(static_cast<int>(t)) +
                                 "s 抽帧失败，已跳过");
      continue;
    }

    std::vector<FrameBuffer> frames;
    const int loaded =
        LoadRawFrames(tmp.path, info.width, info.height, opt.frames_per_point, &frames);
    if (loaded == 0) {
      report->warnings.push_back("采样点 " + std::to_string(static_cast<int>(t)) +
                                 "s 未解出帧，已跳过");
      continue;
    }

    for (auto& f : frames) {
      detector.Feed(f);
      ++report->frames_analyzed;
    }

    // 色彩与颗粒探测：取该采样点里的一帧。
    // 不逐帧平均的原因：不同镜头的颗粒强度差异极大，平均值会两边都不像。
    const int probe_idx = std::min(opt.probe_frame_index, loaded - 1);
    const FrameBuffer& probe_frame = frames[probe_idx];

    if (!have_luma) {
      luma_probe = ProbeLumaRange(probe_frame);
      have_luma = true;
    }

    const GrainEstimate ge = EstimateGrain(probe_frame);
    if (ge.blocks_used > 0) {
      grain_sigmas.push_back(ge.sigma);
      if (ge.blocks_used > grain_best.blocks_used) grain_best = ge;
    }

    ++report->sample_points_done;
    frames.clear(); // 及时释放，避免长片积累大量帧缓冲
  }

  if (report->frames_analyzed == 0) {
    if (err) *err = "所有采样点均抽帧失败，可能是编码不支持或文件损坏";
    return Status::IoError;
  }

  // -------------------------------------------------------------------------
  // 4. 汇总判定
  // -------------------------------------------------------------------------
  report->cadence = detector.Verdict();

  // 颗粒强度取各采样点的中位数：比单点更稳，也比平均更抗极端镜头。
  GrainEstimate grain_agg = grain_best;
  if (!grain_sigmas.empty()) {
    grain_agg.sigma = MedianOf(grain_sigmas);
  }
  report->grain = grain_agg;

  const FrameDesc meta_desc = [&]() {
    FrameDesc d;
    d.width = info.width;
    d.height = info.height;
    d.sar = info.sar;
    d.fps = info.fps;
    d.format = info.format;
    d.colorspace = info.colorspace;
    d.range = info.range;
    d.field_order = info.field_order;
    return d;
  }();

  report->luma_probe = luma_probe;
  report->color = DecideColor(meta_desc, &luma_probe);
  report->grain_decision = DecideGrain(grain_agg, opt.anime);
  report->chain = BuildChain(info, report->cadence, report->color,
                             report->grain_decision, opt.preset, opt.anime);

  if (report->cadence.cadence == SourceCadence::Unknown) {
    report->warnings.push_back(
        "时域结构结论不足，处理链按保守策略生成，建议增加采样点或人工复核");
  }

  // 用户手动指定场序：必须放在 BuildChain 之后，因为它是对自动结论的"覆盖"。
  // 同时在告警里留一条记录——事后看报告的人需要知道"这个结论是人定的"。
  if (opt.field_order_override != FieldOrder::Unknown) {
    OverrideFieldOrder(opt.field_order_override, &report->chain);
    report->warnings.push_back(std::string("场序由用户强制指定为 ") +
                               ToString(opt.field_order_override) + "（已覆盖自动判定）");
  }

  return Status::Ok;
}

// ---------------------------------------------------------------------------
// 报告输出
// ---------------------------------------------------------------------------
std::string InspectionReport::ToText() const {
  std::ostringstream os;
  os << "=========== LumenPlayer 片源自检报告 ===========\n";
  os << "文件      : " << path << "\n";
  os << "容器      : " << info.container << "\n";
  os << "时长      : " << FormatDuration(info.duration_us) << "\n";
  os << "视频编码  : " << info.video_codec << "\n";
  os << "分辨率    : " << info.width << "x" << info.height << "  ("
     << ToString(info.format) << ")\n";
  os << "帧率      : ";
  if (info.fps.Valid()) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f (%d/%d)", info.fps.ToDouble(), info.fps.num,
                  info.fps.den);
    os << buf;
  } else {
    os << "未知";
  }
  os << "\n";
  char sar[64];
  std::snprintf(sar, sizeof(sar), "%d:%d", info.sar.num, info.sar.den);
  os << "像素比    : " << sar << "  → 显示宽高比 " << info.DisplayAspect() << "\n";
  os << "位率      : " << (info.bit_rate / 1000) << " kbps\n";
  os << "音轨      : " << info.audio.size() << " 条\n";

  os << "\n--- 元数据自检 ---\n";
  if (metadata_diagnosis.empty()) {
    os << "无异常\n";
  } else {
    for (const auto& d : metadata_diagnosis) {
      os << "[" << ToString(d.level) << "] " << d.message << "\n";
    }
  }

  os << "\n--- 时域结构（像素分析，共 " << frames_analyzed << " 帧）---\n";
  os << "结论      : " << ToString(cadence.cadence) << "\n";
  char conf[64];
  std::snprintf(conf, sizeof(conf), "%.0f%%", cadence.confidence * 100.0);
  os << "置信度    : " << conf << "\n";
  os << "需去隔行  : " << (cadence.needs_deinterlace ? "是" : "否") << "\n";
  os << "可还原24p : " << (cadence.ivtc_recoverable ? "是" : "否") << "\n";
  os << "依据      : " << cadence.reason << "\n";

  os << "\n--- 色彩 ---\n";
  os << luma_probe.detail << "\n";
  os << "源        : " << ToString(color.source_space) << " / "
     << ToString(color.source_range) << (color.guessed ? "（推断）" : "（元数据）") << "\n";
  os << "转换      : " << (color.needs_matrix_conversion ? "需矩阵转换 601→709 " : "")
     << (color.needs_range_conversion ? "需范围压缩 " : "");
  if (!color.needs_matrix_conversion && !color.needs_range_conversion) os << "无";
  os << "\n";
  os << "依据      : " << color.reason << "\n";

  os << "\n--- 颗粒 ---\n";
  char gb[128];
  std::snprintf(gb, sizeof(gb), "σ≈%.2f", grain.sigma);
  os << "估计强度  : " << gb << "（" << grain.detail << "）\n";
  os << "决策      : " << grain_decision.reason << "\n";
  char amp[64];
  std::snprintf(amp, sizeof(amp), "%.0f%%", grain_decision.amplitude * 100.0);
  os << "回注强度  : " << (grain_decision.enabled ? amp : "不回注") << "\n";

  os << "\n--- 生成的处理链 ---\n";
  os << chain.Describe();

  // 性能预算：这是决定"能不能实时"的唯一算式
  if (info.fps.Valid() && info.fps.ToDouble() > 0) {
    const double frame_ms = 1000.0 / info.fps.ToDouble();
    os << "\n--- 性能预算 ---\n";
    char pb[160];
    std::snprintf(pb, sizeof(pb),
                  "单帧预算 %.2f ms（%.3f fps）；留 30%% 余量后推理需 ≤ %.2f ms\n",
                  frame_ms, info.fps.ToDouble(), frame_ms * 0.7);
    os << pb;
  }

  if (!warnings.empty()) {
    os << "\n--- 评估过程提示 ---\n";
    for (const auto& w : warnings) os << "- " << w << "\n";
  }

  os << "\n提示：本报告只描述「检测到什么」与「将如何处理」，不评价片源内容。\n";
  return os.str();
}

std::string InspectionReport::ToJson() const {
  std::ostringstream os;
  os << "{";
  os << "\"path\":\"" << path << "\",";
  os << "\"width\":" << info.width << ",";
  os << "\"height\":" << info.height << ",";
  os << "\"fps\":" << (info.fps.Valid() ? info.fps.ToDouble() : 0.0) << ",";
  os << "\"duration_s\":" << (info.duration_us / 1000000) << ",";
  os << "\"codec\":\"" << info.video_codec << "\",";
  os << "\"cadence\":\"" << ToString(cadence.cadence) << "\",";
  os << "\"cadence_confidence\":" << cadence.confidence << ",";
  os << "\"field_mode\":\"" << ToString(chain.field_mode) << "\",";
  os << "\"source_space\":\"" << ToString(color.source_space) << "\",";
  os << "\"convert_matrix\":" << (color.needs_matrix_conversion ? "true" : "false") << ",";
  os << "\"grain_sigma\":" << grain.sigma << ",";
  os << "\"grain_amplitude\":" << grain_decision.amplitude << ",";
  os << "\"frames_analyzed\":" << frames_analyzed << ",";
  os << "\"chain\":" << chain.ToJson();
  os << "}";
  return os.str();
}

} // namespace lumen
