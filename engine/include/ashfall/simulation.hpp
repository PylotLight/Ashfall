#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace ashfall {

using MaterialId = std::uint8_t;
constexpr MaterialId kAir = 0;
constexpr MaterialId kStone = 1;
constexpr MaterialId kSand = 2;
constexpr MaterialId kWater = 3;
constexpr int kChunkSize = 64;

enum class Movement : std::uint8_t { Static, Powder, Liquid };

struct MaterialDefinition {
    Movement movement = Movement::Static;
    bool solid = false;
};

struct CellPosition {
    int x = 0;
    int y = 0;
    auto operator<=>(const CellPosition&) const = default;
};

/// Portable, deterministic cellular world. Godot and rendering code must not own this state.
class SimulationWorld {
public:
    SimulationWorld();

    [[nodiscard]] MaterialId cell(CellPosition position) const;
    void set_cell(CellPosition position, MaterialId material);
    void fill_rect(CellPosition first, CellPosition last, MaterialId material);
    void step();

    [[nodiscard]] std::size_t occupied_cell_count() const;
    [[nodiscard]] std::uint64_t tick() const noexcept { return tick_; }
    [[nodiscard]] std::vector<std::uint8_t> save() const;
    [[nodiscard]] bool load(std::span<const std::uint8_t> bytes);

private:
    using CellMap = std::map<CellPosition, MaterialId>;

    [[nodiscard]] const MaterialDefinition& material(MaterialId id) const;
    [[nodiscard]] CellPosition movement_target(CellPosition source, MaterialId material, const CellMap& state) const;
    [[nodiscard]] static bool is_empty(CellPosition position, const CellMap& state);
    [[nodiscard]] static bool prefers_left(CellPosition position, std::uint64_t tick);
    void rebuild_chunks(const CellMap& cells);
    [[nodiscard]] CellMap collect_cells() const;

    std::map<MaterialId, MaterialDefinition> materials_;
    std::map<CellPosition, std::vector<MaterialId>> chunks_;
    std::uint64_t tick_ = 0;
};

} // namespace ashfall
