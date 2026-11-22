// ============================================================================
//  LumenPlayer - 单向流式管道
//  ---------------------------------------------------------------------------
//  所属层次：平台层
//  职责    ：把外部程序（ffmpeg）的 stdout 当输入流读，或把数据写进它的 stdin。
//
//  为什么是"单向"：
//    解码时我们只读 stdout，编码时我们只写 stdin。单向管道不需要
//    同时读写两个方向，也就不存在"双方都在等对方读"的经典死锁。
//    需要双向交互的场景（如果将来要实时控制 ffmpeg）另设接口，不在这里凑。
//
//  用途：离线导出管线
//    ffmpeg(解码) --stdout--> [本进程: 策略+超分] --stdin--> ffmpeg(编码)
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "core/types.h"

namespace lumen {

enum class PipeMode {
  Read,  // 读子进程 stdout
  Write, // 写子进程 stdin
};

class StreamPipe {
 public:
  StreamPipe() = default;
  ~StreamPipe();

  StreamPipe(const StreamPipe&) = delete;
  StreamPipe& operator=(const StreamPipe&) = delete;

  // 启动子进程并建立管道。
  // 参数说明：exe 为程序路径；args 为参数；mode 决定管道方向。
  // 返回值：Ok 成功；NotFound 表示启动失败（通常是路径错误）。
  //
  // 注意：子进程的另一个方向会被重定向到 NUL（Windows）或 /dev/null，
  //       避免 ffmpeg 的进度输出把管道写满而卡死。
  Status Open(const std::string& exe, const std::vector<std::string>& args, PipeMode mode);

  // 读数据。参数说明：bytes 为期望读取的最大字节数。
  // 返回值：实际字节数；0 表示流结束；负数表示错误。
  int Read(void* buffer, int bytes);

  // 写数据。
  // 返回值：实际写入字节数；负数表示错误（通常是子进程已退出）。
  int Write(const void* buffer, int bytes);

  // 关闭管道并等待子进程结束，返回其退出码（-1 表示未知）。
  int Close();

  bool IsOpen() const { return opened_; }

 private:
  bool opened_ = false;
  PipeMode mode_ = PipeMode::Read;

  // 用 void* 承载平台句柄，避免头文件里泄漏 windows.h。
  void* handle_ = nullptr;  // 父进程持有的管道端点
  void* process_ = nullptr; // 子进程句柄
};

} // namespace lumen
