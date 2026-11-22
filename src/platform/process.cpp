// ============================================================================
//  LumenPlayer - 子进程执行实现
//  所属层次：平台层
// ============================================================================
#include "process.h"

#include <cstdio>
#include <cstring>
#include <string>

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

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                    nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
  return w;
}

std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                    nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n,
                      nullptr, nullptr);
  return s;
}

// 按 Windows C 运行库的规则给参数加引号。
// 这一步不能省：中文路径、含空格的目录名在未加引号时会被拆成多个参数，
// ffprobe 会报"文件不存在"，而这种错误极难从报错信息看出来。
std::wstring QuoteArg(const std::wstring& arg) {
  if (arg.empty()) return L"\"\"";
  if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;

  std::wstring out;
  out.push_back(L'"');
  size_t i = 0;
  while (i < arg.size()) {
    size_t backslashes = 0;
    while (i < arg.size() && arg[i] == L'\\') {
      ++i;
      ++backslashes;
    }
    if (i == arg.size()) {
      // 结尾的反斜杠要加倍，否则会转义掉收尾引号
      out.append(backslashes * 2, L'\\');
      break;
    }
    if (arg[i] == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
    } else {
      out.append(backslashes, L'\\');
      out.push_back(arg[i]);
    }
    ++i;
  }
  out.push_back(L'"');
  return out;
}

#endif // _WIN32

} // namespace

std::string FindInPath(const std::string& name) {
  if (name.empty()) return std::string();

  // 已带路径分隔符的直接判断存在性
  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
#ifdef _WIN32
    const DWORD attr = GetFileAttributesW(Utf8ToWide(name).c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) ? name : std::string();
#else
    return (access(name.c_str(), X_OK) == 0) ? name : std::string();
#endif
  }

#ifdef _WIN32
  const wchar_t* path = _wgetenv(L"PATH");
  if (!path) return std::string();
  std::wstring paths(path);
  size_t begin = 0;
  while (begin <= paths.size()) {
    size_t semi = paths.find(L';', begin);
    if (semi == std::wstring::npos) semi = paths.size();
    std::wstring dir = paths.substr(begin, semi - begin);
    if (!dir.empty()) {
      std::wstring full = dir + L"\\" + Utf8ToWide(name);
      if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES) {
        // 转回 UTF-8 返回
        const int n = WideCharToMultiByte(CP_UTF8, 0, full.c_str(),
                                          static_cast<int>(full.size()), nullptr, 0,
                                          nullptr, nullptr);
        std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
        if (n > 0) {
          WideCharToMultiByte(CP_UTF8, 0, full.c_str(), static_cast<int>(full.size()),
                              &s[0], n, nullptr, nullptr);
        }
        return s;
      }
    }
    if (semi >= paths.size()) break;
    begin = semi + 1;
  }
  return std::string();
#else
  const char* path = getenv("PATH");
  if (!path) return std::string();
  std::string paths(path);
  size_t begin = 0;
  while (begin <= paths.size()) {
    size_t colon = paths.find(':', begin);
    if (colon == std::string::npos) colon = paths.size();
    std::string dir = paths.substr(begin, colon - begin);
    if (!dir.empty()) {
      std::string full = dir + "/" + name;
      if (access(full.c_str(), X_OK) == 0) return full;
    }
    if (colon >= paths.size()) break;
    begin = colon + 1;
  }
  return std::string();
#endif
}

ProcessResult RunProcess(const std::string& exe, const std::vector<std::string>& args,
                         int timeout_ms) {
  ProcessResult result;

#ifdef _WIN32
  // -------------------------------------------------------------------------
  // 构造命令行
  // -------------------------------------------------------------------------
  std::wstring cmd = QuoteArg(Utf8ToWide(exe));
  for (const auto& a : args) {
    cmd.push_back(L' ');
    cmd += QuoteArg(Utf8ToWide(a));
  }

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return result;
  // 读端不继承，避免子进程持有它导致 EOF 永远等不到
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  // stdin 指向 NUL：ffmpeg 会尝试读 stdin 做交互控制，
  // 若不给有效句柄它可能阻塞在读取上，表现为"命令卡死"。
  HANDLE nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa,
                              OPEN_EXISTING, 0, nullptr);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = write_pipe;
  si.hStdError = write_pipe; // stderr 并入 stdout，只需读一路
  si.hStdInput = nul_in;

  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
  cmd_buf.push_back(L'\0');

  // CREATE_NO_WINDOW：否则每次调用 ffmpeg 都会闪一个黑框，
  // 在 GUI 播放器里观感极差。
  const BOOL ok = CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  if (nul_in != INVALID_HANDLE_VALUE) CloseHandle(nul_in);

  if (!ok) {
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    return result; // launched 保持 false
  }
  result.launched = true;

  // 父进程必须关掉写端，否则子进程退出后管道也不会关闭
  CloseHandle(write_pipe);

  // -------------------------------------------------------------------------
  // 读输出（轮询 + 超时）
  // -------------------------------------------------------------------------
  const ULONGLONG start_tick = GetTickCount64();
  bool child_done = false;
  char buffer[8192];

  while (true) {
    DWORD avail = 0;
    if (!PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
      // 管道断开：子进程已退出并关闭写端
      child_done = true;
      break;
    }
    if (avail > 0) {
      DWORD read = 0;
      DWORD to_read = avail < sizeof(buffer) ? avail : static_cast<DWORD>(sizeof(buffer));
      if (ReadFile(read_pipe, buffer, to_read, &read, nullptr) && read > 0) {
        result.output.append(buffer, read);
        continue;
      }
    }

    const DWORD w = WaitForSingleObject(pi.hProcess, 20);
    if (w == WAIT_OBJECT_0) {
      child_done = true;
      break;
    }
    if (GetTickCount64() - start_tick > static_cast<ULONGLONG>(timeout_ms)) {
      TerminateProcess(pi.hProcess, 1);
      result.timed_out = true;
      WaitForSingleObject(pi.hProcess, 2000);
      child_done = true;
      break;
    }
  }

  // 收尾：把管道里剩余数据读完（子进程刚退出时缓冲里可能还有）
  for (;;) {
    DWORD avail = 0;
    if (!PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &avail, nullptr)) break;
    if (avail == 0) break;
    DWORD read = 0;
    DWORD to_read = avail < sizeof(buffer) ? avail : static_cast<DWORD>(sizeof(buffer));
    if (!ReadFile(read_pipe, buffer, to_read, &read, nullptr) || read == 0) break;
    result.output.append(buffer, read);
  }

  DWORD code = 0;
  if (child_done) GetExitCodeProcess(pi.hProcess, &code);
  result.exit_code = static_cast<int>(code);

  CloseHandle(read_pipe);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return result;

#else
  // POSIX 实现（Android / HarmonyOS 后续使用）。
  // 这里用 popen 简化：把 stderr 重定向进 stdout，命令整体交给 shell。
  std::string cmd = "'" + exe + "'";
  for (const auto& a : args) {
    cmd += " '";
    for (char c : a) {
      if (c == '\'') cmd += "'\\''";
      else cmd += c;
    }
    cmd += "'";
  }
  cmd += " 2>&1";

  FILE* fp = popen(cmd.c_str(), "r");
  if (!fp) return result;
  result.launched = true;

  char buffer[8192];
  while (fgets(buffer, sizeof(buffer), fp) != nullptr) {
    result.output.append(buffer);
  }
  const int status = pclose(fp);
  if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
  return result;
#endif
}

SpawnResult SpawnProcess(const std::string& exe, const std::vector<std::string>& args) {
  SpawnResult r;
#ifdef _WIN32
  std::wstring cmd = QuoteArg(Utf8ToWide(exe));
  for (const auto& a : args) {
    cmd.push_back(L' ');
    cmd += QuoteArg(Utf8ToWide(a));
  }
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  // CREATE_NO_WINDOW：mpv 自带窗口，不需要控制台黑框
  const BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  if (!ok) return r;
  r.launched = true;
  r.pid = static_cast<unsigned long>(pi.dwProcessId);
  r.handle = pi.hProcess;
  CloseHandle(pi.hThread);
#else
  // 非 Windows 平台留待 Android/HarmonyOS 阶段实现（那边的进程模型完全不同，
  // 播放器会直接链接 libmpv，不再需要外部进程）。
  (void)exe;
  (void)args;
#endif
  return r;
}

void CloseSpawned(SpawnResult* p) {
  if (!p || !p->handle) return;
#ifdef _WIN32
  CloseHandle(static_cast<HANDLE>(p->handle));
#endif
  p->handle = nullptr;
}

bool ProcessAlive(const SpawnResult& p) {
#ifdef _WIN32
  if (p.handle == nullptr) return false;
  DWORD code = 0;
  if (!GetExitCodeProcess(static_cast<HANDLE>(p.handle), &code)) return false;
  return code == STILL_ACTIVE;
#else
  (void)p;
  return false;
#endif
}

void KillProcess(SpawnResult* p) {
#ifdef _WIN32
  if (!p || p->handle == nullptr) return;
  TerminateProcess(static_cast<HANDLE>(p->handle), 0);
  WaitForSingleObject(static_cast<HANDLE>(p->handle), 2000);
  CloseSpawned(p);
#endif
  if (p) p->launched = false;
}

std::string BuildCommandLine(const std::string& exe, const std::vector<std::string>& args) {
#ifdef _WIN32
  std::wstring cmd = QuoteArg(Utf8ToWide(exe));
  for (const auto& a : args) {
    cmd.push_back(L' ');
    cmd += QuoteArg(Utf8ToWide(a));
  }
  return WideToUtf8(cmd);
#else
  std::string cmd = "'" + exe + "'";
  for (const auto& a : args) {
    cmd += " '";
    for (char c : a) {
      if (c == '\'') cmd += "'\\''";
      else cmd += c;
    }
    cmd += "'";
  }
  return cmd;
#endif
}

} // namespace lumen
