// Minimal JSON value + parser/serializer. Enough for settings and the scan database.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace yov {

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool b) : type_(Type::Bool), num_(b ? 1 : 0) {}
  Json(int v) : type_(Type::Number), num_(v) {}
  Json(int64_t v) : type_(Type::Number), num_((double)v) {}
  Json(uint64_t v) : type_(Type::Number), num_((double)v) {}
  Json(double v) : type_(Type::Number), num_(v) {}
  Json(const char* s) : type_(Type::String), str_(s) {}
  Json(std::string s) : type_(Type::String), str_(std::move(s)) {}

  static Json array() {
    Json j;
    j.type_ = Type::Array;
    return j;
  }
  static Json object() {
    Json j;
    j.type_ = Type::Object;
    return j;
  }

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::Null; }
  bool isObject() const { return type_ == Type::Object; }
  bool isArray() const { return type_ == Type::Array; }
  bool isString() const { return type_ == Type::String; }
  bool isNumber() const { return type_ == Type::Number; }

  // Lenient accessors: return the fallback when the type does not match.
  bool asBool(bool fallback = false) const { return type_ == Type::Bool || type_ == Type::Number ? num_ != 0 : fallback; }
  double asNumber(double fallback = 0) const { return type_ == Type::Number ? num_ : fallback; }
  int64_t asInt(int64_t fallback = 0) const { return type_ == Type::Number ? (int64_t)num_ : fallback; }
  const std::string& asString() const;

  // Object access
  Json& operator[](const std::string& key);  // creates (converts null to object)
  const Json& get(const std::string& key) const;  // returns a null Json when missing
  bool has(const std::string& key) const;
  const std::vector<std::pair<std::string, Json>>& items() const { return obj_; }

  // Array access
  void push(Json v);
  size_t size() const { return type_ == Type::Array ? arr_.size() : type_ == Type::Object ? obj_.size() : 0; }
  const Json& at(size_t i) const;
  const std::vector<Json>& elements() const { return arr_; }

  std::string dump(bool pretty = false) const;
  static bool parse(std::string_view text, Json& out, std::string* error = nullptr);

 private:
  void dumpTo(std::string& out, bool pretty, int indent) const;

  Type type_ = Type::Null;
  double num_ = 0;
  std::string str_;
  std::vector<Json> arr_;
  std::vector<std::pair<std::string, Json>> obj_;  // insertion ordered
};

}  // namespace yov
