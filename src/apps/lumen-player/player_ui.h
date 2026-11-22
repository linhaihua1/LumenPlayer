// ============================================================================
//  LumenPlayer - 播放器界面（自绘控件）
//  ---------------------------------------------------------------------------
//  所属层次：应用层（仅 Windows）
//
//  ★ 为什么必须自己画控制条，而不是用 mpv 的 OSC：
//    mpv 的 OSC 绘制在它自己的 vo 窗口里，而我们是 --wid 把它嵌进主窗口的子
//    窗口区域。实测在嵌入模式下，鼠标事件由父窗口接收、vo 子窗口拿不到，
//    OSC 的点击与拖动全部失效——用户看到一条进度条却拖不动，比没有更糟。
//    所以：关掉 OSC（--osc=no），父窗口留出「视频区之外」的固定区域自己画。
//
//  ★ 为什么把控制条/侧栏放在子窗口之外，而不是半透明浮层：
//    mpv 的子窗口铺满视频区，会盖住父窗口在上面画的任何像素。要做到浮层
//    就得再开分层窗口（HUD 已经这么干了），但分层窗口做交互控件意味着
//    自己实现整套命中测试 + 鼠标捕获 + 重绘，收益（省 88px）远小于风险。
//    固定区域还有一个附带好处：文字是纯 GDI 绘制，不会随视频缩放变模糊。
//
//  ★ 本文件只负责「长什么样 / 点在哪」，不持有任何播放状态：
//    数据由 main.cpp 每帧填进来，命中结果再传回去执行。这样 UI 可以独立
//    推理（几何算错时只看这一个文件），也不会和 IPC 纠缠在一起。
// ============================================================================
#pragma once

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace lumen {
namespace ui {

// ---------------------------------------------------------------------------
// 基础几何
// ---------------------------------------------------------------------------
struct Rect {
  int x = 0, y = 0, w = 0, h = 0;

  int right() const { return x + w; }
  int bottom() const { return y + h; }
  bool empty() const { return w <= 0 || h <= 0; }
  bool contains(int px, int py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
};

// 尺寸换算：所有设计稿数值都按 96 DPI 写死，运行时按实际 DPI 放大。
// 不这么做的话，125% 缩放下控制条按钮会挤在一起、文字被裁。
struct Metrics {
  int dpi = 96;
  int px(int v) const { return MulDiv(v, dpi, 96); }
};

// ---------------------------------------------------------------------------
// 区域划分
// ---------------------------------------------------------------------------
//  +------------------------------+-----------+
//  |           视频区              |  信息侧栏  |
//  +------------------------------+           |
//  |          控制条               |           |
//  +------------------------------+-----------+
// 侧栏贯穿全高（更像"详情面板"而不是一条窄边），控制条只占视频区下方。
struct Layout {
  Rect client;    // 整个客户区
  Rect video;     // mpv 子窗口应该占据的区域（父窗口不在此绘制）
  Rect controls;  // 底部控制条
  Rect sidebar;   // 右侧信息栏
  int sidebar_w = 0;
  int controls_h = 0;
  bool fullscreen = false;
  Metrics m;
};

// 参数说明：
//   cw / ch     客户区尺寸（物理像素）
//   dpi         窗口 DPI（用于控件缩放）
//   sidebar_on  是否显示信息侧栏（用户可关）
//   fullscreen  全屏时强制隐藏侧栏、压缩控制条高度
Layout ComputeLayout(int cw, int ch, UINT dpi, bool sidebar_on, bool fullscreen);

// ---------------------------------------------------------------------------
// 数据快照
// ---------------------------------------------------------------------------
// 侧栏渲染需要的一切。全部由 main.cpp 从 mpv 属性/自检报告换算成字符串，
// 这样 UI 层就不需要知道 hwdec 的 "no" 要翻译成"软件解码"这类业务规则。
struct MediaInfo {
  std::wstring title;    // 媒体标题或文件名
  std::wstring subtitle; // 副标题：分辨率 · 帧率
  std::wstring status;   // 正在播放 / 已暂停 / 已结束 / 就绪

  // —— 基本信息（标签在绘制时固定，这里只有值）——
  std::wstring duration, resolution, aspect, fps, vcodec, acodec, bitrate, hwdec;

  // —— 片源分析 ——
  std::wstring cadence, chain, field_order, color, preprocess, super_res;

  // —— 底部按钮上的当前状态 ——
  std::wstring quality;     // 例如 "原生 · 适应窗口"
  std::wstring chain_label; // "处理：开" / "处理：原片"
  std::wstring field_label; // "场序：自动" / "场序：tff"
  std::wstring warning;     // 非空时在侧栏底部显示一行黄字告警

  bool has_media = false;
  bool has_analysis = false;
};

// 控制条状态。
struct PlayState {
  double pos = 0.0;
  double dur = 0.0;
  double volume = 100.0;
  double speed = 1.0;
  bool paused = false;
  bool muted = false;
  bool eof = false; // 播放结束（keep-open 停在最后一帧）

  // 拖动进度条：true 时进度条显示 drag_pos 而不是真实播放位置，
  // 否则滑块会被每 250ms 的属性刷新"拽回去"，手感像卡死。
  bool dragging_seek = false;
  double drag_pos = 0.0;
  bool dragging_volume = false;

  int hover = 0;   // 鼠标悬停的 Hit（用于按钮高亮）
  int pressed = 0; // 按下中的 Hit（用于按钮压下态）

  bool sidebar_on = true;
  bool chain_on = true;
};

// ---------------------------------------------------------------------------
// 命中测试
// ---------------------------------------------------------------------------
enum Hit {
  kHitNone = 0,
  kHitPlayPause,
  kHitBack,      // 后退 10 秒
  kHitForward,   // 前进 10 秒
  kHitSeekBar,
  kHitVolume,
  kHitOpen,
  kHitSettings,
  kHitFullscreen,
  kHitChain,      // A：处理后 / 原片
  kHitFieldOrder, // P：场序轮换
  kHitWelcomeOpen,
  kHitVideoArea, // 视频区空白处（单击 = 暂停，双击 = 全屏）
};

int HitTest(const Layout& L, const PlayState& P, int x, int y);

// ---------------------------------------------------------------------------
// 几何查询（绘制与命中测试共用同一份计算，避免"看得到点不到"）
// ---------------------------------------------------------------------------
Rect SeekTrackRect(const Layout& L);
Rect VolumeTrackRect(const Layout& L);
Rect WelcomeButtonRect(const Layout& L);

// 进度条上某个 x 坐标对应的时间；超出轨道两端会被夹到 [0, dur]
double SeekTimeAt(const Layout& L, int x, double dur);
// 音量条上某个 x 坐标对应的音量 [0, 100]
double VolumeAt(const Layout& L, int x);

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
// 调用方负责双缓冲（内存 DC），本函数只往传入的 DC 上画。
void Paint(HDC dc, const Layout& L, const MediaInfo& M, const PlayState& P);

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
// 秒 → "mm:ss" / "h:mm:ss"。负值/NaN 归零。
std::wstring FormatClock(double sec);
// 码率 → "3.21 Mbps" / "850 kbps"。<=0 返回 "—"。
std::wstring FormatBitrate(double bps);

} // namespace ui
} // namespace lumen
