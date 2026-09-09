#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

namespace ashfall {

using MaterialId = std::uint8_t;
constexpr MaterialId kAir = 0;
constexpr MaterialId kStone = 1;
constexpr MaterialId kSand = 2;
constexpr MaterialId kWater = 3;
constexpr MaterialId kSteam = 4;
constexpr MaterialId kFire = 5;
constexpr MaterialId kWood = 6;
constexpr MaterialId kLava = 7;
constexpr MaterialId kRock = 8;
constexpr MaterialId kOil = 9;

enum class Movement : std::uint8_t { Static, Powder, Liquid, Gas };

constexpr int kChunkSize = 64;

struct CellPosition {
    int x = 0;
    int y = 0;
    auto operator<=>(const CellPosition&) const = default;
};

struct CellPositionHash {
    std::size_t operator()(CellPosition p) const noexcept {
        // Deterministic 32-bit mix (independent of std::hash seed).
        std::uint32_t h = static_cast<std::uint32_t>(p.x) * 0x9E3779B1u;
        h ^= static_cast<std::uint32_t>(p.y) * 0x85EBCA77u + 0x9E3779B9u + (h << 6) + (h >> 2);
        return static_cast<std::size_t>(h);
    }
};

} // namespace ashfall
