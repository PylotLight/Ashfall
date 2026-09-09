#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ashfall {

// Strict JSON subset shared by the versioned data loaders (objects, arrays,
// strings, integers, true/false). No floats; only \" and \\ escapes.
struct JsonValue {
    enum class Type { Object, Array, String, Int, Bool };
    Type type = Type::Object;
    std::string str;
    long long integer = 0;
    bool boolean = false;
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> fields;
};

class JsonParser {
  public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    bool run(JsonValue& root);
    [[nodiscard]] const std::string& error() const { return error_; }

  private:
    [[nodiscard]] bool at_end() const { return pos_ >= text_.size(); }
    void skip_ws();
    bool fail(const std::string& message);
    bool consume(char want, const char* what);
    bool parse_value(JsonValue& out);
    bool parse_object(JsonValue& out);
    bool parse_array(JsonValue& out);
    bool parse_string_value(JsonValue& out);
    bool parse_int(JsonValue& out);
    bool parse_bool(JsonValue& out);

    std::string_view text_;
    std::size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;
    std::string error_;
};

const JsonValue* find_json_field(const JsonValue& obj, std::string_view key);
bool require_json_int(const JsonValue& obj, std::string_view key, long long low, long long high,
                      long long& out, std::string& error);
bool require_json_bool(const JsonValue& obj, std::string_view key, bool& out, std::string& error);
bool require_json_string(const JsonValue& obj, std::string_view key, std::string& out,
                         std::string& error);

} // namespace ashfall
