#include "common/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace yov {

static const std::string kEmpty;
static const Json kNull;

const std::string& Json::asString() const { return type_ == Type::String ? str_ : kEmpty; }

Json& Json::operator[](const std::string& key) {
  if (type_ != Type::Object) {
    *this = object();
  }
  for (auto& kv : obj_)
    if (kv.first == key) return kv.second;
  obj_.emplace_back(key, Json());
  return obj_.back().second;
}

const Json& Json::get(const std::string& key) const {
  if (type_ == Type::Object)
    for (auto& kv : obj_)
      if (kv.first == key) return kv.second;
  return kNull;
}

bool Json::has(const std::string& key) const {
  if (type_ == Type::Object)
    for (auto& kv : obj_)
      if (kv.first == key) return true;
  return false;
}

void Json::push(Json v) {
  if (type_ != Type::Array) *this = array();
  arr_.push_back(std::move(v));
}

const Json& Json::at(size_t i) const { return (type_ == Type::Array && i < arr_.size()) ? arr_[i] : kNull; }

static void escapeTo(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof b, "\\u%04x", c);
          out += b;
        } else {
          out += (char)c;
        }
    }
  }
  out += '"';
}

void Json::dumpTo(std::string& out, bool pretty, int indent) const {
  auto nl = [&](int ind) {
    if (!pretty) return;
    out += '\n';
    out.append(size_t(ind) * 2, ' ');
  };
  switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += num_ != 0 ? "true" : "false"; break;
    case Type::Number: {
      if (!std::isfinite(num_)) {
        out += "0";
      } else if (num_ == std::floor(num_) && std::fabs(num_) < 9.007e15) {
        char b[32];
        snprintf(b, sizeof b, "%lld", (long long)num_);
        out += b;
      } else {
        char b[40];
        snprintf(b, sizeof b, "%.17g", num_);
        out += b;
      }
      break;
    }
    case Type::String: escapeTo(out, str_); break;
    case Type::Array: {
      out += '[';
      for (size_t i = 0; i < arr_.size(); ++i) {
        if (i) out += ',';
        nl(indent + 1);
        arr_[i].dumpTo(out, pretty, indent + 1);
      }
      if (!arr_.empty()) nl(indent);
      out += ']';
      break;
    }
    case Type::Object: {
      out += '{';
      for (size_t i = 0; i < obj_.size(); ++i) {
        if (i) out += ',';
        nl(indent + 1);
        escapeTo(out, obj_[i].first);
        out += pretty ? ": " : ":";
        obj_[i].second.dumpTo(out, pretty, indent + 1);
      }
      if (!obj_.empty()) nl(indent);
      out += '}';
      break;
    }
  }
}

std::string Json::dump(bool pretty) const {
  std::string s;
  dumpTo(s, pretty, 0);
  return s;
}

namespace {

struct Parser {
  std::string_view t;
  size_t i = 0;
  std::string err;

  void ws() {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) ++i;
  }
  bool fail(const char* m) {
    if (err.empty()) err = std::string(m) + " at offset " + std::to_string(i);
    return false;
  }
  static void appendUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) {
      s += char(0xC0 | (cp >> 6));
      s += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      s += char(0xE0 | (cp >> 12));
      s += char(0x80 | ((cp >> 6) & 0x3F));
      s += char(0x80 | (cp & 0x3F));
    } else {
      s += char(0xF0 | (cp >> 18));
      s += char(0x80 | ((cp >> 12) & 0x3F));
      s += char(0x80 | ((cp >> 6) & 0x3F));
      s += char(0x80 | (cp & 0x3F));
    }
  }
  bool hex4(uint32_t& v) {
    if (i + 4 > t.size()) return false;
    v = 0;
    for (int k = 0; k < 4; ++k) {
      char c = t[i++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0';
      else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
      else return false;
    }
    return true;
  }
  bool str(std::string& s) {
    if (t[i] != '"') return fail("expected string");
    ++i;
    while (i < t.size()) {
      char c = t[i++];
      if (c == '"') return true;
      if (c != '\\') {
        s += c;
        continue;
      }
      if (i >= t.size()) break;
      char e = t[i++];
      switch (e) {
        case '"': s += '"'; break;
        case '\\': s += '\\'; break;
        case '/': s += '/'; break;
        case 'b': s += '\b'; break;
        case 'f': s += '\f'; break;
        case 'n': s += '\n'; break;
        case 'r': s += '\r'; break;
        case 't': s += '\t'; break;
        case 'u': {
          uint32_t cp;
          if (!hex4(cp)) return fail("bad \\u escape");
          if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
            i += 2;
            uint32_t lo;
            if (!hex4(lo)) return fail("bad surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          appendUtf8(s, cp);
          break;
        }
        default: return fail("bad escape");
      }
    }
    return fail("unterminated string");
  }
  bool value(Json& out, int depth) {
    if (depth > 64) return fail("nesting too deep");
    ws();
    if (i >= t.size()) return fail("unexpected end");
    char c = t[i];
    if (c == '{') {
      ++i;
      out = Json::object();
      ws();
      if (i < t.size() && t[i] == '}') {
        ++i;
        return true;
      }
      for (;;) {
        ws();
        std::string key;
        if (i >= t.size() || !str(key)) return fail("expected key");
        ws();
        if (i >= t.size() || t[i] != ':') return fail("expected ':'");
        ++i;
        Json v;
        if (!value(v, depth + 1)) return false;
        out[key] = std::move(v);
        ws();
        if (i < t.size() && t[i] == ',') {
          ++i;
          continue;
        }
        if (i < t.size() && t[i] == '}') {
          ++i;
          return true;
        }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      ++i;
      out = Json::array();
      ws();
      if (i < t.size() && t[i] == ']') {
        ++i;
        return true;
      }
      for (;;) {
        Json v;
        if (!value(v, depth + 1)) return false;
        out.push(std::move(v));
        ws();
        if (i < t.size() && t[i] == ',') {
          ++i;
          continue;
        }
        if (i < t.size() && t[i] == ']') {
          ++i;
          return true;
        }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') {
      std::string s;
      if (!str(s)) return false;
      out = Json(std::move(s));
      return true;
    }
    if (t.substr(i, 4) == "true") {
      i += 4;
      out = Json(true);
      return true;
    }
    if (t.substr(i, 5) == "false") {
      i += 5;
      out = Json(false);
      return true;
    }
    if (t.substr(i, 4) == "null") {
      i += 4;
      out = Json();
      return true;
    }
    size_t start = i;
    while (i < t.size() && (std::strchr("+-0123456789.eE", t[i]) != nullptr)) ++i;
    if (start == i) return fail("unexpected character");
    std::string num(t.substr(start, i - start));
    out = Json(std::strtod(num.c_str(), nullptr));
    return true;
  }
};

}  // namespace

bool Json::parse(std::string_view text, Json& out, std::string* error) {
  Parser p{text};
  // Skip a UTF-8 BOM.
  if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) p.i = 3;
  Json v;
  if (!p.value(v, 0)) {
    if (error) *error = p.err;
    return false;
  }
  out = std::move(v);
  return true;
}

}  // namespace yov
