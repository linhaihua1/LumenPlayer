// ============================================================================
//  LumenPlayer - 片源自检命令行工具（lumen-probe）
//  ---------------------------------------------------------------------------
//  所属层次：应用层（薄壳，只做参数解析与输出格式化）
//  职责    ：一条命令打印完整自检报告。开发期用来验证算法，
//            用户侧用来在播放前搞清楚"这个文件到底该怎么处理"。
//
//  用法示例：
//    lumen-probe "D:\剧集\S01E01.mkv"
//    lumen-probe movie.mp4 --json --preset quality
//    lumen-probe clip.ts --anime --points 8 --frames 10
// ============================================================================
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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
      "LumenPlayer 片源自检工具 v0.1\n"
      "\n"
      "用法: lumen-probe <视频文件> [选项]\n"
      "\n"
      "选项:\n"
      "  --json              以 JSON 输出（便于脚本处理）\n"
      "  --preset <档位>     performance | balanced | quality | archive（默认 balanced）\n"
      "  --field-order <序>  auto | tff | bff | progressive（覆盖自动场序判定）\n"
      "  --anime             按动画素材处理（影响颗粒强度与模型选择）\n"
      "  --points <N>        全片采样点数（默认 6）\n"
      "  --frames <N>        每个采样点抽帧数（默认 6）\n"
      "  --ffmpeg <路径>     指定 ffmpeg.exe\n"
      "  --ffprobe <路径>    指定 ffprobe.exe\n"
      "  --out <文件>        把报告写入文件（UTF-8）\n"
      "  --keep-temp         保留临时抽帧文件（排查用）\n"
      "  -h, --help          显示本帮助\n"
      "\n"
      "说明: 本工具只读取文件、输出报告，不修改任何源文件。\n";
}

lumen::QualityPreset ParsePreset(const std::string& s, bool* ok) {
  *ok = true;
  if (s == "performance") return lumen::QualityPreset::Performance;
  if (s == "balanced") return lumen::QualityPreset::Balanced;
  if (s == "quality") return lumen::QualityPreset::Quality;
  if (s == "archive") return lumen::QualityPreset::Archive;
  *ok = false;
  return lumen::QualityPreset::Balanced;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  // 控制台切到 UTF-8：否则中文报告在 cmd 里会变成乱码。
  // 标题与路径都可能含中文，这一步不是可选的优化。
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif

  if (argc < 2) {
    PrintUsage();
    return 2;
  }

  lumen::InspectOptions opt;
  std::string input;
  std::string out_file;
  bool json = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_next = (i + 1 < argc);

    if (a == "-h" || a == "--help") {
      PrintUsage();
      return 0;
    } else if (a == "--json") {
      json = true;
    } else if (a == "--anime") {
      opt.anime = true;
    } else if (a == "--keep-temp") {
      opt.keep_temp = true;
    } else if (a == "--preset" && has_next) {
      bool ok = false;
      opt.preset = ParsePreset(argv[++i], &ok);
      if (!ok) {
        std::cerr << "未知档位: " << argv[i] << "\n";
        return 2;
      }
    } else if (a == "--field-order" && has_next) {
      // 手动指定场序：覆盖"像素分析 + 元数据"得来的自动结论。
      // 典型用法：用户明确知道片源是 BFF（某些港版 DVD 剧集），
      // 而自动判定在强噪点镜头上给了 TFF，导致反交错后上下抖动。
      lumen::FieldOrder fo = lumen::FieldOrder::Unknown;
      if (!lumen::ParseFieldOrder(argv[++i], &fo)) {
        std::cerr << "未知场序: " << argv[i] << "（可选 auto/tff/bff/progressive）\n";
        return 2;
      }
      opt.field_order_override = fo;
    } else if (a == "--points" && has_next) {
      opt.sample_points = std::max(1, std::atoi(argv[++i]));
    } else if (a == "--frames" && has_next) {
      opt.frames_per_point = std::max(1, std::atoi(argv[++i]));
    } else if (a == "--ffmpeg" && has_next) {
      opt.ffmpeg_path = argv[++i];
    } else if (a == "--ffprobe" && has_next) {
      opt.ffprobe_path = argv[++i];
    } else if (a == "--out" && has_next) {
      out_file = argv[++i];
    } else if (!a.empty() && a[0] == '-') {
      std::cerr << "未知选项: " << a << "\n";
      return 2;
    } else if (input.empty()) {
      input = a;
    }
  }

  if (input.empty()) {
    std::cerr << "缺少输入文件\n";
    return 2;
  }

  lumen::InspectionReport report;
  std::string err;
  const lumen::Status st = lumen::InspectSource(input, opt, &report, &err);

  if (st != lumen::Status::Ok) {
    std::cerr << "自检失败（" << lumen::ToString(st) << "）: " << err << "\n";
    return 1;
  }

  const std::string text = json ? report.ToJson() : report.ToText();
  std::cout << text << "\n";

  if (!out_file.empty()) {
    std::ofstream f(out_file, std::ios::binary);
    if (!f) {
      std::cerr << "无法写入: " << out_file << "\n";
      return 1;
    }
    f << text << "\n";
    std::cerr << "报告已写入: " << out_file << "\n";
  }
  return 0;
}
