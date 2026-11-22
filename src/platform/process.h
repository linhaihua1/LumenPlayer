// ============================================================================
//  LumenPlayer - 子进程执行（平台抽象）
//  ---------------------------------------------------------------------------
//  所属层次：平台层
//  职责    ：以子进程方式调用 ffprobe / ffmpeg，捕获其输出。
//
//  为什么用子进程而不是链接 libav*：
//    1. 许可：ffprobe/ffmpeg 以独立可执行程序方式调用属于"聚合"，
//       LGPL 义务不会传染到本产品（详见 06 文档许可红线）；
//    2. 稳定性：解码崩溃在子进程里，不会带走播放器主进程；
//    3. 一期只做"评估/导出"，不需要零拷贝解码，子进程的额外开销可接受。
//       实时播放路径将来走 libmpv，不用这里的接口。
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "core/types.h"

namespace lumen {

struct ProcessResult {
  int exit_code = -1;
  std::string output;      // stdout 与 stderr 合并后的文本
  bool timed_out = false;
  bool launched = false;   // 是否成功启动（区分"程序不存在"与"程序报错"）
};

// 执行外部程序并等待结束。
// 参数说明：
//   exe        —— 可执行文件路径（支持 UTF-8 中文路径）
//   args       —— 参数列表（不含程序名本身）
//   timeout_ms —— 超时毫秒数；超时会强制结束进程
// 返回值：ProcessResult。launched=false 表示找不到程序或启动失败。
//
// 注意：stdout 与 stderr 被合并到同一管道。这样实现只需读一路，
//       且 ffprobe 的报告都走 stdout、错误走 stderr，合并后不会丢信息。
ProcessResult RunProcess(const std::string& exe, const std::vector<std::string>& args,
                         int timeout_ms = 120000);

// 在 PATH 中查找可执行文件。
// 参数说明：name 为程序名（可带 .exe）；返回空串表示未找到。
// 用途：ffmpeg 可能装在任意目录，先查 PATH 再查配置里的显式路径。
std::string FindInPath(const std::string& name);

// ---------------------------------------------------------------------------
// 异步启动（不等待结束）
// ---------------------------------------------------------------------------
// 用途：播放器要启动 mpv.exe 并让它长期运行，不能阻塞在等待上。
// 与 RunProcess 的区别：这里不接管 stdout/stderr，也不等待退出。
struct SpawnResult {
  bool launched = false;
  unsigned long pid = 0;  // 进程 ID
  void* handle = nullptr; // 进程句柄（调用方用 CloseSpawned 关闭）
};

// 启动子进程。参数说明同 RunProcess。
SpawnResult SpawnProcess(const std::string& exe, const std::vector<std::string>& args);

// 关闭句柄但不结束进程（应用退出时用，避免留下僵尸句柄）
void CloseSpawned(SpawnResult* p);

// 进程是否仍在运行
bool ProcessAlive(const SpawnResult& p);

// 强制结束进程并回收句柄
void KillProcess(SpawnResult* p);

// 拼接命令行字符串（含正确的引号转义）。
// 参数说明：exe 为程序路径，args 为参数列表。
// 用途：供流式管道等需要自行调用 CreateProcess 的模块复用同一套转义逻辑，
//       避免两处实现不一致导致"同样的参数在两条路径上行为不同"。
std::string BuildCommandLine(const std::string& exe, const std::vector<std::string>& args);

} // namespace lumen
