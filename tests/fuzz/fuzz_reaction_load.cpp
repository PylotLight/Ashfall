// F1 byte-fuzzer: raw bytes -> parse -> validate -> validate_reaction_links.
//
// Contract (Group 6 plan, Task F1): no crash on any input. The link stage runs
// against the DEFAULT table vocabulary (material names incl. "air" plus the
// tag union from default_material_table()): deterministic, with no simulation
// anywhere inside the fuzzer (reviewer constraint).
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ashfall/material_data.hpp"
#include "ashfall/reaction_data.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view text = size == 0 ? std::string_view{}
                                            : std::string_view{reinterpret_cast<const char*>(data),
                                                               size};
    ashfall::ReactionTable table;
    std::string error;
    if (!ashfall::parse_reaction_data(text, table, error)) return 0;
    std::string validation_error;
    if (!ashfall::validate_reaction_table(table, validation_error)) return 0;
    const ashfall::MaterialTable defaults = ashfall::default_material_table();
    std::vector<std::string> material_names;
    std::vector<std::string> material_tags;
    material_names.reserve(defaults.materials.size());
    for (const ashfall::MaterialSpec& spec : defaults.materials) {
        material_names.push_back(spec.name);
        for (const std::string& tag : spec.tags) {
            bool known = false;
            for (const std::string& have : material_tags) {
                if (have == tag) {
                    known = true;
                    break;
                }
            }
            if (!known) material_tags.push_back(tag);
        }
    }
    std::string link_error;
    (void)ashfall::validate_reaction_links(table, material_names, material_tags, link_error);
    return 0;
}
