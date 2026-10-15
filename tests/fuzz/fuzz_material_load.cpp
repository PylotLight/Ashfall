// F1 byte-fuzzer: raw bytes -> parse_material_data -> validate_material_table.
//
// Contract (Group 6 plan, Task F1): no crash on any input; plus a spot-check
// of the transactional output property (P6 core): a failed parse must leave
// `out` unchanged. A violation aborts (a libFuzzer finding for F4 triage).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "ashfall/material_data.hpp"

namespace {

bool same_spec(const ashfall::MaterialSpec& a, const ashfall::MaterialSpec& b) {
    return a.id == b.id && a.name == b.name && a.movement == b.movement && a.solid == b.solid &&
           a.density == b.density && a.color == b.color && a.tags == b.tags &&
           a.hardness == b.hardness;
}

bool same_table(const ashfall::MaterialTable& a, const ashfall::MaterialTable& b) {
    if (a.version != b.version || a.materials.size() != b.materials.size()) return false;
    for (std::size_t i = 0; i < a.materials.size(); ++i) {
        if (!same_spec(a.materials[i], b.materials[i])) return false;
    }
    return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view text = size == 0 ? std::string_view{}
                                            : std::string_view{reinterpret_cast<const char*>(data),
                                                               size};
    // Sentinel output: parse_material_data commits `out` only on success, so
    // a failed parse must leave it bit-for-bit unchanged.
    const ashfall::MaterialTable sentinel = ashfall::default_material_table();
    ashfall::MaterialTable out = sentinel;
    std::string error;
    if (!ashfall::parse_material_data(text, out, error)) {
        if (!same_table(out, sentinel)) {
            std::fprintf(stderr, "fuzz_material_load: parse failed but `out` was mutated\n");
            std::abort();
        }
        return 0;
    }
    std::string validation_error;
    (void)ashfall::validate_material_table(out, validation_error);
    return 0;
}
