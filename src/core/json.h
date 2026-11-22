// ============================================================================
//  LumenPlayer - 轻量 JSON 解析器
//  ---------------------------------------------------------------------------
//  所属层次：产品层（C 层）
//  职责    ：解析 ffprobe / 配置文件输出。
//
//  为什么自己写而不引入 nlohmann/json：
//    1. 本产品对体积与启动时延敏感，不希望为一个只读场景引入 20k+ 行模板头文件；
//    2. 策略层要求「零第三方依赖」，以便原样迁移到 Android / HarmonyOS；
//    3. 我们只需要「解析 + 只读查询」，不需要序列化与 schema 校验，
//       自己实现约 300 行即可，行为完全可控。
//
//  限制（刻意为之）：
//    - 不支持注释与尾随逗号（ffprobe 不会输出这些）；
//    - 数字统一用 double 承载（2^53 内精确，bit_rate / duration 足够）；
//    - 解析失败返回 Null 并通过 err 回传位置信息，不抛异常。
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lumen {

class JsonValue {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  JsonValue() = default;

  // --- 类型判定 ---
  Type type() const { return type_; }
  bool IsNull() const { return type_ == Type::Null; }
  bool IsBool() const { return type_ == Type::Bool; }
  bool IsNumber() const { return type_ == Type::Number; }
  bool IsString() const { return type_ == Type::String; }
  bool IsArray() const { return type_ == Type::Array; }
  bool IsObject() const { return type_ == Type::Object; }

  // --- 取值（类型不符时返回 defaultValue，绝不抛异常） ---
  std::string AsString(const std::string& defaultValue = std::string()) const;
  double AsDouble(double defaultValue = 0.0) const;
  int64_t AsInt(int64_t defaultValue = 0) const;
  bool AsBool(bool defaultValue = false) const;

  // --- 容器访问 ---
  // 参数说明：key 为对象成员名；不存在时返回一个静态 Null 对象，
  //           这样调用方可以安全地链式查询，无需逐层判空。
  const JsonValue& operator[](const std::string& key) const;
  // 参数说明：index 为数组下标；越界时返回静态 Null 对象
  const JsonValue& operator[](size_t index) const;
  size_t size() const;

  // 路径查询，支持点号分隔与数组下标，例如 "streams.0.width"
  // 用途：ffprobe 输出层级较深，用路径查询可让业务代码保持扁平可读
  const JsonValue& Query(const std::string& path) const;

  // --- 解析入口 ---
  // 参数说明：text 为待解析文本；err 非空时写入错误描述（含字节偏移）
  // 返回值  ：解析成功返回对应值；失败返回 Null
  static JsonValue Parse(const std::string& text, std::string* err = nullptr);

 private:
  friend class JsonParser;

  Type type_ = Type::Null;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  std::vector<JsonValue> array_;
  std::vector<std::pair<std::string, JsonValue>> object_;
};

} // namespace lumen
