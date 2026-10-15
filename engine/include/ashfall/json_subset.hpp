#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ashfall {

// ---------------------------------------------------------------------------
// Strict JSON subset (Group 3 hardened). Shared by the versioned data loaders.
//
// Supported syntax: objects, arrays, strings, integers, `true`/`false`.
// Whitespace is space/tab/CR/LF. Duplicate object keys are rejected. The whole
// input must be consumed (trailing bytes are rejected).
//
// NOT supported (rejected with an explicit diagnostic):
//   - floats (`1.5`), exponents (`1e3`), leading zeros (`01`, `-01`, `00`);
//     a `.`/`e`/`E` immediately after an integer names the offending char.
//   - `null`, and any literal other than `true`/`false`.
//   - string escapes other than `\"` and `\\`. In particular `\u`, `\n`,
//     `\t`, `\r`, `\b`, `\f`, `\/` and any other `\X` are REJECTED (the
//     parser only emits `"` and `\` from escapes). Raw bytes >= 0x20 are
//     accepted as-is (no UTF-8 validation); unescaped bytes < 0x20 reject.
//   - integer tokens outside -1000000000..+1000000000 (a format policy, not a
//     C++ limit; accumulation is precondition-checked so the bound can never
//     overflow regardless of future limit changes).
//
// Identifier policy (Choice A): simulation identifiers (material names, tag
// names, material references, reaction names) must match `[a-z][a-z0-9_-]*`
// (hyphens admitted for shipped names such as "oil-flash"). Enforced by the
// schema loaders via is_valid_identifier(), not by this parser.
//
// Resource limits: every parse is bounded by JsonLimits (input bytes, nesting
// depth, node count, total string bytes, per-array items, per-object fields).
// Checks run BEFORE allocating or descending. Violations report
// `line L col C: limit: ...`.
//
// Allocation-failure policy: this parser performs no `catch (...)` anywhere
// in the engine (verified by grep). `std::bad_alloc` propagates to the host
// boundary; the limits above exist to keep legitimate requests small so an
// allocation failure signals a genuinely exhausted host, never "bad JSON".
// ---------------------------------------------------------------------------
struct JsonValue {
    enum class Type { Object, Array, String, Int, Bool };
    Type type = Type::Object;
    std::string str;
    long long integer = 0;
    bool boolean = false;
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> fields;
};

struct JsonLimits {
    std::size_t max_input_bytes = 1 << 20;
    std::size_t max_nodes = 1 << 17;
    std::size_t max_string_bytes = 1 << 18;
    std::size_t max_array_items = 1 << 16;
    std::size_t max_object_fields = 1 << 13;
    int max_depth = 64;
    std::size_t max_reactions = 1 << 12;
    std::size_t max_materials = 256;
};

inline constexpr JsonLimits kDefaultJsonLimits{};

// `[a-z][a-z0-9_-]*` (Choice A identifier policy). Empty string rejects.
bool is_valid_identifier(std::string_view name);

class JsonParser {
  public:
    // run() resets ALL parser state (cursor, line/col, error, budget
    // counters), so a parser object may be reused for sequential parses.
    // The input string_view is only borrowed during run(): the caller must
    // keep it alive until run() returns (no parsed view outlives the call —
    // strings are copied into JsonValue).
    explicit JsonParser(std::string_view text, JsonLimits limits = kDefaultJsonLimits)
        : text_(text), limits_(limits) {}

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
    JsonLimits limits_;
    std::size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;
    std::string error_;
    int depth_ = 0;
    std::size_t node_count_ = 0;
    std::size_t string_bytes_ = 0;
};

const JsonValue* find_json_field(const JsonValue& obj, std::string_view key);
// Field accessors for schema loaders. On failure `error` names the key
// (`field "density" must be an integer` / `field "density" out of range`);
// loaders prefix it with the schema path (`materials[4].density: ...`).
// require_json_int asserts low <= high (programmer-error hygiene).
bool require_json_int(const JsonValue& obj, std::string_view key, long long low, long long high,
                      long long& out, std::string& error);
bool require_json_bool(const JsonValue& obj, std::string_view key, bool& out, std::string& error);
bool require_json_string(const JsonValue& obj, std::string_view key, std::string& out,
                         std::string& error);

} // namespace ashfall
