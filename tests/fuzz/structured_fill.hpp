#pragma once

// Task F2 shared helper: fills MaterialTable/ReactionTable structs from raw
// byte slices so libFuzzer can forge values the byte-grammar path cannot
// (notably Movement via static_cast<Movement>(byte), out-of-range densities,
// empty/oversize/non-identifier strings, and bad slot counts).
//
// Bounds: at most 8 materials, 4 reactions, 2 reactant/product slots per
// reaction, 2 tags per material, strings capped at 48 chars. Every read goes
// through SliceReader and is bounds-checked: on exhaustion the field keeps a
// zero default, short_fill is set, and the fill returns false (short-fill).
// Validators must still accept-or-reject the partial table without
// crashing, hanging, or throwing.

#include <cstddef>
#include <cstdint>
#include <string>

#include "ashfall/cell_types.hpp"
#include "ashfall/material_data.hpp"
#include "ashfall/reaction_data.hpp"

namespace ashfall {

// Byte -> char map for length-prefixed strings: full lowercase + digits +
// '-'/'_' (valid identifier chars) plus 'A', '!', ' ', '.' (non-identifier
// forgers: uppercase, punctuation, blank). Empty (L=0) and oversize (L>cap)
// lengths are representable.
inline constexpr char kFuzzStringAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789_-A! .";
inline constexpr std::size_t kFuzzStringAlphabetSize = sizeof(kFuzzStringAlphabet) - 1;
inline constexpr std::size_t kFuzzMaxString = 48;
inline constexpr std::size_t kFuzzMaxMaterials = 8;
inline constexpr std::size_t kFuzzMaxReactions = 4;
inline constexpr std::size_t kFuzzMaxSlots = 2;
inline constexpr std::size_t kFuzzMaxTags = 2;

struct SliceReader {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;
    bool short_fill = false;

    bool take_u8(std::uint8_t& out) {
        if (pos >= size) {
            short_fill = true;
            out = 0;
            return false;
        }
        out = data[pos++];
        return true;
    }

    bool take_u16(std::uint16_t& out) {
        std::uint8_t lo = 0;
        std::uint8_t hi = 0;
        if (!take_u8(lo) || !take_u8(hi)) {
            out = 0;
            return false;
        }
        out = static_cast<std::uint16_t>(static_cast<std::uint16_t>(lo) |
                                         (static_cast<std::uint16_t>(hi) << 8));
        return true;
    }

    bool take_u32(std::uint32_t& out) {
        std::uint8_t b0 = 0;
        std::uint8_t b1 = 0;
        std::uint8_t b2 = 0;
        std::uint8_t b3 = 0;
        if (!take_u8(b0) || !take_u8(b1) || !take_u8(b2) || !take_u8(b3)) {
            out = 0;
            return false;
        }
        out = static_cast<std::uint32_t>(b0) | (static_cast<std::uint32_t>(b1) << 8) |
              (static_cast<std::uint32_t>(b2) << 16) | (static_cast<std::uint32_t>(b3) << 24);
        return true;
    }

    // Length-prefixed (1 byte), alphabet-mapped, capped at `cap` chars.
    // A length beyond the remaining input consumes what is left and flags
    // short-fill; lengths above `cap` only consume `cap` bytes (the rest of
    // the notional string stays in the stream for the next field).
    bool take_string(std::string& out, std::size_t cap = kFuzzMaxString) {
        out.clear();
        std::uint8_t len = 0;
        if (!take_u8(len)) {
            return false;
        }
        std::size_t want = len;
        if (want > cap) {
            want = cap;
        }
        out.reserve(want);
        for (std::size_t i = 0; i < want; ++i) {
            std::uint8_t b = 0;
            if (!take_u8(b)) {
                return false;
            }
            out.push_back(kFuzzStringAlphabet[b % kFuzzStringAlphabetSize]);
        }
        return true;
    }
};

// Raw u32 -> ±1e9 density edges: raw=0 gives -1000000000, raw=2000000000
// gives +1000000000, raw=1000000000 gives 0. The valid 0..100000 band and
// both out-of-range sides are reachable.
inline int expand_fuzz_density(std::uint32_t raw) {
    return static_cast<int>(raw % 2000000001u) - 1000000000;
}

// Byte layouts (all integers little-endian):
//   MaterialTable: u16 version_raw | u8 count_raw | materials...
//     material: u8 id | string name | u8 movement_raw | u8 solid_raw |
//               u32 density_raw | u8 color[3] | u8 tagcount_raw | strings tags |
//               u16 hardness_raw
//   version = version_raw % 6 - 1 (valid 1 reachable); count = 1 + raw % 8;
//   movement = static_cast<Movement>(byte) -- THE forge point (0..255);
//   solid = byte & 1; tags = raw % 3 entries;
//   hardness = int16(raw) % 11 (-10..10: negatives and both edges reachable).
inline bool fill_material_table(SliceReader& reader, MaterialTable& table) {
    table = MaterialTable{};
    std::uint16_t version_raw = 0;
    std::uint8_t count_raw = 0;
    bool ok = reader.take_u16(version_raw);
    ok = reader.take_u8(count_raw) && ok;
    table.version = static_cast<int>(version_raw % 6) - 1;
    const std::size_t count = 1 + (static_cast<std::size_t>(count_raw) % kFuzzMaxMaterials);
    table.materials.reserve(count);
    for (std::size_t m = 0; m < count; ++m) {
        MaterialSpec spec;
        std::uint8_t id = 0;
        std::uint8_t movement_byte = 0;
        std::uint8_t solid_byte = 0;
        std::uint32_t density_raw = 0;
        std::uint8_t tagcount_raw = 0;
        std::uint16_t hardness_raw = 0;
        ok = reader.take_u8(id) && ok;
        std::string name;
        ok = reader.take_string(name) && ok;
        ok = reader.take_u8(movement_byte) && ok;
        ok = reader.take_u8(solid_byte) && ok;
        ok = reader.take_u32(density_raw) && ok;
        ok = reader.take_u8(spec.color[0]) && ok;
        ok = reader.take_u8(spec.color[1]) && ok;
        ok = reader.take_u8(spec.color[2]) && ok;
        ok = reader.take_u8(tagcount_raw) && ok;
        spec.id = id;
        spec.name = name;
        spec.movement = static_cast<Movement>(movement_byte);
        spec.solid = (solid_byte & 1) != 0;
        spec.density = expand_fuzz_density(density_raw);
        const std::size_t tags = static_cast<std::size_t>(tagcount_raw) % (kFuzzMaxTags + 1);
        for (std::size_t t = 0; t < tags; ++t) {
            std::string tag;
            ok = reader.take_string(tag) && ok;
            spec.tags.push_back(tag);
        }
        ok = reader.take_u16(hardness_raw) && ok;
        spec.hardness =
            static_cast<int>(static_cast<std::int16_t>(hardness_raw) % 11);
        table.materials.push_back(spec);
    }
    return ok && !reader.short_fill;
}

//   ReactionTable: u16 version_raw | u8 count_raw | reactions...
//     reaction: string name | u16 prob_raw | u8 rcount_raw |
//               reactants... | u8 pcount_raw | products... |
//               u8 has_min | u16 min_raw | u8 has_max | u16 max_raw |
//               u8 has_pt | u16 pt_raw
//   count = 1 + raw % 4; rcount = 1 + raw % 2; pcount = raw % 4 (0..3: forges
//   empty and over-wide product lists); prob = raw % 203 - 51 (-51..151);
//   temps are full int16 (negatives reachable).
inline bool fill_reaction_table(SliceReader& reader, ReactionTable& table) {
    table = ReactionTable{};
    std::uint16_t version_raw = 0;
    std::uint8_t count_raw = 0;
    bool ok = reader.take_u16(version_raw);
    ok = reader.take_u8(count_raw) && ok;
    table.version = static_cast<int>(version_raw % 6) - 1;
    const std::size_t count = 1 + (static_cast<std::size_t>(count_raw) % kFuzzMaxReactions);
    table.reactions.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        ReactionSpec spec;
        std::uint16_t prob_raw = 0;
        std::uint8_t rcount_raw = 0;
        std::uint8_t pcount_raw = 0;
        std::uint8_t flag = 0;
        std::uint16_t temp_raw = 0;
        std::string name;
        ok = reader.take_string(name) && ok;
        ok = reader.take_u16(prob_raw) && ok;
        ok = reader.take_u8(rcount_raw) && ok;
        spec.name = name;
        spec.probability = static_cast<int>(prob_raw % 203) - 51;
        const std::size_t rcount = 1 + (static_cast<std::size_t>(rcount_raw) % kFuzzMaxSlots);
        for (std::size_t s = 0; s < rcount; ++s) {
            ReactantSpec reactant;
            std::uint8_t tag_byte = 0;
            std::string ref;
            ok = reader.take_u8(tag_byte) && ok;
            ok = reader.take_string(ref) && ok;
            reactant.is_tag = (tag_byte & 1) != 0;
            reactant.name = ref;
            spec.reactants.push_back(reactant);
        }
        ok = reader.take_u8(pcount_raw) && ok;
        const std::size_t pcount = static_cast<std::size_t>(pcount_raw) % (kFuzzMaxSlots + 2);
        for (std::size_t s = 0; s < pcount; ++s) {
            ProductSpec product;
            std::string ref;
            ok = reader.take_string(ref) && ok;
            product.material = ref;
            spec.products.push_back(product);
        }
        ok = reader.take_u8(flag) && ok;
        spec.gate.has_min = (flag & 1) != 0;
        ok = reader.take_u16(temp_raw) && ok;
        spec.gate.min_value = static_cast<std::int16_t>(temp_raw);
        ok = reader.take_u8(flag) && ok;
        spec.gate.has_max = (flag & 1) != 0;
        ok = reader.take_u16(temp_raw) && ok;
        spec.gate.max_value = static_cast<std::int16_t>(temp_raw);
        ok = reader.take_u8(flag) && ok;
        spec.has_product_temperature = (flag & 1) != 0;
        ok = reader.take_u16(temp_raw) && ok;
        spec.product_temperature = static_cast<std::int16_t>(temp_raw);
        table.reactions.push_back(spec);
    }
    return ok && !reader.short_fill;
}

} // namespace ashfall
