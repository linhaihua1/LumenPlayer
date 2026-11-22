// ============================================================================
//  LumenPlayer - 批量导出（lumen-batch）
//  ---------------------------------------------------------------------------
//  所属层次：应用层
//  职责    ：把"一堆老片"排成队列，逐个调用 lumen-render 导出 1080p，
//            跳过已完成的、记录日志、最后给一张汇总表。
//
//  ★ 为什么用"调用 lumen-render 子进程"而不是把导出逻辑复制一份：
//    1. 一致性：策略层只有一份实现，批量与单文件导出的画质必然一致。
//       复制一份最容易出现"单文件导出没问题、批量导出颜色不对"这种鬼故事。
//    2. 隔离：某个片子把解码器搞崩了（老片容器千奇百怪），只会挂一个子进程，
//       队列继续跑，不会整批中断。
//    3. 可恢复：失败的文件单独重跑即可，不需要从头来。
//
//  ★ 串行执行是刻意的：
//    超分是 GPU 密集任务，两个任务并行只会互相抢显存与算力，
//    总吞吐不升反降（显存不足还会直接失败）。
//
//  用法：
//    lumen-batch D:\老剧\                   处理整个目录（递归）
//    lumen-batch list.txt -o D:\out         按清单文件处理，输出到指定目录
//    lumen-batch D:\老剧\ --force           已存在的也重新处理
//    lumen-batch D:\老剧\ --dry-run         只打印计划，不实际执行
// ============================================================================
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "core/config.h"
#include "platform/process.h"

namespace fs = std::filesystem;

namespace {

// 认定的视频扩展名。刻意只收常见容器：把 .txt/.jpg 混进队列没有意义，
// 而误判一个非视频文件会让整批多一次失败记录。
const char* kVideoExts[] = {".mp4", ".mkv", ".avi", ".ts",  ".m2ts", ".mpg",
                            ".mpeg", ".wmv", ".flv", ".mov", ".rmvb", ".rm",
                            ".vob", ".m4v", ".webm"};

bool IsVideoFile(const fs::path& p) {
  std::string ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const char* e : kVideoExts) {
    if (ext == e) return true;
  }
  return false;
}

std::string Utf8(const fs::path& p) {
  // C++17 的 u8string 在 C++20 改了返回类型，这里用 string() 再按本地编码处理
  // 会有中文问题；MinGW 下 path::string() 返回的是窄字符（本地代码页）。
  // 为稳妥，统一走 wstring 转 UTF-8。
  const std::wstring w = p.wstring();
#ifdef _WIN32
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                    nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr,
                      nullptr);
  return s;
#else
  return p.string();
#endif
}

fs::path FromUtf8(const std::string& s) {
#ifdef _WIN32
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                    nullptr, 0);
  if (n <= 0) return fs::path();
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
  return fs::path(w);
#else
  return fs::path(s);
#endif
}

bool ReadFileLines(const std::string& path, std::vector<std::string>* out) {
  std::ifstream in(FromUtf8(path), std::ios::binary);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    // 跳过空行与 # 注释，方便用户手工维护清单
    if (line.empty() || line[0] == '#') continue;
    out->push_back(line);
  }
  return true;
}

void PrintUsage() {
  std::printf(
      "LumenPlayer 批量导出 v0.1\n"
      "\n"
      "用法: lumen-batch <目录|清单.txt|视频文件...> [选项]\n"
      "\n"
      "选项:\n"
      "  -o, --out <目录>   输出目录（默认与源文件同目录）\n"
      "  --config <路径>    配置文件（默认找 exe 同目录的 lumen.json）\n"
      "  --preset <档位>    performance | balanced | quality | archive\n"
      "  --crf <N>          x264 质量（默认取配置，18）\n"
      "  --limit <N>        每个文件只处理前 N 帧（联调用）\n"
      "  --force            已存在的输出也重新处理\n"
      "  --dry-run          只打印执行计划\n"
      "  -h, --help         显示帮助\n"
      "\n"
      "说明：输出文件名为 <原名>_1080p.mp4；每个任务的日志写在输出目录的 logs/ 下。\n");
}

std::string ExeDirectory() {
  wchar_t buf[MAX_PATH] = {0};
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0) return std::string();
  std::wstring p(buf, n);
  const size_t pos = p.find_last_of(L"\\/");
  if (pos == std::wstring::npos) return std::string();
  std::wstring dir = p.substr(0, pos + 1);
  const int need = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(need > 0 ? need - 1 : 0), '\0');
  if (need > 0) WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, &s[0], need, nullptr, nullptr);
  return s;
}

struct Job {
  std::string input;
  std::string output;
  bool skipped = false;
  bool ok = false;
  std::string note;
  double seconds = 0.0;
};

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

  std::vector<std::string> inputs;
  std::string out_dir;
  std::string config_path;
  std::string preset_override;
  int crf_override = -1;
  int limit = 0;
  bool force = false;
  bool dry_run = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_next = (i + 1 < argc);
    if (a == "-h" || a == "--help") {
      PrintUsage();
      return 0;
    } else if ((a == "-o" || a == "--out") && has_next) {
      out_dir = argv[++i];
    } else if (a == "--config" && has_next) {
      config_path = argv[++i];
    } else if (a == "--preset" && has_next) {
      preset_override = argv[++i];
    } else if (a == "--crf" && has_next) {
      crf_override = std::atoi(argv[++i]);
    } else if (a == "--limit" && has_next) {
      limit = std::atoi(argv[++i]);
    } else if (a == "--force") {
      force = true;
    } else if (a == "--dry-run") {
      dry_run = true;
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "未知选项: %s\n", a.c_str());
      return 2;
    } else {
      inputs.push_back(a);
    }
  }

  const std::string exe_dir = ExeDirectory();
  lumen::AppConfig cfg = lumen::DefaultAppConfig();
  {
    const std::string p = config_path.empty() ? (exe_dir + "lumen.json") : config_path;
    std::string err;
    const lumen::Status st = lumen::LoadAppConfig(p, &cfg, &err);
    if (st == lumen::Status::InvalidArgument) {
      std::fprintf(stderr, "配置文件有误：%s\n", err.c_str());
      return 2;
    }
    if (st == lumen::Status::Ok) std::printf("配置: %s\n", p.c_str());
  }

  // -------------------------------------------------------------------------
  // 1. 收集任务
  // -------------------------------------------------------------------------
  std::vector<fs::path> sources;
  for (const auto& in : inputs) {
    const fs::path p = FromUtf8(in);
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
      // 递归扫描。老片通常一部剧一个目录，递归是必须的。
      for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
           it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && IsVideoFile(it->path())) sources.push_back(it->path());
      }
    } else if (fs::is_regular_file(p, ec)) {
      // 清单文件：按后缀判断，避免"文本里其实是视频路径"的歧义
      std::string ext = p.extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (ext == ".txt" || ext == ".list" || ext == ".csv") {
        std::vector<std::string> lines;
        if (ReadFileLines(in, &lines)) {
          for (const auto& l : lines) {
            const fs::path lp = FromUtf8(l);
            if (fs::is_regular_file(lp, ec)) sources.push_back(lp);
            else std::fprintf(stderr, "清单中的文件不存在，已跳过：%s\n", l.c_str());
          }
        } else {
          std::fprintf(stderr, "无法读取清单：%s\n", in.c_str());
        }
      } else {
        sources.push_back(p);
      }
    } else {
      std::fprintf(stderr, "路径不存在，已跳过：%s\n", in.c_str());
    }
  }

  if (sources.empty()) {
    std::fprintf(stderr, "没有找到可处理的视频文件。\n");
    return 1;
  }

  // 排序：让同一次运行的顺序可复现（目录遍历顺序在不同文件系统上不一致）
  std::sort(sources.begin(), sources.end());

  // -------------------------------------------------------------------------
  // 2. 生成任务表
  // -------------------------------------------------------------------------
  const std::string render_exe = [&]() {
    const std::string local = exe_dir + "lumen-render.exe";
    if (GetFileAttributesW(FromUtf8(local).wstring().c_str()) != INVALID_FILE_ATTRIBUTES) {
      return local;
    }
    return lumen::FindInPath("lumen-render.exe");
  }();
  if (render_exe.empty()) {
    std::fprintf(stderr, "找不到 lumen-render.exe（应与本程序放在同一目录）\n");
    return 1;
  }

  std::vector<Job> jobs;
  jobs.reserve(sources.size());
  for (const auto& src : sources) {
    Job j;
    j.input = Utf8(src);
    fs::path dir = out_dir.empty() ? src.parent_path() : FromUtf8(out_dir);
    std::error_code ec;
    if (out_dir.empty() && cfg.output_dir.size()) dir = FromUtf8(cfg.output_dir);
    if (!dir.empty()) fs::create_directories(dir, ec);
    j.output = Utf8(dir / (src.stem().wstring() + L"_1080p.mp4"));

    // 断点续跑：已存在且非空就跳过。老片批量导出动辄几小时，
    // 没有这个机制等于"中途断电就要全部重来"。
    if (!force) {
      std::error_code fec;
      const auto sz = fs::file_size(FromUtf8(j.output), fec);
      if (!fec && sz > 0) {
        j.skipped = true;
        j.ok = true;
        j.note = "已存在，跳过";
      }
    }
    jobs.push_back(j);
  }

  std::printf("共 %zu 个文件，其中 %zu 个待处理。\n\n", jobs.size(),
              static_cast<size_t>(std::count_if(jobs.begin(), jobs.end(),
                                                [](const Job& j) { return !j.skipped; })));

  if (dry_run) {
    for (const auto& j : jobs) {
      std::printf("%s\n    -> %s%s\n", j.input.c_str(), j.output.c_str(),
                  j.skipped ? "  (跳过)" : "");
    }
    return 0;
  }

  // 日志目录：失败时用户需要看到 ffmpeg/mpv 的原始报错
  {
    std::error_code ec;
    fs::create_directories(FromUtf8(exe_dir + "logs"), ec);
  }

  // -------------------------------------------------------------------------
  // 3. 逐个执行
  // -------------------------------------------------------------------------
  const auto t_all = std::chrono::steady_clock::now();
  int done = 0;
  int failed = 0;
  int skipped = 0;

  for (size_t idx = 0; idx < jobs.size(); ++idx) {
    Job& j = jobs[idx];
    std::printf("[%zu/%zu] %s\n", idx + 1, jobs.size(), j.input.c_str());
    if (j.skipped) {
      ++skipped;
      std::printf("        跳过（输出已存在）\n");
      continue;
    }

    std::vector<std::string> args = {j.input, j.output};
    if (!config_path.empty()) {
      args.push_back("--config");
      args.push_back(config_path);
    }
    if (!preset_override.empty()) {
      args.push_back("--preset");
      args.push_back(preset_override);
    }
    if (crf_override >= 0) {
      args.push_back("--crf");
      args.push_back(std::to_string(crf_override));
    }
    if (limit > 0) {
      args.push_back("--limit");
      args.push_back(std::to_string(limit));
    }

    const auto t0 = std::chrono::steady_clock::now();
    // 超时给 12 小时：老剧单集导出慢但不会无限卡住；
    // 真卡死时超时能把它掐掉，不至于整晚占着 GPU。
    const lumen::ProcessResult r = lumen::RunProcess(render_exe, args, 12 * 3600 * 1000);
    j.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    j.ok = r.launched && r.exit_code == 0;

    // 每个任务的日志单独落盘：控制台只留一行结论，细节进文件
    {
      fs::path log_path = FromUtf8(exe_dir + "logs") /
                          (FromUtf8(j.output).stem().wstring() + L".log");
      std::ofstream log(log_path, std::ios::binary);
      if (log) {
        log << "input : " << j.input << "\n";
        log << "output: " << j.output << "\n";
        log << "exit  : " << r.exit_code << "\n";
        log << "time  : " << j.seconds << " s\n\n";
        log << r.output;
      }
    }

    if (j.ok) {
      ++done;
      std::printf("        完成，用时 %.1f s\n", j.seconds);
    } else {
      ++failed;
      j.note = r.launched ? ("退出码 " + std::to_string(r.exit_code)) : "无法启动 lumen-render";
      std::printf("        失败：%s（详见 logs/%s.log）\n", j.note.c_str(),
                  Utf8(FromUtf8(j.output).stem()).c_str());
    }
    std::fflush(stdout);
  }

  // -------------------------------------------------------------------------
  // 4. 汇总
  // -------------------------------------------------------------------------
  const double total =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t_all).count();
  std::printf("\n=========== 批量结果 ===========\n");
  std::printf("成功 %d  失败 %d  跳过 %d  总耗时 %.1f s\n", done, failed, skipped, total);

  // 报告落盘：方便事后核对哪几个文件没成
  {
    std::error_code ec;
    const fs::path report = FromUtf8(exe_dir + "batch_report.csv");
    std::ofstream csv(report, std::ios::binary);
    if (csv) {
      // UTF-8 BOM：Excel 打开中文 CSV 不加 BOM 会乱码
      csv << "\xEF\xBB\xBF";
      csv << "状态,源文件,输出文件,耗时秒,说明\n";
      for (const auto& j : jobs) {
        const char* st = j.skipped ? "跳过" : (j.ok ? "成功" : "失败");
        csv << st << "," << j.input << "," << j.output << "," << j.seconds << "," << j.note
            << "\n";
      }
      std::printf("报告：%s\n", Utf8(report).c_str());
    }
  }

  return failed == 0 ? 0 : 1;
}
