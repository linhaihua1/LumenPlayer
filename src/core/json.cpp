// ============================================================================
//  LumenPlayer - 轻量 JSON 解析器实现
//  所属层次：产品层（C 层）
// ============================================================================
#include "json.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace lumen {
namespace {

// 全局只读的 Null 实例：用于「成员不存在」时返回，避免调用方判空炸掉。
// 生命周期与程序一致，返回引用安全。
const JsonValue& NullInstance() {
  static const JsonValue kNull;
  return kNull;
}

} // namespace

// ---------------------------------------------------------------------------
// 取值实现
// ---------------------------------------------------------------------------
std::string JsonValue::AsString(const std::string& defaultValue) const {
  return type_ == Type::String ? string_ : defaultValue;
}

double JsonValue::AsDouble(double defaultValue) const {
  if (type_ == Type::Number) return number_;
  if (type_ == Type::Bool) return bool_ ? 1.0 : 0.0;
  return defaultValue;
}

int64_t JsonValue::AsInt(int64_t defaultValue) const {
  if (type_ != Type::Number) return defaultValue;
  // 注意：ffprobe 偶尔用字符串表示大整数，这里做一次宽容转换。
  return static_cast<int64_t>(number_ >= 0 ? number_ + 0.5 : number_ - 0.5);
}

bool JsonValue::AsBool(bool defaultValue) const {
  if (type_ == Type::Bool) return bool_;
  if (type_ == Type::Number) return number_ != 0.0;
  return defaultValue;
}

const JsonValue& JsonValue::operator[](const std::string& key) const {
  if (type_ != Type::Object) return NullInstance();
  // 线性查找：ffprobe 输出单个对象成员数在 10 以内，不值得为它建哈希表
  for (const auto& kv : object_) {
    if (kv.first == key) return kv.second;
  }
  return NullInstance();
}

const JsonValue& JsonValue::operator[](size_t index) const {
  if (type_ != Type::Array || index >= array_.size()) return NullInstance();
  return array_[index];
}

size_t JsonValue::size() const {
  if (type_ == Type::Array) return array_.size();
  if (type_ == Type::Object) return object_.size();
  return 0;
}

const JsonValue& JsonValue::Query(const std::string& path) const {
  const JsonValue* cur = this;
  size_t begin = 0;
  while (begin <= path.size()) {
    size_t dot = path.find('.', begin);
    if (dot == std::string::npos) dot = path.size();
    std::string token = path.substr(begin, dot - begin);
    if (token.empty()) return NullInstance();

    // 纯数字 token 视为数组下标
    bool allDigits = !token.empty();
    for (char c : token) {
      if (!std::isdigit(static_cast<unsigned char>(c))) { allDigits = false; break; }
    }
    if (allDigits) {
      cur = &(*cur)[static_cast<size_t>(std::strtoul(token.c_str(), nullptr, 10))];
    } else {
      cur = &(*cur)[token];
    }
    if (cur->IsNull()) return NullInstance();

    if (dot >= path.size()) break;
    begin = dot + 1;
  }
  return *cur;
}

// ---------------------------------------------------------------------------
// 递归下降解析器
// ---------------------------------------------------------------------------
// 采用「指针 + 结束指针」而非 stringstream，避免逐字符流的虚函数开销。
class JsonParser {
 public:
  JsonParser(const char* begin, const char* end) : p_(begin), end_(end), start_(begin) {}

  // 返回值：true 表示解析成功
  bool ParseValue(JsonValue* out) {
    SkipWhitespace();
    if (p_ >= end_) return Fail("unexpected end of input");
    switch (*p_) {
      case '{': return ParseObject(out);
      case '[': return ParseArray(out);
      case '"': {
        std::string s;
        if (!ParseString(&s)) return false;
        out->type_ = JsonValue::Type::String;
        out->string_ = std::move(s);
        return true;
      }
      case 't':
        if (!Expect("true")) return false;
        out->type_ = JsonValue::Type::Bool;
        out->bool_ = true;
        return true;
      case 'f':
        if (!Expect("false")) return false;
        out->type_ = JsonValue::Type::Bool;
        out->bool_ = false;
        return true;
      case 'n':
        if (!Expect("null")) return false;
        out->type_ = JsonValue::Type::Null;
        return true;
      default: return ParseNumber(out);
    }
  }

  const std::string& error() const { return error_; }

 private:
  void SkipWhitespace() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
  }

  bool Fail(const char* msg) {
    if (error_.empty()) {
      error_ = std::string(msg) + " at offset " + std::to_string(p_ - start_);
    }
    return false;
  }

  bool Expect(const char* literal) {
    const size_t n = std::strlen(literal);
    if (static_cast<size_t>(end_ - p_) < n || std::memcmp(p_, literal, n) != 0) {
      return Fail("invalid literal");
    }
    p_ += n;
    return true;
  }

  bool ParseNumber(JsonValue* out) {
    // strtod 需要以 '\0' 结尾的缓冲区，这里在栈上拷贝一个小片段即可。
    // 数字最长不超过 32 字符（含科学计数法）。
    char buf[64];
    size_t n = 0;
    while (p_ < end_ && n + 1 < sizeof(buf)) {
      const char c = *p_;
      const bool numeric = std::isdigit(static_cast<unsigned char>(c)) || c == '-' ||
                           c == '+' || c == '.' || c == 'e' || c == 'E';
      if (!numeric) break;
      buf[n++] = c;
      ++p_;
    }
    if (n == 0) return Fail("invalid number");
    buf[n] = '\0';
    char* parseEnd = nullptr;
    const double v = std::strtod(buf, &parseEnd);
    if (parseEnd == buf) return Fail("invalid number format");
    out->type_ = JsonValue::Type::Number;
    out->number_ = v;
    return true;
  }

  // 4 位十六进制 → 码点
  bool ParseHex4(unsigned* code) {
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
      if (p_ >= end_) return Fail("truncated \\u escape");
      const char c = *p_++;
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
      else return Fail("invalid hex digit");
    }
    *code = v;
    return true;
  }

  // 码点 → UTF-8（用于还原 ffprobe 输出的中文 metadata）
  static void AppendUtf8(unsigned cp, std::string* out) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool ParseString(std::string* out) {
    if (p_ >= end_ || *p_ != '"') return Fail("expected string");
    ++p_;
    out->clear();
    while (p_ < end_) {
      const char c = *p_++;
      if (c == '"') return true;
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (p_ >= end_) return Fail("truncated escape");
      const char esc = *p_++;
      switch (esc) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          unsigned cp = 0;
          if (!ParseHex4(&cp)) return false;
          // 处理 UTF-16 代理对（emoji 等）
          if (cp >= 0xD800 && cp <= 0xDBFF && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
            const char* save = p_;
            p_ += 2;
            unsigned lo = 0;
            if (ParseHex4(&lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              p_ = save; // 不是合法低位代理，回退
            }
          }
          AppendUtf8(cp, out);
          break;
        }
        default: return Fail("invalid escape character");
      }
    }
    return Fail("unterminated string");
  }

  bool ParseArray(JsonValue* out) {
    ++p_; // 跳过 '['
    out->type_ = JsonValue::Type::Array;
    SkipWhitespace();
    if (p_ < end_ && *p_ == ']') { ++p_; return true; }
    while (true) {
      JsonValue item;
      if (!ParseValue(&item)) return false;
      out->array_.push_back(std::move(item));
      SkipWhitespace();
      if (p_ >= end_) return Fail("unterminated array");
      if (*p_ == ',') { ++p_; continue; }
      if (*p_ == ']') { ++p_; return true; }
      return Fail("expected ',' or ']'");
    }
  }

  bool ParseObject(JsonValue* out) {
    ++p_; // 跳过 '{'
    out->type_ = JsonValue::Type::Object;
    SkipWhitespace();
    if (p_ < end_ && *p_ == '}') { ++p_; return true; }
    while (true) {
      SkipWhitespace();
      std::string key;
      if (!ParseString(&key)) return false;
      SkipWhitespace();
      if (p_ >= end_ || *p_ != ':') return Fail("expected ':'");
      ++p_;
      JsonValue value;
      if (!ParseValue(&value)) return false;
      out->object_.emplace_back(std::move(key), std::move(value));
      SkipWhitespace();
      if (p_ >= end_) return Fail("unterminated object");
      if (*p_ == ',') { ++p_; continue; }
      if (*p_ == '}') { ++p_; return true; }
      return Fail("expected ',' or '}'");
    }
  }

  const char* p_ = nullptr;
  const char* end_ = nullptr;
  const char* start_ = nullptr;
  std::string error_;
};

JsonValue JsonValue::Parse(const std::string& text, std::string* err) {
  // 允许 UTF-8 BOM：Windows 下用记事本编辑的配置文件常带 BOM
  const char* begin = text.data();
  const char* end = begin + text.size();
  if (text.size() >= 3 && static_cast<unsigned char>(begin[0]) == 0xEF &&
      static_cast<unsigned char>(begin[1]) == 0xBB && static_cast<unsigned char>(begin[2]) == 0xBF) {
    begin += 3;
  }

  JsonParser parser(begin, end);
  JsonValue root;
  if (!parser.ParseValue(&root)) {
    if (err) *err = parser.error().empty() ? "json parse failed" : parser.error();
    return JsonValue();
  }
  if (err) err->clear();
  return root;
}

} // namespace lumen
