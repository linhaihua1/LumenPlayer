// ============================================================================
//  LumenPlayer Android - JNI 桥
//  ---------------------------------------------------------------------------
//  职责：把 Kotlin 侧抽出来的「亮度平面」数组喂给桌面端的 core 层做场结构判定，
//        再把结论以 JSON 字符串返回。
//
//  为什么走 JSON 而不是构造 Java 对象：
//    JNI 里逐字段 FindClass/GetFieldID 调用容易写错，而且每加一个字段就要改两处。
//    返回一个字符串，Kotlin 用 org.json 解析——接口只有一个函数签名，
//    core 侧加字段不会破坏 ABI，Android 与 Windows 两端还能共用同一份判定逻辑。
//
//  ★ 只传 Y 平面是刻意设计：
//    场结构判定（梳齿、场间行差）本质是亮度域的一维/二维差分，色度不参与。
//    传 YUV 全帧会让 JNI 拷贝量翻 1.5 倍，而对结论没有任何影响。
// ============================================================================
#include <jni.h>

#include <android/log.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "core/field_analyzer.h"
#include "core/frame_buffer.h"
#include "core/types.h"

#define LOG_TAG "LumenJni"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace {

// JSON 字符串转义。判定依据里含有中文与引号（例如「判定为"真隔行"」），
// 不转义会让 org.json 解析直接抛异常，表现为「分析完成了但界面空白」。
std::string EscapeJson(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 16);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

std::string Bool(bool b) { return b ? "true" : "false"; }

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_lumen_player_NativeAnalyzer_nativeBuildInfo(JNIEnv* env, jobject /*thiz*/) {
  // 用来快速确认「.so 到底加载成功了没有」——排查 JNI 问题的第一现场。
  char buf[160];
  std::snprintf(buf, sizeof(buf), "lumen-core %d-bit built %s %s", 
                static_cast<int>(sizeof(void*) * 8), __DATE__, __TIME__);
  return env->NewStringUTF(buf);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_lumen_player_NativeAnalyzer_analyzeFrames(JNIEnv* env, jobject /*thiz*/,
                                                   jobjectArray frames, jint width,
                                                   jint height) {
  const int w = static_cast<int>(width);
  const int h = static_cast<int>(height);
  if (w <= 0 || h <= 0 || frames == nullptr) {
    return env->NewStringUTF("{\"error\":\"尺寸或帧数据无效\"}");
  }

  const jsize total = env->GetArrayLength(frames);
  if (total <= 0) {
    return env->NewStringUTF("{\"error\":\"没有可用帧\"}");
  }

  lumen::CadenceDetector detector;
  int used = 0;
  int mismatched = 0;

  for (jsize i = 0; i < total; ++i) {
    auto arr = static_cast<jbyteArray>(env->GetObjectArrayElement(frames, i));
    if (arr == nullptr) continue;

    const jsize len = env->GetArrayLength(arr);
    // 抽帧过程中分辨率可能变化（极少见：可变分辨率码流）。
    // 尺寸不一致的帧必须丢弃——拿它去和前面比较会产生巨大的 SAD，
    // 从而把 telecine 判定彻底带偏。
    if (len < static_cast<jsize>(w) * h) {
      ++mismatched;
      env->DeleteLocalRef(arr);
      continue;
    }

    lumen::FrameBuffer fb;
    if (fb.Allocate(w, h, lumen::PixelFormat::YUV420P) != lumen::Status::Ok) {
      env->DeleteLocalRef(arr);
      continue;
    }

    jbyte* src = env->GetByteArrayElements(arr, nullptr);
    if (src != nullptr) {
      lumen::PlaneView y = fb.Plane(0);
      if (y.data != nullptr) {
        // 逐行拷贝：FrameBuffer 的 stride 是 32 字节对齐的，
        // 直接整块 memcpy 会让第 2 行起的数据错位（这是最典型的「画面看着没问题、
        // 判定结果莫名其妙」的来源）。
        for (int r = 0; r < h; ++r) {
          std::memcpy(y.data + static_cast<size_t>(r) * static_cast<size_t>(y.stride),
                      src + static_cast<size_t>(r) * static_cast<size_t>(w),
                      static_cast<size_t>(w));
        }
        // 色度平面保持全 0：判定只用 Y，且 AnalyzeField 不会读 UV。
        fb.set_index(used);
        detector.Feed(fb);
        ++used;
      }
      env->ReleaseByteArrayElements(arr, src, JNI_ABORT);  // 只读，不做回写
    }
    env->DeleteLocalRef(arr);
  }

  const lumen::CadenceVerdict v = detector.Verdict();

  std::string out = "{";
  out += "\"frames\":" + std::to_string(used);
  out += ",\"dropped\":" + std::to_string(mismatched);
  out += ",\"cadence\":\"";
  out += lumen::ToString(v.cadence);
  out += "\",\"confidence\":" + std::to_string(v.confidence);
  out += ",\"fieldOrder\":\"";
  out += lumen::ToString(v.suggested_order);
  out += "\",\"needsDeinterlace\":" + Bool(v.needs_deinterlace);
  out += ",\"ivtc\":" + Bool(v.ivtc_recoverable);
  out += ",\"reason\":\"" + EscapeJson(v.reason) + "\"";
  out += "}";

  if (used < lumen::CadenceDetector::kMinFramesForVerdict) {
    LOGW("有效帧不足：%d/%d（需要 %d）", used,
         static_cast<int>(lumen::CadenceDetector::kMinFramesForVerdict),
         static_cast<int>(lumen::CadenceDetector::kMinFramesForVerdict));
  }

  return env->NewStringUTF(out.c_str());
}
