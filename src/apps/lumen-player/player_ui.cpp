// ============================================================================
//  LumenPlayer - 播放器界面实现（纯 GDI 自绘）
//  ---------------------------------------------------------------------------
//  设计取舍说明（看代码前先看这段，否则很多"为什么不那样写"会想不通）：
//
//  1) 不引 GDI+：只为了圆角抗锯齿而多一个 DLL 依赖与 startup/shutdown 管理，
//     不值。控制条细条只有 4px 高，RoundRect 的锯齿在深色背景上基本看不出来。
//  2) 不用 DrawThemeText / 系统主题：播放器要的是固定的深色影院风格，
//     跟随系统主题会让侧栏在浅色 Windows 下变成一块白板，很难看。
//  3) 所有坐标都走 Metrics::px()：设计稿按 96 DPI 写，125%/150% 缩放下
//     按钮间距、行高按比例放大。漏掉这一步的典型症状是"字被裁一半"。
//  4) 绘制与命中共用同一套几何函数（SeekTrackRect / ComputeControlGeom）：
//     UI 代码最常见的 bug 就是"画在这、点在那"，共用几何是唯一可靠的做法。
// ============================================================================
#include "player_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace lumen {
namespace ui {
namespace {

// ---------------------------------------------------------------------------
// 调色板
// ---------------------------------------------------------------------------
// 深色影院风：背景接近纯黑但不是纯黑（纯黑会让侧栏和控制条糊成一片），
// 面板比背景亮一档，分隔线再亮一档，形成三层结构。
constexpr COLORREF kBg = RGB(13, 15, 19);
constexpr COLORREF kBar = RGB(17, 20, 25);
constexpr COLORREF kPanel = RGB(21, 24, 30);
constexpr COLORREF kLine = RGB(38, 43, 52);
constexpr COLORREF kText = RGB(229, 233, 240);
constexpr COLORREF kTextDim = RGB(150, 159, 173);
constexpr COLORREF kTextMute = RGB(102, 110, 124);
constexpr COLORREF kAccent = RGB(62, 130, 247);
constexpr COLORREF kAccentHot = RGB(96, 156, 255);
constexpr COLORREF kAccentWarm = RGB(232, 176, 74); // 暂停用暖色，和"播放"区分开
constexpr COLORREF kTrack = RGB(58, 64, 76);
constexpr COLORREF kBtnHover = RGB(34, 39, 48);
constexpr COLORREF kBtnPress = RGB(45, 51, 62);
constexpr COLORREF kDanger = RGB(232, 122, 74);

// ---------------------------------------------------------------------------
// GDI 小工具
// ---------------------------------------------------------------------------
void FillRectC(HDC dc, const RECT& r, COLORREF c) {
  if (r.right <= r.left || r.bottom <= r.top) return;
  HBRUSH br = CreateSolidBrush(c);
  FillRect(dc, &r, br);
  DeleteObject(br);
}

void FillRectC(HDC dc, const Rect& r, COLORREF c) {
  FillRectC(dc, RECT{r.x, r.y, r.right(), r.bottom()}, c);
}

// 圆角实心块。radius 传的是"圆角半径"，RoundRect 需要的是椭圆直径，故 *2。
void FillRound(HDC dc, const Rect& r, int radius, COLORREF c) {
  if (r.empty()) return;
  HBRUSH br = CreateSolidBrush(c);
  HPEN pen = CreatePen(PS_NULL, 0, c);
  HGDIOBJ ob = SelectObject(dc, br);
  HGDIOBJ op = SelectObject(dc, pen);
  RoundRect(dc, r.x, r.y, r.right(), r.bottom(), radius * 2, radius * 2);
  SelectObject(dc, ob);
  SelectObject(dc, op);
  DeleteObject(br);
  DeleteObject(pen);
}

HFONT MakeFont(int pt, bool bold, int dpi, const wchar_t* face = L"Microsoft YaHei UI") {
  // 负高度 = 字符高度（不含内部行距），这是 GDI 里"字号"的正确写法；
  // 传正数会得到"字符单元格高度"，同样字号看起来会明显小一圈。
  return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE,
                     FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
}

void DrawTextC(HDC dc, const std::wstring& text, RECT r, HFONT font, COLORREF color,
               UINT flags) {
  if (text.empty() || r.right <= r.left || r.bottom <= r.top) return;
  HGDIOBJ old = SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, color);
  DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, flags | DT_NOPREFIX);
  SelectObject(dc, old);
}

// 单行左对齐（垂直居中）
void TextLeft(HDC dc, const std::wstring& t, int x, int y, int w, int h, HFONT f,
              COLORREF c) {
  DrawTextC(dc, t, RECT{x, y, x + w, y + h}, f, c,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void TextRight(HDC dc, const std::wstring& t, int x, int y, int w, int h, HFONT f,
               COLORREF c) {
  DrawTextC(dc, t, RECT{x, y, x + w, y + h}, f, c,
            DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void TextCenter(HDC dc, const std::wstring& t, int x, int y, int w, int h, HFONT f,
                COLORREF c) {
  DrawTextC(dc, t, RECT{x, y, x + w, y + h}, f, c,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

// ---------------------------------------------------------------------------
// 图标（全部用几何图元画，不依赖任何字体里的符号）
// ---------------------------------------------------------------------------
void IconPlay(HDC dc, int cx, int cy, int s, COLORREF c) {
  // 三角形的视觉重心偏左，整体右移 1px 让它看起来才"在正中"
  POINT pts[3] = {{cx - s / 2 + s / 8, cy - s / 2}, {cx - s / 2 + s / 8, cy + s / 2},
                  {cx + s / 2, cy}};
  HBRUSH br = CreateSolidBrush(c);
  HPEN pen = CreatePen(PS_NULL, 0, c);
  HGDIOBJ ob = SelectObject(dc, br);
  HGDIOBJ op = SelectObject(dc, pen);
  Polygon(dc, pts, 3);
  SelectObject(dc, ob);
  SelectObject(dc, op);
  DeleteObject(br);
  DeleteObject(pen);
}

void IconPause(HDC dc, int cx, int cy, int s, COLORREF c) {
  const int bw = s / 3;
  const int gap = s / 5;
  FillRound(dc, Rect{cx - gap / 2 - bw, cy - s / 2, bw, s}, 1, c);
  FillRound(dc, Rect{cx + gap / 2, cy - s / 2, bw, s}, 1, c);
}

// 文件夹（"打开文件"）：一个主体方块 + 左上角一个"耳朵"
void IconFolder(HDC dc, int cx, int cy, int s, COLORREF c) {
  const int w = s;
  const int h = s * 3 / 4;
  const int x = cx - w / 2;
  const int y = cy - h / 2 + s / 10;
  FillRound(dc, Rect{x, y, w / 2, h / 3}, 2, c);
  FillRound(dc, Rect{x, y + h / 4, w, h - h / 4}, 2, c);
}

// 四条折线组成的"四角"全屏标志。
// 全屏与退出全屏用同一个图形：真正区分状态的是工具栏上的其他信息，
// 为这个图标单独画一套"朝内折角"收益很低（鼠标移上去也看不清差别）。
void IconFullscreen(HDC dc, int cx, int cy, int s, COLORREF c) {
  const int h = s / 2;
  const int len = s / 3;
  HPEN pen = CreatePen(PS_SOLID, std::max(1, s / 10), c);
  HGDIOBJ op = SelectObject(dc, pen);
  const int l = cx - h, r = cx + h, t = cy - h, b = cy + h;
  MoveToEx(dc, l, t + len, nullptr); LineTo(dc, l, t); LineTo(dc, l + len, t);
  MoveToEx(dc, r - len, t, nullptr); LineTo(dc, r, t); LineTo(dc, r, t + len);
  MoveToEx(dc, l, b - len, nullptr); LineTo(dc, l, b); LineTo(dc, l + len, b);
  MoveToEx(dc, r - len, b, nullptr); LineTo(dc, r, b); LineTo(dc, r, b - len);
  SelectObject(dc, op);
  DeleteObject(pen);
}

// 三条横线（设置菜单）
void IconMenu(HDC dc, int cx, int cy, int s, COLORREF c) {
  const int w = s - s / 5;
  const int x = cx - w / 2;
  for (int i = -1; i <= 1; ++i) {
    const int yy = cy + i * (s / 3) / 2;
    FillRound(dc, Rect{x, yy, w, std::max(1, s / 10)}, 1, c);
  }
}

// 喇叭 + 音量格。level: 0=静音，1/2=音量档
void IconSpeaker(HDC dc, int cx, int cy, int s, COLORREF c, int level) {
  const int bw = s / 3;
  const int bh = s / 3;
  FillRectC(dc, Rect{cx - s / 2, cy - bh / 2, bw - bw / 3, bh}, c);
  POINT pts[4] = {{cx - s / 2 + bw, cy - bh / 2},
                  {cx - s / 2 + bw + s / 4, cy - s / 2},
                  {cx - s / 2 + bw + s / 4, cy + s / 2},
                  {cx - s / 2 + bw, cy + bh / 2}};
  HBRUSH br = CreateSolidBrush(c);
  HPEN pen = CreatePen(PS_NULL, 0, c);
  HGDIOBJ ob = SelectObject(dc, br);
  HGDIOBJ op = SelectObject(dc, pen);
  Polygon(dc, pts, 4);
  SelectObject(dc, ob);
  SelectObject(dc, op);
  DeleteObject(br);
  DeleteObject(pen);

  if (level <= 0) {
    // 静音：一条斜杠
    HPEN p = CreatePen(PS_SOLID, std::max(1, s / 10), kDanger);
    HGDIOBJ o = SelectObject(dc, p);
    MoveToEx(dc, cx + s / 6, cy - s / 5, nullptr);
    LineTo(dc, cx + s / 2, cy + s / 5);
    SelectObject(dc, o);
    DeleteObject(p);
  } else {
    for (int i = 0; i < level; ++i) {
      const int xx = cx + s / 6 + i * (s / 6);
      const int hh = s / 5 + i * (s / 7);
      FillRound(dc, Rect{xx, cy - hh / 2, std::max(1, s / 12), hh}, 1, c);
    }
  }
}

// ---------------------------------------------------------------------------
// 控制条几何
// ---------------------------------------------------------------------------
struct ControlGeom {
  Rect seek;
  Rect open, back, play, forward;
  Rect volume_icon, volume_track;
  Rect time_text, speed_text, settings, fullscreen;
  bool show_volume = true;
  bool show_speed = true;
};

ControlGeom ComputeControlGeom(const Layout& L) {
  const Metrics& m = L.m;
  const int cw = L.controls.w;
  const int top = L.controls.y;
  const int pad = m.px(L.fullscreen ? 14 : 20);

  ControlGeom g;
  g.seek = Rect{pad, top + m.px(L.fullscreen ? 9 : 14), std::max(0, cw - pad * 2), m.px(4)};

  const int row_h = m.px(L.fullscreen ? 34 : 38);
  const int row_y = top + m.px(L.fullscreen ? 24 : 34);
  int x = m.px(14);

  g.open = Rect{x, row_y, m.px(34), row_h};
  x += m.px(34) + m.px(4);
  g.back = Rect{x, row_y, m.px(40), row_h};
  x += m.px(40) + m.px(2);
  g.play = Rect{x, row_y, m.px(46), row_h};
  x += m.px(46) + m.px(2);
  g.forward = Rect{x, row_y, m.px(40), row_h};
  x += m.px(40) + m.px(12);

  g.volume_icon = Rect{x, row_y, m.px(26), row_h};
  x += m.px(26) + m.px(4);
  g.volume_track = Rect{x, row_y + row_h / 2 - m.px(2), m.px(76), m.px(4)};

  // 右侧从右边缘往左排：全屏 → 设置 → 倍速 → 时间
  int rx = cw - m.px(14);
  g.fullscreen = Rect{rx - m.px(36), row_y, m.px(36), row_h};
  rx -= m.px(36) + m.px(2);
  g.settings = Rect{rx - m.px(36), row_y, m.px(36), row_h};
  rx -= m.px(36) + m.px(6);
  g.speed_text = Rect{rx - m.px(46), row_y, m.px(46), row_h};
  rx -= m.px(46) + m.px(4);
  g.time_text = Rect{rx - m.px(150), row_y, m.px(150), row_h};

  // 窗口窄到这个程度就别硬塞了：时间是必须显示的，优先砍音量条和倍速。
  const int left_end = g.volume_track.right() + m.px(10);
  if (g.time_text.x < left_end + m.px(90)) {
    g.show_volume = false;
    g.show_speed = false;
  } else if (g.time_text.x < left_end + m.px(130)) {
    g.show_speed = false;
  }
  return g;
}

// ---------------------------------------------------------------------------
// 侧栏几何
// ---------------------------------------------------------------------------
struct SidebarGeom {
  Rect open, quality, chain, field;
  int content_top = 0;
  int content_bottom = 0;
};

SidebarGeom ComputeSidebarGeom(const Layout& L) {
  const Metrics& m = L.m;
  const int pad = m.px(20);
  const int bw = std::max(0, L.sidebar.w - pad * 2);

  SidebarGeom g;
  // 底部按钮区的高度是"硬预算"：窗口再矮也不会压缩它（压了就没法点了），
  // 所以先把这块从下往上排好，信息区只用剩下的空间——这样无论窗口多矮，
  // 按钮始终完整可见。
  int y = L.sidebar.bottom() - m.px(14);

  const int half = std::max(0, (bw - m.px(8)) / 2);
  g.chain = Rect{L.sidebar.x + pad, y - m.px(28), half, m.px(28)};
  g.field = Rect{L.sidebar.x + pad + half + m.px(8), y - m.px(28),
                 std::max(0, bw - half - m.px(8)), m.px(28)};
  y -= m.px(28) + m.px(6);

  g.quality = Rect{L.sidebar.x + pad, y - m.px(34), bw, m.px(34)};
  y -= m.px(34) + m.px(6);

  g.open = Rect{L.sidebar.x + pad, y - m.px(36), bw, m.px(36)};
  y -= m.px(36) + m.px(10);

  g.content_bottom = y;
  g.content_top = m.px(14);
  return g;
}

// ---------------------------------------------------------------------------
// 按钮底衬
// ---------------------------------------------------------------------------
void ButtonBg(HDC dc, const Rect& r, const PlayState& P, int hit, bool round_rect) {
  const bool hover = (P.hover == hit);
  const bool press = (P.pressed == hit);
  if (!hover && !press) return;
  FillRound(dc, r, round_rect ? r.h / 2 : 6, press ? kBtnPress : kBtnHover);
}

// ---------------------------------------------------------------------------
// 侧栏
// ---------------------------------------------------------------------------
void DrawSidebar(HDC dc, const Layout& L, const MediaInfo& M, const PlayState& P) {
  const Metrics& m = L.m;
  const RECT sr{L.sidebar.x, L.sidebar.y, L.sidebar.right(), L.sidebar.bottom()};
  FillRectC(dc, sr, kPanel);
  FillRectC(dc, RECT{L.sidebar.x, 0, L.sidebar.x + 1, L.sidebar.bottom()}, kLine);

  SaveDC(dc);
  IntersectClipRect(dc, sr.left, sr.top, sr.right, sr.bottom);

  HFONT f_small = MakeFont(9, false, m.dpi);
  HFONT f_body = MakeFont(10, false, m.dpi);
  HFONT f_bold = MakeFont(10, true, m.dpi);
  HFONT f_title = MakeFont(13, true, m.dpi);

  const SidebarGeom g = ComputeSidebarGeom(L);
  const int x = L.sidebar.x + m.px(20);
  const int w = std::max(0, L.sidebar.w - m.px(40));
  int y = m.px(14);

  // 这一行是"栏目名"，下面那行彩色的是"状态"。两行都写"正在播放"
  // 会像复制粘贴出了 bug，所以栏目名固定用"当前文件"。
  TextLeft(dc, L"当前文件", x, y, w, m.px(14), f_small, kTextMute);
  y += m.px(18);

  // 标题最多占两行；超出的部分被 rect 直接裁掉，不做滚动（取值 > 省略更不易误读）
  RECT tr{x, y, x + w, y + m.px(42)};
  DrawTextC(dc, M.title.empty() ? L"未打开文件" : M.title, tr, f_title, kText,
            DT_LEFT | DT_WORDBREAK);
  y += m.px(44);

  TextLeft(dc, M.status.empty() ? L"就绪" : M.status, x, y, w, m.px(14), f_bold,
           P.paused ? kAccentWarm : kAccent);
  y += m.px(21);

  FillRectC(dc, RECT{x, y, x + w, y + 1}, kLine);
  y += m.px(9);

  // ★ 行高与"要不要显示片源分析"按剩余空间决定。
  //   预算不够时不是硬挤（挤出来每行 14px，字都糊在一起），而是退一步只显示
  //   基本信息——8 行硬指标 + 4 行分析。1080p 屏幕上两者都在，
  //   1366×768 的小本上则先保基本信息，这是更合理的取舍。
  const int avail = g.content_bottom - y;
  int row_h = m.px(23);
  bool show_analysis = true;
  if (avail < m.px(23) * 12 + m.px(38)) {
    // 第二档：行高收紧到 20px 仍放得下 12 行（字还能看清），就保留完整信息
    if (avail >= m.px(20) * 12 + m.px(34)) {
      row_h = m.px(20);
    } else {
      show_analysis = false;
      row_h = m.px(22);
      if (avail < m.px(22) * 8) row_h = std::max(m.px(16), avail / 8);
    }
  }

  auto row = [&](const wchar_t* label, const std::wstring& value) {
    if (y + row_h > g.content_bottom) return;
    const int th = row_h - m.px(2);
    TextLeft(dc, label, x, y, m.px(70), th, f_small, kTextMute);
    TextRight(dc, value.empty() ? L"—" : value, x + m.px(70), y, w - m.px(70), th, f_body,
              kText);
    y += row_h;
  };

  row(L"时长", M.duration);
  row(L"分辨率", M.resolution);
  row(L"画面比例", M.aspect);
  row(L"帧率", M.fps);
  row(L"视频编码", M.vcodec);
  row(L"音频编码", M.acodec);
  row(L"码率", M.bitrate);
  row(L"硬件解码", M.hwdec);

  if (show_analysis) {
    y += m.px(2);
    FillRectC(dc, RECT{x, y, x + w, y + 1}, kLine);
    y += m.px(8);

    TextLeft(dc, L"片源分析", x, y, w, m.px(14), f_small, kTextMute);
    y += m.px(16);

    if (!M.has_analysis) {
      TextLeft(dc, M.has_media ? L"分析中…" : L"打开文件后自动分析", x, y, w, row_h,
               f_body, kTextMute);
      y += row_h;
    } else {
      // 6 条独立信息压成 4 行：场序并入片源行、前处理并入处理行。
      // 侧栏不是日志，用户扫一眼要能看懂"这片子经历了什么"。
      std::wstring src = M.cadence;
      if (!M.field_order.empty()) {
        // 逐行片源会出现"逐行 · 逐行（自动）"这种自我重复，合并成一个
        // （保留带"（自动）"的那个，它信息量更大）。
        if (!src.empty() && M.field_order.rfind(src, 0) == 0) {
          src = M.field_order;
        } else if (!src.empty()) {
          src += L" · ";
          src += M.field_order;
        } else {
          src = M.field_order;
        }
      }
      row(L"片源", src);
      row(L"处理", M.chain);
      row(L"色彩", M.color);
      row(L"超分", M.super_res);
    }
  }

  if (!M.warning.empty() && y + m.px(18) <= g.content_bottom) {
    TextLeft(dc, M.warning, x, y + m.px(2), w, m.px(18), f_small, kAccentWarm);
  }

  // —— 底部按钮 ——
  // 打开文件：主操作，用强调色实心块；其余用描边块，避免一屏全是蓝色。
  ButtonBg(dc, g.open, P, kHitOpen, false);
  FillRound(dc, g.open, 8, kAccent);
  TextCenter(dc, L"打开文件…", g.open.x, g.open.y, g.open.w, g.open.h, f_bold,
             RGB(255, 255, 255));

  ButtonBg(dc, g.quality, P, kHitSettings, false);
  FillRound(dc, g.quality, 8, RGB(31, 36, 45));
  TextCenter(dc, M.quality.empty() ? L"画质与尺寸" : M.quality, g.quality.x, g.quality.y,
             g.quality.w, g.quality.h, f_body, kText);

  auto small_btn = [&](const Rect& r, const std::wstring& text, int hit, bool on) {
    ButtonBg(dc, r, P, hit, false);
    FillRound(dc, r, 6, on ? RGB(33, 42, 58) : RGB(28, 32, 40));
    TextCenter(dc, text, r.x, r.y, r.w, r.h, f_small, on ? kAccentHot : kTextDim);
  };
  small_btn(g.chain, M.chain_label, kHitChain, P.chain_on);
  small_btn(g.field, M.field_label, kHitFieldOrder, M.field_label.find(L"自动") ==
                                                          std::wstring::npos);

  // 内容区与按钮之间画一条分隔，避免信息行贴着按钮
  FillRectC(dc, RECT{x, g.content_bottom, x + w, g.content_bottom + 1}, kLine);

  SelectObject(dc, f_small);
  DeleteObject(f_small);
  DeleteObject(f_body);
  DeleteObject(f_bold);
  DeleteObject(f_title);
  RestoreDC(dc, -1);
}

// ---------------------------------------------------------------------------
// 控制条
// ---------------------------------------------------------------------------
void DrawControls(HDC dc, const Layout& L, const MediaInfo& M, const PlayState& P) {
  const Metrics& m = L.m;
  FillRectC(dc, L.controls, kBar);
  FillRectC(dc, RECT{0, L.controls.y, L.controls.right(), L.controls.y + 1}, kLine);

  const ControlGeom g = ComputeControlGeom(L);

  HFONT f_small = MakeFont(9, false, m.dpi);
  HFONT f_mono = MakeFont(10, false, m.dpi, L"Consolas");
  HFONT f_icon = MakeFont(9, true, m.dpi);

  // —— 进度条 ——
  // 轨道画 4px 细条，但热点上下各扩 9px：4px 的高度用鼠标点会很难受，
  // 而视觉上加粗又会让界面显笨重。
  FillRound(dc, g.seek, 2, kTrack);
  const double dur = (P.dur > 0.5) ? P.dur : 0.0;
  const double shown = P.dragging_seek ? P.drag_pos : P.pos;
  double ratio = (dur > 0.0) ? shown / dur : 0.0;
  if (ratio < 0.0) ratio = 0.0;
  if (ratio > 1.0) ratio = 1.0;

  const int fw = static_cast<int>(g.seek.w * ratio + 0.5);
  if (fw > 1) {
    FillRound(dc, Rect{g.seek.x, g.seek.y, fw, g.seek.h}, 2, kAccent);
  }
  // 滑块：悬停/拖动时变大，给"抓得住"的反馈
  const bool active = (P.hover == kHitSeekBar) || P.dragging_seek;
  const int kr = m.px(active ? 7 : 5);
  const int kcx = g.seek.x + fw;
  const int kcy = g.seek.y + g.seek.h / 2;
  {
    HBRUSH br = CreateSolidBrush(active ? kAccentHot : kAccent);
    HPEN pen = CreatePen(PS_NULL, 0, kAccent);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    Ellipse(dc, kcx - kr, kcy - kr, kcx + kr + 1, kcy + kr + 1);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
  }

  // —— 左侧按钮 ——
  ButtonBg(dc, g.open, P, kHitOpen, true);
  IconFolder(dc, g.open.x + g.open.w / 2, g.open.y + g.open.h / 2, m.px(18), kTextDim);

  ButtonBg(dc, g.back, P, kHitBack, false);
  TextCenter(dc, L"«10", g.back.x, g.back.y, g.back.w, g.back.h, f_icon, kText);

  ButtonBg(dc, g.play, P, kHitPlayPause, false);
  {
    const int cx = g.play.x + g.play.w / 2;
    const int cy = g.play.y + g.play.h / 2;
    if (P.eof) {
      TextCenter(dc, L"↻", g.play.x, g.play.y - m.px(1), g.play.w, g.play.h, f_icon,
                 kAccent);
    } else if (P.paused) {
      IconPlay(dc, cx, cy, m.px(18), kAccent);
    } else {
      IconPause(dc, cx, cy, m.px(16), kAccent);
    }
  }

  ButtonBg(dc, g.forward, P, kHitForward, false);
  TextCenter(dc, L"10»", g.forward.x, g.forward.y, g.forward.w, g.forward.h, f_icon, kText);

  // —— 音量 ——
  if (g.show_volume) {
    const int level = P.muted ? 0 : (P.volume >= 50 ? 2 : (P.volume > 0 ? 1 : 0));
    IconSpeaker(dc, g.volume_icon.x + g.volume_icon.w / 2, g.volume_icon.y + g.volume_icon.h / 2,
                m.px(16), kTextDim, level);
    Rect vt = g.volume_track;
    FillRound(dc, vt, 2, kTrack);
    double vr = P.muted ? 0.0 : P.volume / 100.0;
    if (vr < 0.0) vr = 0.0;
    if (vr > 1.0) vr = 1.0;
    const int vw = static_cast<int>(vt.w * vr + 0.5);
    if (vw > 1) FillRound(dc, Rect{vt.x, vt.y, vw, vt.h}, 2, kTextDim);
    const int vkr = m.px(P.dragging_volume ? 6 : 4);
    const int vcx = vt.x + vw;
    const int vcy = vt.y + vt.h / 2;
    HBRUSH br = CreateSolidBrush(kText);
    HPEN pen = CreatePen(PS_NULL, 0, kText);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    Ellipse(dc, vcx - vkr, vcy - vkr, vcx + vkr + 1, vcy + vkr + 1);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
  }

  // —— 右侧 ——
  if (g.show_speed) {
    TextCenter(dc, [&] {
                 wchar_t b[32];
                 swprintf(b, 32, L"%.1f×", P.speed);
                 return std::wstring(b);
               }(),
               g.speed_text.x, g.speed_text.y, g.speed_text.w, g.speed_text.h, f_small,
               kTextDim);
  }

  // 时间：拖动时显示拖动目标，让用户知道松手会跳到哪
  {
    std::wstring t = FormatClock(shown);
    t += L" / ";
    t += FormatClock(dur);
    TextRight(dc, t, g.time_text.x, g.time_text.y, g.time_text.w, g.time_text.h, f_mono,
              P.dragging_seek ? kAccentHot : kTextDim);
  }

  ButtonBg(dc, g.settings, P, kHitSettings, true);
  IconMenu(dc, g.settings.x + g.settings.w / 2, g.settings.y + g.settings.h / 2, m.px(16),
           kTextDim);

  ButtonBg(dc, g.fullscreen, P, kHitFullscreen, true);
  IconFullscreen(dc, g.fullscreen.x + g.fullscreen.w / 2,
                 g.fullscreen.y + g.fullscreen.h / 2, m.px(16), kTextDim);

  DeleteObject(f_small);
  DeleteObject(f_mono);
  DeleteObject(f_icon);
}

// ---------------------------------------------------------------------------
// 欢迎页（没有打开文件时占满视频区）
// ---------------------------------------------------------------------------
void DrawWelcome(HDC dc, const Layout& L, const PlayState& P) {
  const Metrics& m = L.m;
  if (L.video.empty()) return;

  HFONT f_title = MakeFont(26, true, m.dpi);
  HFONT f_sub = MakeFont(11, false, m.dpi);
  HFONT f_btn = MakeFont(11, true, m.dpi);
  HFONT f_tip = MakeFont(9, false, m.dpi);

  const int cx = L.video.x + L.video.w / 2;
  const int cy = L.video.y + L.video.h / 2;
  const int tw = std::min(L.video.w - m.px(40), m.px(560));

  TextCenter(dc, L"LumenPlayer", cx - tw / 2, cy - m.px(118), tw, m.px(40), f_title, kText);
  TextCenter(dc, L"把视频文件拖进窗口，或点击下面的按钮选择", cx - tw / 2, cy - m.px(70),
             tw, m.px(24), f_sub, kTextDim);

  const Rect btn = WelcomeButtonRect(L);
  const bool hover = (P.hover == kHitWelcomeOpen);
  FillRound(dc, btn, 10, hover ? kAccentHot : kAccent);
  TextCenter(dc, L"选择视频文件…", btn.x, btn.y, btn.w, btn.h, f_btn, RGB(255, 255, 255));

  TextCenter(dc, L"支持 mp4 / mkv / avi / ts / m2ts / mov / flv / wmv / rmvb", cx - tw / 2,
             btn.bottom() + m.px(18), tw, m.px(20), f_tip, kTextMute);
  TextCenter(dc, L"空格 暂停   ← → 快退快进   F 全屏   O 打开   H 快捷键", cx - tw / 2,
             btn.bottom() + m.px(38), tw, m.px(20), f_tip, kTextMute);

  DeleteObject(f_title);
  DeleteObject(f_sub);
  DeleteObject(f_btn);
  DeleteObject(f_tip);
}

} // namespace

// ---------------------------------------------------------------------------
// 对外实现
// ---------------------------------------------------------------------------
Layout ComputeLayout(int cw, int ch, UINT dpi, bool sidebar_on, bool fullscreen) {
  Layout L;
  L.m.dpi = (dpi >= 72 && dpi <= 480) ? static_cast<int>(dpi) : 96;
  L.client = Rect{0, 0, cw, ch};
  L.fullscreen = fullscreen;

  int sw = 0;
  if (sidebar_on && !fullscreen) {
    sw = std::min(L.m.px(304), cw / 3);
    // 太窄时侧栏会把视频挤成一条缝，这时自动收起（用户下次拉宽会自动回来）
    if (sw < L.m.px(230)) sw = 0;
  }
  L.sidebar_w = sw;
  L.controls_h = L.m.px(fullscreen ? 62 : 88);

  int vh = ch - L.controls_h;
  if (vh < 0) vh = 0;
  L.video = Rect{0, 0, std::max(0, cw - sw), vh};
  L.controls = Rect{0, vh, std::max(0, cw - sw), L.controls_h};
  L.sidebar = Rect{cw - sw, 0, sw, ch};
  return L;
}

Rect SeekTrackRect(const Layout& L) { return ComputeControlGeom(L).seek; }
Rect VolumeTrackRect(const Layout& L) { return ComputeControlGeom(L).volume_track; }

Rect WelcomeButtonRect(const Layout& L) {
  const Metrics& m = L.m;
  const int w = m.px(208);
  const int h = m.px(44);
  const int cx = L.video.x + L.video.w / 2;
  const int cy = L.video.y + L.video.h / 2;
  return Rect{cx - w / 2, cy - m.px(10), w, h};
}

double SeekTimeAt(const Layout& L, int x, double dur) {
  if (dur <= 0.0) return 0.0;
  const Rect t = SeekTrackRect(L);
  if (t.w <= 0) return 0.0;
  double r = static_cast<double>(x - t.x) / static_cast<double>(t.w);
  if (r < 0.0) r = 0.0;
  if (r > 1.0) r = 1.0;
  return r * dur;
}

double VolumeAt(const Layout& L, int x) {
  const Rect t = VolumeTrackRect(L);
  if (t.w <= 0) return 0.0;
  double r = static_cast<double>(x - t.x) / static_cast<double>(t.w);
  if (r < 0.0) r = 0.0;
  if (r > 1.0) r = 1.0;
  return r * 100.0;
}

int HitTest(const Layout& L, const PlayState& P, int x, int y) {
  if (!L.client.contains(x, y)) return kHitNone;

  // 侧栏优先：侧栏与控制条不重叠，但写在前面的分支更省事、也更好读
  if (!L.sidebar.empty() && L.sidebar.contains(x, y)) {
    const SidebarGeom g = ComputeSidebarGeom(L);
    if (g.open.contains(x, y)) return kHitOpen;
    if (g.quality.contains(x, y)) return kHitSettings;
    if (g.chain.contains(x, y)) return kHitChain;
    if (g.field.contains(x, y)) return kHitFieldOrder;
    return kHitNone;
  }

  if (!L.controls.empty() && L.controls.contains(x, y)) {
    const ControlGeom g = ComputeControlGeom(L);
    // 进度条热点：上下各扩 9px
    const int hot = L.m.px(9);
    if (x >= g.seek.x && x < g.seek.right() && y >= g.seek.y - hot &&
        y < g.seek.bottom() + hot) {
      return kHitSeekBar;
    }
    if (g.open.contains(x, y)) return kHitOpen;
    if (g.back.contains(x, y)) return kHitBack;
    if (g.play.contains(x, y)) return kHitPlayPause;
    if (g.forward.contains(x, y)) return kHitForward;
    if (g.show_volume && g.volume_icon.contains(x, y)) return kHitVolume;
    if (g.show_volume) {
      const int vh = L.m.px(9);
      if (x >= g.volume_track.x && x < g.volume_track.right() &&
          y >= g.volume_track.y - vh && y < g.volume_track.bottom() + vh) {
        return kHitVolume;
      }
    }
    if (g.settings.contains(x, y)) return kHitSettings;
    if (g.fullscreen.contains(x, y)) return kHitFullscreen;
    return kHitNone;
  }

  if (L.video.contains(x, y)) {
    // 欢迎页的"选择视频文件"按钮也落在这里：是否处于欢迎态只有 main.cpp 知道
    // （取决于有没有打开过文件），所以由它先判断，这里统一返回视频区。
    return kHitVideoArea;
  }
  return kHitNone;
}

void Paint(HDC dc, const Layout& L, const MediaInfo& M, const PlayState& P) {
  FillRectC(dc, L.client, kBg);
  if (!L.sidebar.empty()) DrawSidebar(dc, L, M, P);
  if (!L.controls.empty()) DrawControls(dc, L, M, P);
  if (!M.has_media) DrawWelcome(dc, L, P);
}

std::wstring FormatClock(double sec) {
  if (!(sec > 0.0)) sec = 0.0; // NaN 与负数统一归零
  // 用 int 而不是 long long：一部片子再长也不会超过 2^31 秒，
  // 而 MinGW 的 swprintf 对 %lld 的支持依赖具体 CRT，换成 %d 更稳。
  int total = static_cast<int>(sec + 0.5);
  if (total < 0) total = 0;
  const int h = total / 3600;
  const int m = (total % 3600) / 60;
  const int s = total % 60;
  wchar_t buf[32];
  if (h > 0) {
    swprintf(buf, 32, L"%d:%02d:%02d", h, m, s);
  } else {
    swprintf(buf, 32, L"%02d:%02d", m, s);
  }
  return std::wstring(buf);
}

std::wstring FormatBitrate(double bps) {
  if (!(bps > 0.0)) return L"—";
  wchar_t buf[64];
  if (bps >= 1e6) {
    swprintf(buf, 64, L"%.2f Mbps", bps / 1e6);
  } else if (bps >= 1e3) {
    swprintf(buf, 64, L"%.0f kbps", bps / 1e3);
  } else {
    swprintf(buf, 64, L"%.0f bps", bps);
  }
  return std::wstring(buf);
}

} // namespace ui
} // namespace lumen
