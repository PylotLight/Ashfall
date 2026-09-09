#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ashfall {

// One neighbor slot of a reaction rule. Exactly one of the two match modes
// is used: a concrete material name, or an open tag matched against the
// material's "tags" list. R2 naming "air" matches an EMPTY neighbor cell.
struct ReactantSpec {
    bool is_tag = false;
    std::string name;
};

// One product slot, positional with reactants (P1 replaces R1's cell, P2
// replaces R2's cell). "air" deletes the cell. When products are fewer than
// reactants, the missing tail slots keep their material (identity).
struct ProductSpec {
    std::string material;
};

// Optional temperature gate, checked against the primary (R1) cell.
struct TemperatureGate {
    bool has_min = false;
    int min_value = 0;
    bool has_max = false;
    int max_value = 0;
};

struct ReactionSpec {
    std::string name;
    int probability = 0; // 0..100, rolled per primary cell per tick
    std::vector<ReactantSpec> reactants; // 1..2; R1 = primary cell, R2 = neighbor
    std::vector<ProductSpec> products;   // 1..reactants.size()
    TemperatureGate gate;
    bool has_product_temperature = false;
    int product_temperature = 0; // set on transformed cells when present
};

struct ReactionTable {
    int version = 0;
    std::vector<ReactionSpec> reactions; // data order = conflict priority
};

bool parse_reaction_data(std::string_view text, ReactionTable& out, std::string& error);
// Structural validation for programmatically built tables (no material
// resolution — SimulationWorld::load_reactions resolves names).
bool validate_reaction_table(const ReactionTable& table, std::string& error);

} // namespace ashfall
