#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <unordered_map>
#include <vector>

#include "ashfall/flat_cell_map.hpp"

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
    int density = 0;
};

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

/// Portable, deterministic cellular world. Godot and rendering code must not own this state.
class SimulationWorld {
public:
    SimulationWorld();

    [[nodiscard]] MaterialId cell(CellPosition position) const;
    void reset();
    void set_cell(CellPosition position, MaterialId material);
    void fill_rect(CellPosition first, CellPosition last, MaterialId material);
    void step();

    [[nodiscard]] std::size_t occupied_cell_count() const noexcept { return cells_.size(); }
    [[nodiscard]] std::uint64_t tick() const noexcept { return tick_; }
    [[nodiscard]] bool has_chunk(CellPosition chunk_position) const;
    [[nodiscard]] std::vector<CellPosition> active_chunks() const;
    bool read_chunk(CellPosition chunk_position, std::span<MaterialId> out_buffer) const;
    void copy_rect(CellPosition first, CellPosition last, std::span<MaterialId> out_buffer) const;
    [[nodiscard]] std::vector<std::uint8_t> save() const;
    [[nodiscard]] bool load(std::span<const std::uint8_t> bytes);

    [[nodiscard]] bool is_solid(CellPosition position) const;
    [[nodiscard]] bool can_move_to(CellPosition min_pos, CellPosition max_pos) const;
    [[nodiscard]] std::vector<CellPosition> dirty_chunks() const;
    void clear_dirty_chunks();
    void mark_chunk_dirty(CellPosition chunk_position);

    /// Hand-authored P1 play level: floor, walls, platforms, sand dunes and a
    /// water reservoir. Resets the world first; deterministic content.
    void build_play_level();
    /// Player spawn for the play level, in cell units (feet position).
    [[nodiscard]] static CellPosition play_level_spawn() noexcept { return {128, 240}; }

private:
    // Flat open-addressing storage for the hot path: step() performs ~100k
    // inserts/lookups per tick at 512x512, where unordered_map node traffic
    // dominates. cells_ is authoritative and persistent; the scratch tables
    // are cleared and reused every tick (never reallocated in steady state).
    using FlatCells = FlatMap<CellPosition, MaterialId, CellPositionHash>;
    using FlatMarks = FlatMap<CellPosition, std::uint8_t, CellPositionHash>;
    using DynCells = std::vector<std::pair<CellPosition, MaterialId>>;

    [[nodiscard]] const MaterialDefinition& material(MaterialId id) const;
    [[nodiscard]] static bool prefers_left(CellPosition position, std::uint64_t tick);
    void rebuild_chunks(const FlatCells& cells);
    void write_chunk_cell(CellPosition position, MaterialId material_id);
    void write_chunk_cell_deferred(
        CellPosition position, MaterialId material_id, std::set<CellPosition>* maybe_emptied);
    [[nodiscard]] static std::size_t chunk_index(int local_x, int local_y) noexcept {
        return static_cast<std::size_t>(local_y * kChunkSize + local_x);
    }
    [[nodiscard]] static bool chunk_storage_empty(const std::vector<MaterialId>& chunk) noexcept;

    // cells_ is the authoritative occupied set, persistent across ticks:
    // step() reads it in place and swaps in the next state, so there is no
    // per-tick collect/rebuild pass. chunks_ mirrors it for chunk-granular
    // presentation and read_chunk queries.
    FlatCells cells_;
    FlatCells next_scratch_;
    FlatMarks stayed_scratch_;
    DynCells dyn_scratch_;
    DynCells order_scratch_;
    std::map<MaterialId, MaterialDefinition> materials_;
    std::map<CellPosition, std::vector<MaterialId>> chunks_;
    std::set<CellPosition> dirty_chunks_;
    std::uint64_t tick_ = 0;
};

} // namespace ashfall
