// ============================================================================
//  LumenPlayer - 基础类型定义
//  ---------------------------------------------------------------------------
//  所属层次：产品层（C 层，100% 自研，无第三方依赖）
//  职责    ：定义全项目共用的枚举与轻量值对象。
//
//  设计约束：
//    1. 本文件不允许 include 任何第三方头文件（含 mpv / ONNX / ffmpeg），
//       否则策略层会被绑死在某个具体实现上。
//    2. 色彩空间、像素格式、场序这类概念只在这里定义一次，
//       其他模块一律复用，禁止各自定义一套（口径分裂是画质 bug 的头号来源）。
//    3. 全部为值语义，可平凡拷贝，便于跨线程/跨进程传递。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

namespace lumen {

// ---------------------------------------------------------------------------
// 像素格式
//  ---------------------------------------------------------------------------
// 说明：只列出本产品实际会遇到的格式。老片片源绝大多数是 4:2:0 8bit。
//       10bit（P010）只在部分蓝光重制版出现，用于避免反复升降位深造成量化损伤。
enum class PixelFormat {
  Unknown = 0,
  YUV420P,   // 8bit 4:2:0 planar，DVD / 老剧集主力
  YUV420P10, // 10bit 4:2:0 planar，P010 语义
  YUV422P,   // 8bit 4:2:2，部分专业母带
  YUV444P,   // 8bit 4:4:4，极少见
  NV12,      // 半平面 4:2:0，硬解输出常用
  RGB24,     // 8bit 打包 RGB
  RGBA32,    // 8bit RGBA，UI 截图/交换用
};

// 单个像素平均占用的比特数，用于估算显存与带宽。
// 参数说明：fmt 为待查询的像素格式
// 返回值  ：每像素比特数，未知格式返回 0
int BitsPerPixel(PixelFormat fmt);

// 是否为 4:2:0 色度抽样（决定能否直接用 NV12 硬件路径）。
bool Is420(PixelFormat fmt);

// 人可读名称，用于日志与自检报告输出。
const char* ToString(PixelFormat fmt);

// ---------------------------------------------------------------------------
// 色彩空间
//  ---------------------------------------------------------------------------
// 说明：90 年代标清素材按 BT.601 拍摄/压制，若被当作 BT.709 解码，
//       会出现整体偏色（绿色发黄、肤色发灰）。这是老片修复最常见的错误之一。
enum class ColorSpace {
  Unspecified = 0, // 未标记：需按分辨率与时代表推断
  BT601,           // SD 标清标准（525/625 行时代）
  BT709,           // HD 高清标准（本产品的目标空间）
  BT2020,          // UHD / HDR
};

// 色彩范围：Limited（16-235）为广播标准，Full（0-255）为 PC 范围。
// 搞错的典型症状是画面发灰（把 Limited 当 Full）或暗部死黑（反之）。
enum class ColorRange {
  Unspecified = 0,
  Limited,
  Full,
};

// 传递函数（gamma 曲线）。老片只有 BT1886/SDR，此处保留扩展位。
enum class TransferFunction {
  Unspecified = 0,
  BT709,  // SDR 标准
  SRGB,
  PQ,     // HDR10
  HLG,    // 广播 HDR
};

// ---------------------------------------------------------------------------
// 场序 / 扫描方式
// ---------------------------------------------------------------------------
// 说明：这是本产品最关键的一个维度。90 年代电视剧大量是 480i/576i 隔行，
//       或电影转电视产生的 3:2 pulldown（telecine）。
//       把隔行帧直接喂给超分模型 = 把梳齿当成细节放大，结果比不处理更差。
enum class FieldOrder {
  Unknown = 0,
  Progressive, // 逐行，可直接超分
  TopFieldFirst,    // 上场优先（TFF），需反交错
  BottomFieldFirst, // 下场优先（BFF），需反交错
};

// 片源的时域结构判定结论
enum class SourceCadence {
  Unknown = 0,
  Progressive,   // 真逐行
  Interlaced,    // 真隔行（摄像机隔行拍摄，如老剧、体育）
  Telecine,      // 24p 经 3:2 pulldown 转为 29.97i，可逆
  Mixed,         // 混合：部分镜头隔行、部分逐行（最常见也最难）
};

const char* ToString(ColorSpace cs);
const char* ToString(ColorRange cr);
const char* ToString(FieldOrder fo);
const char* ToString(SourceCadence sc);

// 解析用户手写的场序字符串，供命令行 --field-order 与配置文件使用。
// 接受：auto/unknown（自动）、prog/progressive、tff/top、bff/bottom。
// 返回值：true 表示识别成功并写入 *out。
// 为什么需要它：场序的像素级判定（奇偶行差比）在噪点大或运动快的镜头上
// 会给出错误结论，而场序选反的后果是反交错后画面"上下抖动"——肉眼可见但
// 很难归因。给用户一个显式覆盖开关，比追求 100% 自动判定更务实。
bool ParseFieldOrder(const std::string& s, FieldOrder* out);

// ---------------------------------------------------------------------------
// 有理数（帧率、宽高比、时间基都用它，禁止用 double 表示帧率）
// ---------------------------------------------------------------------------
// 原因：29.97 = 30000/1001，用 double 存会在长时间轴上累积漂移，
//       导致音画不同步。这是播放器开发的经典坑。
struct Rational {
  int num = 0; // 分子
  int den = 1; // 分母

  Rational() = default;
  Rational(int n, int d) : num(n), den(d == 0 ? 1 : d) {}

  // 转为浮点，仅用于展示与阈值比较，不得用于时间轴累加
  double ToDouble() const { return den == 0 ? 0.0 : static_cast<double>(num) / den; }

  bool Valid() const { return num > 0 && den > 0; }

  // 帧率是否为 NTSC 系（23.976 / 29.97 / 59.94）
  // 用途：NTSC 系是判断 telecine 与色域（601）的重要线索
  bool IsNtscFamily() const;
};

// ---------------------------------------------------------------------------
// 时间戳
// ---------------------------------------------------------------------------
// 单位统一为「微秒」，与 ffmpeg 的 AV_TIME_BASE 一致，减少单位换算错误。
using Timestamp = int64_t;
constexpr Timestamp kTimestampNone = INT64_MIN;

// 微秒 → 毫秒（浮点），用于性能统计
inline double UsToMs(int64_t us) { return static_cast<double>(us) / 1000.0; }

// ---------------------------------------------------------------------------
// 矩形区域（裁剪、ROI 分析用）
// ---------------------------------------------------------------------------
struct Rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;

  bool Empty() const { return w <= 0 || h <= 0; }
  int Area() const { return Empty() ? 0 : w * h; }
};

// ---------------------------------------------------------------------------
// 错误码
// ---------------------------------------------------------------------------
// 约束（见 ADR-001 接缝契约）：跨模块边界一律用错误码，禁止抛异常。
// 原因：播放器里有实时线程与 GPU 回调，异常穿越 C ABI 是未定义行为。
enum class Status {
  Ok = 0,
  InvalidArgument,  // 参数非法
  NotFound,         // 文件/库/模型不存在
  Unsupported,      // 格式或能力不支持
  IoError,          // 读写失败
  OutOfMemory,      // 显存/内存不足
  DeviceLost,       // GPU 设备丢失（驱动重置、TDR）
  Timeout,          // 超时
  Internal,         // 内部错误（不应发生）
};

const char* ToString(Status st);

} // namespace lumen
