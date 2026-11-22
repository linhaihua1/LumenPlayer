// ============================================================================
//  LumenPlayer - 单元测试
//  ---------------------------------------------------------------------------
//  说明：不引入 gtest / Catch2，用最小的断言宏自建 runner。
//  原因：策略层要求零第三方依赖，测试也不应该例外；
//        且 CI 只需要"通过/失败 + 失败行号"，不需要复杂特性。
//
//  运行：lumen_tests.exe
//  返回：全部通过返回 0，有失败返回 1（便于脚本判断）
// ============================================================================
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/color_policy.h"
#include "core/config.h"
#include "core/field_analyzer.h"
#include "core/frame_buffer.h"
#include "core/grain_policy.h"
#include "core/json.h"
#include "core/processing_chain.h"
#include "core/source_info.h"
#include "engine/dummy_backend.h"
#include "engine/onnx_backend.h"

namespace {

// 构造测试帧的辅助函数里频繁用到这几个类型，做局部 using，
// 避免每处都写全限定名而让断言本身变得难读。
using lumen::FrameBuffer;
using lumen::PixelFormat;
using lumen::PlaneView;

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (cond) {                                                                  \
      ++g_pass;                                                                  \
    } else {                                                                     \
      ++g_fail;                                                                  \
      std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                            \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                    \
  do {                                                                           \
    const double va = (a);                                                       \
    const double vb = (b);                                                       \
    if (std::fabs(va - vb) <= (eps)) {                                           \
      ++g_pass;                                                                  \
    } else {                                                                     \
      ++g_fail;                                                                  \
      std::printf("  [FAIL] %s:%d  %s=%.4f != %s=%.4f (eps %.4f)\n", __FILE__,   \
                  __LINE__, #a, va, #b, vb, static_cast<double>(eps));           \
    }                                                                            \
  } while (0)

#define CHECK_EQ_STR(a, b)                                                       \
  do {                                                                           \
    const std::string va = (a);                                                   \
    const std::string vb = (b);                                                   \
    if (va == vb) {                                                               \
      ++g_pass;                                                                   \
    } else {                                                                      \
      ++g_fail;                                                                   \
      std::printf("  [FAIL] %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__,       \
                  va.c_str(), vb.c_str());                                        \
    }                                                                             \
  } while (0)

struct TestCase {
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& Registry() {
  static std::vector<TestCase> r;
  return r;
}

struct Register {
  Register(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};

#define TEST(name)                                        \
  static void name();                                     \
  static Register reg_##name(#name, &name);               \
  static void name()

// ---------------------------------------------------------------------------
// 构造测试帧
// ---------------------------------------------------------------------------
// 生成"垂直条纹"画面：水平方向有强边缘、垂直方向恒定。
// 垂直恒定这一点很重要——它让逐行画面完全没有行间差异，
// 从而能干净地把"梳齿"与"景物纹理"区分开。
//
// 参数说明：shift 为奇数行相对偶数行的水平位移（像素）。
//          shift != 0 时模拟隔行拍摄：两场来自不同时刻，边缘错位。
void MakeStripes(FrameBuffer* fb, int shift, int w = 320, int h = 240) {
  fb->Allocate(w, h, PixelFormat::YUV420P);
  const PlaneView y = fb->Plane(0);
  for (int r = 0; r < h; ++r) {
    uint8_t* row = y.data + static_cast<size_t>(r) * y.stride;
    const int s = (r & 1) ? shift : 0;
    for (int x = 0; x < w; ++x) {
      const int xx = (x + s) % w;
      row[x] = ((xx / 16) % 2) ? 210 : 40;
    }
  }
  // 色度填中性灰，避免后续转换出现非法值
  for (int p = 1; p <= 2; ++p) {
    const PlaneView c = fb->Plane(p);
    for (int r = 0; r < fb->PlaneHeight(p); ++r) {
      std::memset(c.data + static_cast<size_t>(r) * c.stride, 128,
                  static_cast<size_t>(fb->PlaneWidth(p)));
    }
  }
}

// 生成平坦灰 + 均匀噪声（用于颗粒估计测试）
void MakeFlatNoise(FrameBuffer* fb, int noise, int w = 160, int h = 160) {
  fb->Allocate(w, h, PixelFormat::YUV420P);
  const PlaneView y = fb->Plane(0);
  unsigned seed = 12345u;
  for (int r = 0; r < h; ++r) {
    uint8_t* row = y.data + static_cast<size_t>(r) * y.stride;
    for (int x = 0; x < w; ++x) {
      // 线性同余伪随机，避免依赖 <random> 的实现差异
      seed = seed * 1103515245u + 12345u;
      const int rnd = static_cast<int>((seed >> 16) & 0x7FFF) % (2 * noise + 1);
      row[x] = static_cast<uint8_t>(128 + rnd - noise);
    }
  }
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
TEST(JsonBasic) {
  const char* text =
      "{\"a\":1,\"b\":[10,20,{\"c\":\"x\"}],\"d\":\"he said \\\"hi\\\"\","
      "\"e\":true,\"f\":null,\"nested\":{\"deep\":{\"v\":42}}}";
  std::string err;
  const lumen::JsonValue v = lumen::JsonValue::Parse(text, &err);
  CHECK(err.empty());
  CHECK(v.IsObject());
  CHECK(v["a"].AsInt() == 1);
  CHECK(v["b"].size() == 3);
  CHECK_EQ_STR(v.Query("b.2.c").AsString(), "x");
  CHECK_EQ_STR(v["d"].AsString(), "he said \"hi\"");
  CHECK(v["e"].AsBool());
  CHECK(v["f"].IsNull());
  CHECK(v.Query("nested.deep.v").AsInt() == 42);

  // 不存在的路径必须安全返回 Null，不能崩
  CHECK(v.Query("no.such.path").IsNull());
  CHECK(v["missing"].IsNull());
}

TEST(JsonUnicodeEscape) {
  // \u4e2d\u6587 = "中文"
  const char* text = "{\"t\":\"\\u4e2d\\u6587\"}";
  const lumen::JsonValue v = lumen::JsonValue::Parse(text);
  CHECK_EQ_STR(v["t"].AsString(), "中文");
}

TEST(JsonInvalid) {
  std::string err;
  const lumen::JsonValue v1 = lumen::JsonValue::Parse("{\"a\":", &err);
  CHECK(v1.IsNull());
  CHECK(!err.empty());

  err.clear();
  const lumen::JsonValue v2 = lumen::JsonValue::Parse("not json", &err);
  CHECK(v2.IsNull());
  CHECK(!err.empty());
}

// ---------------------------------------------------------------------------
// 帧缓冲
// ---------------------------------------------------------------------------
TEST(FrameBufferLayout) {
  lumen::FrameBuffer fb;
  CHECK(fb.Allocate(720, 480, lumen::PixelFormat::YUV420P) == lumen::Status::Ok);
  CHECK(fb.width() == 720);
  CHECK(fb.height() == 480);
  CHECK(fb.plane_count() == 3);
  CHECK(fb.PlaneHeight(0) == 480);
  CHECK(fb.PlaneHeight(1) == 240);
  CHECK(fb.Plane(0).data != nullptr);
  CHECK(fb.Plane(0).stride >= 720); // 行对齐后不应小于有效宽度
  CHECK(fb.byte_size() > 0);

  const lumen::FrameDesc d = fb.ToDesc();
  CHECK(d.width == 720);
  CHECK(d.location == lumen::FrameLocation::Cpu);
  CHECK(d.plane_data[0] == fb.Plane(0).data);
}

TEST(FrameBufferInvalid) {
  lumen::FrameBuffer fb;
  CHECK(fb.Allocate(0, 100, lumen::PixelFormat::YUV420P) ==
        lumen::Status::InvalidArgument);
  CHECK(fb.Allocate(100, 100, lumen::PixelFormat::Unknown) ==
        lumen::Status::Unsupported);
}

// ---------------------------------------------------------------------------
// 宽高比分类
// ---------------------------------------------------------------------------
TEST(AspectClassify) {
  CHECK(lumen::ClassifyAspect(4.0 / 3.0) == lumen::AspectClass::Ratio4x3);
  CHECK(lumen::ClassifyAspect(1.37) == lumen::AspectClass::Ratio4x3); // 学院画幅
  CHECK(lumen::ClassifyAspect(16.0 / 9.0) == lumen::AspectClass::Ratio16x9);
  CHECK(lumen::ClassifyAspect(1.78) == lumen::AspectClass::Ratio16x9);
  CHECK(lumen::ClassifyAspect(2.35) == lumen::AspectClass::Ratio235);
  CHECK(lumen::ClassifyAspect(0.0) == lumen::AspectClass::Unknown);
}

// ---------------------------------------------------------------------------
// 场结构分析（核心算法）
// ---------------------------------------------------------------------------
TEST(FieldProgressiveIsClean) {
  lumen::FrameBuffer fb;
  MakeStripes(&fb, 0); // 所有行一致 = 逐行
  const lumen::FieldStats st = lumen::AnalyzeField(fb);
  CHECK(st.Valid());
  CHECK(st.combing_pixel_ratio < 0.002); // 应几乎为 0
  CHECK(!st.has_field_structure);
}

TEST(FieldInterlacedIsDetected) {
  lumen::FrameBuffer fb;
  MakeStripes(&fb, 4); // 奇偶行错位 4 像素 = 隔行梳齿
  const lumen::FieldStats st = lumen::AnalyzeField(fb);
  CHECK(st.Valid());
  CHECK(st.combing_pixel_ratio > 0.01);
  CHECK(st.has_field_structure);
}

TEST(CadenceDetectorInterlaced) {
  lumen::CadenceDetector det;
  for (int i = 0; i < 16; ++i) {
    lumen::FrameBuffer fb;
    MakeStripes(&fb, 4);
    det.Feed(fb);
  }
  const lumen::CadenceVerdict v = det.Verdict();
  CHECK(v.cadence == lumen::SourceCadence::Interlaced);
  CHECK(v.needs_deinterlace);
  CHECK(v.confidence > 0.5);
}

TEST(CadenceDetectorProgressive) {
  lumen::CadenceDetector det;
  for (int i = 0; i < 16; ++i) {
    lumen::FrameBuffer fb;
    MakeStripes(&fb, 0);
    det.Feed(fb);
  }
  const lumen::CadenceVerdict v = det.Verdict();
  CHECK(v.cadence == lumen::SourceCadence::Progressive);
  CHECK(!v.needs_deinterlace);
}

TEST(CadenceInsufficientSamples) {
  lumen::CadenceDetector det;
  lumen::FrameBuffer fb;
  MakeStripes(&fb, 0);
  det.Feed(fb); // 只喂 1 帧
  const lumen::CadenceVerdict v = det.Verdict();
  CHECK(v.cadence == lumen::SourceCadence::Unknown);
  CHECK_NEAR(v.confidence, 0.0, 1e-9);
}

// ---------------------------------------------------------------------------
// 色彩策略
// ---------------------------------------------------------------------------
TEST(ColorInferSd601) {
  lumen::FrameDesc d;
  d.width = 720;
  d.height = 480;
  d.format = lumen::PixelFormat::YUV420P;
  const lumen::ColorDecision c = lumen::DecideColor(d, nullptr);
  CHECK(c.source_space == lumen::ColorSpace::BT601);
  CHECK(c.needs_matrix_conversion);
  CHECK(c.guessed);
  CHECK(c.source_range == lumen::ColorRange::Limited);
}

TEST(ColorInferHd709) {
  lumen::FrameDesc d;
  d.width = 1280;
  d.height = 720;
  const lumen::ColorDecision c = lumen::DecideColor(d, nullptr);
  CHECK(c.source_space == lumen::ColorSpace::BT709);
  CHECK(!c.needs_matrix_conversion);
}

TEST(ColorExplicitTagWins) {
  lumen::FrameDesc d;
  d.width = 720;
  d.height = 480;
  d.colorspace = lumen::ColorSpace::BT709; // 标清却标 709
  const lumen::ColorDecision c = lumen::DecideColor(d, nullptr);
  CHECK(c.source_space == lumen::ColorSpace::BT709);
  CHECK(!c.needs_matrix_conversion); // 元数据优先：标注了 709 就不做矩阵转换
  // 注意：guessed 的语义是"任一维度来自推断"。本例中色彩空间有元数据，
  // 但色彩范围未标注仍需推断，因此 guessed 为 true 是预期行为。
}

TEST(ColorRangeProbeFull) {
  lumen::FrameBuffer fb;
  MakeStripes(&fb, 0);
  // 把条纹明暗拉到 0 与 255，构造 Full range 特征
  const lumen::PlaneView y = fb.Plane(0);
  for (int r = 0; r < fb.height(); ++r) {
    uint8_t* row = y.data + static_cast<size_t>(r) * y.stride;
    for (int x = 0; x < fb.width(); ++x) {
      row[x] = row[x] > 100 ? 255 : 0;
    }
  }
  const lumen::LumaRangeProbe p = lumen::ProbeLumaRange(fb);
  CHECK(p.suggests_full_range);
}

// ---------------------------------------------------------------------------
// 颗粒
// ---------------------------------------------------------------------------
TEST(GrainEstimateNoise) {
  lumen::FrameBuffer fb;
  MakeFlatNoise(&fb, 6); // 均匀噪声 ±6
  const lumen::GrainEstimate e = lumen::EstimateGrain(fb);
  CHECK(e.blocks_used > 0);
  // 均匀分布 ±6 的标准差约 3.46，估计值应有合理量级（放宽到 1—8）
  CHECK(e.sigma > 1.0);
  CHECK(e.sigma < 8.0);
}

TEST(GrainEstimateFlat) {
  lumen::FrameBuffer fb;
  MakeFlatNoise(&fb, 0); // 纯灰，无噪声
  const lumen::GrainEstimate e = lumen::EstimateGrain(fb);
  CHECK(e.sigma < 0.5);
  const lumen::GrainDecision d = lumen::DecideGrain(e, false);
  CHECK(!d.enabled);
}

// ---------------------------------------------------------------------------
// 处理链
// ---------------------------------------------------------------------------
TEST(ChainTelecineUsesIvtc) {
  lumen::SourceInfo src;
  src.width = 720;
  src.height = 480;
  src.format = lumen::PixelFormat::YUV420P;
  src.fps = lumen::Rational(30000, 1001);
  src.field_order = lumen::FieldOrder::Unknown;
  src.video_codec = "mpeg2video";

  lumen::CadenceVerdict cad;
  cad.cadence = lumen::SourceCadence::Telecine;
  cad.ivtc_recoverable = true;
  cad.needs_deinterlace = false;

  lumen::ColorDecision color;
  color.source_space = lumen::ColorSpace::BT601;
  color.needs_matrix_conversion = true;

  lumen::GrainDecision grain;
  grain.enabled = true;
  grain.amplitude = 0.6;
  grain.sigma = 3.0;

  const lumen::ProcessingChain c =
      lumen::BuildChain(src, cad, color, grain, lumen::QualityPreset::Balanced, false);
  CHECK(c.field_mode == lumen::FieldMode::IVTC);
  CHECK(c.convert_matrix);
  CHECK(c.grain_restore);
  CHECK(c.ChangesFrameRate());
}

TEST(ChainInterlacedUsesDeinterlace) {
  lumen::SourceInfo src;
  src.width = 720;
  src.height = 480;
  src.fps = lumen::Rational(30000, 1001);
  src.field_order = lumen::FieldOrder::BottomFieldFirst;

  lumen::CadenceVerdict cad;
  cad.cadence = lumen::SourceCadence::Interlaced;
  cad.needs_deinterlace = true;

  lumen::ColorDecision color;
  lumen::GrainDecision grain;

  const lumen::ProcessingChain c =
      lumen::BuildChain(src, cad, color, grain, lumen::QualityPreset::Balanced, false);
  CHECK(c.field_mode == lumen::FieldMode::Deinterlace);
  // 元数据标了 BFF，就必须用 BFF，不能被默认值覆盖
  CHECK(c.assumed_field_order == lumen::FieldOrder::BottomFieldFirst);
}

// ---------------------------------------------------------------------------
// 场序人工覆盖（--field-order / 配置 field_order / 播放器 P 键共用同一套逻辑）
// ---------------------------------------------------------------------------
TEST(ParseFieldOrderAcceptsCommonSpellings) {
  lumen::FieldOrder fo = lumen::FieldOrder::TopFieldFirst;
  CHECK(lumen::ParseFieldOrder("auto", &fo) && fo == lumen::FieldOrder::Unknown);
  CHECK(lumen::ParseFieldOrder("TFF", &fo) && fo == lumen::FieldOrder::TopFieldFirst);
  CHECK(lumen::ParseFieldOrder("bottom", &fo) && fo == lumen::FieldOrder::BottomFieldFirst);
  CHECK(lumen::ParseFieldOrder("progressive", &fo) && fo == lumen::FieldOrder::Progressive);
  // 拼错的输入必须被拒绝：静默接受会让用户以为设置生效了
  CHECK(!lumen::ParseFieldOrder("tf", &fo));
  // 空串按 auto 处理（配置文件里漏这一项时不该报错）
  CHECK(lumen::ParseFieldOrder("", &fo) && fo == lumen::FieldOrder::Unknown);
}

TEST(OverrideFieldOrderForcesDeinterlace) {
  // 自动判定为逐行，但用户坚持这是隔行片源 → 必须改回反交错，
  // 否则"按了 P 却没反应"。
  lumen::ProcessingChain c;
  c.field_mode = lumen::FieldMode::Passthrough;
  c.assumed_field_order = lumen::FieldOrder::Progressive;

  lumen::OverrideFieldOrder(lumen::FieldOrder::BottomFieldFirst, &c);
  CHECK(c.field_mode == lumen::FieldMode::Deinterlace);
  CHECK(c.assumed_field_order == lumen::FieldOrder::BottomFieldFirst);
}

TEST(OverrideFieldOrderProgressiveCancelsDeinterlace) {
  // 反向：自动误判成隔行，用户认定是逐行 → 取消场处理（逐行画面去隔行会掉解析度）。
  lumen::ProcessingChain c;
  c.field_mode = lumen::FieldMode::Deinterlace;
  c.assumed_field_order = lumen::FieldOrder::TopFieldFirst;

  lumen::OverrideFieldOrder(lumen::FieldOrder::Progressive, &c);
  CHECK(c.field_mode == lumen::FieldMode::Passthrough);
  CHECK(c.assumed_field_order == lumen::FieldOrder::Progressive);
}

TEST(ChainJsonIsWellFormed) {
  lumen::ProcessingChain c;
  c.field_mode = lumen::FieldMode::IVTC;
  c.rationale = "含\"引号\"与\\反斜杠";
  const std::string json = c.ToJson();

  std::string err;
  const lumen::JsonValue v = lumen::JsonValue::Parse(json, &err);
  CHECK(err.empty());
  CHECK(v.IsObject());
  CHECK_EQ_STR(v["field_mode"].AsString(), "ivtc");
  CHECK_EQ_STR(v["rationale"].AsString(), c.rationale); // 转义必须可逆
}

TEST(ChainScaleUsesDisplayWidth) {
  // 720x480 的 4:3 素材：显示宽度是 640，放大倍数应按 640 算而不是 720。
  // 若按 720 算，输出会被拉扁 12%，这是老片重制最常见的几何错误。
  lumen::SourceInfo src;
  src.width = 720;
  src.height = 480;
  src.sar = lumen::Rational(8, 9); // 720*8/9 = 640 显示宽度

  lumen::CadenceVerdict cad;
  lumen::ColorDecision color;
  lumen::GrainDecision grain;

  const lumen::ProcessingChain c =
      lumen::BuildChain(src, cad, color, grain, lumen::QualityPreset::Balanced, false);
  CHECK_NEAR(c.scale_y, 1080.0 / 480.0, 0.01);
  CHECK_NEAR(c.scale_x, 1920.0 / 640.0, 0.01); // 3.0，而不是 2.67
}

// ---------------------------------------------------------------------------
// 引擎（参考实现）
// ---------------------------------------------------------------------------
TEST(DummyBackendUpscales) {
  lumen::DummyBackend backend;
  lumen::SRBackendConfig cfg;
  cfg.scale = 2;
  CHECK(backend.Init(cfg) == lumen::Status::Ok);
  CHECK(backend.Warmup() == lumen::Status::Ok);

  lumen::FrameBuffer in;
  MakeStripes(&in, 0, 160, 120);
  const lumen::FrameDesc in_desc = in.ToDesc();

  lumen::FrameDesc out;
  CHECK(backend.Process(in_desc, &out) == lumen::Status::Ok);
  CHECK(out.width == 320);
  CHECK(out.height == 240);
  CHECK(out.plane_data[0] != nullptr);
  // 超分后必须是逐行，且时间戳原样透传
  CHECK(out.field_order == lumen::FieldOrder::Progressive);
  CHECK(backend.Stats().frames_done >= 2); // Warmup 也算一帧
}

TEST(DummyBackendRejectsGpuFrame) {
  lumen::DummyBackend backend;
  lumen::SRBackendConfig cfg;
  cfg.scale = 2;
  backend.Init(cfg);

  lumen::FrameDesc gpu;
  gpu.width = 64;
  gpu.height = 64;
  gpu.format = lumen::PixelFormat::YUV420P;
  gpu.location = lumen::FrameLocation::Gpu; // 故意串路径
  lumen::FrameDesc out;
  CHECK(backend.Process(gpu, &out) == lumen::Status::Unsupported);
}

// ---------------------------------------------------------------------------
// 元数据自检
// ---------------------------------------------------------------------------
TEST(DiagnoseNonSquarePixel) {
  lumen::SourceInfo info;
  info.width = 720;
  info.height = 480;
  info.sar = lumen::Rational(8, 9);
  info.video_codec = "mpeg2video";
  const std::vector<lumen::Diagnosis> list = lumen::DiagnoseMetadata(info);

  bool found = false;
  for (const auto& d : list) {
    if (d.code == "SAR_NON_SQUARE") {
      found = true;
      CHECK(d.level == lumen::Diagnosis::Level::Critical);
    }
  }
  CHECK(found);
}

TEST(ParseFfprobeMinimal) {
  // 模拟 ffprobe 输出，覆盖本项目关心的全部字段
  const char* json = R"({
    "streams": [
      {"codec_type":"video","codec_name":"mpeg2video","width":720,"height":480,
       "pix_fmt":"yuv420p","avg_frame_rate":"30000/1001","r_frame_rate":"30000/1001",
       "sample_aspect_ratio":"8:9","display_aspect_ratio":"4:3",
       "color_range":"tv","color_space":"smpte170m","field_order":"tt",
       "nb_frames":"1000"},
      {"codec_type":"audio","codec_name":"ac3","sample_rate":"48000","channels":2}
    ],
    "format": {"format_name":"mpeg","duration":"60.5","bit_rate":"5000000"}
  })";

  lumen::SourceInfo info;
  std::string err;
  CHECK(lumen::ParseFfprobeJson(json, &info, &err) == lumen::Status::Ok);
  CHECK(info.width == 720);
  CHECK(info.height == 480);
  CHECK(info.format == lumen::PixelFormat::YUV420P);
  CHECK(info.colorspace == lumen::ColorSpace::BT601); // smpte170m → 601
  CHECK(info.range == lumen::ColorRange::Limited);
  CHECK(info.field_order == lumen::FieldOrder::TopFieldFirst);
  CHECK(info.audio.size() == 1);
  CHECK_NEAR(info.DisplayAspect(), 4.0 / 3.0, 0.02);
  CHECK(info.duration_us == 60500000);
  CHECK_NEAR(info.fps.ToDouble(), 29.97, 0.01);
}


// ---------------------------------------------------------------------------
// 配置体系
// ---------------------------------------------------------------------------
TEST(ConfigDefaults) {
  const lumen::AppConfig c = lumen::DefaultAppConfig();
  CHECK_EQ_STR(c.engine.backend, "dummy"); // 默认不开 AI：没模型时必须能跑
  CHECK_EQ_STR(c.engine.provider, "auto");
  CHECK(c.engine.channels == 3);
  CHECK(c.engine.scale == 0); // 0 = 自动
  CHECK(c.crf == 18);
  CHECK_EQ_STR(c.preset, "balanced");
}

TEST(ConfigTemplateRoundTrip) {
  // 这条测试守的是一个真实踩过的坑：把缩进拼进了 JSON 键名里，
  // 生成出 "    backend": ... 这种带空格的键，读回来全是默认值，
  // 表面上"配置生效了"，实际一个字段都没读到。
  const std::string path = "lumen_test_cfg.json";
  std::string err;
  CHECK(lumen::WriteAppConfigTemplate(path, &err) == lumen::Status::Ok);

  lumen::AppConfig loaded;
  CHECK(lumen::LoadAppConfig(path, &loaded, &err) == lumen::Status::Ok);
  CHECK_EQ_STR(loaded.engine.backend, "dummy");
  CHECK_EQ_STR(loaded.engine.provider, "auto");
  CHECK(loaded.engine.channels == 3);
  CHECK(loaded.crf == 18);
  // 本次新增的三个字段必须同样能往返——它们分别控制场序覆盖、HUD、进度记忆，
  // 任何一个读不回来都会让用户"改了配置没效果"。
  CHECK_EQ_STR(loaded.field_order, "auto");
  CHECK(loaded.player.hud);
  CHECK(loaded.player.resume);
  std::remove(path.c_str());
}

TEST(ConfigRelativePathResolution) {
  // 相对路径必须按"配置文件所在目录"解析，而不是当前工作目录。
  // 双击启动与命令行启动的 cwd 不同，用 cwd 解析会让同一份配置行为不一致。
  const std::string path = "lumen_test_cfg2.json";
  {
    FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    if (f) {
      const char* text =
          "{\"engine\":{\"backend\":\"ort\",\"model_path\":\"models\\\\x4.onnx\","
          "\"channels\":1,\"scale\":4,\"tile_size\":256},"
          "\"player\":{\"mpv_path\":\"mpv\\\\mpv.exe\"}}";
      std::fwrite(text, 1, std::strlen(text), f);
      std::fclose(f);
    }
  }
  lumen::AppConfig c;
  std::string err;
  CHECK(lumen::LoadAppConfig(path, &c, &err) == lumen::Status::Ok);
  CHECK_EQ_STR(c.engine.backend, "ort");
  CHECK(c.engine.channels == 1);
  CHECK(c.engine.scale == 4);
  CHECK(c.engine.tile_size == 256);
  // 文件名前应带有配置目录（这里是当前目录，以 "./" 或空形式体现）
  CHECK(c.engine.model_path.find("models") != std::string::npos);
  CHECK(c.engine.model_path.find("x4.onnx") != std::string::npos);
  CHECK(c.player.mpv_path.find("mpv.exe") != std::string::npos);
  std::remove(path.c_str());
}

TEST(ConfigInvalidJsonRejected) {
  // 语法错误必须报错，不能静默用默认值——否则用户会以为改动生效了
  const std::string path = "lumen_test_cfg3.json";
  {
    FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    if (f) {
      const char* text = "{\"engine\": {\"backend\": \"ort\"";
      std::fwrite(text, 1, std::strlen(text), f);
      std::fclose(f);
    }
  }
  lumen::AppConfig c;
  std::string err;
  CHECK(lumen::LoadAppConfig(path, &c, &err) == lumen::Status::InvalidArgument);
  CHECK(!err.empty());
  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// ONNX Runtime 后端
// ---------------------------------------------------------------------------
namespace {

bool FileExists(const char* p) {
  FILE* f = std::fopen(p, "rb");
  if (!f) return false;
  std::fclose(f);
  return true;
}

// ---------------------------------------------------------------------------
// 内嵌测试模型（nearest2x，292 字节 ONNX：Resize + nearest 放大 2 倍）
// ---------------------------------------------------------------------------
// ★ 为什么不从 testdata/ 读：
//   testdata 是可随时清空的临时资产。ONNX 那几个断言一旦依赖它，
//   "清空测试数据" 之后就会静默 SKIP——跳过比失败更危险，因为回归报告
//   会变成"136 项全绿，看上去没问题"，没人会注意到少了 15 项。
//   把最小模型直接编进测试二进制，任何机器、任何目录都能跑满全量。
static const char* const kEmbeddedModelB64 =
    "CAgSCmx1bWVuLXRlc3Q6jQIKgwEKBWlucHV0CgNyb2kKBnNjYWxlcxIGb3V0cHV0IgZSZXNpemUqLwoeY29vcmRpbmF0"
    "ZV90cmFuc2Zvcm1hdGlvbl9tb2RlIgphc3ltbWV0cmljoAEDKhIKBG1vZGUiB25lYXJlc3SgAQMqGAoMbmVhcmVzdF9t"
    "b2RlIgVmbG9vcqABAxIPbHVtZW5fbmVhcmVzdDJ4KgkIABABQgNyb2kqHggEEAEiEAAAgD8AAIA/AAAAQAAAAEBCBnNj"
    "YWxlc1oiCgVpbnB1dBIZChcIARITCgMSAU4KAggDCgMSAUgKAxIBV2IlCgZvdXRwdXQSGwoZCAESFQoDEgFOCgIIAwoE"
    "EgJIMgoEEgJXMkIECgAQDQ==";

// 极简 base64 解码（只为内嵌模型服务，不做错误恢复：输入是我们自己的常量）
std::string Base64Decode(const std::string& in) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::string out;
  int buf = 0, bits = 0;
  for (char c : in) {
    const int v = val(c);
    if (v < 0) continue; // 跳过换行与 '='
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buf >> bits) & 0xFF));
    }
  }
  return out;
}

// 依次尝试：testdata 下的大模型（人工放置时优先） → 内嵌模型落盘。
const char* FindTestModel() {
  static const char* kCandidates[] = {
      "testdata/models/nearest2x.onnx",
      "../testdata/models/nearest2x.onnx",
      "models/nearest2x.onnx",
  };
  for (const char* p : kCandidates) {
    if (FileExists(p)) return p;
  }

  // 内嵌模型解到当前目录：只在首次调用时写一次，后续复用同一路径。
  static std::string written;
  if (!written.empty()) return written.c_str();
  written = "lumen_test_model.onnx";
  const std::string bytes = Base64Decode(kEmbeddedModelB64);
  FILE* f = std::fopen(written.c_str(), "wb");
  if (f == nullptr) return nullptr;
  const size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  return (n == bytes.size()) ? written.c_str() : nullptr;
}

} // namespace

TEST(OnnxBackendRejectsEmptyModel) {
  lumen::OnnxBackend backend;
  lumen::SRBackendConfig cfg;
  cfg.model_path.clear();
  // 没有模型必须是 NotFound，而不是"随便跑点什么"——
  // 静默降级会让用户以为在用 AI 超分，实际看到的是双三次。
  CHECK(backend.Init(cfg) == lumen::Status::NotFound);
  CHECK(!backend.LastError().empty());
}

TEST(OnnxBackendUniformFrame) {
  const char* model = FindTestModel();
  if (model == nullptr) {
    std::printf("  [FAIL] 内嵌模型落盘失败（磁盘只读或权限不足）\n");
    return;
  }

  lumen::OnnxBackend backend;
  lumen::SRBackendConfig cfg;
  cfg.model_path = model;
  cfg.provider = "cpu"; // 测试不依赖 GPU
  cfg.channels = 3;
  cfg.scale = 2;
  cfg.tile_size = 0; // 整帧

  const lumen::Status st = backend.Init(cfg);
  if (st == lumen::Status::NotFound &&
      backend.LastError().find("onnxruntime.dll") != std::string::npos) {
    std::printf("  [SKIP] 缺少 onnxruntime.dll（%s）\n", backend.LastError().c_str());
    return;
  }
  CHECK(st == lumen::Status::Ok);
  if (st != lumen::Status::Ok) {
    std::printf("  初始化失败：%s\n", backend.LastError().c_str());
    return;
  }
  CHECK(backend.Warmup() == lumen::Status::Ok);

  // 纯中性灰：Y=100 / U=V=128。经过 YUV→RGB→模型→RGB→YUV 往返后
  // 应当基本不变。这个断言能同时抓住：通道顺序错、矩阵用错、量化范围错。
  FrameBuffer fb;
  CHECK(fb.Allocate(40, 24, PixelFormat::YUV420P) == lumen::Status::Ok);
  for (int p = 0; p < 3; ++p) {
    const PlaneView v = fb.Plane(p);
    std::memset(v.data, p == 0 ? 100 : 128,
                static_cast<size_t>(v.stride) * fb.PlaneHeight(p));
  }
  const lumen::FrameDesc in = fb.ToDesc();
  lumen::FrameDesc out;
  CHECK(backend.Process(in, &out) == lumen::Status::Ok);
  CHECK(out.width == 80);
  CHECK(out.height == 48);

  // 逐像素检查（只抽样，避免测试过慢）
  int bad = 0;
  for (int r = 0; r < out.height; r += 3) {
    const uint8_t* row = out.plane_data[0] + static_cast<size_t>(r) * out.plane_stride[0];
    for (int x = 0; x < out.width; x += 3) {
      if (std::abs(static_cast<int>(row[x]) - 100) > 2) ++bad;
    }
  }
  CHECK(bad == 0); // 往返误差应 ≤2（8bit 量化的正常范围）

  const lumen::SRStats stats = backend.Stats();
  CHECK(stats.frames_done >= 2); // 预热 1 帧 + 本次 1 帧
  CHECK(stats.avg_ms > 0.0);
}

TEST(OnnxBackendTiledNoSeams) {
  // 分块 + 羽化最容易出的问题是接缝：块边界出现亮度台阶。
  // 用纯色画面放大后，如果接缝存在，边界像素值会与内部不同。
  const char* model = FindTestModel();
  if (model == nullptr) {
    std::printf("  [SKIP] 未找到测试模型\n");
    return;
  }

  lumen::OnnxBackend backend;
  lumen::SRBackendConfig cfg;
  cfg.model_path = model;
  cfg.provider = "cpu";
  cfg.channels = 3;
  cfg.scale = 2;
  cfg.tile_size = 64; // 强制分块（画面 100x60 > 64）

  const lumen::Status st = backend.Init(cfg);
  if (st != lumen::Status::Ok) {
    std::printf("  [SKIP] 后端不可用：%s\n", backend.LastError().c_str());
    return;
  }
  backend.Warmup();

  FrameBuffer fb;
  CHECK(fb.Allocate(100, 60, PixelFormat::YUV420P) == lumen::Status::Ok);
  for (int p = 0; p < 3; ++p) {
    const PlaneView v = fb.Plane(p);
    std::memset(v.data, p == 0 ? 120 : 128,
                static_cast<size_t>(v.stride) * fb.PlaneHeight(p));
  }
  lumen::FrameDesc out;
  CHECK(backend.Process(fb.ToDesc(), &out) == lumen::Status::Ok);
  CHECK(out.width == 200);
  CHECK(out.height == 120);

  int min_v = 255;
  int max_v = 0;
  for (int r = 0; r < out.height; ++r) {
    const uint8_t* row = out.plane_data[0] + static_cast<size_t>(r) * out.plane_stride[0];
    for (int x = 0; x < out.width; ++x) {
      min_v = std::min(min_v, static_cast<int>(row[x]));
      max_v = std::max(max_v, static_cast<int>(row[x]));
    }
  }
  // 纯色画面：整幅画面（含块边界）的亮度极差应很小。
  // 若羽化写错，这里会出现几十级的台阶。
  CHECK(max_v - min_v <= 3);

  CHECK(backend.Stats().tiles_used > 1); // 确认真的走了分块路径
}

} // namespace

int main() {
  std::printf("LumenPlayer 单元测试\n");
  std::printf("----------------------------------------\n");
  for (const auto& t : Registry()) {
    const int before_fail = g_fail;
    t.fn();
    std::printf("%s %s\n", (g_fail == before_fail) ? "[PASS]" : "[FAIL]", t.name);
  }
  std::printf("----------------------------------------\n");
  std::printf("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
