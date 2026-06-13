#pragma once

#include <cstddef>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Minimal recursive-descent JSON parser (header-only). Enough for reading case
// files: objects, arrays, numbers, strings, true/false/null. Permissive, no
// third-party dependency. For writing JSON the geometry report uses its own
// emitter; this is read-only.
namespace nabla::json {

struct Value {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool b = false;
  double num = 0.0;
  std::string str;
  std::vector<Value> arr;
  std::vector<std::pair<std::string, Value>> obj;

  [[nodiscard]] bool isObject() const { return type == Type::Object; }
  [[nodiscard]] bool isArray() const { return type == Type::Array; }
  [[nodiscard]] bool isNumber() const { return type == Type::Number; }
  [[nodiscard]] bool isString() const { return type == Type::String; }

  // Object lookup; returns nullptr when absent or not an object.
  [[nodiscard]] const Value* find(const std::string& key) const {
    if (type != Type::Object) {
      return nullptr;
    }
    for (const auto& kv : obj) {
      if (kv.first == key) {
        return &kv.second;
      }
    }
    return nullptr;
  }
  [[nodiscard]] bool contains(const std::string& key) const { return find(key) != nullptr; }

  [[nodiscard]] double number(const std::string& key, double fallback) const {
    const Value* v = find(key);
    return (v && v->type == Type::Number) ? v->num : fallback;
  }
  [[nodiscard]] int integer(const std::string& key, int fallback) const {
    const Value* v = find(key);
    return (v && v->type == Type::Number) ? static_cast<int>(v->num) : fallback;
  }
  [[nodiscard]] bool boolean(const std::string& key, bool fallback) const {
    const Value* v = find(key);
    return (v && v->type == Type::Bool) ? v->b : fallback;
  }
  [[nodiscard]] std::string string(const std::string& key, const std::string& fb) const {
    const Value* v = find(key);
    return (v && v->type == Type::String) ? v->str : fb;
  }
};

namespace detail {

class Parser {
 public:
  explicit Parser(const std::string& s) : s_(s) {}

  Value parse() {
    skipWs();
    Value v = parseValue();
    skipWs();
    if (pos_ != s_.size()) {
      fail("trailing characters after JSON value");
    }
    return v;
  }

 private:
  [[noreturn]] void fail(const std::string& msg) const {
    throw std::runtime_error("JSON parse error at " + std::to_string(pos_) + ": " + msg);
  }
  char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }
  char get() { return pos_ < s_.size() ? s_[pos_++] : '\0'; }
  void skipWs() {
    while (pos_ < s_.size()) {
      const char c = s_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '/') {
        while (pos_ < s_.size() && s_[pos_] != '\n') {
          ++pos_;  // tolerate // comments
        }
      } else {
        break;
      }
    }
  }

  Value parseValue() {
    skipWs();
    const char c = peek();
    switch (c) {
      case '{': return parseObject();
      case '[': return parseArray();
      case '"': {
        Value v;
        v.type = Value::Type::String;
        v.str = parseString();
        return v;
      }
      case 't':
      case 'f': return parseBool();
      case 'n': return parseNull();
      default: return parseNumber();
    }
  }

  Value parseObject() {
    Value v;
    v.type = Value::Type::Object;
    get();  // {
    skipWs();
    if (peek() == '}') {
      get();
      return v;
    }
    for (;;) {
      skipWs();
      if (peek() != '"') {
        fail("expected string key");
      }
      const std::string key = parseString();
      skipWs();
      if (get() != ':') {
        fail("expected ':'");
      }
      v.obj.emplace_back(key, parseValue());
      skipWs();
      const char c = get();
      if (c == ',') {
        continue;
      }
      if (c == '}') {
        break;
      }
      fail("expected ',' or '}'");
    }
    return v;
  }

  Value parseArray() {
    Value v;
    v.type = Value::Type::Array;
    get();  // [
    skipWs();
    if (peek() == ']') {
      get();
      return v;
    }
    for (;;) {
      v.arr.push_back(parseValue());
      skipWs();
      const char c = get();
      if (c == ',') {
        continue;
      }
      if (c == ']') {
        break;
      }
      fail("expected ',' or ']'");
    }
    return v;
  }

  unsigned parseHex4() {
    unsigned cp = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = get();
      cp <<= 4;
      if (c >= '0' && c <= '9') {
        cp |= static_cast<unsigned>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        cp |= static_cast<unsigned>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        cp |= static_cast<unsigned>(c - 'A' + 10);
      } else {
        fail("invalid \\u escape");
      }
    }
    return cp;
  }

  void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  std::string parseString() {
    get();  // opening quote
    std::string out;
    for (;;) {
      const char c = get();
      if (c == '\0') {
        fail("unterminated string");
      }
      if (c == '"') {
        break;
      }
      if (c == '\\') {
        const char e = get();
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'n': out.push_back('\n'); break;
          case 't': out.push_back('\t'); break;
          case 'r': out.push_back('\r'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'u': {
            unsigned cp = parseHex4();
            // surrogate pair -> single code point
            if (cp >= 0xD800 && cp <= 0xDBFF && peek() == '\\') {
              const std::size_t save = pos_;
              get();  // backslash
              if (peek() == 'u') {
                get();
                const unsigned lo = parseHex4();
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                  cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else {
                  pos_ = save;  // not a low surrogate; rewind
                }
              } else {
                pos_ = save;
              }
            }
            appendUtf8(out, cp);
            break;
          }
          default: out.push_back(e); break;  // tolerate unknown escapes
        }
      } else {
        out.push_back(c);
      }
    }
    return out;
  }

  Value parseBool() {
    Value v;
    v.type = Value::Type::Bool;
    if (s_.compare(pos_, 4, "true") == 0) {
      v.b = true;
      pos_ += 4;
    } else if (s_.compare(pos_, 5, "false") == 0) {
      v.b = false;
      pos_ += 5;
    } else {
      fail("invalid literal");
    }
    return v;
  }

  Value parseNull() {
    if (s_.compare(pos_, 4, "null") != 0) {
      fail("invalid literal");
    }
    pos_ += 4;
    return Value{};
  }

  Value parseNumber() {
    const std::size_t start = pos_;
    if (peek() == '-' || peek() == '+') {
      get();
    }
    while (pos_ < s_.size()) {
      const char c = s_[pos_];
      if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' ||
          c == '-') {
        ++pos_;
      } else {
        break;
      }
    }
    if (pos_ == start) {
      fail("invalid number");
    }
    Value v;
    v.type = Value::Type::Number;
    v.num = std::stod(s_.substr(start, pos_ - start));
    return v;
  }

  const std::string& s_;
  std::size_t pos_ = 0;
};

}  // namespace detail

inline Value parse(const std::string& text) { return detail::Parser(text).parse(); }

inline Value parseFile(const std::string& path) {
  std::ifstream f(path);
  if (!f) {
    throw std::runtime_error("JSON: cannot open file: " + path);
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return parse(ss.str());
}

}  // namespace nabla::json
