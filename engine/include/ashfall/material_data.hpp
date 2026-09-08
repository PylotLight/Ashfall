#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ashfall/simulation.hpp"

namespace ashfall {

// One row of the versioned material table.
struct MaterialSpec {
    MaterialId id = 0;
    std::string name;
    Movement movement = Movement::Static;
    bool solid = false;
    int density = 0;
    std::array<std::uint8_t, 3> color = {0, 0, 0};
};

// A validated, load-ready material table.
struct MaterialTable {
    int version = 0;
    std::vector<MaterialSpec> materials;
};

// "static" | "powder" | "liquid". Returns false for anything else
// ("gas" arrives in P2.5 with gas movement).
bool movement_from_string(std::string_view name, Movement& out);

// Strict JSON subset (objects, arrays, strings, integers, true/false).
// On success fills `out` and returns true. On failure returns false with
// `error` set to "line L col C: message" (first error wins).
bool parse_material_data(std::string_view text, MaterialTable& out, std::string& error);

} // namespace ashfall
