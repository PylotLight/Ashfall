#include "ashfall/reaction_data.hpp"

#include "ashfall/json_subset.hpp"

#include <string>
#include <utility>

namespace ashfall {

bool validate_reaction_table(const ReactionTable& table, std::string& error) {
    if (table.version != 1) {
        error = "unsupported reaction data version (want 1)";
        return false;
    }
    if (table.reactions.empty()) {
        error = "\"reactions\" must be a non-empty array";
        return false;
    }
    for (const ReactionSpec& reaction : table.reactions) {
        if (reaction.name.empty()) {
            error = "reaction \"name\" must be a non-empty string";
            return false;
        }
        for (const ReactionSpec& other : table.reactions) {
            if (&other != &reaction && other.name == reaction.name) {
                error = "duplicate reaction name \"" + reaction.name + "\"";
                return false;
            }
        }
        if (reaction.probability < 0 || reaction.probability > 100) {
            error = "reaction \"" + reaction.name + "\" probability must be 0..100";
            return false;
        }
        if (reaction.reactants.empty() || reaction.reactants.size() > 2) {
            error = "reaction \"" + reaction.name + "\" needs 1..2 reactants";
            return false;
        }
        if (reaction.products.empty() || reaction.products.size() > reaction.reactants.size()) {
            error = "reaction \"" + reaction.name +
                    "\" needs 1 product per reactant slot (missing tail = identity)";
            return false;
        }
        for (const ReactantSpec& reactant : reaction.reactants) {
            if (reactant.name.empty()) {
                error =
                    "reaction \"" + reaction.name + "\" reactant must name a material or tag";
                return false;
            }
        }
        for (const ProductSpec& product : reaction.products) {
            if (product.material.empty()) {
                error = "reaction \"" + reaction.name + "\" product must name a material";
                return false;
            }
        }
        // No spontaneous creation: an air (empty) neighbor slot can only
        // stay air.
        if (reaction.reactants.size() == 2 && !reaction.reactants[1].is_tag &&
            reaction.reactants[1].name == "air" && reaction.products.size() == 2 &&
            reaction.products[1].material != "air") {
            error = "reaction \"" + reaction.name + "\" cannot create matter from air";
            return false;
        }
        if (reaction.gate.has_min && reaction.gate.has_max &&
            reaction.gate.min_value > reaction.gate.max_value) {
            error = "reaction \"" + reaction.name + "\" temperature min exceeds max";
            return false;
        }
        const auto in_temp_range = [](int value) { return value >= -32767 && value <= 32767; };
        if ((reaction.gate.has_min && !in_temp_range(reaction.gate.min_value)) ||
            (reaction.gate.has_max && !in_temp_range(reaction.gate.max_value)) ||
            (reaction.has_product_temperature && !in_temp_range(reaction.product_temperature))) {
            error = "reaction \"" + reaction.name + "\" temperature out of int16 range";
            return false;
        }
    }
    return true;
}

bool parse_reaction_data(std::string_view text, ReactionTable& out, std::string& error) {
    out = ReactionTable{};
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
    const JsonValue* reactions = find_json_field(root, "reactions");
    if (reactions == nullptr || reactions->type != JsonValue::Type::Array ||
        reactions->items.empty()) {
        error = "\"reactions\" must be a non-empty array";
        return false;
    }
    ReactionTable table;
    table.version = 1;
    for (const JsonValue& entry : reactions->items) {
        if (entry.type != JsonValue::Type::Object) {
            error = "reaction entry must be an object";
            return false;
        }
        ReactionSpec spec;
        if (!require_json_string(entry, "name", spec.name, error) || spec.name.empty()) {
            error = "reaction \"name\" must be a non-empty string";
            return false;
        }
        long long probability = 0;
        if (!require_json_int(entry, "probability", 0, 100, probability, error)) return false;
        spec.probability = static_cast<int>(probability);
        const JsonValue* reactants = find_json_field(entry, "reactants");
        if (reactants == nullptr || reactants->type != JsonValue::Type::Array ||
            reactants->items.empty() || reactants->items.size() > 2) {
            error = "reaction \"" + spec.name + "\" needs a reactants array of 1..2 entries";
            return false;
        }
        for (const JsonValue& slot : reactants->items) {
            if (slot.type != JsonValue::Type::Object) {
                error = "reaction \"" + spec.name + "\" reactant must be an object";
                return false;
            }
            const JsonValue* material = find_json_field(slot, "material");
            const JsonValue* tag = find_json_field(slot, "tag");
            const bool has_material = material != nullptr &&
                                      material->type == JsonValue::Type::String &&
                                      !material->str.empty();
            const bool has_tag = tag != nullptr && tag->type == JsonValue::Type::String &&
                                 !tag->str.empty();
            if (has_material == has_tag) {
                error = "reaction \"" + spec.name +
                        "\" reactant needs exactly one of \"material\"/\"tag\"";
                return false;
            }
            ReactantSpec reactant;
            reactant.is_tag = has_tag;
            reactant.name = has_tag ? tag->str : material->str;
            spec.reactants.push_back(std::move(reactant));
        }
        const JsonValue* products = find_json_field(entry, "products");
        if (products == nullptr || products->type != JsonValue::Type::Array ||
            products->items.empty() || products->items.size() > spec.reactants.size()) {
            error = "reaction \"" + spec.name +
                    "\" needs 1 product per reactant slot (missing tail = identity)";
            return false;
        }
        for (const JsonValue& slot : products->items) {
            if (slot.type != JsonValue::Type::Object) {
                error = "reaction \"" + spec.name + "\" product must be an object";
                return false;
            }
            ProductSpec product;
            if (!require_json_string(slot, "material", product.material, error) ||
                product.material.empty()) {
                error = "reaction \"" + spec.name + "\" product needs a non-empty \"material\"";
                return false;
            }
            spec.products.push_back(std::move(product));
        }
        const JsonValue* temperature = find_json_field(entry, "temperature");
        if (temperature != nullptr) {
            if (temperature->type != JsonValue::Type::Object) {
                error = "reaction \"" + spec.name + "\" temperature must be an object";
                return false;
            }
            if (find_json_field(*temperature, "min_temperature") != nullptr) {
                long long value = 0;
                if (!require_json_int(
                        *temperature, "min_temperature", -32767, 32767, value, error))
                    return false;
                spec.gate.has_min = true;
                spec.gate.min_value = static_cast<int>(value);
            }
            if (find_json_field(*temperature, "max_temperature") != nullptr) {
                long long value = 0;
                if (!require_json_int(
                        *temperature, "max_temperature", -32767, 32767, value, error))
                    return false;
                spec.gate.has_max = true;
                spec.gate.max_value = static_cast<int>(value);
            }
        }
        if (find_json_field(entry, "product_temperature") != nullptr) {
            long long value = 0;
            if (!require_json_int(entry, "product_temperature", -32767, 32767, value, error))
                return false;
            spec.has_product_temperature = true;
            spec.product_temperature = static_cast<int>(value);
        }
        table.reactions.push_back(std::move(spec));
    }
    if (!validate_reaction_table(table, error)) return false;
    out = std::move(table);
    return true;
}

} // namespace ashfall
