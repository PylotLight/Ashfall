// Task F2: structured validator fuzzing for ReactionTable.
// Fills the struct from byte slices (slot counts, probability, temperature
// gate, and tag/material refs forged structurally) and runs
// validate_reaction_table, then validate_reaction_links against the
// default_material_table() vocabulary when structural validation passes
// (the link stage assumes structural validity, matching load_reactions()).
// Same bar: no crash, hang, or escaping exception. -max_len=512.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ashfall/material_data.hpp"
#include "ashfall/reaction_data.hpp"

#include "structured_fill.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    ashfall::SliceReader reader{data, size};
    ashfall::ReactionTable table;
    ashfall::fill_reaction_table(reader, table);
    std::string error;
    if (ashfall::validate_reaction_table(table, error)) {
        const ashfall::MaterialTable vocab = ashfall::default_material_table();
        std::vector<std::string> names;
        std::vector<std::string> tags;
        names.reserve(vocab.materials.size());
        for (const ashfall::MaterialSpec& spec : vocab.materials) {
            names.push_back(spec.name);
            for (const std::string& tag : spec.tags) {
                tags.push_back(tag);
            }
        }
        std::string link_error;
        (void)ashfall::validate_reaction_links(table, names, tags, link_error);
    }
    return 0;
}
