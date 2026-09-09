#include "ashfall/json_subset.hpp"

#include <string>
#include <utility>

namespace ashfall {

bool JsonParser::run(JsonValue& root) {
    if (!parse_value(root)) return false;
    skip_ws();
    if (pos_ != text_.size()) return fail("trailing characters after top-level value");
    return true;
}

void JsonParser::skip_ws() {
    while (!at_end()) {
        const char c = text_[pos_];
        if (c == ' ' || c == '\t' || c == '\r') {
            ++pos_;
            ++col_;
        } else if (c == '\n') {
            ++pos_;
            ++line_;
            col_ = 1;
        } else {
            break;
        }
    }
}

bool JsonParser::fail(const std::string& message) {
    if (error_.empty()) {
        error_ = "line " + std::to_string(line_) + " col " + std::to_string(col_) + ": " + message;
    }
    return false;
}

bool JsonParser::consume(char want, const char* what) {
    if (at_end() || text_[pos_] != want) return fail(std::string("expected ") + what);
    ++pos_;
    ++col_;
    return true;
}

bool JsonParser::parse_value(JsonValue& out) {
    skip_ws();
    if (at_end()) return fail("unexpected end of input");
    const char c = text_[pos_];
    if (c == '{') return parse_object(out);
    if (c == '[') return parse_array(out);
    if (c == '"') return parse_string_value(out);
    if (c == 't' || c == 'f') return parse_bool(out);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_int(out);
    return fail(std::string("unexpected character '") + c + "'");
}

bool JsonParser::parse_object(JsonValue& out) {
    if (!consume('{', "'{'")) return false;
    out = JsonValue{};
    out.type = JsonValue::Type::Object;
    skip_ws();
    if (!at_end() && text_[pos_] == '}') {
        ++pos_;
        ++col_;
        return true;
    }
    while (true) {
        skip_ws();
        if (at_end() || text_[pos_] != '"') return fail("expected string key");
        JsonValue key;
        if (!parse_string_value(key)) return false;
        for (const auto& field : out.fields) {
            if (field.first == key.str) return fail("duplicate key \"" + field.first + "\"");
        }
        skip_ws();
        if (!consume(':', "':'")) return false;
        JsonValue value;
        if (!parse_value(value)) return false;
        out.fields.emplace_back(key.str, std::move(value));
        skip_ws();
        if (at_end()) return fail("unexpected end of input in object");
        if (text_[pos_] == ',') {
            ++pos_;
            ++col_;
            continue;
        }
        if (text_[pos_] == '}') {
            ++pos_;
            ++col_;
            return true;
        }
        return fail("expected ',' or '}' in object");
    }
}

bool JsonParser::parse_array(JsonValue& out) {
    if (!consume('[', "'['")) return false;
    out = JsonValue{};
    out.type = JsonValue::Type::Array;
    skip_ws();
    if (!at_end() && text_[pos_] == ']') {
        ++pos_;
        ++col_;
        return true;
    }
    while (true) {
        JsonValue value;
        if (!parse_value(value)) return false;
        out.items.push_back(std::move(value));
        skip_ws();
        if (at_end()) return fail("unexpected end of input in array");
        if (text_[pos_] == ',') {
            ++pos_;
            ++col_;
            continue;
        }
        if (text_[pos_] == ']') {
            ++pos_;
            ++col_;
            return true;
        }
        return fail("expected ',' or ']' in array");
    }
}

bool JsonParser::parse_string_value(JsonValue& out) {
    if (!consume('"', "'\"'")) return false;
    out = JsonValue{};
    out.type = JsonValue::Type::String;
    while (!at_end()) {
        const char c = text_[pos_];
        if (c == '"') {
            ++pos_;
            ++col_;
            return true;
        }
        if (c == '\\') {
            if (pos_ + 1 >= text_.size()) break;
            const char e = text_[pos_ + 1];
            if (e != '"' && e != '\\') return fail("unsupported escape (only \\\" and \\\\ allowed)");
            out.str.push_back(e);
            pos_ += 2;
            col_ += 2;
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20) return fail("unescaped control character in string");
        out.str.push_back(c);
        ++pos_;
        ++col_;
    }
    return fail("unterminated string");
}

bool JsonParser::parse_int(JsonValue& out) {
    bool negative = false;
    if (!at_end() && text_[pos_] == '-') {
        negative = true;
        ++pos_;
        ++col_;
    }
    if (at_end() || text_[pos_] < '0' || text_[pos_] > '9') return fail("expected integer");
    long long value = 0;
    while (!at_end() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        value = value * 10 + (text_[pos_] - '0');
        if (value > 1000000000LL) return fail("integer out of range");
        ++pos_;
        ++col_;
    }
    out = JsonValue{};
    out.type = JsonValue::Type::Int;
    out.integer = negative ? -value : value;
    return true;
}

bool JsonParser::parse_bool(JsonValue& out) {
    for (const char* word : {"true", "false"}) {
        const std::string_view view(word);
        if (text_.substr(pos_, view.size()) == view) {
            out = JsonValue{};
            out.type = JsonValue::Type::Bool;
            out.boolean = view == "true";
            pos_ += view.size();
            col_ += static_cast<int>(view.size());
            return true;
        }
    }
    return fail("expected 'true' or 'false'");
}

const JsonValue* find_json_field(const JsonValue& obj, std::string_view key) {
    if (obj.type != JsonValue::Type::Object) return nullptr;
    for (const auto& [name, value] : obj.fields) {
        if (name == key) return &value;
    }
    return nullptr;
}

bool require_json_int(const JsonValue& obj, std::string_view key, long long low, long long high,
                      long long& out, std::string& error) {
    const JsonValue* field = find_json_field(obj, key);
    if (field == nullptr || field->type != JsonValue::Type::Int) {
        error = std::string("field \"") + std::string(key) + "\" must be an integer";
        return false;
    }
    if (field->integer < low || field->integer > high) {
        error = std::string("field \"") + std::string(key) + "\" out of range";
        return false;
    }
    out = field->integer;
    return true;
}

bool require_json_bool(const JsonValue& obj, std::string_view key, bool& out, std::string& error) {
    const JsonValue* field = find_json_field(obj, key);
    if (field == nullptr || field->type != JsonValue::Type::Bool) {
        error = std::string("field \"") + std::string(key) + "\" must be true/false";
        return false;
    }
    out = field->boolean;
    return true;
}

bool require_json_string(const JsonValue& obj, std::string_view key, std::string& out,
                         std::string& error) {
    const JsonValue* field = find_json_field(obj, key);
    if (field == nullptr || field->type != JsonValue::Type::String) {
        error = std::string("field \"") + std::string(key) + "\" must be a string";
        return false;
    }
    out = field->str;
    return true;
}

} // namespace ashfall
