// ============================================================================
//  LumenPlayer - mpv JSON IPC 客户端实现
//  所属层次：平台层
// ============================================================================
#include "platform/mpv_ipc.h"

#include <algorithm>
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
#include <unistd.h>
#endif

namespace lumen {
namespace {

// 最小 JSON 字符串转义。mpv 的命令里会出现 Windows 路径（反斜杠极多），
// 不转义会直接产出非法 JSON，表现为"命令发出去没有反应"。
std::string Escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        // 控制字符必须转成 \u00XX，否则 JSON 非法
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

std::string BuildCommand(const std::vector<std::string>& args, int request_id) {
  std::string s = "{\"command\":[";
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) s += ",";
    s += "\"" + Escape(args[i]) + "\"";
  }
  s += "],\"request_id\":" + std::to_string(request_id) + "}\n";
  return s;
}

} // namespace

MpvIpcClient::MpvIpcClient() = default;

MpvIpcClient::~MpvIpcClient() { Close(); }

Status MpvIpcClient::Connect(const std::string& pipe_name, int timeout_ms) {
  Close();
  if (pipe_name.empty()) {
    last_error_ = "管道名为空";
    return Status::InvalidArgument;
  }

#ifdef _WIN32
  const std::string full = "\\\\.\\pipe\\" + pipe_name;
  const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
  for (;;) {
    HANDLE h = CreateFileA(full.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      pipe_ = h;
      connected_ = true;
      buffer_.clear();
      events_.clear();
      closed_by_peer_ = false;
      return Status::Ok;
    }
    // ERROR_PIPE_BUSY：mpv 已创建管道但实例都占用中；ERROR_FILE_NOT_FOUND：还没建好。
    // 两者都只能等——这也是这里必须重试而不是直接失败的原因。
    if (GetTickCount64() >= deadline) {
      last_error_ = "无法连接到 mpv 的 IPC 管道：" + full +
                    "（mpv 可能没有启动，或 --input-ipc-server 参数没生效）";
      return Status::Timeout;
    }
    Sleep(50);
  }
#else
  (void)pipe_name;
  (void)timeout_ms;
  last_error_ = "当前平台未实现 mpv IPC（Windows 专用）";
  return Status::Unsupported;
#endif
}

void MpvIpcClient::Close() {
#ifdef _WIN32
  if (pipe_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(pipe_));
    pipe_ = nullptr;
  }
#endif
  connected_ = false;
  buffer_.clear();
  events_.clear();
  closed_by_peer_ = false;
}

bool MpvIpcClient::WriteCommand(const std::string& json) {
#ifdef _WIN32
  if (!connected_) return false;
  DWORD written = 0;
  if (!WriteFile(static_cast<HANDLE>(pipe_), json.data(),
                 static_cast<DWORD>(json.size()), &written, nullptr)) {
    last_error_ = "写入 mpv 管道失败";
    closed_by_peer_ = true;
    return false;
  }
  return written == json.size();
#else
  (void)json;
  return false;
#endif
}

void MpvIpcClient::Pump() {
#ifdef _WIN32
  if (!connected_) return;
  HANDLE h = static_cast<HANDLE>(pipe_);

  // 把管道里当前所有可用数据读干净。不能用阻塞 ReadFile：
  // 那会让 UI 线程卡在等 mpv 回复上，表现为"点一下界面就假死"。
  for (;;) {
    DWORD avail = 0;
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
      closed_by_peer_ = true;
      return;
    }
    if (avail == 0) break;

    char buf[4096];
    const DWORD want = avail > sizeof(buf) ? static_cast<DWORD>(sizeof(buf)) : avail;
    DWORD got = 0;
    if (!ReadFile(h, buf, want, &got, nullptr) || got == 0) {
      closed_by_peer_ = true;
      return;
    }
    buffer_.append(buf, got);
  }

  // 按行切分。mpv 的输出是"一行一个 JSON 对象"，这是它的协议约定。
  size_t pos = 0;
  while ((pos = buffer_.find('\n')) != std::string::npos) {
    std::string line = buffer_.substr(0, pos);
    buffer_.erase(0, pos + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    std::string perr;
    const JsonValue v = JsonValue::Parse(line, &perr);
    if (!v.IsObject()) continue;
    if (v["request_id"].IsNumber()) {
      replies_.push_back(v);
    } else {
      events_.push_back(v);
    }
  }
#endif
}

Status MpvIpcClient::AwaitReply(int req_id, std::string* err, int timeout_ms) {
#ifdef _WIN32
  const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
  for (;;) {
    Pump();
    for (auto it = replies_.begin(); it != replies_.end(); ++it) {
      if (static_cast<int>((*it)["request_id"].AsInt(-1)) == req_id) {
        const std::string e = (*it)["error"].AsString("success");
        replies_.erase(it);
        if (e != "success") {
          if (err) *err = e;
          return Status::IoError;
        }
        return Status::Ok;
      }
    }
    if (closed_by_peer_) {
      last_error_ = "mpv 进程已退出或管道已断开";
      return Status::IoError;
    }
    if (GetTickCount64() >= deadline) {
      last_error_ = "等待 mpv 回复超时";
      return Status::Timeout;
    }
    Sleep(1); // 1ms 轮询：既不让出太多 CPU，也不会让交互有可感知延迟
  }
#else
  (void)req_id;
  (void)err;
  (void)timeout_ms;
  return Status::Unsupported;
#endif
}

bool MpvIpcClient::ResolveOne(const std::vector<std::string>& args, JsonValue* reply,
                              std::string* err, int timeout_ms) {
  const int id = next_request_id_++;
  if (!WriteCommand(BuildCommand(args, id))) {
    if (err) *err = last_error_;
    return false;
  }
  const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
  for (;;) {
    Pump();
    for (auto it = replies_.begin(); it != replies_.end(); ++it) {
      if (static_cast<int>((*it)["request_id"].AsInt(-1)) == id) {
        const JsonValue copy = *it;
        replies_.erase(it);
        const std::string e = copy["error"].AsString("success");
        if (reply) *reply = copy;
        if (e != "success") {
          if (err) *err = e;
          return false;
        }
        return true;
      }
    }
    if (closed_by_peer_) {
      last_error_ = "mpv 进程已退出或管道已断开";
      if (err) *err = last_error_;
      return false;
    }
    if (GetTickCount64() >= deadline) {
      last_error_ = "等待 mpv 回复超时";
      if (err) *err = last_error_;
      return false;
    }
    Sleep(1);
  }
}

Status MpvIpcClient::Command(const std::vector<std::string>& args, std::string* err,
                             int timeout_ms) {
  if (!connected_) {
    last_error_ = "未连接 mpv";
    return Status::IoError;
  }
  const int id = next_request_id_++;
  if (!WriteCommand(BuildCommand(args, id))) return Status::IoError;
  return AwaitReply(id, err, timeout_ms);
}

bool MpvIpcClient::GetString(const std::string& name, std::string* out, int timeout_ms) {
  JsonValue reply;
  if (!ResolveOne({"get_property", name}, &reply, nullptr, timeout_ms)) return false;
  const JsonValue& d = reply["data"];
  if (d.IsString()) {
    if (out) *out = d.AsString();
    return true;
  }
  return false;
}

bool MpvIpcClient::GetBool(const std::string& name, bool* out, int timeout_ms) {
  JsonValue reply;
  if (!ResolveOne({"get_property", name}, &reply, nullptr, timeout_ms)) return false;
  const JsonValue& d = reply["data"];
  if (d.IsBool()) {
    if (out) *out = d.AsBool();
    return true;
  }
  return false;
}

bool MpvIpcClient::GetDouble(const std::string& name, double* out, int timeout_ms) {
  JsonValue reply;
  if (!ResolveOne({"get_property", name}, &reply, nullptr, timeout_ms)) return false;
  const JsonValue& d = reply["data"];
  if (d.IsNumber()) {
    if (out) *out = d.AsDouble();
    return true;
  }
  return false;
}

bool MpvIpcClient::GetPropertyString(const std::string& name, std::string* out,
                                    int timeout_ms) {
  JsonValue reply;
  if (!ResolveOne({"get_property_string", name}, &reply, nullptr, timeout_ms)) return false;
  const JsonValue& d = reply["data"];
  if (d.IsString()) {
    if (out) *out = d.AsString();
    return true;
  }
  return false;
}

Status MpvIpcClient::SetProperty(const std::string& name, const std::string& value,
                                 std::string* err) {
  return Command({"set_property", name, value}, err);
}

bool MpvIpcClient::PollEvent(JsonValue* out) {
  Pump();
  if (events_.empty()) return false;
  if (out) *out = events_.front();
  events_.pop_front();
  return true;
}

} // namespace lumen
