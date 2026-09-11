// tests/fuzz/json_verdict.cpp (Task F6, offline differential only — NOT wired into
// CMakeLists.txt by this task; the integrator adds the json_verdict target).
//
// Stdin bytes -> stdout verdict for differential_json.py:
//   ACCEPT\n
//   REJECT <class> <full parser diagnostic>\n
//
// <class> is a coarse, stable taxonomy mapped from JsonParser::error() so the
// Python side can allowlist KNOWN subset divergences without string-matching
// free-form diagnostics. Classification is substring-based on the exact
// messages in engine/src/json_subset.cpp (Group-3 hardened grammar).
#include <iostream>
#include <string>
#include <string_view>

#include "ashfall/json_subset.hpp"

namespace {

std::string_view classify(std::string_view err) {
    if (err.find("limit:") != std::string_view::npos) {
        if (err.find("depth") != std::string_view::npos) {
            return "limit-depth";
        }
        if (err.find("integer magnitude") != std::string_view::npos) {
            return "int-clamp";
        }
        return "limit-size";
    }
    if (err.find("duplicate key") != std::string_view::npos) {
        return "duplicate-key";
    }
    if (err.find("trailing characters") != std::string_view::npos) {
        return "trailing";
    }
    if (err.find("leading zero") != std::string_view::npos) {
        return "leading-zero";
    }
    if (err.find("continuation") != std::string_view::npos) {
        return "float-exp";
    }
    if (err.find("unsupported escape") != std::string_view::npos) {
        return "escape";
    }
    if (err.find("control character") != std::string_view::npos) {
        return "control-char";
    }
    if (err.find("unterminated string") != std::string_view::npos) {
        return "unterminated";
    }
    if (err.find("unexpected end") != std::string_view::npos) {
        return "truncated";
    }
    if (err.find("expected 'true' or 'false'") != std::string_view::npos) {
        return "literal";
    }
    if (err.find("expected integer") != std::string_view::npos) {
        return "integer";
    }
    if (err.find("expected string key") != std::string_view::npos) {
        return "non-string-key";
    }
    if (err.find("expected ',' or") != std::string_view::npos) {
        return "separator";
    }
    if (err.find("unexpected character") != std::string_view::npos) {
        return "unexpected-char";
    }
    return "syntax";
}

}  // namespace

int main() {
    std::string input((std::istreambuf_iterator<char>(std::cin)),
                      std::istreambuf_iterator<char>());
    ashfall::JsonValue root;
    ashfall::JsonParser parser(std::string_view(input.data(), input.size()));
    if (parser.run(root)) {
        std::cout << "ACCEPT\n";
        return 0;
    }
    const std::string& err = parser.error();
    std::cout << "REJECT " << classify(err) << " " << err << "\n";
    return 0;  // exit 0 even on REJECT: rejection is data, not failure.
}
