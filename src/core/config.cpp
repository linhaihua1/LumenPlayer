// ============================================================================
//  LumenPlayer - 应用配置实现
//  所属层次：产品层
// ============================================================================
#include "core/config.h"

#include <cstdio>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "core/json.h"

namespace lumen {

namespace {

// 读取整个文件到字符串。用二进制模式，避免 Windows 文本模式把 \r\n 转成 \n
// 导致 JSON 解析器算出的"第几行"偏移。
bool ReadFileText(const std::string& path, std::string* out) {
  FILE* f = nullptr;
#ifdef _WIN32
  // 中文路径必须走宽字符接口，否则 fopen 直接失败
  const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  if (n <= 0) return false;
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
  f = _wfopen(w.c_str(), L"rb");
#else
  f = std::fopen(path.c_str(), "rb");
#endif
  if (f == nullptr) return false;
  char buf[8192];
  out->clear();
  size_t got = 0;
  while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    out->append(buf, got);
  }
  std::fclose(f);
  return true;
}

// JSON 写出辅助。
// 注意：缩进必须写在引号外——曾经把缩进拼进 key 里，生成出
// "    backend": ... 这种带空格的键名，读回来当然取不到值。
void PutKey(std::string* s, const char* indent, const char* key) {
  *s += indent;
  *s += "\"";
  *s += key;
  *s += "\": ";
}

void PutString(std::string* s, const char* indent, const char* key, const std::string& v,
               bool comma = true) {
  PutKey(s, indent, key);
  *s += "\"";
  // 最小转义：反斜杠与双引号。Windows 路径里反斜杠极多，不转义必然产出非法 JSON。
  for (char c : v) {
    if (c == '\\' || c == '"') s->push_back('\\');
    s->push_back(c);
  }
  *s += comma ? "\",\n" : "\"\n";
}

void PutInt(std::string* s, const char* indent, const char* key, long long v,
            bool comma = true) {
  PutKey(s, indent, key);
  *s += std::to_string(v) + (comma ? ",\n" : "\n");
}

void PutBool(std::string* s, const char* indent, const char* key, bool v, bool comma = true) {
  PutKey(s, indent, key);
  *s += (v ? "true" : "false");
  *s += comma ? ",\n" : "\n";
}

} // namespace

std::string ResolvePath(const std::string& base_dir, const std::string& p) {
  if (p.empty()) return p;
  // 绝对路径判定：Windows 盘符（C:\ / C:/）与 UNC（\\server）
  const bool absolute =
      (p.size() >= 2 && p[1] == ':') || (p.size() >= 1 && (p[0] == '/' || p[0] == '\\'));
  if (absolute) return p;
  if (base_dir.empty()) return p;
  std::string base = base_dir;
  if (!base.empty() && base.back() != '\\' && base.back() != '/') base.push_back('\\');
  return base + p;
}

std::string DirectoryOf(const std::string& file_path) {
  const size_t pos = file_path.find_last_of("\\/");
  if (pos == std::string::npos) return std::string();
  return file_path.substr(0, pos + 1);
}

AppConfig DefaultAppConfig() {
  AppConfig c;
  c.version = 1;
  c.preset = "balanced";
  c.crf = 18;
  c.engine.backend = "dummy";
  c.engine.provider = "auto";
  c.engine.channels = 3;
  c.player.hwdec = "auto";
  return c;
}

Status LoadAppConfig(const std::string& path, AppConfig* out, std::string* err) {
  if (out == nullptr) return Status::InvalidArgument;
  *out = DefaultAppConfig();

  std::string text;
  if (!ReadFileText(path, &text)) {
    if (err) *err = "配置文件不存在或无法读取：" + path;
    return Status::NotFound;
  }

  std::string perr;
  const JsonValue root = JsonValue::Parse(text, &perr);
  if (!perr.empty() || !root.IsObject()) {
    if (err) *err = "配置文件 JSON 解析失败：" + perr;
    return Status::InvalidArgument;
  }

  const std::string base = DirectoryOf(path);

  out->version = static_cast<int>(root["version"].AsInt(out->version));
  out->ffmpeg_path = ResolvePath(base, root["ffmpeg"].AsString(""));
  out->ffprobe_path = ResolvePath(base, root["ffprobe"].AsString(""));
  out->output_dir = ResolvePath(base, root["output_dir"].AsString(""));
  out->preset = root["preset"].AsString(out->preset);
  out->anime = root["anime"].AsBool(out->anime);
  out->crf = static_cast<int>(root["crf"].AsInt(out->crf));
  if (out->crf < 0) out->crf = 0;
  if (out->crf > 51) out->crf = 51;
  out->field_order = root["field_order"].AsString(out->field_order);

  const JsonValue& eng = root["engine"];
  out->engine.backend = eng["backend"].AsString(out->engine.backend);
  out->engine.provider = eng["provider"].AsString(out->engine.provider);
  out->engine.model_path = ResolvePath(base, eng["model_path"].AsString(""));
  out->engine.runtime_dir = ResolvePath(base, eng["runtime_dir"].AsString(""));
  out->engine.channels = static_cast<int>(eng["channels"].AsInt(out->engine.channels));
  out->engine.scale = static_cast<int>(eng["scale"].AsInt(out->engine.scale));
  out->engine.tile_size = static_cast<int>(eng["tile_size"].AsInt(out->engine.tile_size));
  out->engine.allow_fp16 = eng["allow_fp16"].AsBool(out->engine.allow_fp16);
  if (out->engine.channels != 1 && out->engine.channels != 3) out->engine.channels = 3;

  const JsonValue& pl = root["player"];
  out->player.mpv_path = ResolvePath(base, pl["mpv_path"].AsString(""));
  out->player.hwdec = pl["hwdec"].AsString(out->player.hwdec);
  out->player.hud = pl["hud"].AsBool(out->player.hud);
  out->player.resume = pl["resume"].AsBool(out->player.resume);
  out->player.extra_args = pl["extra_args"].AsString("");

  return Status::Ok;
}

std::string AppConfig::ToJson() const {
  std::string s = "{\n";
  PutInt(&s, "  ", "version", version);
  PutString(&s, "  ", "_readme", "路径为相对时按本文件所在目录解析；留空表示自动查找。");

  PutString(&s, "  ", "ffmpeg", ffmpeg_path);
  PutString(&s, "  ", "ffprobe", ffprobe_path);
  PutString(&s, "  ", "output_dir", output_dir);
  PutString(&s, "  ", "preset", preset);
  PutBool(&s, "  ", "anime", anime);
  PutInt(&s, "  ", "crf", crf);
  PutString(&s, "  ", "field_order", field_order);

  s += "  \"engine\": {\n";
  PutString(&s, "    ", "backend", engine.backend);
  PutString(&s, "    ", "provider", engine.provider);
  PutString(&s, "    ", "model_path", engine.model_path);
  PutString(&s, "    ", "runtime_dir", engine.runtime_dir);
  PutInt(&s, "    ", "channels", engine.channels);
  PutInt(&s, "    ", "scale", engine.scale);
  PutInt(&s, "    ", "tile_size", engine.tile_size);
  PutBool(&s, "    ", "allow_fp16", engine.allow_fp16, false);
  s += "  },\n";

  s += "  \"player\": {\n";
  PutString(&s, "    ", "mpv_path", player.mpv_path);
  PutString(&s, "    ", "hwdec", player.hwdec);
  PutBool(&s, "    ", "hud", player.hud);
  PutBool(&s, "    ", "resume", player.resume);
  PutString(&s, "    ", "extra_args", player.extra_args, false);
  s += "  }\n";
  s += "}\n";
  return s;
}

Status WriteAppConfigTemplate(const std::string& path, std::string* err) {
  const AppConfig cfg = DefaultAppConfig();
  const std::string text = cfg.ToJson();

  FILE* f = nullptr;
#ifdef _WIN32
  const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
  f = _wfopen(w.c_str(), L"wb");
#else
  f = std::fopen(path.c_str(), "wb");
#endif
  if (f == nullptr) {
    if (err) *err = "无法写入配置文件：" + path;
    return Status::IoError;
  }
  const size_t written = std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
  if (written != text.size()) {
    if (err) *err = "写入配置文件不完整：" + path;
    return Status::IoError;
  }
  return Status::Ok;
}

} // namespace lumen
