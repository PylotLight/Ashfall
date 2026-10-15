// F1 byte-fuzzer: raw input bytes -> hardened JSON subset parser.
//
// Contract (Group 6 plan, Task F1): every input must be handled with no crash
// or hang. The fuzzer uses the DEFAULT JsonLimits and must never bypass them:
// a libFuzzer timeout against these capped paths is a real finding (triage to
// F4), not fuzzer noise.
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ashfall/json_subset.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // size == 0 implies data may be null; use an empty view in that case.
    const std::string_view text = size == 0 ? std::string_view{}
                                            : std::string_view{reinterpret_cast<const char*>(data),
                                                               size};
    ashfall::JsonParser parser(text);
    ashfall::JsonValue root;
    (void)parser.run(root);
    return 0;
}
