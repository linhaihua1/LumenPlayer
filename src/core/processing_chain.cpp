// ============================================================================
//  LumenPlayer - 处理链实现
//  所属层次：产品层（C 层）
// ============================================================================
#include "processing_chain.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <sstream>

namespace lumen {
namespace {

std::string Fmt(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return std::string(buf);
}

// JSON 字符串转义：rationale 里含中文与引号，必须转义否则前端解析失败
std::string EscapeJson(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

} // namespace

const char* ToString(FieldMode m) {
  switch (m) {
    case FieldMode::Passthrough: return "passthrough";
    case FieldMode::Deinterlace: return "deinterlace";
    case FieldMode::IVTC: return "ivtc";
  }
  return "unknown";
}

const char* ToString(DenoiseMode m) {
  switch (m) {
    case DenoiseMode::Off: return "off";
    case DenoiseMode::Light: return "light";
    case DenoiseMode::Medium: return "medium";
  }
  return "unknown";
}

const char* ToString(SharpenMode m) {
  switch (m) {
    case SharpenMode::Off: return "off";
    case SharpenMode::Light: return "light";
  }
  return "unknown";
}

const char* ToString(QualityPreset p) {
  switch (p) {
    case QualityPreset::Performance: return "performance";
    case QualityPreset::Balanced: return "balanced";
    case QualityPreset::Quality: return "quality";
    case QualityPreset::Archive: return "archive";
  }
  return "unknown";
}

void ApplyPreset(QualityPreset preset, ProcessingChain* chain) {
  if (chain == nullptr) return;
  chain->preset = preset;

  // 档位只影响"放大目标 + 增强强度"，不改变是否去隔行/转色域。
  // 原因：场处理与色彩正确性是"正确性"问题，不能因为选了性能档就做错。
  switch (preset) {
    case QualityPreset::Performance:
      chain->sharpen = SharpenMode::Off;
      chain->sharpen_amount = 0.0;
      chain->grain_amplitude *= 0.5; // 颗粒回注有额外开销，性能档减半
      break;
    case QualityPreset::Balanced:
      chain->sharpen = SharpenMode::Light;
      chain->sharpen_amount = 0.15;
      break;
    case QualityPreset::Quality:
      chain->sharpen = SharpenMode::Light;
      chain->sharpen_amount = 0.25;
      chain->grain_amplitude = std::min(1.0, chain->grain_amplitude * 1.15);
      break;
    case QualityPreset::Archive:
      chain->sharpen = SharpenMode::Light;
      chain->sharpen_amount = 0.3;
      chain->grain_amplitude = std::min(1.0, chain->grain_amplitude * 1.25);
      // 收藏档不考虑实时性能，允许更重的降噪与去块
      if (chain->denoise == DenoiseMode::Off) chain->denoise = DenoiseMode::Light;
      break;
  }

  if (chain->grain_amplitude < 0.02) {
    chain->grain_restore = false;
    chain->grain_amplitude = 0.0;
  } else {
    chain->grain_restore = true;
  }
}

void OverrideFieldOrder(FieldOrder fo, ProcessingChain* chain) {
  if (chain == nullptr || fo == FieldOrder::Unknown) return;

  chain->assumed_field_order = fo;
  if (fo == FieldOrder::Progressive) {
    if (chain->field_mode != FieldMode::Passthrough) {
      chain->field_mode = FieldMode::Passthrough;
      chain->rationale += "用户强制指定逐行，取消场处理；";
    }
  } else {
    // 用户明确说"这是隔行的"时，若原结论是逐行就必须改回反交错——
    // 否则会出现"我设了 TFF 但画面没变化"的困惑。
    if (chain->field_mode == FieldMode::Passthrough) {
      chain->field_mode = FieldMode::Deinterlace;
      chain->rationale += std::string("用户强制指定场序 ") + ToString(fo) +
                          "，改为反交错；";
    } else {
      chain->rationale += std::string("用户强制场序 ") + ToString(fo) + "；";
    }
  }
}

ProcessingChain BuildChain(const SourceInfo& src, const CadenceVerdict& cadence,
                           const ColorDecision& color, const GrainDecision& grain,
                           QualityPreset preset, bool anime) {
  ProcessingChain c;
  std::ostringstream why;

  // -------------------------------------------------------------------------
  // 1. 场处理：优先级最高，错了后面全白做
  // -------------------------------------------------------------------------
  switch (cadence.cadence) {
    case SourceCadence::Telecine:
      c.field_mode = FieldMode::IVTC;
      // IVTC 的前提是场序正确。telecine 只存在于 NTSC 体系，标准为 TFF。
      c.assumed_field_order = FieldOrder::TopFieldFirst;
      why << "检测到 3:2 pulldown，采用 IVTC 还原 24p（信息无损）；";
      break;
    case SourceCadence::Interlaced:
    case SourceCadence::Mixed:
      c.field_mode = FieldMode::Deinterlace;
      // 场序优先用容器元数据，其次用分析器的建议值，最后回落 TFF。
      if (src.field_order == FieldOrder::TopFieldFirst ||
          src.field_order == FieldOrder::BottomFieldFirst) {
        c.assumed_field_order = src.field_order;
      } else if (cadence.suggested_order != FieldOrder::Unknown) {
        c.assumed_field_order = cadence.suggested_order;
      } else {
        c.assumed_field_order = FieldOrder::TopFieldFirst;
      }
      why << "判定为" << (cadence.cadence == SourceCadence::Mixed ? "混合" : "隔行")
          << "素材，先反交错（场序 " << ToString(c.assumed_field_order) << "）；";
      break;
    case SourceCadence::Progressive:
      c.field_mode = FieldMode::Passthrough;
      c.assumed_field_order = FieldOrder::Progressive;
      why << "判定为逐行，跳过场处理；";
      break;
    case SourceCadence::Unknown:
    default:
      // 结论不明时不能盲目去隔行（会把逐行素材的垂直分辨率砍半），
      // 但也不能不管。策略：按元数据走，元数据也没有就 passthrough 并标注待复核。
      if (src.field_order == FieldOrder::TopFieldFirst ||
          src.field_order == FieldOrder::BottomFieldFirst) {
        c.field_mode = FieldMode::Deinterlace;
        c.assumed_field_order = src.field_order;
        why << "像素分析结论不足，按容器标注的隔行处理；";
      } else {
        c.field_mode = FieldMode::Passthrough;
        why << "时域结论不足且无隔行标记，按逐行处理（建议复核）；";
      }
      break;
  }

  // -------------------------------------------------------------------------
  // 2. 色彩
  // -------------------------------------------------------------------------
  c.source_space = color.source_space;
  c.source_range = color.source_range;
  c.target_space = color.target_space;
  c.convert_matrix = color.needs_matrix_conversion;
  c.convert_range = color.needs_range_conversion;
  if (c.convert_matrix) {
    why << "色彩按 " << ToString(c.source_space) << " 解码并转 " << ToString(c.target_space);
    if (color.guessed) why << "（推断值，建议 A/B 确认）";
    why << "；";
  }
  if (c.convert_range) why << "色彩范围 " << ToString(c.source_range) << " → limited；";

  // -------------------------------------------------------------------------
  // 3. 前处理：只在有明确证据时启用
  // -------------------------------------------------------------------------
  // MPEG-2 标清几乎都有块效应；位率越低越明显
  if (src.video_codec == "mpeg2video" && src.IsSd()) {
    c.deblock = true;
    c.denoise = DenoiseMode::Light;
    why << "MPEG-2 标清片源，启用去块 + 轻度降噪；";
  }
  if (grain.needs_denoise) {
    c.denoise = DenoiseMode::Medium;
    why << "颗粒过强，提升至中度降噪；";
  }

  // -------------------------------------------------------------------------
  // 4. 颗粒
  // -------------------------------------------------------------------------
  c.grain_restore = grain.enabled;
  c.grain_amplitude = grain.enabled ? grain.amplitude : 0.0;
  if (grain.enabled) {
    why << Fmt("颗粒回注强度 %.0f%%（σ≈%.1f）；", grain.amplitude * 100.0, grain.sigma);
  }

  // -------------------------------------------------------------------------
  // 5. 超分目标与模型
  // -------------------------------------------------------------------------
  c.target_width = 1920;
  c.target_height = 1080;
  c.use_anime_model = anime;

  const double src_h = (src.height > 0) ? static_cast<double>(src.height) : 480.0;
  c.scale_y = 1080.0 / src_h;
  // 水平方向要按"显示宽度"算，不能按编码宽度算：
  // 720x480 的 4:3 素材，显示宽度是 640 而非 720，直接按 1920/720 会把画面拉扁。
  const double dar = src.DisplayAspect();
  const double display_w = (dar > 0.1) ? src_h * dar : static_cast<double>(src.width);
  c.scale_x = 1920.0 / std::max(1.0, display_w);

  if (anime) {
    why << "按动画素材选择专用模型；";
  }
  why << Fmt("放大 %.2f×%.2f → %dx%d；", c.scale_x, c.scale_y, c.target_width,
             c.target_height);

  // 档位最后应用，保证它只覆盖"强度"而不破坏正确性决策
  ApplyPreset(preset, &c);

  c.rationale = why.str();
  return c;
}

std::string ProcessingChain::ToJson() const {
  std::ostringstream os;
  os << "{";
  os << "\"field_mode\":\"" << ToString(field_mode) << "\",";
  os << "\"field_order\":\"" << ToString(assumed_field_order) << "\",";
  os << "\"source_space\":\"" << ToString(source_space) << "\",";
  os << "\"source_range\":\"" << ToString(source_range) << "\",";
  os << "\"target_space\":\"" << ToString(target_space) << "\",";
  os << "\"convert_matrix\":" << (convert_matrix ? "true" : "false") << ",";
  os << "\"convert_range\":" << (convert_range ? "true" : "false") << ",";
  os << "\"grain_restore\":" << (grain_restore ? "true" : "false") << ",";
  os << "\"grain_amplitude\":" << grain_amplitude << ",";
  os << "\"denoise\":\"" << ToString(denoise) << "\",";
  os << "\"deblock\":" << (deblock ? "true" : "false") << ",";
  os << "\"target_width\":" << target_width << ",";
  os << "\"target_height\":" << target_height << ",";
  os << "\"scale_x\":" << scale_x << ",";
  os << "\"scale_y\":" << scale_y << ",";
  os << "\"use_anime_model\":" << (use_anime_model ? "true" : "false") << ",";
  os << "\"sharpen\":\"" << ToString(sharpen) << "\",";
  os << "\"sharpen_amount\":" << sharpen_amount << ",";
  os << "\"preset\":\"" << ToString(preset) << "\",";
  os << "\"rationale\":\"" << EscapeJson(rationale) << "\"";
  os << "}";
  return os.str();
}

std::string ProcessingChain::Describe() const {
  std::ostringstream os;
  os << "场处理: " << ToString(field_mode);
  if (field_mode == FieldMode::Deinterlace || field_mode == FieldMode::IVTC) {
    os << "（场序 " << ToString(assumed_field_order) << "）";
  }
  os << "\n";
  os << "色彩  : " << ToString(source_space) << "/" << ToString(source_range)
     << " → " << ToString(target_space) << "/limited"
     << (convert_matrix ? " [矩阵转换]" : "") << (convert_range ? " [范围压缩]" : "")
     << "\n";
  os << "颗粒  : " << (grain_restore ? "回注" : "不回注")
     << (grain_restore ? Fmt(" %.0f%%", grain_amplitude * 100.0) : std::string()) << "\n";
  os << "降噪  : " << ToString(denoise) << (deblock ? " + 去块" : "") << "\n";
  os << "超分  : " << target_width << "x" << target_height << " (" << Fmt("%.2f", scale_x)
     << "x" << Fmt("%.2f", scale_y) << ") 模型:" << (use_anime_model ? "动画" : "实景")
     << "\n";
  os << "锐化  : " << ToString(sharpen) << " " << Fmt("%.2f", sharpen_amount) << "\n";
  os << "依据  : " << rationale << "\n";
  return os.str();
}

} // namespace lumen
