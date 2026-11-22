// ============================================================================
//  LumenPlayer - mpv JSON IPC 客户端
//  ---------------------------------------------------------------------------
//  所属层次：平台层
//  职责    ：通过命名管道与独立的 mpv.exe 进程通信：
//            发命令（loadfile / vf / set_property）、读属性、收事件。
//
//  ★ 为什么走 IPC 而不是链接 libmpv：
//    1. 许可：mpv 是 GPLv2+，libmpv 是 LGPL。链接 libmpv 时我们受 LGPL 约束
//       （可闭源，但要允许替换该库）；链接 mpv 的 GPL 部分则必须整程序开源。
//       以独立进程 + 通用 IPC 的方式调用，两个进程各自独立授权，
//       我们的 UI 与策略层可以闭源。这是 ADR-001 里"靠进程边界隔离许可"的落地。
//    2. 稳定：解码崩溃、显卡驱动重置都发生在 mpv 进程里，UI 不会跟着挂。
//    3. 省事：不用自己搭渲染管线，mpv 的渲染器、字幕、音画同步都是现成的。
//    代价：跨进程序列化有开销（命令是控制流，不是逐帧数据，实际可忽略）。
//
//  ★ 线程模型（重要）：
//    本类是"单线程轮询"实现——所有调用都在 UI 线程完成，没有后台线程。
//    播放器崩溃里最难查的就是跨线程回调（在渲染线程里碰 UI 对象），
//    所以这里刻意不用回调线程，改用 PeekNamedPipe + 短超时轮询。
//    命令都是用户触发的（打开文件、切滤镜），延迟 1—3ms 完全可以接受。
// ============================================================================
#pragma once

#include <deque>
#include <string>
#include <vector>

#include "core/json.h"
#include "core/types.h"

namespace lumen {

class MpvIpcClient {
 public:
  MpvIpcClient();
  ~MpvIpcClient();

  MpvIpcClient(const MpvIpcClient&) = delete;
  MpvIpcClient& operator=(const MpvIpcClient&) = delete;

  // 连接命名管道。
  // 参数说明：pipe_name 只要管道名（如 "lumen_1234"），不含 \\.\pipe\ 前缀；
  //           timeout_ms 为最长等待时间——mpv 启动后才创建管道，必须重试。
  Status Connect(const std::string& pipe_name, int timeout_ms = 8000);
  void Close();
  bool connected() const { return connected_; }

  // 最近一次失败的原因（中文）
  const std::string& last_error() const { return last_error_; }

  // 发送命令并等待回复。
  // 参数说明：args 为 mpv 命令数组，如 {"set_property","pause","yes"}。
  // 返回值  ：Ok 表示 mpv 回 error == "success"；
  //           *err 非空时收到 mpv 的错误字符串（如 "property unavailable"）。
  // 注意：mpv 的"属性暂不可用"（还没加载文件时查 duration）是常态，不是故障，
  //       调用方应把它当成"取不到值"而不是"出错了"。
  Status Command(const std::vector<std::string>& args, std::string* err = nullptr,
                 int timeout_ms = 3000);

  // 属性读取。取值失败（属性不存在 / 尚未可用）返回 false，不修改 *out。
  bool GetString(const std::string& name, std::string* out, int timeout_ms = 2000);
  bool GetBool(const std::string& name, bool* out, int timeout_ms = 2000);
  bool GetDouble(const std::string& name, double* out, int timeout_ms = 2000);

  // 以"字符串形式"读取属性。
  // 用途：像 vf 这种属性是"列表"类型，get_property 会返回 JSON 数组，
  // 用 GetString 取不到；get_property_string 会给出 mpv 内部的字符串表示，
  // 正好适合做"回读校验"（确认滤镜真的挂上了）。
  bool GetPropertyString(const std::string& name, std::string* out, int timeout_ms = 2000);

  // 设置属性。value 用字符串形式传递，mpv 自己解析（"yes"/"no"/数字）。
  Status SetProperty(const std::string& name, const std::string& value,
                     std::string* err = nullptr);

  // 取出一个待处理事件（如 "file-loaded"、"end-file"、"property-change"）。
  // 没有事件时返回 false。UI 定时器里循环调用直到返回 false。
  bool PollEvent(JsonValue* out);

 private:
  bool WriteCommand(const std::string& json);
  // 读走管道里当前可用的全部数据，切成行；回复按 request_id 匹配，其余进事件队列。
  void Pump();
  Status AwaitReply(int req_id, std::string* err, int timeout_ms);
  bool ResolveOne(const std::vector<std::string>& args, JsonValue* reply, std::string* err,
                  int timeout_ms);

  void* pipe_ = nullptr; // HANDLE
  bool connected_ = false;
  std::string last_error_;
  std::string buffer_;             // 未成行的残留数据
  std::deque<JsonValue> replies_;  // 已到达但还没被匹配的回复（按 request_id 取）
  std::deque<JsonValue> events_;   // 已解析但还没被取走的事件
  int next_request_id_ = 1;
  bool closed_by_peer_ = false;
};

} // namespace lumen
