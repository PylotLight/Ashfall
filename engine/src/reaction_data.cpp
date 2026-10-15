#include "ashfall/reaction_data.hpp"

#include "ashfall/json_subset.hpp"

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ashfall {

namespace {

constexpr std::string_view kIdentifierWant = "[a-z][a-z0-9_-]*";

bool reject_unknown_keys(const JsonValue& obj, std::string_view path,
                         std::initializer_list<std::string_view> allowed, std::string& error) {
    for (const auto& field : obj.fields) {
        bool known = false;
        for (std::string_view want : allowed) {
            if (field.first == want) {
                known = true;
                break;
            }
        }
        if (!known) {
            error = std::string(path) + ": unknown key \"" + field.first + "\"";
            return false;
        }
    }
    return true;
}

std::string identifier_error(std::string_view path, std::string_view value) {
    return std::string(path) + ": \"" + std::string(value) + "\" is not a valid identifier (want " +
           std::string(kIdentifierWant) + ")";
}

} // namespace

bool validate_reaction_table(const ReactionTable& table, std::string& error,
                             std::vector<std::string>* warnings) {
    error.clear();
    if (warnings != nullptr) warnings->clear();
    if (table.version != 1) {
        error = "unsupported reaction data version (want 1)";
        return false;
    }
    if (table.reactions.empty()) {
        error = "\"reactions\" must be a non-empty array";
        return false;
    }
    // Group 3 (J1): reaction-count cap on the programmatic route too (the
    // JSON route enforces it before row decoding; see parse_reaction_data).
    if (table.reactions.size() > kDefaultJsonLimits.max_reactions) {
        error = "limit: reaction count exceeds " +
                std::to_string(kDefaultJsonLimits.max_reactions) + " entries";
        return false;
    }
    for (std::size_t index = 0; index < table.reactions.size(); ++index) {
        const ReactionSpec& reaction = table.reactions[index];
        const std::string at = "reactions[" + std::to_string(index) + "]";
        if (reaction.name.empty()) {
            error = at + ": reaction \"name\" must be a non-empty string";
            return false;
        }
        for (std::size_t other = 0; other < table.reactions.size(); ++other) {
            if (other != index && table.reactions[other].name == reaction.name) {
                error = at + ": duplicate reaction name \"" + reaction.name + "\"";
                return false;
            }
        }
        const std::string rule = at + " \"" + reaction.name + "\"";
        if (reaction.probability < 0 || reaction.probability > 100) {
            error = rule + ": probability must be 0..100";
            return false;
        }
        if (reaction.reactants.empty() || reaction.reactants.size() > 2) {
            error = rule + ": needs 1..2 reactants";
            return false;
        }
        if (reaction.products.empty() || reaction.products.size() > reaction.reactants.size()) {
            error = rule + ": needs 1 product per reactant slot (missing tail = identity)";
            return false;
        }
        for (std::size_t s = 0; s < reaction.reactants.size(); ++s) {
            const ReactantSpec& reactant = reaction.reactants[s];
            const std::string slot =
                at + ".reactants[" + std::to_string(s) + "]";
            if (reactant.name.empty()) {
                error = rule + ": reactant must name a material or tag";
                return false;
            }
            // Group 3 (J6): Choice A identifiers for both match modes.
            if (!is_valid_identifier(reactant.name)) {
                error = identifier_error(slot, reactant.name);
                return false;
            }
        }
        for (std::size_t s = 0; s < reaction.products.size(); ++s) {
            const ProductSpec& product = reaction.products[s];
            const std::string slot = at + ".products[" + std::to_string(s) + "]";
            if (product.material.empty()) {
                error = rule + ": product must name a material";
                return false;
            }
            if (!is_valid_identifier(product.material)) {
                error = identifier_error(slot, product.material);
                return false;
            }
        }
        // Group 3 (J5): primary concrete "air" can never fire (the executor
        // iterates occupied cells only). A primary TAG is fine — tags only
        // match occupied cells. Re-checked at link time for direct callers.
        if (!reaction.reactants[0].is_tag && reaction.reactants[0].name == "air") {
            error = rule + ": primary reactant cannot be \"air\" (executor visits occupied cells only)";
            return false;
        }
        // Narrow no-spontaneous-creation guarantee (P8): an explicit empty
        // neighbor slot may not be directly replaced with occupied material
        // by this rule format. This is NOT mass conservation (density may
        // change; overlap affects consumption) — exactly this slot rule.
        if (reaction.reactants.size() == 2 && !reaction.reactants[1].is_tag &&
            reaction.reactants[1].name == "air" && reaction.products.size() == 2 &&
            reaction.products[1].material != "air") {
            error = rule + ": air reactant cannot yield occupied product";
            return false;
        }
        if (reaction.gate.has_min && reaction.gate.has_max &&
            reaction.gate.min_value > reaction.gate.max_value) {
            error = rule + ": temperature min exceeds max";
            return false;
        }
        const auto in_temp_range = [](int value) { return value >= -32767 && value <= 32767; };
        if ((reaction.gate.has_min && !in_temp_range(reaction.gate.min_value)) ||
            (reaction.gate.has_max && !in_temp_range(reaction.gate.max_value)) ||
            (reaction.has_product_temperature && !in_temp_range(reaction.product_temperature))) {
            error = rule + ": temperature out of int16 range";
            return false;
        }
        // Group 3 (J5): p=0 is a legitimate priority-suppressor, never a
        // removable dead rule — warn, do not reject.
        if (reaction.probability == 0 && warnings != nullptr) {
            warnings->push_back(rule +
                                ": p=0 never fires but suppresses lower rules on full match");
        }
    }
    return true;
}

bool validate_reaction_links(const ReactionTable& table,
                             const std::vector<std::string>& material_names,
                             const std::vector<std::string>& material_tags, std::string& error) {
    error.clear();
    const auto known_name = [&](std::string_view name) {
        for (const std::string& known : material_names) {
            if (known == name) return true;
        }
        return false;
    };
    const auto known_tag = [&](std::string_view tag) {
        for (const std::string& known : material_tags) {
            if (known == tag) return true;
        }
        return false;
    };
    for (std::size_t index = 0; index < table.reactions.size(); ++index) {
        const ReactionSpec& reaction = table.reactions[index];
        const std::string at = "reactions[" + std::to_string(index) + "]";
        const std::string rule = at + " \"" + reaction.name + "\"";
        // Primary-"air" re-check for direct link callers (structural stage
        // rejects it first in the normal parse -> structure -> link flow).
        if (!reaction.reactants.empty() && !reaction.reactants[0].is_tag &&
            reaction.reactants[0].name == "air") {
            error = rule + ": primary reactant cannot be \"air\" (executor visits occupied cells only)";
            return false;
        }
        for (std::size_t s = 0; s < reaction.reactants.size(); ++s) {
            const ReactantSpec& reactant = reaction.reactants[s];
            const std::string slot = "reactants[" + std::to_string(s) + "]";
            if (reactant.is_tag) {
                // Group 3 (J5/P8): a tag matching nothing is a config bug
                // (dead rule arm), not a wildcard — reject. Tag matching
                // requires an occupied cell, so tags can never match empty
                // air: no air-creation bypass exists through this arm.
                if (!known_tag(reactant.name)) {
                    error = rule + ": " + slot + ": tag \"" + reactant.name +
                            "\" matches no material";
                    return false;
                }
            } else if (!known_name(reactant.name)) {
                error =
                    rule + ": " + slot + ": unknown material \"" + reactant.name + "\"";
                return false;
            }
        }
        for (std::size_t s = 0; s < reaction.products.size(); ++s) {
            const std::string& material = reaction.products[s].material;
            const std::string slot = "products[" + std::to_string(s) + "]";
            if (!known_name(material)) {
                error = rule + ": " + slot + ": unknown material \"" + material + "\"";
                return false;
            }
        }
    }
    return true;
}

bool parse_reaction_data(std::string_view text, ReactionTable& out, std::string& error) {
    // BEHAVIOR(v2-loader): transactional output (P6 core). Old behavior
    // cleared `out` up front, so a failed parse destroyed the caller's prior
    // table; now `out` is committed only on success. Pinned by Task T V-battery
    // (failed reload preserves cells+heat+reactions+tick).
    error.clear();
    JsonParser parser(text);
    JsonValue root;
    if (!parser.run(root)) {
        error = parser.error();
        return false;
    }
    if (root.type != JsonValue::Type::Object) {
        error = "top-level value must be an object";
        return false;
    }
    const JsonValue* version = find_json_field(root, "version");
    if (version == nullptr || version->type != JsonValue::Type::Int || version->integer != 1) {
        error = "unsupported reaction data version (want 1)";
        return false;
    }
    // Group 3 (J3): strict allowed keys at the document root (`metadata` and
    // any other extension key reject — no extensions exist today).
    if (!reject_unknown_keys(root, "(root)", {"version", "reactions"}, error)) return false;
    const JsonValue* reactions = find_json_field(root, "reactions");
    if (reactions == nullptr || reactions->type != JsonValue::Type::Array ||
        reactions->items.empty()) {
        error = "\"reactions\" must be a non-empty array";
        return false;
    }
    // Group 3 (J1): schema-level reaction cap, checked before row decoding
    // allocates any spec.
    if (reactions->items.size() > kDefaultJsonLimits.max_reactions) {
        error = "limit: reactions array exceeds " +
                std::to_string(kDefaultJsonLimits.max_reactions) + " entries";
        return false;
    }
    ReactionTable table;
    table.version = 1;
    for (std::size_t index = 0; index < reactions->items.size(); ++index) {
        const JsonValue& entry = reactions->items[index];
        const std::string at = "reactions[" + std::to_string(index) + "]";
        if (entry.type != JsonValue::Type::Object) {
            error = at + ": reaction entry must be an object";
            return false;
        }
        // Group 3 (J3): allowed reaction keys (misspelled optionals such as
        // "product_temperatur" reject instead of silently disabling output).
        if (!reject_unknown_keys(entry, at,
                                 {"name", "probability", "reactants", "products", "temperature",
                                  "product_temperature"},
                                 error)) {
            return false;
        }
        ReactionSpec spec;
        if (!require_json_string(entry, "name", spec.name, error) || spec.name.empty()) {
            error = at + ".name: reaction \"name\" must be a non-empty string";
            return false;
        }
        // Group 3 (J6): reaction names share the identifier policy
        // (`[a-z][a-z0-9_-]*` admits shipped hyphenated names like
        // "oil-flash"), so they are checked like every other name.
        if (!is_valid_identifier(spec.name)) {
            error = identifier_error(at + ".name", spec.name);
            return false;
        }
        long long probability = 0;
        if (!require_json_int(entry, "probability", 0, 100, probability, error)) {
            error = at + ".probability: " + error;
            return false;
        }
        spec.probability = static_cast<int>(probability);
        const JsonValue* reactants = find_json_field(entry, "reactants");
        if (reactants == nullptr || reactants->type != JsonValue::Type::Array ||
            reactants->items.empty() || reactants->items.size() > 2) {
            error = at + ".reactants: need an array of 1..2 reactant slots";
            return false;
        }
        for (std::size_t s = 0; s < reactants->items.size(); ++s) {
            const JsonValue& slot = reactants->items[s];
            const std::string slot_path =
                at + ".reactants[" + std::to_string(s) + "]";
            if (slot.type != JsonValue::Type::Object) {
                error = slot_path + ": reactant must be an object";
                return false;
            }
            // BEHAVIOR(v2-loader): presence-based reactant exclusivity (P3).
            // Old behavior tested value validity, so {"material":"", "tag":"x"}
            // or {"material":5, "tag":"x"} silently accepted the tag arm and
            // unknown slot keys (e.g. "materail") were ignored. Now exactly
            // one of "material"/"tag" may be PRESENT and no other key may
            // appear. Pinned by Task T V-battery (dual-key both-valid reject).
            if (!reject_unknown_keys(slot, slot_path, {"material", "tag"}, error)) return false;
            const bool has_material = find_json_field(slot, "material") != nullptr;
            const bool has_tag = find_json_field(slot, "tag") != nullptr;
            if (has_material == has_tag) {
                error = slot_path + ": specify exactly one of \"material\" or \"tag\"";
                return false;
            }
            const JsonValue* material = find_json_field(slot, "material");
            const JsonValue* tag = find_json_field(slot, "tag");
            const bool material_ok = material != nullptr &&
                                     material->type == JsonValue::Type::String &&
                                     !material->str.empty();
            const bool tag_ok = tag != nullptr && tag->type == JsonValue::Type::String &&
                                !tag->str.empty();
            if ((has_material && !material_ok) || (has_tag && !tag_ok)) {
                error = slot_path + ": specify exactly one of \"material\" or \"tag\"";
                return false;
            }
            const std::string& ref = has_tag ? tag->str : material->str;
            // Group 3 (J6): Choice A identifiers for material/tag references.
            if (!is_valid_identifier(ref)) {
                error = identifier_error(slot_path, ref);
                return false;
            }
            ReactantSpec reactant;
            reactant.is_tag = has_tag;
            reactant.name = ref;
            spec.reactants.push_back(std::move(reactant));
        }
        const JsonValue* products = find_json_field(entry, "products");
        if (products == nullptr || products->type != JsonValue::Type::Array ||
            products->items.empty() || products->items.size() > spec.reactants.size()) {
            error = at + ".products: need 1 product per reactant slot (missing tail = identity)";
            return false;
        }
        for (std::size_t s = 0; s < products->items.size(); ++s) {
            const JsonValue& slot = products->items[s];
            const std::string slot_path = at + ".products[" + std::to_string(s) + "]";
            if (slot.type != JsonValue::Type::Object) {
                error = slot_path + ": product must be an object";
                return false;
            }
            // Group 3 (J3): product slots take exactly {"material"}.
            if (!reject_unknown_keys(slot, slot_path, {"material"}, error)) return false;
            ProductSpec product;
            if (!require_json_string(slot, "material", product.material, error) ||
                product.material.empty()) {
                error = slot_path + ": product needs a non-empty \"material\"";
                return false;
            }
            if (!is_valid_identifier(product.material)) {
                error = identifier_error(slot_path, product.material);
                return false;
            }
            spec.products.push_back(std::move(product));
        }
        const JsonValue* temperature = find_json_field(entry, "temperature");
        if (temperature != nullptr) {
            if (temperature->type != JsonValue::Type::Object) {
                error = at + ".temperature: temperature must be an object";
                return false;
            }
            // Group 3 (J3/owner decision): `{}` rejects — omitting the key
            // already means "no gate", so an empty object is an authoring
            // mistake, not a meaningful gate.
            if (temperature->fields.empty()) {
                error = at + ".temperature: empty temperature object is not allowed (omit "
                             "\"temperature\" for no gate)";
                return false;
            }
            if (!reject_unknown_keys(*temperature, at + ".temperature",
                                     {"min_temperature", "max_temperature"}, error)) {
                return false;
            }
            if (find_json_field(*temperature, "min_temperature") != nullptr) {
                long long value = 0;
                if (!require_json_int(
                        *temperature, "min_temperature", -32767, 32767, value, error)) {
                    error = at + ".temperature.min_temperature: " + error;
                    return false;
                }
                spec.gate.has_min = true;
                spec.gate.min_value = static_cast<int>(value);
            }
            if (find_json_field(*temperature, "max_temperature") != nullptr) {
                long long value = 0;
                if (!require_json_int(
                        *temperature, "max_temperature", -32767, 32767, value, error)) {
                    error = at + ".temperature.max_temperature: " + error;
                    return false;
                }
                spec.gate.has_max = true;
                spec.gate.max_value = static_cast<int>(value);
            }
        }
        if (find_json_field(entry, "product_temperature") != nullptr) {
            long long value = 0;
            if (!require_json_int(entry, "product_temperature", -32767, 32767, value, error)) {
                error = at + ".product_temperature: " + error;
                return false;
            }
            spec.has_product_temperature = true;
            spec.product_temperature = static_cast<int>(value);
        }
        table.reactions.push_back(std::move(spec));
    }
    // Structural validation closes the parse (linking against the material
    // table stays with the load path — see validate_reaction_links).
    // NOTE: parse drops p=0 warnings (no warnings out-param on this frozen
    // signature); programmatic/load-path callers use validate_reaction_table
    // directly for the warning channel.
    if (!validate_reaction_table(table, error)) return false;
    out = std::move(table);
    return true;
}

} // namespace ashfall
