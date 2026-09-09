#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ashfall/cell_types.hpp"

namespace ashfall {

// One row of the versioned material table.
struct MaterialSpec {
    MaterialId id = 0;
    std::string name;
    Movement movement = Movement::Static;
    bool solid = false;
    int density = 0;
    std::array<std::uint8_t, 3> color = {0, 0, 0};
    // Reaction vocabulary: open string set matched by reaction tag slots
    // (e.g. "burnable", "static", "vapor"). Empty by default; v1 JSON
    // without "tags" still parses (backward compatible).
    std::vector<std::string> tags{};
    // Dig tier 0..6 (Noita scale; consumed by a future digging phase).
    int hardness = 0;
};

// A validated, load-ready material table.
struct MaterialTable {
    int version = 0;
    std::vector<MaterialSpec> materials;
};

// "static" | "powder" | "liquid" | "gas". Returns false for anything else.
bool movement_from_string(std::string_view name, Movement& out);

// Structural validation for programmatically built tables (the parser calls
// this too, after syntax): non-empty, no duplicate ids/names, id 0 is
// non-solid static air with density 0, solid implies static.
bool validate_material_table(const MaterialTable& table, std::string& error);

// Code-built legacy table (air 0, stone 1, sand 2, water 3). The default
// SimulationWorld table; data/materials.json must match it field-for-field
// (locked by test_material_data_matches_canonical_json).
MaterialTable default_material_table();

// Strict JSON subset (objects, arrays, strings, integers, true/false).
// On success fills `out` and returns true. On failure returns false with
// `error` set to "line L col C: message" (first error wins).
bool parse_material_data(std::string_view text, MaterialTable& out, std::string& error);

} // namespace ashfall
