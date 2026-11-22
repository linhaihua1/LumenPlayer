// ============================================================================
//  LumenPlayer - 播放器外壳（Windows）
//  ---------------------------------------------------------------------------
//  所属层次：应用层
//
//  架构：本程序是 UI 外壳，真正的解码/渲染跑在独立的 mpv.exe 里。
//        mpv 通过 --wid 把画面渲染进我们的窗口，通过命名管道接收命令。
//
//  ★ 为什么这样做（而不是链接 libmpv）：
//    许可。mpv 的 GPL 只在"分发"时要求整程序开源；以独立进程 + 通用 IPC
//    的方式协作，两个程序各自保持自己的许可。我们的策略层与 UI 因此可以闭源。
//    详见 ADR-001 与 06 文档的许可红线。
//
//  ★ 当前能力边界（必须诚实，否则用户会误判产品）：
//    已实现：片源自检 → 按结论自动做场处理（反交错 / IVTC）、去块、降噪，
//            A/B 即时对比（按 Tab 看原片）、性能 HUD、播放控制、进度记忆、
//            场序人工覆盖。
//    未实现：AI 超分。播放路径的 GPU 超分要等 libaji 接入（其仓库当前无 LICENSE，
//            商用需作者书面授权）。所以现在播放的是"正确处理过的原分辨率"，
//            不是 1080p。离线导出（lumen-render）走 ONNX，是真正的 1080p。
//
//  用法：
//    lumen-player.exe                     打开空白窗口，拖入文件
//    lumen-player.exe 影片.mkv            直接播放
//    lumen-player.exe --self-test 影片.mkv  无窗口自检（诊断用，输出到控制台）
// ============================================================================
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h> // GET_X_LPARAM / GET_Y_LPARAM（鼠标坐标必须这样拆，不能用 LOWORD）

#include "core/config.h"
#include "core/processing_chain.h"
#include "platform/mpv_ipc.h"
#include "platform/process.h"
#include "player_ui.h"
#include "service/inspector.h"

namespace {

constexpr const wchar_t* kClassName = L"LumenPlayerMainWindow";
constexpr const wchar_t* kHudClass = L"LumenPlayerHudWindow";
constexpr const wchar_t* kWindowTitle = L"LumenPlayer";
constexpr int kMinWidth = 900;
constexpr int kMinHeight = 560;

// 自定义消息
constexpr UINT WM_APP_INSPECT_DONE = WM_APP + 1;
// 界面刷新定时器：250ms 一档。进度条要跟着走，500ms 在长片里看起来一顿一顿的；
// 再快（60ms）就变成每秒十几次 IPC 往返，收益不抵开销。
constexpr UINT_PTR kUiTimerId = 1;
constexpr int kUiIntervalMs = 250;
// 单击视频区"暂停"的延迟定时器：用来区分单击与双击（双击要全屏，不能顺带暂停）。
constexpr UINT_PTR kClickTimerId = 2;
// 布局夺回定时器：见 StartVideoRelayout() 的说明。
// mpv 在**自己的线程**里轮询父窗口客户区，发现尺寸变化后会把 vo 子窗口
// 拉回整块客户区，覆盖我们设的视频区。这个重写发生在我们 WM_SIZE 之后的
// 某个不确定的时刻，只能用短定时器延时把视频区重新夺回来。
constexpr UINT_PTR kRelayoutTimerId = 3;
constexpr UINT kRelayoutIntervalMs = 80;
constexpr int kRelayoutRetries = 6; // 80ms × 6 = 480ms 的兜底窗口

// HUD 窗口初始尺寸（逻辑像素）。真实尺寸由文字量决定，这里只是创建时的占位。
constexpr int kHudWidth = 520;
constexpr int kHudHeight = 128;

// ---------------------------------------------------------------------------
// 实时统计
// ---------------------------------------------------------------------------
// 每 250ms 从 mpv 采一次。集中存放的理由：HUD、侧栏、控制条三处都要用，
// 各读各的会让"画面显示的时长"和"进度条用的时长"出现半秒级的不一致。
struct LiveStats {
  double pos = 0.0, dur = 0.0;
  double fps = 0.0, speed = 1.0;
  double volume = 100.0;
  double drops = 0.0, dec_drops = 0.0;
  double vw = 0.0, vh = 0.0, aspect = 0.0;
  double bitrate = 0.0;
  bool paused = false, mute = false, eof = false;
  std::string pixfmt, hwdec, vcodec, acodec, title;
};

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
// 说明：Win32 的窗口过程是 C 函数，没有 this 指针。进程内只有主窗口一个实例，
// 所以用单例持有状态；这也是 Win32 程序最常见的做法（比把指针塞进 GWLP_USERDATA
// 再到处取更直观）。
struct PlayerState {
  HINSTANCE inst = nullptr;
  HWND hwnd = nullptr;
  HWND video = nullptr; // mpv 创建的子窗口

  lumen::MpvIpcClient ipc;
  lumen::SpawnResult mpv;
  lumen::AppConfig cfg;
  std::string exe_dir;

  std::string current_file;
  lumen::ProcessingChain chain;      // 实际生效的处理链（可能被人工覆盖过）
  lumen::ProcessingChain base_chain; // 自检原始结论，用于"覆盖后还能退回"
  lumen::InspectionReport report;    // 保留完整报告，供信息面板与后续功能使用
  bool has_analysis = false;
  bool processed = true; // A/B 开关：true = 处理后，false = 原片
  bool ab_hold = false;  // 是否处于"按住 Tab 看原片"状态
  bool ab_prev = true;   // 按住 Tab 之前的处理状态（抬起时恢复到这个值）
  bool fullscreen = false;
  WINDOWPLACEMENT saved_place{};
  bool has_saved_place = false;

  // --- 自绘界面 ---
  lumen::ui::Layout layout;
  lumen::ui::MediaInfo info;
  lumen::ui::PlayState ui;
  bool sidebar_on = true;
  bool video_subclassed = false;
  WNDPROC video_proc = nullptr;
  bool pending_video_click = false; // 等待确认"是单击还是双击"
  int relayout_retries = 0;         // 布局夺回定时器的剩余次数
  int output_height = 0;            // 输出分辨率（0 = 原生）
  int aspect_mode = 0;              // 0 适应 1 拉伸 2 裁剪 3 原始像素
  bool has_sized_window = false;    // 是否按过"窗口尺寸"

  // --- 性能 HUD（F-401）---
  HWND hud = nullptr;
  bool hud_on = true;         // 用户可关（配置 player.hud 或 I 键）
  std::wstring hud_lines[5];  // 预先排好的显示行
  int hud_line_count = 0;

  // --- 场序人工覆盖（F-202 补充）---
  lumen::FieldOrder field_override = lumen::FieldOrder::Unknown; // Unknown = 自动
  lumen::FieldOrder cfg_field_order = lumen::FieldOrder::Unknown; // 配置文件里的默认值

  // --- 进度记忆（F-108）---
  // 待跳转的位置：loadfile 之后管道可用但文件还没解析完，立即 seek 会失败，
  // 所以先记下来，等定时器确认能读到 duration 再跳。
  double pending_resume = 0.0;
  // 定时器采到的最近一次播放位置/总时长。退出时用它写进度——
  // 那时 mpv 已经跟着子窗口一起退出，读不到属性了。
  double last_pos = 0.0;
  double last_dur = 0.0;

  LiveStats stats;
  int stat_tick = 0; // 属性采样的分频计数（重属性 500ms 采一次）
};

PlayerState* g_state = nullptr;

// 进度记忆的读写实现在文件后半部分（见"进度记忆"小节），此处前置声明。
struct ResumeEntry {
  double pos = 0.0; // 上次看到的位置（秒）
  double dur = 0.0; // 该片总时长（秒），0 表示未知
};
std::map<std::string, ResumeEntry> LoadResumeTable();
void RememberCurrentPosition();
void AttemptPendingResume();
std::string NormalizePathKey(const std::string& path);

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                    nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr,
                      nullptr);
  return s;
}

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                    nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
  return w;
}

// ---------------------------------------------------------------------------
// 界面用中文名
// ---------------------------------------------------------------------------
// ★ 为什么不直接改 core 的 ToString：
//   那几个 ToString 输出的英文名同时被 JSON 报告、日志和 Android 侧使用，
//   改成中文会污染"机器可读"的场合（日志里出现中文反而难 grep）。
//   界面要的是"给人看的中文"，所以在应用层单独翻译一层。

std::wstring CadenceZh(lumen::SourceCadence c) {
  switch (c) {
    case lumen::SourceCadence::Progressive: return L"逐行";
    case lumen::SourceCadence::Interlaced: return L"隔行";
    case lumen::SourceCadence::Telecine: return L"3:2 折叠";
    case lumen::SourceCadence::Mixed: return L"混合（逐行+隔行）";
    default: return L"未判定";
  }
}

std::wstring FieldModeZh(lumen::FieldMode m) {
  switch (m) {
    case lumen::FieldMode::Deinterlace: return L"反交错";
    case lumen::FieldMode::IVTC: return L"逆折叠";
    default: return L"直通";
  }
}

std::wstring FieldOrderZh(lumen::FieldOrder o) {
  switch (o) {
    case lumen::FieldOrder::Progressive: return L"逐行";
    case lumen::FieldOrder::TopFieldFirst: return L"上场优先";
    case lumen::FieldOrder::BottomFieldFirst: return L"下场优先";
    default: return L"自动";
  }
}

std::wstring ColorSpaceZh(lumen::ColorSpace cs) {
  switch (cs) {
    case lumen::ColorSpace::BT601: return L"BT.601";
    case lumen::ColorSpace::BT709: return L"BT.709";
    case lumen::ColorSpace::BT2020: return L"BT.2020";
    default: return L"未标记";
  }
}

// mpv 的 video-codec 会长成一串 "H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 (High)"，
// 侧栏只有 200 多像素，直接显示必然被截成半句话。这里只保留主名 + 短档次。
std::wstring CodecZh(const std::string& raw) {
  if (raw.empty()) return L"—";
  std::string main = raw;
  const size_t slash = main.find(" / ");
  if (slash != std::string::npos) main = main.substr(0, slash);

  std::string profile;
  const size_t lp = main.find(" (");
  if (lp != std::string::npos) {
    profile = main.substr(lp + 2);
    if (!profile.empty() && profile.back() == ')') profile.pop_back();
    main = main.substr(0, lp);
  }

  std::wstring name;
  if (main == "h264") name = L"H.264";
  else if (main == "hevc") name = L"H.265";
  else if (main == "mpeg2video") name = L"MPEG-2";
  else if (main == "mpeg4") name = L"MPEG-4";
  else if (main == "vc1") name = L"VC-1";
  else if (main == "vp9") name = L"VP9";
  else if (main == "av1") name = L"AV1";
  else if (main == "aac") name = L"AAC";
  else if (main == "ac3") name = L"AC-3";
  else if (main == "eac3") name = L"E-AC-3";
  else if (main == "dca") name = L"DTS";
  else if (main == "mp3") name = L"MP3";
  else if (main == "flac") name = L"FLAC";
  else if (main.rfind("pcm", 0) == 0) name = L"PCM";
  else name = Utf8ToWide(main);

  // 档次只在够短时才带（"High"/"Main 10" 有意义，"Advanced Audio Coding" 只会挤掉别的信息）
  if (!profile.empty() && profile.size() <= 10) {
    name += L"（";
    name += Utf8ToWide(profile);
    name += L"）";
  }
  return name;
}

std::wstring HwdecZh(const std::string& raw) {
  if (raw.empty() || raw == "no") return L"软件解码";
  if (raw == "d3d11va") return L"D3D11 硬解";
  if (raw == "d3d11va-copy") return L"D3D11 硬解（回拷）";
  if (raw == "dxva2") return L"DXVA2 硬解";
  if (raw == "dxva2-copy") return L"DXVA2 硬解（回拷）";
  if (raw == "cuda") return L"CUDA 硬解";
  if (raw == "nvdec") return L"NVDEC 硬解";
  if (raw == "auto" || raw == "auto-safe") return L"自动";
  return Utf8ToWide(raw);
}

// 像素格式（yuv420p / p010 / nv12 …）中文名。
// mpv 的 video-params/pixelformat 是内部名，直接打到界面上没人看得懂；
// 但也不能瞎编，认不出来的就原样返回——宁可露出内部名，也不要给错信息。
std::wstring PixelFormatZh(const std::string& raw) {
  if (raw.empty()) return L"未知";

  std::string s;
  for (char c : raw) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

  // 硬解时 mpv 报的不是像素格式，而是后端名（d3d11 / cuda / …）。
  // 这个名字和紧跟其后的"xx 硬解"是同一件事，打两遍纯属噪音，直接省略。
  if (s == "d3d11" || s == "d3d11va" || s == "cuda" || s == "vaapi" || s == "vdpau" ||
      s == "videotoolbox" || s == "mediacodec" || s == "drm_prime" || s == "vulkan") {
    return std::wstring();
  }

  // yuvj420p 这类是"全范围"变体，单独记一笔再从主名里摘掉
  const bool full_range = s.rfind("yuvj", 0) == 0;
  if (full_range) s = "yuv" + s.substr(4);

  // 高位深：p010 / p016 前缀式，先于 "p<数字>" 后缀判断处理
  const char* pre_name = nullptr;
  int bits = 8;
  if (s.rfind("p010", 0) == 0) {
    pre_name = "P010";
    bits = 10;
  } else if (s.rfind("p016", 0) == 0) {
    pre_name = "P016";
    bits = 16;
  } else if (s.rfind("nv12", 0) == 0) {
    pre_name = "NV12";
  } else if (s.rfind("nv21", 0) == 0) {
    pre_name = "NV21";
  }

  wchar_t buf[64];
  if (pre_name != nullptr) {
    std::wstring out = Utf8ToWide(pre_name);
    swprintf(buf, 64, L" 4:2:0 %d 位", bits);
    out += buf;
    if (full_range) out += L"（全范围）";
    return out;
  }

  // 后缀式位深：yuv420p10le / yuv422p12 —— 截到结尾的 'p' 为止
  const size_t ppos = s.find('p', 1);
  if (ppos != std::string::npos && ppos + 1 < s.size() &&
      std::isdigit(static_cast<unsigned char>(s[ppos + 1]))) {
    bits = std::atoi(s.c_str() + ppos + 1);
    s = s.substr(0, ppos + 1);
  }

  const wchar_t* layout = nullptr;
  if (s == "yuv420p") layout = L"YUV 4:2:0";
  else if (s == "yuv422p") layout = L"YUV 4:2:2";
  else if (s == "yuv444p") layout = L"YUV 4:4:4";
  else if (s == "gbrp") layout = L"平面 RGB";

  if (layout == nullptr) {
    if (s == "rgb24") return L"RGB 8 位";
    if (s == "bgr24") return L"BGR 8 位";
    if (s == "rgb0") return L"RGB 8 位（带填充）";
    return Utf8ToWide(raw);
  }

  std::wstring out = layout;
  swprintf(buf, 64, L" %d 位", bits);
  out += buf;
  if (full_range) out += L"（全范围）";
  return out;
}

std::string FileNameOf(const std::string& path) {
  const size_t pos = path.find_last_of("\\/");
  return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

// 当前 exe 所在目录（含尾部分隔符）
std::string ExeDirectory() {
  wchar_t buf[MAX_PATH] = {0};
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0) return std::string();
  std::wstring p(buf, n);
  const size_t pos = p.find_last_of(L"\\/");
  if (pos == std::wstring::npos) return std::string();
  return WideToUtf8(p.substr(0, pos + 1));
}

bool FileExists(const std::string& p) {
  return !p.empty() && GetFileAttributesW(Utf8ToWide(p).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 取窗口 DPI。GetDpiForWindow 是 Win10 1607 才有的 API，用 GetProcAddress 动态取：
// 为一个"可用可不用"的接口把最低系统要求抬上去不值得。
// 不做缩放的表现很具体：125% 缩放下 HUD 字小三分之一、控制条按钮挤在一起，
// 而面板尺寸与文字不匹配还会把最后一行裁掉。
UINT WindowDpi(HWND hwnd) {
  using GetDpiFn = UINT(WINAPI*)(HWND);
  static GetDpiFn get_dpi = reinterpret_cast<GetDpiFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                             "GetDpiForWindow")));
  UINT dpi = 96;
  if (get_dpi != nullptr && hwnd != nullptr) {
    const UINT d = get_dpi(hwnd);
    if (d >= 72 && d <= 480) dpi = d;
  }
  return dpi;
}

// ---------------------------------------------------------------------------
// 内嵌运行时（单文件分发）
// ---------------------------------------------------------------------------
// ★ 为什么要把 mpv 塞进 exe：
//    用户拿到播放器最自然的期待是"双击就播"。之前的发布包里有 4 个 exe
//    （player/probe/render/batch）+ 一个 mpv 目录，用户根本不知道该点哪个，
//    也常把 mpv.exe 当成本程序本体去双击。
//    现在只发一个 exe，mpv 以 RCDATA 资源躺在里面，首次运行时释放到本地目录。
//
// ★ 为什么不把 onnxruntime/DirectML 也塞进来：
//    那是离线导出（lumen-render）用的，32MB 且播放路径完全用不到。
//    为了"单文件"把一个播放器撑到 150MB 没有意义。
//
// ★ 资源 ID 必须与 tools/build_single.py 生成的 embed.rc 一致。
constexpr int kResMpv = 101;
constexpr int kResD3dCompiler = 102;

bool ExtractOneResource(int id, const std::wstring& dest, std::string* err) {
  // RT_RCDATA 宏在非 Unicode 目标下展开成 MAKEINTRESOURCEA，传给 FindResourceW
  // 会编译失败（MinGW 尤其严格）。资源类型 10 就是 RCDATA，直接写数字最稳。
  constexpr int kTypeRcData = 10;
  HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(kTypeRcData));
  if (res == nullptr) return false; // 开发构建没链资源，走后续查找即可
  const DWORD size = SizeofResource(nullptr, res);
  HGLOBAL handle = LoadResource(nullptr, res);
  void* data = (handle != nullptr) ? LockResource(handle) : nullptr;
  if (data == nullptr || size == 0) return false;

  // 已释放过且大小一致就跳过：每次启动重写 120MB 会让"打开播放器"明显变慢
  // （机械盘上尤其明显，用户会以为程序卡死）。
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (GetFileAttributesExW(dest.c_str(), GetFileExInfoStandard, &fad)) {
    const ULONGLONG have =
        (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    if (have == size) return true;
  }

  // 先写 .tmp 再原子替换：
  //   1) 上一个实例可能还在跑，直接覆盖正在使用的 exe 会失败；
  //   2) 半截文件比没有更糟——下次启动会把它当成"已释放"，然后启动失败。
  const std::wstring tmp = dest + L".tmp";
  HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    if (err) *err = "无法写入：" + WideToUtf8(dest);
    return false;
  }
  DWORD written = 0;
  const BOOL ok = WriteFile(f, data, size, &written, nullptr);
  CloseHandle(f);
  if (!ok || written != size) {
    DeleteFileW(tmp.c_str());
    if (err) *err = "运行时写入不完整（磁盘空间不足？）";
    return false;
  }
  if (!MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(tmp.c_str());
    if (err) *err = "无法替换旧文件：" + WideToUtf8(dest);
    return false;
  }
  return true;
}

// 释放内嵌的 mpv 运行时，返回 mpv.exe 的路径（失败返回空）。
// 放在 %LOCALAPPDATA%\LumenPlayer 而不是 exe 同目录：
//   1) 程序可能被装进 Program Files，那里没有写权限；
//   2) 单文件分发时"旁边长出一堆文件"会让用户以为装坏了。
std::string ExtractEmbeddedRuntime(std::string* err) {
  wchar_t buf[MAX_PATH] = {0};
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  std::wstring base;
  if (n > 0 && n < MAX_PATH) {
    base.assign(buf, n);
  } else {
    wchar_t tmp[MAX_PATH] = {0};
    if (GetTempPathW(MAX_PATH, tmp) > 0) base = tmp;
  }
  if (base.empty()) {
    if (err) *err = "找不到可写的本地目录";
    return std::string();
  }

  const std::wstring dir = base + L"\\LumenPlayer";
  CreateDirectoryW(dir.c_str(), nullptr);

  const std::wstring mpv = dir + L"\\mpv.exe";
  if (!ExtractOneResource(kResMpv, mpv, err)) return std::string();

  // d3dcompiler_43 是 mpv 编译着色器要用的旧版组件，Win10 不自带 43。
  // 缺了它 mpv 可能起不来或黑屏，所以一起释放；失败也只是"可能出问题"，
  // 不阻塞启动（部分系统已经通过其他途径装了它）。
  ExtractOneResource(kResD3dCompiler, dir + L"\\d3dcompiler_43.dll", nullptr);
  return WideToUtf8(mpv);
}

// ---------------------------------------------------------------------------
// 查找 mpv.exe
// ---------------------------------------------------------------------------
// 顺序：配置 → exe 同目录（便携布局）→ 内嵌资源释放 → PATH。
// 保留"同目录优先"是为了让高级用户能直接替换 mpv 版本，
// 也让开发时的 build 目录布局继续可用。
std::string FindMpv(const lumen::AppConfig& cfg, const std::string& exe_dir) {
  if (!cfg.player.mpv_path.empty() && FileExists(cfg.player.mpv_path)) {
    return cfg.player.mpv_path;
  }
  const char* kRel[] = {"mpv.exe", "mpv\\mpv.exe", "runtime\\mpv.exe"};
  for (const char* rel : kRel) {
    const std::string p = exe_dir + rel;
    if (FileExists(p)) return p;
  }

  std::string err;
  const std::string embedded = ExtractEmbeddedRuntime(&err);
  if (!embedded.empty() && FileExists(embedded)) return embedded;

  return lumen::FindInPath("mpv.exe");
}

// ---------------------------------------------------------------------------
// 处理链 → mpv 滤镜
// ---------------------------------------------------------------------------
// 注意：这里刻意不生成色彩矩阵转换滤镜。
// 离线导出时我们自己做 YUV→RGB→YUV，必须显式转 601→709 并写容器标记；
// 而 mpv 是直接把 YUV 转成 RGB 上屏，它会读源色彩标记用正确矩阵转换，
// 再按显示器色彩空间输出。此时若再插一个 scale=in_color_matrix=...，
// 等于转换两次，画面会明显偏移。这是"照搬离线管线"最容易犯的错。
std::vector<std::string> BuildMpvFilters(const lumen::ProcessingChain& c) {
  std::vector<std::string> f;
  const bool tff = (c.assumed_field_order != lumen::FieldOrder::BottomFieldFirst);
  const char* parity = tff ? "tff" : "bff";

  if (c.field_mode == lumen::FieldMode::Deinterlace) {
    // send_frame 保持帧率不变；deint=all 表示所有帧都处理
    f.push_back(std::string("bwdif=mode=send_frame:parity=") + parity + ":deint=all");
  } else if (c.field_mode == lumen::FieldMode::IVTC) {
    f.push_back(std::string("fieldmatch=order=") + parity + ":combmatch=full,decimate=cycle=5");
  }
  if (c.deblock) f.push_back("deblock=filter=strong:block=8");
  if (c.denoise == lumen::DenoiseMode::Light) f.push_back("hqdn3d=1.5:1.5:6:6");
  else if (c.denoise == lumen::DenoiseMode::Medium) f.push_back("hqdn3d=3:3:9:9");
  return f;
}

// 我们添加的滤镜统一用 @lumen0 / @lumen1 … 命名。
// 必须逐个加标签：实测发现把 "a,b,c" 一次性 add 时，标签只挂在第一个滤镜上，
// 用 vf remove @lumen 移除后 b、c 仍然留在管线里——
// 表现为"按 A 切回原片，画面还是处理过的"，而且很难看出来。
constexpr int kMaxLumenFilters = 4;

void ClearOurFilters(lumen::MpvIpcClient* ipc) {
  if (!ipc) return;
  for (int i = 0; i < kMaxLumenFilters; ++i) {
    // 忽略返回值：本来就不存在时返回错误，这正是我们要的结果
    ipc->Command({"vf", "remove", "@lumen" + std::to_string(i)});
  }
}

void SetStatus(const std::string& text) {
  if (!g_state || !g_state->hwnd) return;
  std::wstring title = Utf8ToWide("LumenPlayer  |  " + text);
  SetWindowTextW(g_state->hwnd, title.c_str());
}

void Osd(const std::string& text, int ms = 3000) {
  if (!g_state || !g_state->ipc.connected()) return;
  g_state->ipc.Command({"show-text", text, std::to_string(ms)});
}

// ---------------------------------------------------------------------------
// 性能 HUD（F-401）
// ---------------------------------------------------------------------------
// ★ 为什么不用 mpv 的 show-text 做 HUD：
//    每次 show-text 都是一次新的 OSD 消息，500ms 刷新一次会让文字不断"闪一下"；
//    而且 mpv 的 OSD 只能显示文本，没法做半透明底衬，白字压在亮画面上看不清。
// ★ 为什么单独开一个分层窗口（而不是画在主窗口上）：
//    mpv 通过 --wid 创建的子窗口铺满整个客户区，会盖住我们在父窗口上画的任何东西。
//    顶层 + 分层（WS_EX_LAYERED）+ 鼠标穿透（WS_EX_TRANSPARENT）是唯一能做到
//    "悬浮在视频上、又不影响鼠标操作"的组合。
// ★ 为什么用 UpdateLayeredWindow 而不是 SetLayeredWindowAttributes：
//    色键（color key）方案会把字形抗锯齿边缘也抠掉，文字边缘出现彩色毛刺；
//    逐像素 alpha 才能得到干净的半透明底 + 抗锯齿文字。
void HudReposition(); // 前置声明：HudPaint 在尺寸变化后需要重新贴位

void HudPaint(const std::vector<std::wstring>& lines) {
  PlayerState& s = *g_state;
  if (s.hud == nullptr) return;

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);

  const UINT dpi = WindowDpi(s.hud);

  const int line_h = MulDiv(21, static_cast<int>(dpi), 96);
  const int pad_x = MulDiv(12, static_cast<int>(dpi), 96);
  const int pad_y = MulDiv(8, static_cast<int>(dpi), 96);

  HFONT font = CreateFontW(-MulDiv(11, static_cast<int>(dpi), 96), 0, 0, 0, FW_NORMAL, FALSE,
                           FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
                           L"Microsoft YaHei UI");
  HGDIOBJ old_font = SelectObject(mem, font);

  // 先量文字宽度再决定面板宽度：固定宽度在长文件名/中文下必然裁字，
  // 而被裁掉的通常正是最需要看到的那一行。
  int text_w = 0;
  int max_chars = 0;
  for (const auto& l : lines) {
    SIZE sz{};
    if (GetTextExtentPoint32W(mem, l.c_str(), static_cast<int>(l.size()), &sz)) {
      if (sz.cx > text_w) text_w = sz.cx;
    }
    if (static_cast<int>(l.size()) > max_chars) max_chars = static_cast<int>(l.size());
  }
  (void)max_chars;
  int w = text_w + pad_x * 2;
  if (w < MulDiv(280, static_cast<int>(dpi), 96)) w = MulDiv(280, static_cast<int>(dpi), 96);
  int h = line_h * static_cast<int>(lines.size()) + pad_y * 2;

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h; // 负高度 = 自上而下，行序与视觉一致
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bmp == nullptr || bits == nullptr) {
    if (bmp) DeleteObject(bmp);
    SelectObject(mem, old_font);
    DeleteObject(font);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return;
  }
  HGDIOBJ old_bmp = SelectObject(mem, bmp);

  // 1) 先把整块清成"纯黑"，随后用亮度反推 alpha：
  //    文字画成白色，抗锯齿边缘是灰阶 —— 灰度值直接就是该像素的不透明度。
  //    这样不用做两遍合成，也不需要读回 alpha 通道（GDI 根本不写 alpha）。
  std::memset(bits, 0, static_cast<size_t>(w) * h * 4);

  RECT rc{0, 0, w, h};
  HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
  FillRect(mem, &rc, bg);
  DeleteObject(bg);

  SetBkMode(mem, TRANSPARENT);
  SetTextColor(mem, RGB(255, 255, 255));

  int y = pad_y;
  for (const auto& line : lines) {
    RECT tr{pad_x, y, w - pad_x, y + line_h};
    DrawTextW(mem, line.c_str(), -1, &tr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    y += line_h;
  }

  SelectObject(mem, old_font);
  DeleteObject(font);

  // 2) 黑白 → 预乘 ARGB：文字处 alpha = 灰度，底色处 alpha = 常量（半透明黑板）
  constexpr BYTE kPanelAlpha = 156;
  BYTE* px = static_cast<BYTE*>(bits);
  for (int i = 0; i < w * h; ++i) {
    BYTE* p = px + static_cast<size_t>(i) * 4;
    const BYTE lum = p[0] > p[1] ? p[0] : p[1]; // B/G 取大者近似亮度
    if (lum > 6) {
      // 文字：白色，预乘后 RGB 各分量都等于 alpha
      p[0] = p[1] = p[2] = lum;
      p[3] = lum;
    } else {
      // 底衬：纯黑 + 固定 alpha（预乘时 RGB 必须为 0）
      p[0] = p[1] = p[2] = 0;
      p[3] = kPanelAlpha;
    }
  }

  // ★ 直接改写 CreateDIBSection 返回的 bits 属于"绕过 GDI 写内存"，
  //   而 UpdateLayeredWindow 是 GDI 调用。两者混用必须先冲刷批处理队列，
  //   否则 ULW 读到的是 GDI 缓存里的旧位图 —— 症状非常隐蔽：
  //   只有 GDI 画过的字形能出现，我们手写的半透明底衬整块消失。
  GdiFlush();

  // 尺寸变了先调整窗口再定位（内容行数/文件名长度会变）
  RECT cur{};
  GetWindowRect(s.hud, &cur);
  if (cur.right - cur.left != w || cur.bottom - cur.top != h) {
    SetWindowPos(s.hud, nullptr, 0, 0, w, h,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    HudReposition();
    GetWindowRect(s.hud, &cur);
  }

  POINT dst{cur.left, cur.top};
  SIZE sz{w, h};
  POINT src{0, 0};
  BLENDFUNCTION bf{};
  bf.BlendOp = AC_SRC_OVER;
  bf.SourceConstantAlpha = 255;
  bf.AlphaFormat = AC_SRC_ALPHA;
  UpdateLayeredWindow(s.hud, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);

  SelectObject(mem, old_bmp);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}

// 把 HUD 贴到视频区右上角。
// 每次窗口移动/缩放都要调用：分层窗口不是子窗口，不会自动跟随父窗口。
// 注意锚点是"视频区"而不是客户区——右侧信息栏出现后，贴客户区右上角会被
// 侧栏压住一半，看起来像 HUD 缺了一块。
void HudReposition() {
  PlayerState& s = *g_state;
  if (s.hud == nullptr || s.hwnd == nullptr) return;

  RECT wr{};
  GetWindowRect(s.hud, &wr);
  int w = wr.right - wr.left;
  int h = wr.bottom - wr.top;
  if (w <= 0) w = kHudWidth;
  if (h <= 0) h = kHudHeight;

  const lumen::ui::Rect& v = s.layout.video;
  POINT tl{v.x, v.y};
  ClientToScreen(s.hwnd, &tl);
  const int x = tl.x + v.w - w - 16;
  const int y = tl.y + 16;
  SetWindowPos(s.hud, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void HudCreate() {
  PlayerState& s = *g_state;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DefWindowProcW; // 分层窗口不参与常规绘制，消息交给默认处理
  wc.hInstance = s.inst;
  wc.lpszClassName = kHudClass;
  RegisterClassExW(&wc); // 重复注册返回 0，属正常

  s.hud = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                              WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                          kHudClass, L"LumenHUD", WS_POPUP, 0, 0, kHudWidth, kHudHeight,
                          s.hwnd, nullptr, s.inst, nullptr);
}

void HudSetVisible(bool on) {
  PlayerState& s = *g_state;
  s.hud_on = on;
  if (s.hud == nullptr) return;
  ShowWindow(s.hud, on ? SW_SHOWNOACTIVATE : SW_HIDE);
  if (on) HudReposition();
}

std::wstring FormatClockW(double sec) { return lumen::ui::FormatClock(sec); }

// 把采样到的实时属性画成 HUD。
// 属性采集已经统一在 RefreshPlaybackState 里做（控制条、侧栏、HUD 共用一份），
// 这里只负责排版，不再自己发 IPC。
void HudRefresh() {
  PlayerState& s = *g_state;
  if (s.hud == nullptr) return;

  const LiveStats& st = s.stats;
  std::vector<std::wstring> lines;

  // 第一行：文件名（长名字截断，避免整块 HUD 被一个长片名撑变形）
  std::string name = FileNameOf(s.current_file);
  if (name.size() > 46) name = name.substr(0, 45) + "…";
  lines.push_back(Utf8ToWide(name.empty() ? "(未打开文件)" : name));

  // 第二行：片源规格
  // 这里刻意不用 swprintf 的 %hs/%s 混排：宽字符 printf 里 %s 与 %hs 的含义
  // 依赖 CRT 实现（MinGW 用的 UCRT 与 MSVC 一致，但换平台就会踩坑），
  // 直接做宽字符串拼接最稳。
  wchar_t buf[256];
  if (st.vw > 0 && st.vh > 0) {
    swprintf(buf, 256, L"片源 %d×%d", static_cast<int>(st.vw), static_cast<int>(st.vh));
    std::wstring line = buf;
    // 像素格式在硬解下会被省略（返回空串），此时不留空档
    const std::wstring pixfmt = PixelFormatZh(st.pixfmt);
    if (!pixfmt.empty()) {
      line += L"  ";
      line += pixfmt;
    }
    line += L"  ";
    // hwdec-current 在"没有硬件解码"时返回字符串 "no"，
    // 直接把它打到界面上等于没说，这里翻译成用户能懂的话。
    line += HwdecZh(st.hwdec);
    lines.push_back(line);
  } else {
    lines.push_back(L"片源 —（尚未解码出画面）");
  }

  // 第三行：进度、帧率、丢帧
  // 注意：这里不用 swprintf 的 %s 拼接宽字符串——在 MinGW + 不同 CRT 下
  // 宽字符 %s 的行为不一致（实测会打印出 "0 / 0" 这种莫名其妙的输出）。
  // 数字用 swprintf 格式化，字符串一律用 wstring 相加。
  {
    std::wstring line;
    if (st.dur > 0.5) {
      line += FormatClockW(st.pos);
      line += L" / ";
      line += FormatClockW(st.dur);
      line += L"   ";
    }
    swprintf(buf, 256, L"%.2f 帧/秒   丢帧 %.0f（解码层 %.0f）", st.fps, st.drops, st.dec_drops);
    line += buf;
    lines.push_back(line);
  }

  // 第四行：处理链（这是本产品"有别于普通播放器"的核心信息）
  {
    std::wstring line = L"处理 ";
    if (!s.has_analysis) {
      line += L"分析中…";
    } else if (!s.processed) {
      line += L"已旁路（原片）";
    } else {
      line += FieldModeZh(s.chain.field_mode);
      if (s.chain.assumed_field_order == lumen::FieldOrder::TopFieldFirst) {
        line += L"（上场优先）";
      } else if (s.chain.assumed_field_order == lumen::FieldOrder::BottomFieldFirst) {
        line += L"（下场优先）";
      }
      if (s.chain.deblock) line += L" + 去块";
      if (s.chain.denoise != lumen::DenoiseMode::Off) line += L" + 降噪";
      if (s.chain.field_mode == lumen::FieldMode::Passthrough && !s.chain.deblock &&
          s.chain.denoise == lumen::DenoiseMode::Off) {
        line += L"（无需处理）";
      }
      if (s.field_override != lumen::FieldOrder::Unknown) line += L" [人工场序]";
    }
    lines.push_back(line);
  }

  // 第五行：超分状态。这里必须写"未启用"而不是含糊其辞——
  // HUD 存在的意义就是让用户确认"到底有没有在增强"。
  {
    std::wstring line;
    if (st.speed != 1.0) {
      swprintf(buf, 256, L"速度 %.2f×   ", st.speed);
      line += buf;
    }
    if (s.output_height > 0) {
      swprintf(buf, 256, L"输出 %d 线（缩放滤镜）  超分 未接入实时路径", s.output_height);
      line += buf;
    } else {
      line += L"超分 未接入实时路径（当前输出原分辨率）";
    }
    lines.push_back(line);
  }

  HudPaint(lines);
}

// ---------------------------------------------------------------------------
// 应用 / 撤销处理链
// ---------------------------------------------------------------------------
// ★ 为什么输出缩放也在这里挂载：
//    mpv 的 vf 是一条有序链，"add" 永远追加到末尾。如果单独在别处 add 缩放，
//    之后再切 A/B（先删 @lumen* 再重新 add 场处理）就会出现「缩放 → 反交错」
//    的倒序：交错线先被放大两倍，再去做场匹配，画面反而更差。
//    所以每次重建链路都按「场处理 → 缩放」的顺序一次性拼好。
void ApplyChain(bool processed) {
  PlayerState& s = *g_state;
  if (!s.ipc.connected()) return;

  ClearOurFilters(&s.ipc);
  s.ipc.Command({"vf", "remove", "@lumenout"});
  s.processed = processed;

  std::string err;
  bool any = false;

  if (processed && s.has_analysis) {
    const std::vector<std::string> filters = BuildMpvFilters(s.chain);
    for (size_t i = 0; i < filters.size() && i < kMaxLumenFilters; ++i) {
      const lumen::Status st =
          s.ipc.Command({"vf", "add", "@lumen" + std::to_string(i) + ":" + filters[i]}, &err);
      if (st != lumen::Status::Ok) {
        // 滤镜加载失败是"看得见的故障"（画面不变），必须让用户知道原因，
        // 而不是默默什么都没做。
        Osd("滤镜加载失败：" + (err.empty() ? s.ipc.last_error() : err));
        return;
      }
      any = true;
    }
  }

  // 输出分辨率：h=-2 让宽度按原比例自动算（并保证偶数），避免 4:3 片源被拉成 16:9。
  if (s.output_height > 0) {
    const std::string f =
        "scale=w=-2:h=" + std::to_string(s.output_height) + ":flags=lanczos";
    if (s.ipc.Command({"vf", "add", "@lumenout:" + f}, &err) != lumen::Status::Ok) {
      Osd("输出分辨率设置失败：" + (err.empty() ? s.ipc.last_error() : err));
    }
  }

  if (!processed) {
    Osd("A/B：原片（未做场处理）");
  } else if (!s.has_analysis) {
    // 分析还没回来，稍后 WM_APP_INSPECT_DONE 会再调一次
  } else if (!any) {
    Osd("A/B：处理后（本片为逐行，无需场处理）");
  } else {
    Osd(WideToUtf8(L"A/B：处理后（" + FieldModeZh(s.chain.field_mode) + L"）"));
  }
}

// ---------------------------------------------------------------------------
// 自绘界面：布局、刷新、鼠标、菜单
// ---------------------------------------------------------------------------
// 这一段的职责边界：
//   1. 把客户区分成「视频区 / 控制条 / 信息侧栏」三块，并同步 mpv 子窗口；
//   2. 每 250ms 采一次属性，转成界面用的字符串快照；
//   3. 处理鼠标：进度条拖动、按钮点击、滚轮调音量；
//   4. 画面设置（窗口尺寸 / 画面比例 / 输出分辨率 / 倍速）。
// 真正的「画」在 player_ui.cpp 里，这里只负责"什么时候画、画什么数据"。

// 后文定义的函数，这里先声明（菜单和鼠标处理会用到）
void OpenFileDialog();
void ToggleFullscreen();
void ApplyFieldOverride(lumen::FieldOrder fo);
void CycleFieldOrder();

// 菜单命令 ID。用 41000 起是为了避开系统命令与 comctl32 保留区间。
enum MenuCmd : int {
  kCmdSizeFit = 41001,
  kCmdSize100,
  kCmdSize150,
  kCmdSize200,
  kCmdSizeMax,
  kCmdAspectFit = 41010,
  kCmdAspectStretch,
  kCmdAspectCrop,
  kCmdAspectNative,
  kCmdOutNative = 41020,
  kCmdOut720,
  kCmdOut1080,
  kCmdOut1440,
  kCmdOut2160,
  kCmdSpeed05 = 41030,
  kCmdSpeed075,
  kCmdSpeed10,
  kCmdSpeed125,
  kCmdSpeed15,
  kCmdSpeed20,
  kCmdSidebar = 41040,
  kCmdHud,
  kCmdFieldCycle,
  kCmdChainToggle,
  kCmdOpenFile,
  kCmdAbout,
};

// 版本号：单文件发布后用户只能看到一个 exe，"关于"里要有明确的版本，
// 否则反馈问题时说不清用的是哪一版。
constexpr const wchar_t* kAppVersion = L"0.3.0";

void ShowAbout(HWND hwnd) {
  // 单文件分发没有随附的 THIRD-PARTY-NOTICES.txt 了，许可信息必须能在
  // 程序内看到——这是用独立进程方式调用 GPL 程序时该做的事。
  std::wstring text;
  text += L"LumenPlayer ";
  text += kAppVersion;
  text += L"\n老片修复播放器（Windows）\n\n";
  text += L"许可：源代码公开，非商业使用免费；商用需书面授权（LNC-SL 1.0）\n";
  text += L"完整条款见项目根目录 LICENSE 与 THIRD-PARTY-NOTICES.md\n\n";
  text += L"画面解码与渲染由独立的 mpv 进程完成，两者通过 JSON IPC 通信。\n";
  text += L"本程序没有链接 mpv 的代码，自有代码按上述许可单独授权。\n\n";
  text += L"mpv：GPLv2 或更高版本\n";
  text += L"源码：https://github.com/mpv-player/mpv\n\n";
  text += L"内嵌的 mpv 运行时首次启动时释放到：\n";
  {
    wchar_t buf[MAX_PATH] = {0};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
      text += buf;
      text += L"\\LumenPlayer\\mpv.exe";
    } else {
      text += L"%LOCALAPPDATA%\\LumenPlayer\\mpv.exe";
    }
  }
  text += L"\n\n快捷键：按 H 查看";
  MessageBoxW(hwnd, text.c_str(), L"关于 LumenPlayer", MB_OK | MB_ICONINFORMATION);
}

void RepaintAll() {
  if (g_state && g_state->hwnd) InvalidateRect(g_state->hwnd, nullptr, FALSE);
}

// 只重画控制条与侧栏。视频区是 mpv 的子窗口，往那里 InvalidateRect
// 会引发一次无谓的全窗口重绘（拖动窗口时看得见的闪）。
void RepaintChrome() {
  PlayerState& s = *g_state;
  if (!s.hwnd || !IsWindowVisible(s.hwnd)) return;
  if (!s.layout.controls.empty()) {
    const lumen::ui::Rect& c = s.layout.controls;
    RECT rc{c.x, c.y, c.right(), c.bottom()};
    InvalidateRect(s.hwnd, &rc, FALSE);
  }
  if (!s.layout.sidebar.empty()) {
    const lumen::ui::Rect& b = s.layout.sidebar;
    RECT rc{b.x, b.y, b.right(), b.bottom()};
    InvalidateRect(s.hwnd, &rc, FALSE);
  }
  if (!s.info.has_media) RepaintAll(); // 欢迎页在视频区里，必须整块重画
}

// mpv 的 --wid 子窗口：客户区里唯一的可见子窗口就是它。
// （我们自己一个子窗口都没创建，HUD 是独立顶层窗口，所以这个判断是安全的。）
BOOL CALLBACK FindVideoProc(HWND child, LPARAM lp) {
  if (!IsWindowVisible(child)) return TRUE;
  RECT rc{};
  GetClientRect(child, &rc);
  if (rc.right - rc.left <= 0) return TRUE; // 跳过 mpv 内部的辅助隐藏窗口
  *reinterpret_cast<HWND*>(lp) = child;
  return FALSE;
}

LRESULT CALLBACK VideoSubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

// 视频子窗口的挂接。为什么要子类化：
//   mpv 用 --wid 创建的子窗口会吃掉所有键盘消息（即使 --input-vo-keyboard=no
//   让 mpv 自己不处理，消息也不会冒泡到父窗口）。用户点一下画面再按空格
//   就没反应——这正是"快捷键时灵时不灵"的根因。子类化后把键盘与鼠标
//   转发给主窗口，行为才稳定。
//
//   同样把鼠标转发过来，是为了让我们自己决定：单击暂停、双击全屏、滚轮调音量。
//   默认已关掉 mpv 的 input bindings（--input-default-bindings=no），
//   不会出现"两个播放器同时响应一次点击"的双触发。
void EnsureVideoAttached() {
  PlayerState& s = *g_state;
  if (!s.hwnd) return;

  if (s.video && !IsWindow(s.video)) {
    s.video = nullptr;      // mpv 重建过窗口，旧句柄已失效
    s.video_subclassed = false;
  }

  if (!s.video) {
    HWND found = nullptr;
    EnumChildWindows(s.hwnd, FindVideoProc, reinterpret_cast<LPARAM>(&found));
    if (!found) return;
    s.video = found;
    s.video_subclassed = false;
    const lumen::ui::Rect& v = s.layout.video;
    SetWindowPos(found, nullptr, v.x, v.y, v.w, v.h, SWP_NOZORDER | SWP_NOACTIVATE);
  }

  if (!s.video_subclassed && s.video) {
    s.video_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        s.video, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(VideoSubclassProc)));
    s.video_subclassed = (s.video_proc != nullptr);
  }

  // 没有打开文件时把 mpv 的空闲窗口藏起来，否则它会把欢迎页整块盖住。
  // 打开文件后再显示——这一步在 OpenFile 里做。
  if (s.video && s.current_file.empty()) ShowWindow(s.video, SW_HIDE);
}

LRESULT CALLBACK VideoSubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  PlayerState* s = g_state;
  if (s && s->hwnd) {
    switch (msg) {
      // 子窗口左上角与视频区左上角重合（视频区恒从 (0,0) 开始），
      // 客户端坐标可以原样转发，不需要 MapWindowPoints。
      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_LBUTTONDBLCLK:
      case WM_RBUTTONUP:
      case WM_MOUSEMOVE:
        PostMessageW(s->hwnd, msg, wp, lp);
        return 0;
      case WM_MOUSEWHEEL:
        // 滚轮的 lParam 本来就是屏幕坐标，原样转发即可
        PostMessageW(s->hwnd, msg, wp, lp);
        return 0;
      default:
        break;
    }
  }
  WNDPROC orig = (s ? s->video_proc : nullptr);
  if (orig) return CallWindowProcW(orig, h, msg, wp, lp);
  return DefWindowProcW(h, msg, wp, lp);
}

// 取子窗口在父窗口客户区坐标系下的矩形。
// 不能用缓存的 s.layout.video 去判断"要不要挪"——mpv 会在自己的线程里
// 偷偷把子窗口改成整块客户区，而缓存值还是我们要的那块，两者永远"相等"，
// 于是布局再也夺不回来（全屏退出后控制条被盖住就是这个成因）。
static bool ChildRectInClient(HWND parent, HWND child, lumen::ui::Rect* out) {
  RECT wr{};
  if (!GetWindowRect(child, &wr)) return false;
  POINT org{0, 0};
  if (!ClientToScreen(parent, &org)) return false;
  out->x = static_cast<int>(wr.left - org.x);
  out->y = static_cast<int>(wr.top - org.y);
  out->w = static_cast<int>(wr.right - wr.left);
  out->h = static_cast<int>(wr.bottom - wr.top);
  return true;
}

// 重新计算三块区域，并把 mpv 子窗口挪到位。
// 只在"子窗口的实际位置"与目标不同时才 SetWindowPos：每次 WM_SIZE 都硬设
// 一遍会让 mpv 反复重建交换链，拖动窗口边缘时会看到画面一跳一跳。
void UpdateLayout() {
  PlayerState& s = *g_state;
  if (!s.hwnd) return;

  RECT cr{};
  GetClientRect(s.hwnd, &cr);
  s.layout = lumen::ui::ComputeLayout(cr.right, cr.bottom, WindowDpi(s.hwnd), s.sidebar_on,
                                      s.fullscreen);

  if (s.video && IsWindow(s.video)) {
    const lumen::ui::Rect& v = s.layout.video;
    lumen::ui::Rect actual{};
    if (!ChildRectInClient(s.hwnd, s.video, &actual) ||
        v.x != actual.x || v.y != actual.y || v.w != actual.w || v.h != actual.h) {
      SetWindowPos(s.video, nullptr, v.x, v.y, v.w, v.h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }
  HudReposition();
}

// 父窗口尺寸变化后启动"布局夺回"：mpv 的 vo 子窗口归它自己的线程管，
// 它发现父窗口变大/变小后会异步把子窗口拉满整块客户区，把我们的控制条和
// 侧栏盖住。SetWindowPos 改父窗口尺寸会同步发 WM_SIZE，但 mpv 的改写
// 发生在那之后，所以只在 WM_SIZE 里挪一次不够，要延时再抢几次。
// 计数有限，窗口稳定后定时器自动停，不会一直空转。
void StartVideoRelayout() {
  PlayerState& s = *g_state;
  s.relayout_retries = kRelayoutRetries;
  if (s.hwnd) SetTimer(s.hwnd, kRelayoutTimerId, kRelayoutIntervalMs, nullptr);
}

// ---------------------------------------------------------------------------
// 属性采样
// ---------------------------------------------------------------------------
// 分两档：
//   轻量（每 250ms）：位置、时长、暂停、音量、静音、结束标志 —— 进度条要跟手。
//   重量（每 500ms）：帧率、分辨率、编码、码率 —— 这些值不会跳，读太勤纯浪费。
void RefreshPlaybackState() {
  PlayerState& s = *g_state;
  if (!s.ipc.connected()) return;

  constexpr int kT = 200;
  LiveStats& st = s.stats;

  double d = 0.0;
  if (s.ipc.GetDouble("time-pos", &d, kT)) st.pos = d;
  if (s.ipc.GetDouble("duration", &d, kT)) st.dur = d;
  if (s.ipc.GetDouble("volume", &d, kT)) st.volume = d;
  if (s.ipc.GetDouble("speed", &d, kT)) st.speed = d;

  bool b = false;
  if (s.ipc.GetBool("pause", &b, kT)) st.paused = b;
  if (s.ipc.GetBool("mute", &b, kT)) st.mute = b;
  // eof-reached 比"用户看到的结束"更早为真：视频流比音频短的老片（很常见，
  // 片尾一条黑场加长音频）会在 32/40 秒就报 EOF。此时按空格应该是"继续播放"
  // 而不是"从头重播"，所以要求位置确实到片尾才认这个状态。
  if (s.ipc.GetBool("eof-reached", &b, kT)) {
    st.eof = b && (st.dur <= 1.0 || st.pos >= st.dur - 3.0);
  }

  // 进度记忆依赖这两个值，即使界面全关也照采（成本只有两次 IPC）。
  s.last_pos = st.pos;
  s.last_dur = st.dur;

  s.stat_tick = (s.stat_tick + 1) % 2;
  if (s.stat_tick == 0) {
    // 帧率优先取 container-fps（片源标称帧率），而不是 estimated-vf-fps。
    // 后者是"当前实际输出帧率"，在 3:2 pulldown 反交错后会在 23.976/29.97 之间
    // 跳变，用户看到"帧率 14.8"只会以为是播放器出了问题。
    double cfps = 0.0;
    if (s.ipc.GetDouble("container-fps", &cfps, kT) && cfps > 0.1) {
      st.fps = cfps;
    } else if (s.ipc.GetDouble("estimated-vf-fps", &d, kT)) {
      st.fps = d;
    }
    if (s.ipc.GetDouble("frame-drop-count", &d, kT)) st.drops = d;
    if (s.ipc.GetDouble("decoder-frame-drop-count", &d, kT)) st.dec_drops = d;
    if (s.ipc.GetDouble("video-params/w", &d, kT)) st.vw = d;
    if (s.ipc.GetDouble("video-params/h", &d, kT)) st.vh = d;
    if (s.ipc.GetDouble("video-params/aspect", &d, kT)) st.aspect = d;
    double vb = 0.0, ab = 0.0;
    if (s.ipc.GetDouble("video-bitrate", &vb, kT)) st.bitrate = vb;
    if (s.ipc.GetDouble("audio-bitrate", &ab, kT)) st.bitrate += ab;
    // 有些容器（尤其是 TS/老 AVI）不报码率，用文件大小/时长兜底
    if (st.bitrate <= 0.0 && st.dur > 1.0) {
      double fsz = 0.0;
      if (s.ipc.GetDouble("file-size", &fsz, kT) && fsz > 0.0) {
        st.bitrate = fsz * 8.0 / st.dur;
      }
    }
    s.ipc.GetString("video-params/pixelformat", &st.pixfmt, kT);
    s.ipc.GetString("hwdec-current", &st.hwdec, kT);
    s.ipc.GetString("video-codec", &st.vcodec, kT);
    s.ipc.GetString("audio-codec", &st.acodec, kT);
    std::string title;
    if (s.ipc.GetString("media-title", &title, kT) && !title.empty()) st.title = title;
  }

  // 同步给绘制层
  s.ui.pos = st.pos;
  s.ui.dur = st.dur;
  s.ui.paused = st.paused;
  s.ui.eof = st.eof;
  s.ui.volume = st.volume;
  s.ui.speed = st.speed;
  s.ui.muted = st.mute;
  s.ui.sidebar_on = s.sidebar_on;
  s.ui.chain_on = s.processed;
  if (!s.ui.dragging_seek) s.ui.drag_pos = st.pos;
}

// ---------------------------------------------------------------------------
// 快照 → 界面文本
// ---------------------------------------------------------------------------
std::wstring AspectText(double a) {
  if (!(a > 0.1)) return L"—";
  struct Entry {
    double v;
    const wchar_t* t;
  };
  static const Entry kList[] = {
      {4.0 / 3.0, L"4:3"},  {16.0 / 9.0, L"16:9"}, {3.0 / 2.0, L"3:2"},
      {5.0 / 4.0, L"5:4"},  {2.35, L"2.35:1"},     {2.39, L"2.39:1"},
      {1.85, L"1.85:1"},    {1.66, L"1.66:1"},
  };
  for (const Entry& e : kList) {
    if (std::fabs(a - e.v) < 0.02) return e.t;
  }
  wchar_t buf[32];
  swprintf(buf, 32, L"%.2f:1", a);
  return buf;
}

std::wstring ChainSummary() {
  PlayerState& s = *g_state;
  if (!s.has_analysis) return L"分析中…";
  if (!s.processed) return L"已旁路（原片）";
  std::wstring t = FieldModeZh(s.chain.field_mode);
  if (s.chain.assumed_field_order == lumen::FieldOrder::TopFieldFirst) t += L"（上场优先）";
  if (s.chain.assumed_field_order == lumen::FieldOrder::BottomFieldFirst) t += L"（下场优先）";
  if (s.chain.deblock) t += L" + 去块";
  if (s.chain.denoise != lumen::DenoiseMode::Off) t += L" + 降噪";
  if (s.chain.field_mode == lumen::FieldMode::Passthrough && !s.chain.deblock &&
      s.chain.denoise == lumen::DenoiseMode::Off) {
    t += L"（无需处理）";
  }
  return t;
}

void BuildMediaInfo() {
  PlayerState& s = *g_state;
  const LiveStats& st = s.stats;
  lumen::ui::MediaInfo& M = s.info;

  M.has_media = !s.current_file.empty();
  M.has_analysis = s.has_analysis;

  const std::string raw_title = st.title.empty() ? FileNameOf(s.current_file) : st.title;
  M.title = Utf8ToWide(raw_title);

  if (!M.has_media) {
    M.status = L"就绪";
  } else if (st.eof) {
    M.status = L"播放结束";
  } else if (st.paused) {
    M.status = L"已暂停";
  } else {
    M.status = L"正在播放";
  }

  M.duration = M.has_media ? lumen::ui::FormatClock(st.dur) : std::wstring();

  // 分辨率：优先用 mpv 实时值；还没解码出来时退回自检报告
  if (st.vw > 1 && st.vh > 1) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%dx%d", static_cast<int>(st.vw), static_cast<int>(st.vh));
    M.resolution = buf;
  } else if (s.has_analysis && s.report.info.width > 0) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%dx%d", s.report.info.width, s.report.info.height);
    M.resolution = buf;
  } else {
    M.resolution.clear();
  }

  M.aspect = AspectText(st.aspect);

  if (st.fps > 0.1) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%.3f 帧/秒", st.fps);
    M.fps = buf;
  } else {
    M.fps.clear();
  }

  M.vcodec = CodecZh(st.vcodec);
  M.acodec = CodecZh(st.acodec);
  M.bitrate = lumen::ui::FormatBitrate(st.bitrate);
  M.hwdec = HwdecZh(st.hwdec);

  if (s.has_analysis) {
    M.cadence = CadenceZh(s.report.cadence.cadence);
    M.chain = ChainSummary();
    M.field_order = FieldOrderZh(s.chain.assumed_field_order);
    M.field_order +=
        (s.field_override != lumen::FieldOrder::Unknown) ? L"（人工）" : L"（自动）";
    M.color = ColorSpaceZh(s.chain.source_space) + L" → BT.709";
    std::wstring pre;
    if (s.chain.deblock) pre += L"去块 ";
    if (s.chain.denoise != lumen::DenoiseMode::Off) pre += L"降噪 ";
    M.preprocess = pre.empty() ? L"无" : pre;
    M.warning = s.report.warnings.empty() ? L"" : Utf8ToWide(s.report.warnings.front());
  } else {
    M.cadence.clear();
    M.chain.clear();
    M.field_order.clear();
    M.color.clear();
    M.preprocess.clear();
    M.warning.clear();
  }

  // 超分：这一版实时路径确实没接 GPU 超分，必须说清楚，不能让用户以为已经是 1080p
  if (s.output_height > 0) {
    wchar_t buf[64];
    swprintf(buf, 64, L"缩放至 %dp", s.output_height);
    M.super_res = buf;
  } else {
    M.super_res = L"未接入（原分辨率）";
  }

  // 底部按钮上的当前画质摘要
  {
    std::wstring q = (s.output_height > 0) ? (std::to_wstring(s.output_height) + L"p")
                                           : std::wstring(L"原生");
    q += L" · ";
    switch (s.aspect_mode) {
      case 1: q += L"拉伸"; break;
      case 2: q += L"裁剪"; break;
      case 3: q += L"原始像素"; break;
      default: q += L"适应"; break;
    }
    M.quality = q;
  }

  M.chain_label = s.processed ? L"处理：开" : L"处理：原片";
  M.field_label = L"场序：";
  M.field_label += FieldOrderZh(s.field_override);
}

// ---------------------------------------------------------------------------
// 播放控制
// ---------------------------------------------------------------------------
void SeekAbsolute(double t) {
  PlayerState& s = *g_state;
  if (!s.ipc.connected()) return;
  if (t < 0.0) t = 0.0;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.3f", t);
  // absolute+exact：拖动进度条时用户要的是"就到这一帧"，
  // 默认的关键帧对齐会跳到几秒之前，体感像"拖不准"。
  s.ipc.Command({"seek", buf, "absolute+exact"});
}

void SeekRelative(double delta) {
  PlayerState& s = *g_state;
  if (!s.ipc.connected()) return;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%+.2f", delta);
  s.ipc.Command({"seek", buf, "relative"});
}

void TogglePause() {
  PlayerState& s = *g_state;
  if (!s.ipc.connected()) return;
  // keep-open 停在片尾时，"播放"的语义是从头重播，而不是把一个已经结束的
  // 播放器解暂停（那样会停在最后一帧不动，用户会以为卡死了）。
  if (s.stats.eof) {
    SeekAbsolute(0.0);
    s.ipc.SetProperty("pause", "no");
    s.stats.paused = false;
    s.stats.eof = false;
    return;
  }
  // ★ 用"显式置位"而不是 cycle pause：
  //   cycle 依赖 mpv 内部的当前值，一旦我们本地缓存和它不同步（比如刚 seek 完
  //   属性还没刷新），点一下会变成"没有变化"或"停两次"。显式写目标值可预测，
  //   而且能立刻把结果反映到界面上，不用等下一个 250ms 采样。
  const bool next = !s.stats.paused;
  std::string err;
  if (s.ipc.SetProperty("pause", next ? "yes" : "no", &err) != lumen::Status::Ok) {
    return;
  }
  s.stats.paused = next;
  s.ui.paused = next;
  // 暂停状态变化后立刻重画：等定时器刷新的那 250ms 里，用户会觉得"点了没反应"。
  BuildMediaInfo();
  RepaintChrome();
}

void SetVolumeBy(double value) {
  PlayerState& s = *g_state;
  if (!s.ipc.connected()) return;
  if (value < 0.0) value = 0.0;
  if (value > 130.0) value = 130.0;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.0f", value);
  s.ipc.SetProperty("volume", buf);
  if (s.stats.mute && value > 0.0) s.ipc.SetProperty("mute", "no");
  s.stats.volume = value;
  if (s.stats.mute && value > 0.0) s.stats.mute = false;
  s.ui.volume = value;
  RepaintChrome();
}

void SetSpeedBy(double v) {
  PlayerState& s = *g_state;
  if (!s.ipc.connected()) return;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.3f", v);
  s.ipc.SetProperty("speed", buf);
  s.stats.speed = v;
  s.ui.speed = v;
  char osd[64];
  std::snprintf(osd, sizeof(osd), "播放速度 %.2fx", v);
  Osd(osd);
  RepaintChrome();
}

// ---------------------------------------------------------------------------
// 画面设置
// ---------------------------------------------------------------------------
// 「画面比例」四个模式对应 mpv 的三组属性：
//   keepaspect 控制是否保持宽高比；panscan 控制裁掉溢出部分；
//   video-unscaled 控制是否禁止缩放（1:1 像素）。
// 这四个是用户真正能看出差别的组合，比暴露一堆 zoom/pan 参数实用。
void ApplyAspectMode(int mode) {
  PlayerState& s = *g_state;
  s.aspect_mode = mode;
  if (!s.ipc.connected()) return;

  switch (mode) {
    case 1: // 拉伸铺满
      s.ipc.SetProperty("keepaspect", "no");
      s.ipc.SetProperty("panscan", "0");
      s.ipc.SetProperty("video-unscaled", "no");
      break;
    case 2: // 裁剪填满
      s.ipc.SetProperty("keepaspect", "yes");
      s.ipc.SetProperty("panscan", "1.0");
      s.ipc.SetProperty("video-unscaled", "no");
      break;
    case 3: // 原始像素
      s.ipc.SetProperty("keepaspect", "yes");
      s.ipc.SetProperty("panscan", "0");
      s.ipc.SetProperty("video-unscaled", "yes");
      break;
    default: // 适应窗口
      s.ipc.SetProperty("keepaspect", "yes");
      s.ipc.SetProperty("panscan", "0");
      s.ipc.SetProperty("video-unscaled", "no");
      break;
  }
  static const wchar_t* kNames[] = {L"保持比例（适应窗口）", L"拉伸铺满", L"裁剪填满",
                                    L"原始像素（不缩放）"};
  Osd(WideToUtf8(kNames[mode < 0 || mode > 3 ? 0 : mode]));
  BuildMediaInfo();
  RepaintChrome();
}

// 按「视频原始像素 × 倍率」调整整个窗口。
// 为什么要算外框：SetWindowPos 的宽高是含标题栏与边框的，直接用视频尺寸
// 会让视频区比预期小一圈（边框 + 标题栏 + 侧栏 + 控制条都要加回来）。
void ApplyWindowScale(double scale, bool maximize) {
  PlayerState& s = *g_state;
  if (!s.hwnd) return;
  if (s.fullscreen) ToggleFullscreen();
  if (maximize) {
    ShowWindow(s.hwnd, SW_MAXIMIZE);
    return;
  }

  if (s.stats.vh < 1.0) {
    Osd("先打开一个视频文件，再按分辨率调整窗口");
    return;
  }

  const double aspect =
      (s.stats.aspect > 0.1) ? s.stats.aspect : (s.stats.vw / s.stats.vh);
  const int dw = static_cast<int>(s.stats.vh * aspect * scale + 0.5);
  const int dh = static_cast<int>(s.stats.vh * scale + 0.5);

  int want_w = dw + s.layout.sidebar_w;
  int want_h = dh + s.layout.controls_h;

  RECT wr{}, cr{};
  GetWindowRect(s.hwnd, &wr);
  GetClientRect(s.hwnd, &cr);
  const int frame_w = (wr.right - wr.left) - cr.right;
  const int frame_h = (wr.bottom - wr.top) - cr.bottom;
  int out_w = want_w + frame_w;
  int out_h = want_h + frame_h;

  // 不允许超出当前显示器工作区（否则标题栏跑到屏幕外，用户就拖不回来了）
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(MonitorFromWindow(s.hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
    const int max_w = mi.rcWork.right - mi.rcWork.left;
    const int max_h = mi.rcWork.bottom - mi.rcWork.top;
    if (out_w > max_w) out_w = max_w;
    if (out_h > max_h) out_h = max_h;
  }

  const int cx = (wr.left + wr.right) / 2;
  const int cy = (wr.top + wr.bottom) / 2;
  SetWindowPos(s.hwnd, nullptr, cx - out_w / 2, cy - out_h / 2, out_w, out_h,
               SWP_NOZORDER | SWP_NOACTIVATE);
  s.has_sized_window = true;

  wchar_t osd[96];
  swprintf(osd, 96, L"窗口 %d×%d（视频区 %d×%d）", out_w, out_h, dw, dh);
  Osd(WideToUtf8(osd));
}

void ApplyOutputHeight(int h) {
  PlayerState& s = *g_state;
  s.output_height = h;
  ApplyChain(s.processed);
  BuildMediaInfo();
  RepaintChrome();
}

void HandleMenuCommand(int cmd) {
  PlayerState& s = *g_state;
  switch (cmd) {
    case kCmdSizeFit: ApplyWindowScale(1.0, false); break;
    case kCmdSize100: ApplyWindowScale(1.0, false); break;
    case kCmdSize150: ApplyWindowScale(1.5, false); break;
    case kCmdSize200: ApplyWindowScale(2.0, false); break;
    case kCmdSizeMax: ApplyWindowScale(1.0, true); break;
    case kCmdAspectFit: ApplyAspectMode(0); break;
    case kCmdAspectStretch: ApplyAspectMode(1); break;
    case kCmdAspectCrop: ApplyAspectMode(2); break;
    case kCmdAspectNative: ApplyAspectMode(3); break;
    case kCmdOutNative: ApplyOutputHeight(0); break;
    case kCmdOut720: ApplyOutputHeight(720); break;
    case kCmdOut1080: ApplyOutputHeight(1080); break;
    case kCmdOut1440: ApplyOutputHeight(1440); break;
    case kCmdOut2160: ApplyOutputHeight(2160); break;
    case kCmdSpeed05: SetSpeedBy(0.5); break;
    case kCmdSpeed075: SetSpeedBy(0.75); break;
    case kCmdSpeed10: SetSpeedBy(1.0); break;
    case kCmdSpeed125: SetSpeedBy(1.25); break;
    case kCmdSpeed15: SetSpeedBy(1.5); break;
    case kCmdSpeed20: SetSpeedBy(2.0); break;
    case kCmdSidebar:
      s.sidebar_on = !s.sidebar_on;
      UpdateLayout();
      RepaintAll();
      Osd(s.sidebar_on ? "信息栏已显示" : "信息栏已隐藏");
      break;
    case kCmdHud:
      HudSetVisible(!s.hud_on);
      Osd(s.hud_on ? "性能浮层已开启" : "性能浮层已关闭");
      break;
    case kCmdFieldCycle: CycleFieldOrder(); break;
    case kCmdChainToggle: ApplyChain(!s.processed); break;
    case kCmdOpenFile: OpenFileDialog(); break;
    case kCmdAbout: ShowAbout(s.hwnd); break;
    default: break;
  }
}

void ShowSettingsMenu(HWND hwnd, int client_x, int client_y) {
  PlayerState& s = *g_state;
  POINT pt{client_x, client_y};
  ClientToScreen(hwnd, &pt);

  // 鼠标在窗口里就用鼠标位置：菜单贴着鼠标弹出最符合直觉，
  // 而且不会出现"菜单从右下角斜着展开、盖住半边界面"的怪异观感。
  {
    POINT cur{};
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    if (GetCursorPos(&cur) && PtInRect(&wr, cur)) pt = cur;
  }

  // ★ TrackPopupMenu 要求 owner 窗口是前台窗口，否则它会立刻返回 0，
  //   菜单一闪都不闪。这一步不能省——"点齿轮没反应"就是这么来的。
  SetForegroundWindow(hwnd);

  HMENU m = CreatePopupMenu();
  HMENU size_menu = CreatePopupMenu();
  HMENU aspect_menu = CreatePopupMenu();
  HMENU out_menu = CreatePopupMenu();
  HMENU speed_menu = CreatePopupMenu();

  AppendMenuW(size_menu, MF_STRING | (s.has_sized_window ? 0 : MF_CHECKED), kCmdSize100,
              L"按视频原始像素（100%）");
  AppendMenuW(size_menu, MF_STRING, kCmdSizeFit, L"适应视频（回到 100%）");
  AppendMenuW(size_menu, MF_STRING, kCmdSize150, L"150%");
  AppendMenuW(size_menu, MF_STRING, kCmdSize200, L"200%");
  AppendMenuW(size_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(size_menu, MF_STRING, kCmdSizeMax, L"最大化窗口");

  const int aspect_ids[] = {kCmdAspectFit, kCmdAspectStretch, kCmdAspectCrop,
                            kCmdAspectNative};
  static const wchar_t* kAspectNames[] = {L"保持比例（适应窗口）", L"拉伸铺满",
                                          L"裁剪填满", L"原始像素（不缩放）"};
  for (int i = 0; i < 4; ++i) {
    const UINT flags = MF_STRING | (s.aspect_mode == i ? MF_CHECKED : 0);
    AppendMenuW(aspect_menu, flags, aspect_ids[i], kAspectNames[i]);
  }

  const int out_ids[] = {kCmdOutNative, kCmdOut720, kCmdOut1080, kCmdOut1440, kCmdOut2160};
  const int out_vals[] = {0, 720, 1080, 1440, 2160};
  static const wchar_t* kOutNames[] = {L"原生（不缩放）", L"720p", L"1080p", L"1440p",
                                       L"2160p（4K）"};
  for (int i = 0; i < 5; ++i) {
    const UINT flags = MF_STRING | (s.output_height == out_vals[i] ? MF_CHECKED : 0);
    AppendMenuW(out_menu, flags, out_ids[i], kOutNames[i]);
  }

  const int speed_ids[] = {kCmdSpeed05, kCmdSpeed075, kCmdSpeed10,
                           kCmdSpeed125, kCmdSpeed15, kCmdSpeed20};
  const double speed_vals[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
  for (int i = 0; i < 6; ++i) {
    wchar_t label[32];
    // 0.75 / 1.25 这两档用 %.1f 会被四舍五入成 0.8 / 1.3，
    // 菜单上写的值和实际设给 mpv 的值就对不上了。整数档保留一位小数
    // （和控制条上的 1.0× 口径一致），两位小数的档位如实显示。
    const double v = speed_vals[i];
    // 只有 0.75 / 1.25 需要两位小数；用「分值是否为 10 的倍数」判断，
    // 避免 std::round(0.5)=1 把 0.5 误判成两位小数写成 0.50。
    const int cents = static_cast<int>(std::round(v * 100.0));
    swprintf(label, 32, (cents % 10 == 0) ? L"%.1f 倍速" : L"%.2f 倍速", v);
    const UINT flags = MF_STRING | (std::fabs(s.stats.speed - v) < 0.01 ? MF_CHECKED : 0);
    AppendMenuW(speed_menu, flags, speed_ids[i], label);
  }

  AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(size_menu), L"窗口尺寸");
  AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(aspect_menu), L"画面比例");
  AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(out_menu), L"输出分辨率");
  AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(speed_menu), L"播放速度");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (s.sidebar_on ? MF_CHECKED : 0), kCmdSidebar, L"显示信息侧栏");
  AppendMenuW(m, MF_STRING | (s.hud_on ? MF_CHECKED : 0), kCmdHud, L"性能浮层");
  // 场序直接显示"当前是什么"，比写死一串英文缩写有用：
  // 点一次就换一个值，用户看着标签就能知道现在挂的是哪一档。
  AppendMenuW(m, MF_STRING, kCmdFieldCycle,
              (L"场序：" + FieldOrderZh(s.field_override) + L"（点击切换）").c_str());
  AppendMenuW(m, MF_STRING | (s.processed ? MF_CHECKED : 0), kCmdChainToggle,
              L"画面处理效果（关掉看原片）");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, kCmdOpenFile, L"打开文件…");
  AppendMenuW(m, MF_STRING, kCmdAbout, L"关于 LumenPlayer");

  // TPM_BOTTOMALIGN：菜单以 pt 为底边向上展开，这样不会盖住控制条上的按钮。
  // 靠近屏幕顶部时 Windows 会自动改成向下展开。
  const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN, pt.x,
                                 pt.y - 6, 0, hwnd, nullptr);
  DestroyMenu(m); // 子菜单随父菜单一起销毁

  if (cmd != 0) HandleMenuCommand(cmd);
  // 菜单关掉后鼠标可能停在新弹出的位置，重新算一次 hover 避免高亮留在旧按钮上
  POINT cur{};
  GetCursorPos(&cur);
  ScreenToClient(hwnd, &cur);
  g_state->ui.hover = lumen::ui::HitTest(g_state->layout, g_state->ui, cur.x, cur.y);
  RepaintChrome();
}

// ---------------------------------------------------------------------------
// 鼠标
// ---------------------------------------------------------------------------
void ActivateHit(int hit) {
  switch (hit) {
    case lumen::ui::kHitPlayPause: TogglePause(); break;
    case lumen::ui::kHitBack: SeekRelative(-10.0); Osd("-10 秒"); break;
    case lumen::ui::kHitForward: SeekRelative(10.0); Osd("+10 秒"); break;
    case lumen::ui::kHitOpen: OpenFileDialog(); break;
    case lumen::ui::kHitFullscreen: ToggleFullscreen(); break;
    case lumen::ui::kHitChain: ApplyChain(!g_state->processed); break;
    case lumen::ui::kHitFieldOrder: CycleFieldOrder(); break;
    case lumen::ui::kHitSettings: {
      PlayerState& s = *g_state;
      // 从"设置"按钮下方弹出（侧栏入口也一样），避免菜单盖住按钮本身
      RECT rc{};
      GetClientRect(s.hwnd, &rc);
      ShowSettingsMenu(s.hwnd, rc.right - s.layout.m.px(40), rc.bottom - s.layout.m.px(60));
      break;
    }
    default: break;
  }
}

void HandleMouseMove(int x, int y) {
  PlayerState& s = *g_state;

  if (s.ui.dragging_seek) {
    s.ui.drag_pos = lumen::ui::SeekTimeAt(s.layout, x, s.stats.dur);
    RepaintChrome();
    return;
  }
  if (s.ui.dragging_volume) {
    SetVolumeBy(lumen::ui::VolumeAt(s.layout, x));
    return;
  }

  const int hit = lumen::ui::HitTest(s.layout, s.ui, x, y);
  if (hit != s.ui.hover) {
    s.ui.hover = hit;
    RepaintChrome();
  }
}

void HandleLButtonDown(int x, int y) {
  PlayerState& s = *g_state;
  const int hit = lumen::ui::HitTest(s.layout, s.ui, x, y);
  s.ui.pressed = hit;

  if (hit == lumen::ui::kHitSeekBar) {
    s.ui.dragging_seek = true;
    s.ui.drag_pos = lumen::ui::SeekTimeAt(s.layout, x, s.stats.dur);
    SetCapture(s.hwnd);
    RepaintChrome();
    return;
  }
  if (hit == lumen::ui::kHitVolume) {
    s.ui.dragging_volume = true;
    SetCapture(s.hwnd);
    SetVolumeBy(lumen::ui::VolumeAt(s.layout, x));
    return;
  }
  RepaintChrome();
}

void HandleLButtonUp(int x, int y) {
  PlayerState& s = *g_state;

  if (s.ui.dragging_seek) {
    const double target = s.ui.drag_pos;
    s.ui.dragging_seek = false;
    ReleaseCapture();
    SeekAbsolute(target);
    RepaintChrome();
    return;
  }
  if (s.ui.dragging_volume) {
    s.ui.dragging_volume = false;
    ReleaseCapture();
    return;
  }

  const int hit = lumen::ui::HitTest(s.layout, s.ui, x, y);
  const int pressed = s.ui.pressed;
  s.ui.pressed = 0;

  // 视频区：先别急着暂停，等 1/4 秒确认不是双击。
  // 不等的话"双击全屏"会先触发一次暂停，用户看到的是画面停了一下才放大。
  if (hit == lumen::ui::kHitVideoArea && pressed == hit) {
    if (!s.info.has_media) {
      OpenFileDialog(); // 欢迎页的大按钮落在这里
      return;
    }
    s.pending_video_click = true;
    SetTimer(s.hwnd, kClickTimerId, GetDoubleClickTime() / 2, nullptr);
    return;
  }

  if (hit == pressed && hit != lumen::ui::kHitNone) ActivateHit(hit);
  RepaintChrome();
}

// 返回 true 表示这个滚轮事件已被处理
bool HandleMouseWheel(short delta) {
  PlayerState& s = *g_state;
  const double step = (delta > 0) ? 5.0 : -5.0;
  SetVolumeBy(s.stats.volume + step);
  wchar_t buf[64];
  swprintf(buf, 64, L"音量 %.0f%%", s.stats.volume);
  Osd(WideToUtf8(buf), 1200);
  return true;
}

// ---------------------------------------------------------------------------
// 自检线程
// ---------------------------------------------------------------------------
struct InspectJob {
  std::string path;
  lumen::InspectOptions opt;
  lumen::InspectionReport report;
  std::string error;
  bool ok = false;
  HWND notify = nullptr;
};

DWORD WINAPI InspectThreadProc(LPVOID param) {
  InspectJob* job = static_cast<InspectJob*>(param);
  job->ok = (lumen::InspectSource(job->path, job->opt, &job->report, &job->error) ==
             lumen::Status::Ok);
  PostMessageW(job->notify, WM_APP_INSPECT_DONE, 0, reinterpret_cast<LPARAM>(job));
  return 0;
}

void StartInspection(const std::string& path) {
  PlayerState& s = *g_state;
  InspectJob* job = new InspectJob();
  job->path = path;
  job->opt.preset = lumen::QualityPreset::Balanced;
  job->opt.anime = s.cfg.anime;
  job->opt.ffmpeg_path = s.cfg.ffmpeg_path;
  job->opt.ffprobe_path = s.cfg.ffprobe_path;
  // 配置里的默认场序直接交给自检：这样"自检报告"与"实际挂载的滤镜"从一开始就一致，
  // 不会出现报告说 tff、画面按 bff 处理的分裂。
  job->opt.field_order_override = s.cfg_field_order;
  job->notify = s.hwnd;

  // 用 CreateThread 而不是 std::thread：MinGW 的 posix 线程模型需要额外链接参数，
  // 而这里只是跑一个"分析完就退出"的任务，Win32 原生线程足够且零依赖。
  HANDLE th = CreateThread(nullptr, 0, InspectThreadProc, job, 0, nullptr);
  if (th) {
    CloseHandle(th);
    SetStatus(FileNameOf(path) + "  |  正在分析片源…");
  } else {
    delete job;
  }
}

// ---------------------------------------------------------------------------
// 打开文件
// ---------------------------------------------------------------------------
void OpenFile(const std::string& path) {
  PlayerState& s = *g_state;
  if (!FileExists(path)) {
    MessageBoxW(s.hwnd, (L"文件不存在：\n" + Utf8ToWide(path)).c_str(), L"LumenPlayer",
                MB_ICONWARNING);
    return;
  }
  // 换片之前先把上一条的进度写下来，否则"打开新文件"会丢掉旧文件的记忆。
  if (!s.current_file.empty()) RememberCurrentPosition();

  s.current_file = path;
  s.has_analysis = false;
  s.pending_resume = 0.0;
  s.pending_video_click = false;
  KillTimer(s.hwnd, kClickTimerId);
  // 清掉上一部片子的实时数据：不清的话，新片加载前那一两秒侧栏会显示
  // 上一部片子的 1920x1080 与码率，看起来像"这部片子分辨率识别错了"。
  s.stats = LiveStats();
  s.ui.dragging_seek = false;
  s.ui.dragging_volume = false;

  // 先把画面放出来，让用户立刻看到东西；分析在后台跑，出来再套滤镜。
  // 反过来（先分析再播放）会让用户盯着黑屏好几秒，体感像卡死。
  std::string err;
  if (s.ipc.Command({"loadfile", path}, &err) != lumen::Status::Ok) {
    MessageBoxW(s.hwnd, (L"无法播放该文件：\n" + Utf8ToWide(err)).c_str(), L"LumenPlayer",
                MB_ICONERROR);
    return;
  }

  // 有了片子就把 mpv 的子窗口放出来（空闲时它是藏着的，否则会盖住欢迎页）
  EnsureVideoAttached();
  if (s.video) ShowWindow(s.video, SW_SHOW);
  UpdateLayout();

  // 进度恢复：此处不能立刻 seek（文件还没解析完，seek 会失败），
  // 只记下目标位置，由定时器在 duration 可读时执行。
  if (s.cfg.player.resume) {
    const std::map<std::string, ResumeEntry> table = LoadResumeTable();
    const auto it = table.find(NormalizePathKey(path));
    if (it != table.end() && it->second.pos > 3.0) s.pending_resume = it->second.pos;
  }

  BuildMediaInfo();
  RepaintAll();
  StartInspection(path);
}

void OpenFileDialog() {
  PlayerState& s = *g_state;
  wchar_t file[MAX_PATH * 4] = {0};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = s.hwnd;
  ofn.lpstrFilter =
      L"视频文件\0*.mp4;*.mkv;*.avi;*.ts;*.m2ts;*.mpg;*.mpeg;*.wmv;*.flv;*.mov;*.rmvb\0"
      L"所有文件\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH * 4;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  if (GetOpenFileNameW(&ofn)) {
    OpenFile(WideToUtf8(file));
  }
}

// ---------------------------------------------------------------------------
// 窗口尺寸同步
// ---------------------------------------------------------------------------
// 说明：mpv 用 --wid 创建的子窗口不会自动跟随父窗口缩放，必须我们手动 MoveWindow。
// 这件事现在统一由 UpdateLayout() 做（它同时处理侧栏与控制条占位），
// 不再单独维护一份"把子窗口铺满客户区"的逻辑——两份布局代码必然跑偏。

// ---------------------------------------------------------------------------
// 全屏
// ---------------------------------------------------------------------------
void ToggleFullscreen() {
  PlayerState& s = *g_state;
  // WS_CLIPCHILDREN 是主窗口创建时就有的：父窗口重画时不能盖到 mpv 子窗口上。
  // 早先这里直接写死两个样式，来回切一次全屏就把它丢了，画面会闪。
  constexpr LONG_PTR kBaseStyle = WS_VISIBLE | WS_CLIPCHILDREN;
  if (!s.fullscreen) {
    s.saved_place.length = sizeof(s.saved_place);
    s.has_saved_place = GetWindowPlacement(s.hwnd, &s.saved_place) != FALSE;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromWindow(s.hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
      // ★ 标志必须先翻：下面 SetWindowPos 会同步发 WM_SIZE，那里要用
      //   fullscreen 计算布局。翻晚了会按"非全屏"排一次，画面先错一帧。
      s.fullscreen = true;
      SetWindowLongPtrW(s.hwnd, GWL_STYLE, WS_POPUP | kBaseStyle);
      SetWindowPos(s.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                   mi.rcMonitor.right - mi.rcMonitor.left,
                   mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    }
  } else {
    // 同理，退出时先翻标志，让恢复尺寸引发的 WM_SIZE 直接按窗口模式布局。
    s.fullscreen = false;
    SetWindowLongPtrW(s.hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | kBaseStyle);
    if (s.has_saved_place) SetWindowPlacement(s.hwnd, &s.saved_place);
    SetWindowPos(s.hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
  }
  // 全屏时侧栏自动收起（ComputeLayout 负责），控制条压薄一点。
  // 必须重新布局并重画：mpv 子窗口要跟着挪，HUD 也要重新贴位。
  UpdateLayout();
  RepaintAll();
  StartVideoRelayout();
}

// ---------------------------------------------------------------------------
// 场序人工覆盖（F-202 补充）
// ---------------------------------------------------------------------------
// 场序自动判定依赖行间差统计，在强噪点/高速运动镜头上会误判。
// 这里给一个"一键换场序"的开关：auto → tff → bff → auto。
// 判定是否正确的肉眼标准：反复按 P，哪种设置下画面横线抖动/锯齿最少就是它。
void ApplyFieldOverride(lumen::FieldOrder fo) {
  PlayerState& s = *g_state;
  s.field_override = fo;
  if (!s.has_analysis) {
    Osd(WideToUtf8(L"场序已设为「" + FieldOrderZh(fo) + L"」（打开文件后生效）"));
    return;
  }
  // 从"自检原始结论"重新推导，而不是在当前链上叠加——
  // 否则在 tff/bff 之间来回切几次，rationale 会被追问出好几条重复记录。
  s.chain = s.base_chain;
  if (fo != lumen::FieldOrder::Unknown) lumen::OverrideFieldOrder(fo, &s.chain);
  ApplyChain(s.processed);
  Osd(WideToUtf8(L"场序：" + FieldOrderZh(fo) +
                 (fo == lumen::FieldOrder::Unknown ? L"（恢复自动判定）"
                                                   : L"（已重新挂载滤镜）")));
}

void CycleFieldOrder() {
  PlayerState& s = *g_state;
  switch (s.field_override) {
    case lumen::FieldOrder::Unknown: ApplyFieldOverride(lumen::FieldOrder::TopFieldFirst); break;
    case lumen::FieldOrder::TopFieldFirst:
      ApplyFieldOverride(lumen::FieldOrder::BottomFieldFirst);
      break;
    case lumen::FieldOrder::BottomFieldFirst: ApplyFieldOverride(lumen::FieldOrder::Unknown); break;
    default: ApplyFieldOverride(lumen::FieldOrder::Unknown); break;
  }
}

// ---------------------------------------------------------------------------
// 键盘
// ---------------------------------------------------------------------------
void ShowInfo(); // 前置声明：HandleKey 里要用

void ShowHelp() {
  Osd(
      "快捷键\n"
      "空格 暂停/继续    ← → 快退/快进 5 秒\n"
      "Shift+← → 30 秒    PgUp / PgDn 1 分钟\n"
      "[ ] 减速/加速   \\ 恢复 1 倍速   . , 逐帧\n"
      "↑ ↓ 音量   M 静音   Home/End 片头片尾\n"
      "0-9 跳到 0%~90%   Tab 按住看原片\n"
      "A 处理↔原片   P 场序   I 性能浮层   S 信息栏\n"
      "F 全屏   O 打开文件   H 帮助   Q 退出",
      9000);
}

void HandleKey(WPARAM vk) {
  PlayerState& s = *g_state;
  if (!s.ipc.connected()) return;

  // 是否按着 Shift：用于"5 秒 / 30 秒"两档跳转。
  // 不用 GetKeyState(VK_LBUTTON) 那类全局状态，只关心当前按键修饰。
  const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

  switch (vk) {
    case VK_SPACE:
      // 走 TogglePause 而不是直接 cycle pause：片尾按空格应该是"重播"，
      // 而不是把一个已经结束的播放器解暂停然后卡在最后一帧。
      TogglePause();
      return;
    case VK_LEFT:
      SeekRelative(shift ? -30.0 : -5.0);
      return;
    case VK_RIGHT:
      SeekRelative(shift ? 30.0 : 5.0);
      return;
    case VK_PRIOR: // PageUp
      SeekRelative(60.0);
      Osd("+1 分钟");
      return;
    case VK_NEXT: // PageDown
      SeekRelative(-60.0);
      Osd("-1 分钟");
      return;
    case VK_HOME:
      SeekAbsolute(0.0);
      Osd("回到片头");
      return;
    case VK_END:
      if (s.stats.dur > 0.0) {
        SeekAbsolute(s.stats.dur - 2.0);
        Osd("跳到片尾");
      }
      return;
    case VK_UP:
      SetVolumeBy(s.stats.volume + 5.0);
      return;
    case VK_DOWN:
      SetVolumeBy(s.stats.volume - 5.0);
      return;
    case VK_TAB:
      // 按住看原片（F-305 的"按住对比"）。
      // 注意 WM_KEYDOWN 会因按住不放而连续触发，必须用 ab_hold 去重，
      // 否则每次重复都重挂一遍滤镜，画面会闪。
      if (!s.ab_hold) {
        s.ab_hold = true;
        s.ab_prev = s.processed;
        ApplyChain(false);
      }
      return;
    case VK_OEM_4: // [
      s.ipc.Command({"add", "speed", "-0.1"});
      Osd("减速");
      return;
    case VK_OEM_6: // ]
      s.ipc.Command({"add", "speed", "0.1"});
      Osd("加速");
      return;
    case VK_OEM_5: // 反斜杠：恢复常速
      SetSpeedBy(1.0);
      return;
    case VK_OEM_PERIOD: // . 逐帧前进
      s.ipc.Command({"frame-step"});
      return;
    case VK_OEM_COMMA: // , 逐帧后退
      s.ipc.Command({"frame-back-step"});
      return;
    case VK_ESCAPE:
      if (s.fullscreen) {
        ToggleFullscreen();
        return;
      }
      PostMessageW(s.hwnd, WM_CLOSE, 0, 0);
      return;
    case 'A':
      ApplyChain(!s.processed);
      return;
    case 'P':
      CycleFieldOrder();
      return;
    case 'M': {
      const bool next = !s.stats.mute;
      s.ipc.SetProperty("mute", next ? "yes" : "no");
      s.stats.mute = next;
      Osd(next ? "静音" : "取消静音");
      RepaintChrome();
      return;
    }
    case 'I':
      HudSetVisible(!s.hud_on);
      Osd(s.hud_on ? "性能浮层已开启" : "性能浮层已关闭");
      return;
    case 'S':
      s.sidebar_on = !s.sidebar_on;
      UpdateLayout();
      RepaintAll();
      Osd(s.sidebar_on ? "信息栏已显示" : "信息栏已隐藏");
      return;
    case 'H':
      ShowInfo();
      ShowHelp();
      return;
    case 'F':
      ToggleFullscreen();
      return;
    case 'O':
      OpenFileDialog();
      return;
    case 'Q':
      PostMessageW(s.hwnd, WM_CLOSE, 0, 0);
      return;
    default:
      // 0-9：按百分比跳转。长片里比连按 ← 快得多，也是所有播放器的通用约定。
      if (vk >= '0' && vk <= '9') {
        if (s.stats.dur > 1.0) {
          SeekAbsolute(s.stats.dur * (static_cast<double>(vk - '0') / 10.0));
        }
        return;
      }
      return;
  }
}

void ShowInfo() {
  PlayerState& s = *g_state;
  if (!s.has_analysis) {
    Osd("分析尚未完成…");
    return;
  }
  // OSD 一行显示不下，用换行拆成几段
  const lumen::SourceInfo& info = s.report.info;
  std::wstring text;
  text += L"片源：" + std::to_wstring(info.width) + L"×" + std::to_wstring(info.height) +
          L"  " + CadenceZh(s.report.cadence.cadence) + L"\n";
  text += L"场处理：" + FieldModeZh(s.chain.field_mode) + L"　场序：" +
          FieldOrderZh(s.chain.assumed_field_order) +
          (s.field_override != lumen::FieldOrder::Unknown ? L"（人工）" : L"（自动）") + L"\n";
  text += L"色彩：" + ColorSpaceZh(s.chain.source_space) + L" → BT.709\n";
  text += L"前处理：";
  if (s.chain.deblock) text += L"去块 ";
  if (s.chain.denoise != lumen::DenoiseMode::Off) text += L"降噪";
  if (!s.chain.deblock && s.chain.denoise == lumen::DenoiseMode::Off) text += L"无";
  text += L"\nAI 超分：尚未接入（当前播放原分辨率）";
  Osd(WideToUtf8(text), 6000);
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      DragAcceptFiles(hwnd, TRUE);
      return 0;

    case WM_GETMINMAXINFO: {
      // 太小会连标题栏都挤掉，限制一个下限
      MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lp);
      mmi->ptMinTrackSize.x = kMinWidth;
      mmi->ptMinTrackSize.y = kMinHeight;
      return 0;
    }

    case WM_SIZE:
      // 只改内部布局，不碰窗口本身：WM_SIZE 是在系统已经调整完窗口后发的，
      // 这里再 SetWindowPos 会触发第二轮 WM_SIZE（经典的重入抖动）。
      UpdateLayout();
      RepaintChrome();
      // mpv 随后会在自己的线程里把子窗口拉满整块客户区，必须延后夺回。
      StartVideoRelayout();
      return 0;

    case WM_MOVE:
      HudReposition();
      return 0;

    case WM_TIMER:
      if (wp == kUiTimerId) {
        AttemptPendingResume(); // 文件解析完后才能 seek，借同一个定时器做这件事
        EnsureVideoAttached();  // mpv 可能重建过窗口，句柄失效时要重新挂接
        RefreshPlaybackState();
        BuildMediaInfo();
        HudRefresh();
        RepaintChrome();
        return 0;
      }
      if (wp == kRelayoutTimerId) {
        // 抢回被 mpv 拉满的视频区。重试有上限，窗口尺寸稳定后就停。
        PlayerState& s = *g_state;
        UpdateLayout();
        RepaintChrome();
        if (--s.relayout_retries > 0) {
          SetTimer(hwnd, kRelayoutTimerId, kRelayoutIntervalMs, nullptr);
        } else {
          KillTimer(hwnd, kRelayoutTimerId);
        }
        return 0;
      }
      if (wp == kClickTimerId) {
        // 单击视频区的"暂停"延迟到这里执行：给了双击（全屏）一个取消窗口。
        KillTimer(hwnd, kClickTimerId);
        PlayerState& s = *g_state;
        if (s.pending_video_click) {
          s.pending_video_click = false;
          TogglePause();
        }
        return 0;
      }
      return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_KEYDOWN:
      HandleKey(wp);
      return 0;

    case WM_KEYUP:
      // 松开 Tab 恢复"处理后"。放在 KEYUP 而不是再按一次，
      // 是为了让"按住对比"符合直觉：手指抬起 = 恢复增强。
      if (wp == VK_TAB) {
        PlayerState& s = *g_state;
        if (s.ab_hold) {
          s.ab_hold = false;
          ApplyChain(s.ab_prev); // 恢复到按住之前的状态，而不是无脑设成"处理后"
        }
      }
      return 0;

    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      if (rc.right > 0 && rc.bottom > 0 && g_state) {
        // 双缓冲：先把整个客户区画进内存位图，再一次性 BitBlt。
        // 直接往窗口 DC 上画的话，控制条每 250ms 刷新一次就会看见明显闪烁
        // （每个矩形、每段文字都是独立的 GDI 调用，中间状态会被显示出来）。
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        if (mem && bmp) {
          HGDIOBJ old = SelectObject(mem, bmp);
          lumen::ui::Paint(mem, g_state->layout, g_state->info, g_state->ui);
          BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
          SelectObject(mem, old);
        }
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
      }
      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_MOUSEMOVE:
      if (g_state) HandleMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;

    case WM_LBUTTONDOWN:
      SetFocus(hwnd); // 点界面任何地方都把键盘焦点拉回主窗口
      if (g_state) HandleLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;

    case WM_LBUTTONUP:
      if (g_state) HandleLButtonUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;

    case WM_LBUTTONDBLCLK: {
      KillTimer(hwnd, kClickTimerId);
      if (!g_state) return 0;
      g_state->pending_video_click = false;
      const int x = GET_X_LPARAM(lp);
      const int y = GET_Y_LPARAM(lp);
      // 双击视频区 = 全屏；双击控制条/侧栏不做任何事（否则误触很容易）
      if (g_state->layout.video.contains(x, y)) ToggleFullscreen();
      return 0;
    }

    case WM_RBUTTONUP:
      // 右键任意位置弹设置菜单：这是"找不到设置入口"时的兜底路径，
      // 比让用户去猜右下角那个图标要可靠。
      ShowSettingsMenu(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;

    case WM_MOUSEWHEEL:
      if (g_state) HandleMouseWheel(GET_WHEEL_DELTA_WPARAM(wp));
      return 0;

    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT && g_state) {
        POINT p{};
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
        const int hit = lumen::ui::HitTest(g_state->layout, g_state->ui, p.x, p.y);
        const bool clickable =
            (hit != lumen::ui::kHitNone) && (hit != lumen::ui::kHitVideoArea);
        // 为什么不用 IDC_HAND：它在非 Unicode 目标下展开成 MAKEINTRESOURCEA，
        // 传给 LoadCursorW 会编译失败（MinGW 尤其严格）。直接用 ID 最稳。
        SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(clickable ? 32649 : 32512)));
        return TRUE;
      }
      return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_DROPFILES: {
      HDROP drop = reinterpret_cast<HDROP>(wp);
      wchar_t path[MAX_PATH * 4] = {0};
      if (DragQueryFileW(drop, 0, path, MAX_PATH * 4) > 0) {
        OpenFile(WideToUtf8(path));
      }
      DragFinish(drop);
      return 0;
    }

    case WM_APP_INSPECT_DONE: {
      InspectJob* job = reinterpret_cast<InspectJob*>(lp);
      if (job) {
        PlayerState& s = *g_state;
        if (job->ok) {
          // 用户在"分析中…"那几秒里按过 P 的话，pending 就是他的选择——
          // 不能在分析结束时被配置默认值覆盖掉，否则表现为"按了没反应"。
          const lumen::FieldOrder pending = s.field_override;
          s.report = job->report;
          s.base_chain = job->report.chain;
          s.chain = job->report.chain;
          s.field_override =
              (pending != lumen::FieldOrder::Unknown) ? pending : s.cfg_field_order;
          // 报告里的链已按 cfg_field_order 覆盖过；只有"用户临时改过"才需要再覆盖一次。
          if (s.field_override != s.cfg_field_order) {
            lumen::OverrideFieldOrder(s.field_override, &s.chain);
          }
          s.has_analysis = true;
          ApplyChain(s.processed);
          BuildMediaInfo();
          RepaintAll();
          const lumen::SourceInfo& info = s.report.info;
          // 标题栏也用中文：这里是用户最先看到的一行，出现 deinterlace/tff
          // 这种英文内部名会让人觉得"这软件没做完"。
          std::wstring title = Utf8ToWide(FileNameOf(job->path)) + L"  |  " +
                               std::to_wstring(info.width) + L"×" +
                               std::to_wstring(info.height) + L"  " +
                               CadenceZh(s.report.cadence.cadence) + L"  " +
                               FieldModeZh(s.chain.field_mode);
          SetStatus(WideToUtf8(title));
        } else {
          SetStatus(FileNameOf(job->path) + "  |  分析失败：" + job->error);
          BuildMediaInfo();
          RepaintAll();
        }
        delete job;
      }
      return 0;
    }

    case WM_ERASEBKGND: {
      // 自绘背景，避免拖动窗口时闪白（视频窗口的周围区域）
      RECT rc{};
      GetClientRect(hwnd, &rc);
      FillRect(reinterpret_cast<HDC>(wp), &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      return 1;
    }

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

    default:
      return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

// ---------------------------------------------------------------------------
// 启动 mpv
// ---------------------------------------------------------------------------
bool StartMpv(const std::string& mpv_path) {
  PlayerState& s = *g_state;
  const std::string pipe = "lumen_" + std::to_string(GetCurrentProcessId());

  char wid[32];
  std::snprintf(wid, sizeof(wid), "%lld",
                static_cast<long long>(reinterpret_cast<intptr_t>(s.hwnd)));

  std::vector<std::string> args = {
      "--wid=" + std::string(wid),
      "--input-ipc-server=\\\\.\\pipe\\" + pipe,
      "--no-terminal",
      // --no-input-terminal：不读标准输入。GUI 程序的标准输入常常是无效句柄，
      // mpv 读到 EOF 会当成"用户关闭了终端"直接退出——表现为"窗口一闪就没了"。
      "--no-input-terminal",
      // --no-config：不让用户全局 mpv.conf 里的 vf/去隔行设置干扰 A/B 对比。
      // 代价是丢失用户偏好，但对"画面处理工具"来说可复现性更重要。
      "--no-config",
      "--idle=yes",
      "--keep-open=yes",
      // ★ OSC（mpv 自带的屏幕控制条）必须关掉。原因不是审美，是它在这个场景下
      //   根本不能用：--wid 嵌入后鼠标事件由父窗口接收，OSC 收不到点击与拖动。
      //   用户会看到一条拖不动的进度条——比没有更糟。我们自己在控制条上画。
      "--osc=no",
      "--osd-bar=no",
      // 键盘与鼠标绑定全部交给外壳：mpv 只负责解码与显示。
      // 这样"单击画面暂停 / 双击全屏 / 滚轮调音量"的行为完全由我们定义，
      // 不会出现两边同时响应一次点击的双触发。
      "--input-default-bindings=no",
      "--input-vo-keyboard=no",
      // osd-level=0：不开 mpv 自带的 OSD（暂停时会冒出一个时钟，和我们的
      // 控制条信息重复）。注意 show-text 命令不受这个开关影响，
      // A/B 切换、场序这类提示仍会正常显示。
      "--osd-level=0",
      "--osd-font-size=26",
      "--osd-duration=1500",
      "--msg-level=all=warn",
      "--force-window=yes",
      // 子窗口不要边框：我们按视频区尺寸精确 MoveWindow，有边框会露一条缝
      "--no-border",
      // 留黑边时用和界面一致的深色，避免视频区比侧栏"亮一档"的割裂感
      "--background-color=#0d0f13",
  };

  // -------------------------------------------------------------------------
  // 进度记忆不依赖 mpv：见下方 LoadResumeTable / AttemptPendingResume。
  // 实测 mpv 0.42 的 --save-position-on-quit + --watch-later-dir 会"接受参数但不落盘"，
  // 所以进度由我们通过 IPC 读 time-pos 自己维护，行为确定、可测、能看懂。
  // -------------------------------------------------------------------------

  const std::string hwdec = s.cfg.player.hwdec.empty() ? "auto" : s.cfg.player.hwdec;
  args.push_back("--hwdec=" + hwdec);
  if (!s.cfg.player.extra_args.empty()) {
    // 简单按空格切分（够用；复杂参数请写进配置的单引号里也支持不了，属已知限制）
    std::string cur;
    for (char c : s.cfg.player.extra_args) {
      if (c == ' ') {
        if (!cur.empty()) args.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    if (!cur.empty()) args.push_back(cur);
  }

  s.mpv = lumen::SpawnProcess(mpv_path, args);
  if (!s.mpv.launched) {
    MessageBoxW(s.hwnd,
                L"无法启动 mpv.exe。\n请把它放到本程序同目录，或在 lumen.json 里设置 "
                L"player.mpv_path。",
                L"LumenPlayer", MB_ICONERROR);
    return false;
  }

  if (s.ipc.Connect(pipe, 10000) != lumen::Status::Ok) {
    MessageBoxW(s.hwnd,
                (L"启动 mpv 后无法建立控制通道：\n" + Utf8ToWide(s.ipc.last_error())).c_str(),
                L"LumenPlayer", MB_ICONERROR);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 进度记忆：一个极简的制表符分隔文本文件
// ---------------------------------------------------------------------------
// 格式：每行 "路径 \t 位置秒 \t 总时长秒"。选 TSV 而不是 JSON 的理由：
// 播放器退出路径上不能有任何可能失败的复杂序列化（进程正在关闭，时间很紧），
// 而且人可以直接打开看/删。路径里出现制表符的概率可以忽略。
// 把路径规范化成"稳定的键"：绝对路径 + 统一反斜杠 + 全小写。
// 为什么必须做：同一个文件用 "../testdata/a.mpg"、"..\\testdata\\a.mpg"、
// "C:////x////a.mpg" 三种写法打开时，字符串完全不同。不规范化的话，
// 用户在资源管理器里双击和拖进窗口，会得到"两份互不相干的进度"。
// Windows 文件系统大小写不敏感，所以小写化是安全的。
std::string NormalizePathKey(const std::string& path) {
  const std::wstring w = Utf8ToWide(path);
  wchar_t full[MAX_PATH * 4] = {0};
  const DWORD n = GetFullPathNameW(w.c_str(), MAX_PATH * 4, full, nullptr);
  std::wstring key = (n > 0 && n < MAX_PATH * 4) ? std::wstring(full, n) : w;
  if (!key.empty()) CharLowerBuffW(&key[0], static_cast<DWORD>(key.size()));
  return WideToUtf8(key);
}

std::string ResumeFilePath() { return g_state->exe_dir + "lumen_resume.tsv"; }

std::map<std::string, ResumeEntry> LoadResumeTable() {
  std::map<std::string, ResumeEntry> table;
  std::ifstream in(ResumeFilePath());
  if (!in) return table;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const size_t t1 = line.find('\t');
    if (t1 == std::string::npos) continue;
    const size_t t2 = line.find('\t', t1 + 1);
    const std::string path = line.substr(0, t1);
    const std::string pos = line.substr(t1 + 1, (t2 == std::string::npos ? std::string::npos
                                                                       : t2 - t1 - 1));
    if (path.empty() || pos.empty()) continue;
    ResumeEntry e;
    e.pos = std::atof(pos.c_str());
    if (t2 != std::string::npos) e.dur = std::atof(line.c_str() + t2 + 1);
    // 读的时候也规范化：老文件、手工编辑的文件都可能用别的写法（正斜杠、相对路径），
    // 统一成同一种键才不会"明明有记录却匹配不上"。
    table[NormalizePathKey(path)] = e;
  }
  return table;
}

void SaveResumeTable(const std::map<std::string, ResumeEntry>& table) {
  std::ofstream out(ResumeFilePath(), std::ios::trunc | std::ios::binary);
  if (!out) return;
  // 写 UTF-8 BOM：Windows 上的记事本/Excel 默认按本地代码页读，
  // 没有 BOM 时中文注释会变成乱码（批量报告 CSV 踩过同一个坑）。
  out << "\xEF\xBB\xBF";
  out << "# LumenPlayer 播放进度（可安全删除，删除后所有片子从头发起重播）\n";
  out << "# path\tposition_seconds\tduration_seconds\n";
  for (const auto& kv : table) {
    if (kv.second.pos <= 0.0) continue;
    out << kv.first << '\t' << kv.second.pos << '\t' << kv.second.dur << "\n";
  }
}

// 记录"当前文件"的播放位置。
// 只在"确实看到一个合理位置"时写：暂停在 0.0 秒或已经播完（剩余 <5 秒）
// 都应该清掉记录，否则下次打开会跳到一个莫名其妙的片尾。
void RememberCurrentPosition() {
  PlayerState& s = *g_state;
  if (!s.cfg.player.resume || s.current_file.empty()) return;

  // ★ 这里刻意用"定时器缓存的位置"，而不是现读 time-pos。
  //   实测：主窗口销毁的瞬间，mpv 的 --wid 子窗口随之消失，mpv 立刻退出，
  //   IPC 管道同时断开——此时再 GetDouble("time-pos") 必然失败。
  //   位置信息本来每 500ms 就已经采过一次，用缓存值既准确又不受这条竞态影响。
  const double pos = s.last_pos;
  const double dur = s.last_dur;

  std::map<std::string, ResumeEntry> table = LoadResumeTable();
  const bool keep = pos > 3.0 && (dur <= 0.0 || pos < dur - 5.0);
  if (keep) {
    ResumeEntry e;
    e.pos = pos;
    e.dur = dur;
    table[NormalizePathKey(s.current_file)] = e;
  } else {
    table.erase(NormalizePathKey(s.current_file));
  }
  SaveResumeTable(table);
}

// 打开文件后调用：如果上次看到一半，跳回去。
// 阈值 3 秒的判断和保存端一致——两端不一致是最常见的"记忆错乱"来源。
// 由 500ms 定时器反复调用，直到 duration 可读（说明文件真的解析完了）。
void AttemptPendingResume() {
  PlayerState& s = *g_state;
  if (s.pending_resume <= 0.0 || !s.ipc.connected()) return;

  double dur = 0.0;
  if (!s.ipc.GetDouble("duration", &dur, 200) || dur <= 0.5) return; // 还没解析完，下次再试

  char seek[32];
  std::snprintf(seek, sizeof(seek), "%.3f", s.pending_resume);
  const int total = static_cast<int>(s.pending_resume);
  if (s.ipc.Command({"seek", seek, "absolute"}) == lumen::Status::Ok) {
    char osd[128];
    std::snprintf(osd, sizeof(osd), "已跳回上次进度 %02d:%02d:%02d", total / 3600,
                  (total % 3600) / 60, total % 60);
    Osd(osd, 4000);
  }
  s.pending_resume = 0.0;
}

// ---------------------------------------------------------------------------
// 自检模式（无窗口，用于诊断）
// ---------------------------------------------------------------------------
int RunSelfTest(const std::string& file, const lumen::AppConfig& cfg,
                const std::string& exe_dir) {
  std::printf("LumenPlayer 播放链路自检\n");
  std::printf("----------------------------------------\n");

  const std::string mpv = FindMpv(cfg, exe_dir);
  if (mpv.empty()) {
    std::printf("[失败] 未找到 mpv.exe\n");
    return 1;
  }
  std::printf("[1/5] mpv: %s\n", mpv.c_str());

  if (file.empty() || !FileExists(file)) {
    std::printf("[失败] 文件不存在：%s\n", file.c_str());
    return 1;
  }

  const std::string pipe = "lumen_selftest_" + std::to_string(GetCurrentProcessId());
  std::vector<std::string> args = {
      "--input-ipc-server=\\\\.\\pipe\\" + pipe,
      "--no-terminal", "--no-input-terminal", "--no-config", "--idle=yes",
      // --vo=null 而不是 --no-video：后者会连视频解码一起关掉，
      // 遇到"纯视频无音轨"的测试片源时 mpv 会认为"无内容可播"直接 end-file。
      "--vo=null", "--ao=null", "--msg-level=all=warn",
  };
  lumen::SpawnResult proc = lumen::SpawnProcess(mpv, args);
  if (!proc.launched) {
    std::printf("[失败] 无法启动 mpv\n");
    return 1;
  }

  lumen::MpvIpcClient ipc;
  if (ipc.Connect(pipe, 8000) != lumen::Status::Ok) {
    std::printf("[失败] IPC 连接失败：%s\n", ipc.last_error().c_str());
    lumen::KillProcess(&proc);
    return 1;
  }
  std::printf("[2/5] IPC 已连接：\\\\.\\pipe\\%s\n", pipe.c_str());

  std::string ver;
  if (ipc.GetString("mpv-version", &ver)) {
    std::printf("      mpv 版本：%s\n", ver.c_str());
  }

  std::string err;
  if (ipc.Command({"loadfile", file}, &err) != lumen::Status::Ok) {
    std::printf("[失败] loadfile：%s\n", err.c_str());
    lumen::KillProcess(&proc);
    return 1;
  }

  // 等 file-loaded 事件（最多 10 秒），同时把事件打出来——
  // 播放失败时这一行是唯一能说明"卡在哪一步"的线索。
  bool loaded = false;
  for (int i = 0; i < 1000; ++i) {
    lumen::JsonValue ev;
    while (ipc.PollEvent(&ev)) {
      const std::string name = ev["event"].AsString();
      const std::string reason = ev["reason"].AsString();
      std::printf("      事件：%s%s%s\n", name.c_str(), reason.empty() ? "" : "  reason=",
                  reason.c_str());
      if (name == "file-loaded") loaded = true;
    }
    if (loaded) break;
    Sleep(10);
  }
  std::printf("[3/5] file-loaded: %s\n", loaded ? "是" : "否（超时）");

  double duration = 0.0;
  if (ipc.GetDouble("duration", &duration)) {
    std::printf("      时长：%.2f s\n", duration);
  }

  // 片源自检
  lumen::InspectOptions opt;
  opt.preset = lumen::QualityPreset::Balanced;
  opt.anime = cfg.anime;
  opt.ffmpeg_path = cfg.ffmpeg_path;
  opt.ffprobe_path = cfg.ffprobe_path;
  // 自检模式也遵守配置里的场序覆盖，否则"自检通过"与"实际播放"可能不是同一套处理。
  lumen::FieldOrder fo = lumen::FieldOrder::Unknown;
  if (lumen::ParseFieldOrder(cfg.field_order, &fo)) opt.field_order_override = fo;
  lumen::InspectionReport report;
  std::string ierr;
  if (lumen::InspectSource(file, opt, &report, &ierr) != lumen::Status::Ok) {
    std::printf("[失败] 片源自检：%s\n", ierr.c_str());
  } else {
    std::printf("[4/5] 自检结论：%s  %s  %dx%d\n",
                lumen::ToString(report.cadence.cadence),
                lumen::ToString(report.chain.field_mode), report.info.width,
                report.info.height);
    std::printf("      处理依据：%s\n", report.chain.rationale.c_str());
    if (report.warnings.empty()) {
      std::printf("      告警：无\n");
    } else {
      for (const auto& w : report.warnings) std::printf("      告警：%s\n", w.c_str());
    }

    // 真正的关键一步：把滤镜挂上去，再从 mpv 读回来确认它真的生效了。
    // 只发命令不校验，是"看起来调用了但其实没生效"的典型来源。
    const std::vector<std::string> filters = BuildMpvFilters(report.chain);
    if (!filters.empty()) {
      bool all_ok = true;
      for (size_t i = 0; i < filters.size(); ++i) {
        const lumen::Status st =
            ipc.Command({"vf", "add", "@lumen" + std::to_string(i) + ":" + filters[i]}, &err);
        std::printf("[5/5] 挂载滤镜 %zu：%s  ->  %s\n", i, filters[i].c_str(),
                    st == lumen::Status::Ok ? "成功" : err.c_str());
        if (st != lumen::Status::Ok) all_ok = false;
      }
      (void)all_ok;
      std::string cur;
      if (ipc.GetPropertyString("vf", &cur)) {
        std::printf("      mpv 回报 vf = %s\n", cur.empty() ? "(空)" : cur.c_str());
      }
      // A/B 切换验证：逐个移除后应为空
      ClearOurFilters(&ipc);
      std::string after;
      if (ipc.GetPropertyString("vf", &after)) {
        std::printf("      A/B 切回原片后 vf = %s\n", after.empty() ? "(空)" : after.c_str());
      }
    } else {
      std::printf("[5/5] 本片为逐行，无需挂载场处理滤镜\n");
    }
  }

  ipc.Command({"quit"});
  ipc.Close();
  Sleep(300);
  lumen::KillProcess(&proc);
  std::printf("----------------------------------------\n");
  std::printf("自检结束。\n");
  return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
// 用 main 而不是 wWinMain：入口参数我们用 CommandLineToArgvW 自己解析，
// 这样中文文件名的编码完全可控（CRT 的 argv 是本地代码页，中文会乱码）。
// 代价是控制台子系统，GUI 模式下把控制台窗口藏起来即可。
int main() {
  HINSTANCE inst = GetModuleHandleW(nullptr);
  const int show = SW_SHOWDEFAULT;
  // 声明 DPI 感知：否则系统会把我们整个窗口当位图拉伸，
  // 视频画面与 HUD 文字都会发虚——这是"明明是高清片却看着糊"的常见原因之一。
  // 用旧 API SetProcessDPIAware 而不是 SetProcessDpiAwarenessContext：
  // 前者从 Vista 起就有，不需要为 DPI 感知抬高最低系统要求。
  SetProcessDPIAware();
  // 控制台：GUI 模式下藏起来（否则旁边挂个黑框很难看）；
  // --self-test 模式下保留，因为要往控制台打诊断信息。
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

  std::string file;
  std::string config_path;
  bool self_test = false;
  // 未指定时用配置里的 field_order；命令行优先
  lumen::FieldOrder field_cli = lumen::FieldOrder::Unknown;
  bool field_cli_set = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = WideToUtf8(argv[i]);
    if (a == "--self-test") {
      self_test = true;
    } else if (a == "--field-order" && i + 1 < argc) {
      lumen::FieldOrder fo = lumen::FieldOrder::Unknown;
      if (lumen::ParseFieldOrder(WideToUtf8(argv[++i]), &fo)) {
        field_cli = fo;
        field_cli_set = true;
      }
    } else if (a == "--config" && i + 1 < argc) {
      config_path = WideToUtf8(argv[++i]);
    } else if (!a.empty() && a[0] == '-') {
      // 忽略未知选项，避免用户多敲一个参数就打不开
    } else if (file.empty()) {
      file = a;
    }
  }
  if (argv) LocalFree(argv);

  const std::string exe_dir = ExeDirectory();
  lumen::AppConfig cfg = lumen::DefaultAppConfig();
  {
    const std::string cfg_path =
        config_path.empty() ? (exe_dir + "lumen.json") : config_path;
    std::string err;
    const lumen::Status st = lumen::LoadAppConfig(cfg_path, &cfg, &err);
    if (st == lumen::Status::InvalidArgument) {
      std::printf("配置文件有误：%s\n", err.c_str());
    }
  }

  if (self_test) {
    return RunSelfTest(file, cfg, exe_dir);
  }

  HWND console = GetConsoleWindow();
  if (console) ShowWindow(console, SW_HIDE);

  PlayerState state;
  g_state = &state;
  state.inst = inst;
  state.cfg = cfg;
  state.exe_dir = exe_dir;
  // 场序优先级：命令行 > 配置文件 > 自动
  {
    lumen::FieldOrder fo = lumen::FieldOrder::Unknown;
    if (lumen::ParseFieldOrder(cfg.field_order, &fo)) state.cfg_field_order = fo;
    if (field_cli_set) state.cfg_field_order = field_cli;
    state.field_override = state.cfg_field_order;
  }
  state.hud_on = cfg.player.hud;

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.lpszClassName = kClassName;
  wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)); // IDI_APPLICATION
  if (!RegisterClassExW(&wc)) return 1;

  // 初始窗口按屏幕工作区取，最多 1360×820。
  // 写死尺寸在 1366×768 的小本上会被系统裁到屏幕高度，侧栏内容因此显示不全，
  // 用户会以为"信息面板是坏的"。
  int win_w = 1360;
  int win_h = 820;
  {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    POINT origin{0, 0};
    if (GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi)) {
      const int aw = mi.rcWork.right - mi.rcWork.left;
      const int ah = mi.rcWork.bottom - mi.rcWork.top;
      if (win_w > aw * 92 / 100) win_w = aw * 92 / 100;
      if (win_h > ah * 92 / 100) win_h = ah * 92 / 100;
    }
  }

  // WS_CLIPCHILDREN 是自绘界面的前提：父窗口重绘时自动排除子窗口区域。
  // 少了它，控制条每 250ms 刷新一次都会把视频区刷成黑底再等 mpv 重画，
  // 表现就是画面持续轻微闪烁。
  state.hwnd = CreateWindowExW(0, kClassName, kWindowTitle,
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                               CW_USEDEFAULT, win_w, win_h, nullptr, nullptr, inst, nullptr);
  if (!state.hwnd) return 1;
  ShowWindow(state.hwnd, show);

  // mpv 必须在本进程里找到（FindMpv 已按优先级找过）
  const std::string mpv = FindMpv(cfg, exe_dir);
  if (mpv.empty()) {
    MessageBoxW(state.hwnd,
                L"未找到 mpv.exe。\n\n"
                L"请把 mpv 解压到本程序目录（与 lumen-player.exe 同级），\n"
                L"或在 lumen.json 中设置 \"player\": { \"mpv_path\": \"...\" }。",
                L"LumenPlayer", MB_ICONERROR);
    return 1;
  }
  if (!StartMpv(mpv)) return 1;

  // HUD：先建窗口再启动定时器。窗口本身是鼠标穿透的分层窗口，
  // 建得不成功也不影响播放（HudRefresh 内部会跳过），所以不做失败重试。
  HudCreate();
  HudSetVisible(state.hud_on);

  // 布局必须在第一帧绘制之前算好，否则首帧会把控制条画在错位置。
  UpdateLayout();
  BuildMediaInfo();

  SetTimer(state.hwnd, kUiTimerId, kUiIntervalMs, nullptr);

  // mpv 的子窗口是异步创建的：这里试一次，失败也不要紧，
  // 250ms 后的定时器会通过 EnsureVideoAttached 再试。
  EnsureVideoAttached();

  SetStatus("就绪 · 拖入视频文件，或按 O 打开（空格 暂停 / ← → 快退快进 / H 帮助）");

  if (!file.empty()) OpenFile(file);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  // 收尾：先让 mpv 自己退出，超时再强杀
  KillTimer(state.hwnd, kUiTimerId);
  KillTimer(state.hwnd, kClickTimerId);
  if (state.hud) DestroyWindow(state.hud);
  RememberCurrentPosition(); // 必须在 quit 之前读：quit 之后就查不到 time-pos 了
  state.ipc.Command({"quit"});
  state.ipc.Close();
  for (int i = 0; i < 30 && lumen::ProcessAlive(state.mpv); ++i) Sleep(50);
  lumen::KillProcess(&state.mpv);
  return 0;
}
