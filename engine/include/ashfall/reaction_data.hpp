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
//
// Error contract: syntax failures are the parser's "line L col C: message";
// every structural failure carries the rule path (`reactions[i] "<name>": ...`
// once the name is known, `reactions[i]: ...` before that, and slot paths
// such as `reactions[i].reactants[j]`). Unknown keys are rejected at every
// level. At most 4096 reactions. Reactant material refs, tag refs, and
// product refs must match `[a-z][a-z0-9_-]*` (hyphens admitted for shipped
// names such as "heat-ignition"; reaction names are checked too).
//
// p=0 rules are ACCEPTED (they are legitimate priority-suppressors: `roll < 0`
// never fires, but a full match still owns the tick). When `warnings` is
// non-null, each p=0 rule appends exactly:
//   reactions[i] "<name>": p=0 never fires but suppresses lower rules on full match
// `warnings` is cleared on entry (when non-null), like `error`.
//
// Narrow no-spontaneous-creation guarantee: this stage enforces that an
// explicit empty ("air") neighbor slot may not become occupied, and that a
// primary reactant may not be concrete "air" (the executor iterates occupied
// cells only, so such a rule could never fire). Tag-to-material resolution is
// NOT checked here — see validate_reaction_links. This is not general mass
// conservation: transformations may change density and rule overlap affects
// consumption.
bool validate_reaction_table(const ReactionTable& table, std::string& error,
                             std::vector<std::string>* warnings = nullptr);

// Link stage: validate a structurally valid table against the loaded material
// vocabulary. `material_names` must list every material name INCLUDING "air";
// `material_tags` is the union of all material tag lists. Rejects:
//   - unknown material names: reactions[i] "<name>": reactants[j]: unknown material "x"
//     (same shape for products[j]);
//   - dead tags: reactions[i] "<name>": reactants[j]: tag "x" matches no material;
//   - primary-"air" rules (re-checked here for direct callers; the structural
//     stage already rejects them).
// Narrow claim (enforced here + structurally, nothing more): an explicit
// empty neighbor slot may not be directly replaced with occupied material; a
// tag neighbor always requires an occupied cell, so tags cannot match empty
// air and no air-creation bypass exists through them. NOT claimed: mass
// conservation across transformations.
// The simulation's load_reactions() owns calling this after structural
// validation (it already rejects unknown materials silently; wiring this in
// gives the rule+field context and the tag/primary-air rejects).
bool validate_reaction_links(const ReactionTable& table,
                             const std::vector<std::string>& material_names,
                             const std::vector<std::string>& material_tags, std::string& error);

} // namespace ashfall
