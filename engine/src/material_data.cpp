#include "ashfall/material_data.hpp"
#include "ashfall/json_subset.hpp"

#include <string>
#include <utility>

namespace ashfall {

bool movement_from_string(std::string_view name, Movement& out) {
    if (name == "static") {
        out = Movement::Static;
        return true;
    }
    if (name == "powder") {
        out = Movement::Powder;
        return true;
    }
    if (name == "liquid") {
        out = Movement::Liquid;
        return true;
    }
    if (name == "gas") {
        out = Movement::Gas;
        return true;
    }
    return false;
}

bool validate_material_table(const MaterialTable& table, std::string& error) {
    if (table.version != 1) {
        error = "unsupported data version (want 1)";
        return false;
    }
    if (table.materials.empty()) {
        error = "\"materials\" must be a non-empty array";
        return false;
    }
    bool seen_id[256] = {};
    bool has_air = false;
    for (const MaterialSpec& spec : table.materials) {
        if (seen_id[spec.id]) {
            error = "duplicate material id";
            return false;
        }
        seen_id[spec.id] = true;
        for (const MaterialSpec& other : table.materials) {
            if (&other != &spec && other.name == spec.name) {
                error = "duplicate material name \"" + spec.name + "\"";
                return false;
            }
        }
        if (spec.name.empty()) {
            error = "material \"name\" must be a non-empty string";
            return false;
        }
        if (spec.solid && spec.movement != Movement::Static) {
            error = "material \"" + spec.name + "\" is solid but not static";
            return false;
        }
        if (spec.movement == Movement::Gas && spec.solid) {
            error = "material \"" + spec.name + "\" is gas but solid";
            return false;
        }
        if (spec.hardness < 0 || spec.hardness > 6) {
            error = "material \"" + spec.name + "\" hardness must be 0..6";
            return false;
        }
        for (std::size_t t = 0; t < spec.tags.size(); ++t) {
            if (spec.tags[t].empty()) {
                error = "material \"" + spec.name + "\" has an empty tag";
                return false;
            }
            for (std::size_t u = t + 1; u < spec.tags.size(); ++u) {
                if (spec.tags[u] == spec.tags[t]) {
                    error = "material \"" + spec.name + "\" has a duplicate tag";
                    return false;
                }
            }
        }
        if (spec.id == 0) {
            has_air = true;
            if (spec.name != "air" || spec.movement != Movement::Static || spec.solid || spec.density != 0) {
                error = "material id 0 must be non-solid static air with density 0";
                return false;
            }
        }
    }
    if (!has_air) {
        error = "material id 0 (air) is required";
        return false;
    }
    return true;
}

MaterialTable default_material_table() {
    // Densities follow docs/NOITA_MATERIALS.md §2 (water = 100 baseline):
    // oil 40 floats, lava 140 and blood 110 sink. Sand (160) still sinks
    // through water (100); static anchors sit at 2000, above every dynamic.
    MaterialTable table;
    table.version = 1;
    table.materials = {
        {kAir, "air", Movement::Static, false, 0, {18, 20, 24}, {}, 0},
        {kStone, "stone", Movement::Static, true, 2000, {128, 133, 138}, {"static"}, 3},
        {kSand, "sand", Movement::Powder, false, 160, {230, 175, 50}, {"powder"}, 0},
        {kWater, "water", Movement::Liquid, false, 100, {86, 178, 214}, {"liquid"}, 0},
        {kSteam, "steam", Movement::Gas, false, 1, {205, 205, 210}, {"vapour"}, 0},
        {kFire, "fire", Movement::Gas, false, 1, {255, 180, 40}, {"fire"}, 0},
        {kWood, "wood", Movement::Static, true, 2000, {146, 104, 58}, {"burnable", "static"}, 2},
        {kLava, "lava", Movement::Liquid, false, 140, {255, 80, 10}, {"liquid"}, 0},
        {kRock, "rock", Movement::Static, true, 2000, {120, 115, 110}, {"static"}, 3},
    };
    return table;
}

bool parse_material_data(std::string_view text, MaterialTable& out, std::string& error) {
    out = MaterialTable{};
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
        error = "unsupported data version (want 1)";
        return false;
    }
    const JsonValue* materials = find_json_field(root, "materials");
    if (materials == nullptr || materials->type != JsonValue::Type::Array || materials->items.empty()) {
        error = "\"materials\" must be a non-empty array";
        return false;
    }
    bool seen_id[256] = {};
    std::vector<std::string> seen_names;
    MaterialTable table;
    table.version = 1;
    for (const JsonValue& entry : materials->items) {
        if (entry.type != JsonValue::Type::Object) {
            error = "material entry must be an object";
            return false;
        }
        long long id = 0;
        if (!require_json_int(entry, "id", 0, 255, id, error)) return false;
        if (seen_id[static_cast<std::size_t>(id)]) {
            error = "duplicate material id";
            return false;
        }
        std::string name;
        if (!require_json_string(entry, "name", name, error) || name.empty()) {
            error = "material \"name\" must be a non-empty string";
            return false;
        }
        for (const std::string& taken : seen_names) {
            if (taken == name) {
                error = "duplicate material name \"" + name + "\"";
                return false;
            }
        }
        std::string movement_name;
        if (!require_json_string(entry, "movement", movement_name, error)) return false;
        Movement movement = Movement::Static;
        if (!movement_from_string(movement_name, movement)) {
            error = "unknown movement \"" + movement_name + "\" (want static/powder/liquid/gas)";
            return false;
        }
        bool solid = false;
        if (!require_json_bool(entry, "solid", solid, error)) return false;
        if (solid && movement != Movement::Static) {
            error = "material \"" + name + "\" is solid but not static";
            return false;
        }
        long long density = 0;
        if (!require_json_int(entry, "density", 0, 100000, density, error)) return false;
        const JsonValue* color = find_json_field(entry, "color");
        if (color == nullptr || color->type != JsonValue::Type::Array || color->items.size() != 3) {
            error = "material \"color\" must be an array of 3 integers";
            return false;
        }
        std::array<std::uint8_t, 3> rgb = {0, 0, 0};
        for (std::size_t c = 0; c < 3; ++c) {
            if (color->items[c].type != JsonValue::Type::Int || color->items[c].integer < 0 ||
                color->items[c].integer > 255) {
                error = "material \"color\" channels must be integers 0..255";
                return false;
            }
            rgb[c] = static_cast<std::uint8_t>(color->items[c].integer);
        }
        MaterialSpec spec;
        spec.id = static_cast<MaterialId>(id);
        spec.name = name;
        spec.movement = movement;
        spec.solid = solid;
        spec.density = static_cast<int>(density);
        spec.color = rgb;
        const JsonValue* tags = find_json_field(entry, "tags");
        if (tags != nullptr) {
            if (tags->type != JsonValue::Type::Array) {
                error = "material \"tags\" must be an array of strings";
                return false;
            }
            for (const JsonValue& tag : tags->items) {
                if (tag.type != JsonValue::Type::String || tag.str.empty()) {
                    error = "material \"tags\" must be non-empty strings";
                    return false;
                }
                spec.tags.push_back(tag.str);
            }
        }
        const JsonValue* hardness_field = find_json_field(entry, "hardness");
        if (hardness_field != nullptr) {
            long long hardness = 0;
            if (!require_json_int(entry, "hardness", 0, 6, hardness, error)) return false;
            spec.hardness = static_cast<int>(hardness);
        }
        table.materials.push_back(std::move(spec));
        seen_id[static_cast<std::size_t>(id)] = true;
        seen_names.push_back(name);
    }
    if (!validate_material_table(table, error)) return false;
    out = std::move(table);
    return true;
}

} // namespace ashfall
