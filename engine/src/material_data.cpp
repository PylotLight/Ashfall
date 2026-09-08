#include "ashfall/material_data.hpp"

#include <string>
#include <utility>

namespace ashfall {
namespace {

struct JsonValue {
    enum class Type { Object, Array, String, Int, Bool };
    Type type = Type::Object;
    std::string str;
    long long integer = 0;
    bool boolean = false;
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> fields;
};

class Parser {
  public:
    explicit Parser(std::string_view text) : text_(text) {}

    bool run(JsonValue& root) {
        if (!parse_value(root)) return false;
        skip_ws();
        if (pos_ != text_.size()) return fail("trailing characters after top-level value");
        return true;
    }

    [[nodiscard]] const std::string& error() const { return error_; }

  private:
    [[nodiscard]] bool at_end() const { return pos_ >= text_.size(); }

    void skip_ws() {
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

    bool fail(const std::string& message) {
        if (error_.empty()) {
            error_ = "line " + std::to_string(line_) + " col " + std::to_string(col_) + ": " + message;
        }
        return false;
    }

    bool consume(char want, const char* what) {
        if (at_end() || text_[pos_] != want) return fail(std::string("expected ") + what);
        ++pos_;
        ++col_;
        return true;
    }

    bool parse_value(JsonValue& out) {
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

    bool parse_object(JsonValue& out) {
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

    bool parse_array(JsonValue& out) {
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

    bool parse_string_value(JsonValue& out) {
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

    bool parse_int(JsonValue& out) {
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

    bool parse_bool(JsonValue& out) {
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

    std::string_view text_;
    std::size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;
    std::string error_;
};

const JsonValue* find_field(const JsonValue& obj, std::string_view key) {
    if (obj.type != JsonValue::Type::Object) return nullptr;
    for (const auto& [name, value] : obj.fields) {
        if (name == key) return &value;
    }
    return nullptr;
}

bool require_int(const JsonValue& obj, std::string_view key, long long low, long long high, long long& out,
                 std::string& error) {
    const JsonValue* field = find_field(obj, key);
    if (field == nullptr || field->type != JsonValue::Type::Int) {
        error = std::string("material field \"") + std::string(key) + "\" must be an integer";
        return false;
    }
    if (field->integer < low || field->integer > high) {
        error = std::string("material field \"") + std::string(key) + "\" out of range";
        return false;
    }
    out = field->integer;
    return true;
}

bool require_bool(const JsonValue& obj, std::string_view key, bool& out, std::string& error) {
    const JsonValue* field = find_field(obj, key);
    if (field == nullptr || field->type != JsonValue::Type::Bool) {
        error = std::string("material field \"") + std::string(key) + "\" must be true/false";
        return false;
    }
    out = field->boolean;
    return true;
}

bool require_string(const JsonValue& obj, std::string_view key, std::string& out, std::string& error) {
    const JsonValue* field = find_field(obj, key);
    if (field == nullptr || field->type != JsonValue::Type::String) {
        error = std::string("material field \"") + std::string(key) + "\" must be a string";
        return false;
    }
    out = field->str;
    return true;
}

} // namespace

bool movement_from_string(std::string_view name, Movement& out) {
    if (name == "static") {
        out = Movement::Static;
        return true;
    }
    if (name == "powder") {
        out = Movement::Powder;
        return true;
    }
    if (name == "liquid") {
        out = Movement::Liquid;
        return true;
    }
    return false;
}

bool parse_material_data(std::string_view text, MaterialTable& out, std::string& error) {
    out = MaterialTable{};
    Parser parser(text);
    JsonValue root;
    if (!parser.run(root)) {
        error = parser.error();
        return false;
    }
    if (root.type != JsonValue::Type::Object) {
        error = "top-level value must be an object";
        return false;
    }
    const JsonValue* version = find_field(root, "version");
    if (version == nullptr || version->type != JsonValue::Type::Int || version->integer != 1) {
        error = "unsupported data version (want 1)";
        return false;
    }
    const JsonValue* materials = find_field(root, "materials");
    if (materials == nullptr || materials->type != JsonValue::Type::Array || materials->items.empty()) {
        error = "\"materials\" must be a non-empty array";
        return false;
    }
    bool seen_id[256] = {};
    std::vector<std::string> seen_names;
    MaterialTable table;
    table.version = 1;
    for (const JsonValue& entry : materials->items) {
        if (entry.type != JsonValue::Type::Object) {
            error = "material entry must be an object";
            return false;
        }
        long long id = 0;
        if (!require_int(entry, "id", 0, 255, id, error)) return false;
        if (seen_id[static_cast<std::size_t>(id)]) {
            error = "duplicate material id";
            return false;
        }
        std::string name;
        if (!require_string(entry, "name", name, error) || name.empty()) {
            error = "material \"name\" must be a non-empty string";
            return false;
        }
        for (const std::string& taken : seen_names) {
            if (taken == name) {
                error = "duplicate material name \"" + name + "\"";
                return false;
            }
        }
        std::string movement_name;
        if (!require_string(entry, "movement", movement_name, error)) return false;
        Movement movement = Movement::Static;
        if (!movement_from_string(movement_name, movement)) {
            error = "unknown movement \"" + movement_name + "\" (want static/powder/liquid)";
            return false;
        }
        bool solid = false;
        if (!require_bool(entry, "solid", solid, error)) return false;
        if (solid && movement != Movement::Static) {
            error = "material \"" + name + "\" is solid but not static";
            return false;
        }
        long long density = 0;
        if (!require_int(entry, "density", 0, 255, density, error)) return false;
        const JsonValue* color = find_field(entry, "color");
        if (color == nullptr || color->type != JsonValue::Type::Array || color->items.size() != 3) {
            error = "material \"color\" must be an array of 3 integers";
            return false;
        }
        std::array<std::uint8_t, 3> rgb = {0, 0, 0};
        for (std::size_t c = 0; c < 3; ++c) {
            if (color->items[c].type != JsonValue::Type::Int || color->items[c].integer < 0 ||
                color->items[c].integer > 255) {
                error = "material \"color\" channels must be integers 0..255";
                return false;
            }
            rgb[c] = static_cast<std::uint8_t>(color->items[c].integer);
        }
        MaterialSpec spec;
        spec.id = static_cast<MaterialId>(id);
        spec.name = name;
        spec.movement = movement;
        spec.solid = solid;
        spec.density = static_cast<int>(density);
        spec.color = rgb;
        table.materials.push_back(std::move(spec));
        seen_id[static_cast<std::size_t>(id)] = true;
        seen_names.push_back(name);
    }
    bool has_air = false;
    for (const MaterialSpec& spec : table.materials) {
        if (spec.id != 0) continue;
        has_air = true;
        if (spec.name != "air" || spec.movement != Movement::Static || spec.solid || spec.density != 0) {
            error = "material id 0 must be non-solid static air with density 0";
            return false;
        }
    }
    if (!has_air) {
        error = "material id 0 (air) is required";
        return false;
    }
    out = std::move(table);
    return true;
}

} // namespace ashfall
