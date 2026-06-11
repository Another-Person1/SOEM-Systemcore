#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace limelight_ec {

class Json final {
 public:
  using Object = std::map<std::string, Json>;
  using Array = std::vector<Json>;

  Json() = default;
  explicit Json(std::nullptr_t) {}
  explicit Json(Object object) : value_(std::move(object)) {}
  explicit Json(Array array) : value_(std::move(array)) {}
  explicit Json(std::string text) : value_(std::move(text)) {}
  explicit Json(bool value) : value_(value) {}
  explicit Json(double value) : value_(value) {}

  static Json Parse(const std::string& text) {
    Parser parser{text};
    Json result = parser.parseValue();
    parser.skipWhitespace();
    if (!parser.finished()) {
      throw std::runtime_error("unexpected trailing JSON data");
    }
    return result;
  }

  bool isObject() const { return std::holds_alternative<Object>(value_); }
  bool isArray() const { return std::holds_alternative<Array>(value_); }
  bool isString() const { return std::holds_alternative<std::string>(value_); }
  bool isBool() const { return std::holds_alternative<bool>(value_); }
  bool isNumber() const { return std::holds_alternative<double>(value_); }

  const Object& object() const { return std::get<Object>(value_); }
  const Array& array() const { return std::get<Array>(value_); }
  const std::string& string() const { return std::get<std::string>(value_); }
  bool boolean() const { return std::get<bool>(value_); }
  double number() const { return std::get<double>(value_); }

  const Json* find(const std::string& key) const {
    if (!isObject()) {
      return nullptr;
    }
    const auto& objectValue = object();
    auto it = objectValue.find(key);
    return it == objectValue.end() ? nullptr : &it->second;
  }

  std::string stringValue(const std::string& key,
                          const std::string& fallback = "") const {
    const Json* found = find(key);
    return found && found->isString() ? found->string() : fallback;
  }

  bool boolValue(const std::string& key, bool fallback = false) const {
    const Json* found = find(key);
    return found && found->isBool() ? found->boolean() : fallback;
  }

  int64_t integerValue(const std::string& key, int64_t fallback = 0) const {
    const Json* found = find(key);
    return found && found->isNumber() ? static_cast<int64_t>(found->number())
                                      : fallback;
  }

 private:
  class Parser final {
   public:
    explicit Parser(const std::string& text) : text_(text) {}

    Json parseValue() {
      skipWhitespace();
      if (finished()) {
        throw std::runtime_error("unexpected end of JSON input");
      }

      switch (peek()) {
        case '{':
          return parseObject();
        case '[':
          return parseArray();
        case '"':
          return Json{parseString()};
        case 't':
          consumeLiteral("true");
          return Json{true};
        case 'f':
          consumeLiteral("false");
          return Json{false};
        case 'n':
          consumeLiteral("null");
          return Json{nullptr};
        default:
          if (peek() == '-' || std::isdigit(static_cast<unsigned char>(peek()))) {
            return Json{parseNumber()};
          }
          throw std::runtime_error("unexpected JSON token");
      }
    }

    void skipWhitespace() {
      while (!finished() &&
             std::isspace(static_cast<unsigned char>(text_[offset_]))) {
        ++offset_;
      }
    }

    bool finished() const { return offset_ >= text_.size(); }

   private:
    char peek() const { return text_[offset_]; }

    char get() {
      if (finished()) {
        throw std::runtime_error("unexpected end of JSON input");
      }
      return text_[offset_++];
    }

    Json parseObject() {
      expect('{');
      Object objectValue;
      skipWhitespace();
      if (!finished() && peek() == '}') {
        get();
        return Json{std::move(objectValue)};
      }

      while (true) {
        skipWhitespace();
        if (finished() || peek() != '"') {
          throw std::runtime_error("JSON object key must be a string");
        }
        std::string key = parseString();
        skipWhitespace();
        expect(':');
        auto inserted = objectValue.emplace(std::move(key), parseValue());
        if (!inserted.second) {
          throw std::runtime_error("duplicate JSON object key");
        }
        skipWhitespace();
        char separator = get();
        if (separator == '}') {
          break;
        }
        if (separator != ',') {
          throw std::runtime_error("expected ',' or '}' in JSON object");
        }
      }
      return Json{std::move(objectValue)};
    }

    Json parseArray() {
      expect('[');
      Array arrayValue;
      skipWhitespace();
      if (!finished() && peek() == ']') {
        get();
        return Json{std::move(arrayValue)};
      }

      while (true) {
        arrayValue.emplace_back(parseValue());
        skipWhitespace();
        char separator = get();
        if (separator == ']') {
          break;
        }
        if (separator != ',') {
          throw std::runtime_error("expected ',' or ']' in JSON array");
        }
      }
      return Json{std::move(arrayValue)};
    }

    std::string parseString() {
      expect('"');
      std::string value;
      while (!finished()) {
        char ch = get();
        if (ch == '"') {
          return value;
        }
        if (static_cast<unsigned char>(ch) < 0x20U) {
          throw std::runtime_error("invalid control character in JSON string");
        }
        if (ch != '\\') {
          value.push_back(ch);
          continue;
        }

        char escaped = get();
        switch (escaped) {
          case '"':
          case '\\':
          case '/':
            value.push_back(escaped);
            break;
          case 'b':
            value.push_back('\b');
            break;
          case 'f':
            value.push_back('\f');
            break;
          case 'n':
            value.push_back('\n');
            break;
          case 'r':
            value.push_back('\r');
            break;
          case 't':
            value.push_back('\t');
            break;
          case 'u':
            value.push_back(parseBasicUnicodeEscape());
            break;
          default:
            throw std::runtime_error("unsupported JSON string escape");
        }
      }
      throw std::runtime_error("unterminated JSON string");
    }

    char parseBasicUnicodeEscape() {
      uint32_t value = 0;
      for (int i = 0; i < 4; ++i) {
        value <<= 4U;
        char ch = get();
        if (ch >= '0' && ch <= '9') {
          value += static_cast<uint32_t>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
          value += static_cast<uint32_t>(ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
          value += static_cast<uint32_t>(ch - 'A' + 10);
        } else {
          throw std::runtime_error("invalid JSON unicode escape");
        }
      }
      return value < 0x80 ? static_cast<char>(value) : '?';
    }

    double parseNumber() {
      const size_t start = offset_;
      if (peek() == '-') {
        get();
      }
      if (finished() || !std::isdigit(static_cast<unsigned char>(peek()))) {
        throw std::runtime_error("invalid JSON number");
      }
      if (peek() == '0') {
        get();
        if (!finished() && std::isdigit(static_cast<unsigned char>(peek()))) {
          throw std::runtime_error("invalid leading zero in JSON number");
        }
      } else {
        while (!finished() &&
               std::isdigit(static_cast<unsigned char>(peek()))) {
          get();
        }
      }
      if (!finished() && peek() == '.') {
        get();
        if (finished() || !std::isdigit(static_cast<unsigned char>(peek()))) {
          throw std::runtime_error("invalid JSON fraction");
        }
        while (!finished() &&
               std::isdigit(static_cast<unsigned char>(peek()))) {
          get();
        }
      }
      if (!finished() && (peek() == 'e' || peek() == 'E')) {
        get();
        if (!finished() && (peek() == '+' || peek() == '-')) {
          get();
        }
        if (finished() || !std::isdigit(static_cast<unsigned char>(peek()))) {
          throw std::runtime_error("invalid JSON exponent");
        }
        while (!finished() &&
               std::isdigit(static_cast<unsigned char>(peek()))) {
          get();
        }
      }
      return std::stod(text_.substr(start, offset_ - start));
    }

    void consumeLiteral(const char* literal) {
      for (const char* cursor = literal; *cursor != '\0'; ++cursor) {
        if (get() != *cursor) {
          throw std::runtime_error("invalid JSON literal");
        }
      }
    }

    void expect(char expected) {
      if (get() != expected) {
        throw std::runtime_error("unexpected JSON character");
      }
    }

    const std::string& text_;
    size_t offset_ = 0;
  };

  std::variant<std::nullptr_t, Object, Array, std::string, bool, double> value_ =
      nullptr;
};

}  // namespace limelight_ec
