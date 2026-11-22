// ============================================================================
//  LumenPlayer - 单向流式管道实现
//  所属层次：平台层
// ============================================================================
#include "platform/stream_pipe.h"

#include <cstdio>
#include <string>

#include "platform/process.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace lumen {
namespace {

#ifdef _WIN32
std::wstring ToWide(const std::string& s) {
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

StreamPipe::~StreamPipe() { Close(); }

Status StreamPipe::Open(const std::string& exe, const std::vector<std::string>& args,
                        PipeMode mode) {
  if (opened_) Close();
  mode_ = mode;

  const std::string cmdline = BuildCommandLine(exe, args);

#ifdef _WIN32
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE pipe_read = nullptr;
  HANDLE pipe_write = nullptr;
  if (!CreatePipe(&pipe_read, &pipe_write, &sa, 1 << 20)) return Status::IoError;

  // NUL 用于填掉不需要的那一侧。不填会给子进程继承父进程的标准句柄，
  // 在无控制台的进程里表现为 ffmpeg 输出丢失或直接失败。
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0,
                           nullptr);

  HANDLE child_in = nullptr;
  HANDLE child_out = nullptr;

  // 注意这里的顺序：父进程必须保留另一端的副本直到 CreateProcess 完成，
  // 否则子进程拿到的是一个"看起来有效但已被关闭"的句柄，
  // 表现为 ffmpeg 静默退出、管道读到 0 字节——极难排查的一类 bug。
  if (mode == PipeMode::Read) {
    // 父进程读，子进程写
    SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0);
    child_out = pipe_write;
    child_in = nul;
    handle_ = pipe_read;
  } else {
    // 父进程写，子进程读
    SetHandleInformation(pipe_write, HANDLE_FLAG_INHERIT, 0);
    child_in = pipe_read;
    child_out = nul;
    handle_ = pipe_write;
  }

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = child_in;
  si.hStdOutput = child_out;
  si.hStdError = nul; // 子进程的错误输出丢弃；致命错误靠退出码判断

  PROCESS_INFORMATION pi{};
  std::wstring wcmd = ToWide(cmdline);
  std::vector<wchar_t> buf(wcmd.begin(), wcmd.end());
  buf.push_back(L'\0');

  const BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

  // 子进程已持有自己的副本，父进程这时才释放另一端。
  // 不释放写端会导致读端永远等不到 EOF；不释放读端会导致写端永远等不到断开。
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (mode == PipeMode::Read) {
    CloseHandle(pipe_write);
  } else {
    CloseHandle(pipe_read);
  }

  if (!ok) {
    if (handle_ != nullptr) CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
    return Status::NotFound;
  }

  process_ = pi.hProcess;
  CloseHandle(pi.hThread);
  opened_ = true;
  return Status::Ok;

#else
  // POSIX：用 popen 简化实现（Android / HarmonyOS 阶段会替换为 posix_spawn）
  FILE* fp = popen(cmdline.c_str(), mode == PipeMode::Read ? "r" : "w");
  if (fp == nullptr) return Status::NotFound;
  handle_ = fp;
  opened_ = true;
  return Status::Ok;
#endif
}

int StreamPipe::Read(void* buffer, int bytes) {
  if (!opened_ || mode_ != PipeMode::Read || buffer == nullptr || bytes <= 0) return -1;

#ifdef _WIN32
  DWORD read = 0;
  if (!ReadFile(static_cast<HANDLE>(handle_), buffer, static_cast<DWORD>(bytes), &read,
                nullptr)) {
    // 管道断开是正常的流结束（子进程退出并关闭写端）
    return (GetLastError() == ERROR_BROKEN_PIPE) ? 0 : -1;
  }
  return static_cast<int>(read);
#else
  const size_t n = fread(buffer, 1, static_cast<size_t>(bytes), static_cast<FILE*>(handle_));
  return static_cast<int>(n);
#endif
}

int StreamPipe::Write(const void* buffer, int bytes) {
  if (!opened_ || mode_ != PipeMode::Write || buffer == nullptr || bytes <= 0) return -1;

#ifdef _WIN32
  DWORD written = 0;
  if (!WriteFile(static_cast<HANDLE>(handle_), buffer, static_cast<DWORD>(bytes), &written,
                 nullptr)) {
    return -1;
  }
  return static_cast<int>(written);
#else
  const size_t n = fwrite(buffer, 1, static_cast<size_t>(bytes), static_cast<FILE*>(handle_));
  fflush(static_cast<FILE*>(handle_));
  return static_cast<int>(n);
#endif
}

int StreamPipe::Close() {
  if (!opened_) return -1;
  opened_ = false;

#ifdef _WIN32
  int code = -1;
  if (handle_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
  if (process_ != nullptr) {
    // 关闭管道后子进程会读到 EOF 并正常收尾，这里等它写完尾部数据
    WaitForSingleObject(static_cast<HANDLE>(process_), 60000);
    DWORD exit_code = 0;
    if (GetExitCodeProcess(static_cast<HANDLE>(process_), &exit_code)) {
      code = static_cast<int>(exit_code);
    }
    CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
  }
  return code;
#else
  FILE* fp = static_cast<FILE*>(handle_);
  handle_ = nullptr;
  if (fp == nullptr) return -1;
  const int status = pclose(fp);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

} // namespace lumen
